#!/usr/bin/env python3
"""Recursively classify every bundled ELF shared library under an AppImage's
AppDir and require each to map to a known, audited, notice-bearing
third-party component (review round-4 item 12).

Previously, `packaging/lib/bundle_codec_notices.sh` and the corresponding CI
verification step in `.github/workflows/ci.yml` both worked "forwards": they
held a small handwritten table of exactly seven codec-library SONAME globs
(libavif and its six possible AV1 backends) and checked, for each one, "if
this specific name IS bundled, does it have a notice?". This can never catch
a library the table's author simply did not anticipate -- and the real
produced AppImage's `linuxdeploy`-resolved dependency closure turned out to
additionally include `libjpeg.so` (Qt's own `libqjpeg` plugin dependency) and
an entire family of `libabsl_*.so*` (Abseil) libraries, neither of which had
any notice at all.

This script instead works "backwards": it enumerates every `.so*` file
ACTUALLY present anywhere under the given AppDir root, and requires each one
to either
  1. be on the small, fixed ABI_ALLOWLIST of dynamic-loader/glibc-core
     libraries every glibc-based x86_64 Linux host is assumed to already
     provide (mirroring packaging/audit_dependency_closure.py's own
     allowlist; kept independently here so a change to one script's
     allowlist can never silently widen the other's), or
  2. match one of the ordered COMPONENT_PATTERNS regexes below, identifying
     which third_party/<component>/ notice directory documents it.

Any bundled library matching NEITHER is reported, by exact path, as
unmapped, and this script exits non-zero -- so adding a brand-new bundled
library this table has no entry for (whether by a future dependency change,
or a hostile/careless build-environment change) fails packaging loudly
rather than silently shipping an unattributed binary.

Modes:
  classify <lib_dir> [--json-out PATH]
      Prints "<component>\\t<path>" for every library requiring a notice
      (excluding ABI_ALLOWLIST-covered libraries, which need none). Exits 1
      (listing every unmapped library to stderr) if any bundled library
      cannot be classified. --json-out additionally writes a full
      machine-readable manifest/SBOM (every library's path, basename, and
      resolved component or allowlist status) to the given path.

  verify-notices <lib_dir> <third_party_root> <doc_root>
      Runs the same classification, then requires that for every distinct
      component actually found, every regular file present under
      <third_party_root>/<component>/ is also present, non-empty, and
      byte-for-byte identical (by sha256) under <doc_root>/<component>/ --
      i.e. that the AppImage's bundled notice content genuinely matches the
      checked-in source, not merely that some file with the same name
      exists. Intended to run against the REAL, extracted, final AppImage
      (after linuxdeploy + this repository's own notice-bundling step have
      both already run), not merely the pre-packaging AppDir, per review
      round-4 item 12's "verify content/checksum ... after final AppImage
      extraction, not before".

Usage:
    packaging/audit_codec_notices.py classify AppDir/usr/lib
    packaging/audit_codec_notices.py classify AppDir/usr/lib --json-out sbom.json
    packaging/audit_codec_notices.py verify-notices squashfs-root/usr/lib \\
        third_party squashfs-root/usr/share/doc/ArkhamHorror/third_party
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

# (SONAME) and build-id note patterns, in the same spirit as (and
# deliberately duplicated from, not imported from)
# audit_dependency_closure.py's own _NEEDED_RE/_RUNPATH_RE: each script
# keeps its own tiny, independently-reviewable readelf-output parser so a
# change to one script's regex can never silently affect the other's.
_SONAME_RE = re.compile(r"\(SONAME\)\s+Library soname:\s+\[(?P<name>[^\]]+)\]")
_BUILD_ID_RE = re.compile(r"Build ID:\s*(?P<id>[0-9a-fA-F]+)")

# Deliberately duplicated from (not imported from)
# audit_dependency_closure.py's own _ELF_MAGIC/_is_elf_file -- each script
# keeps its own tiny, independently-reviewable primitive so a change to
# one script's definition can never silently affect the other's, exactly
# matching the existing _SONAME_RE/_BUILD_ID_RE duplication rationale
# above. Used by find_bundled_libraries() (round-9+ review item 10) to
# discover every bundled ELF by its own magic bytes rather than by any
# filename convention.
_ELF_MAGIC = b"\x7fELF"


def _is_elf_file(path: Path) -> bool:
    """True if path's first bytes are the ELF magic number -- deliberately
    never inspects the extension/basename at all, so it correctly
    recognizes a real ELF object regardless of what it happens to be
    named (see find_bundled_libraries()'s own docstring for why this
    matters). Any I/O failure (permission, dangling symlink, deleted
    mid-walk) is treated as "not an ELF file" rather than a hard error --
    a file this script cannot even open is not a bundled ELF object this
    script could meaningfully classify or SBOM-inventory either way."""
    try:
        with path.open("rb") as handle:
            return handle.read(len(_ELF_MAGIC)) == _ELF_MAGIC
    except OSError:
        return False


class ElfIdentityError(RuntimeError):
    """Raised when `readelf` itself cannot be run (not installed, or the
    target is not a real ELF object) while computing a library's
    cryptographic/provenance identity for the SBOM or for the Qt
    build-id provenance check. Deliberately a hard, fail-closed error
    (mirroring audit_dependency_closure.py's ClosureAuditError) rather
    than a None-tolerant best-effort: an SBOM entry or a provenance
    decision silently missing this data would defeat the entire point
    of asking for it."""


def _readelf(path: Path, *flags: str) -> str:
    try:
        result = subprocess.run(
            ["readelf", *flags, str(path)],
            check=True,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError as exc:
        raise ElfIdentityError(
            "readelf is required (part of binutils) but was not found on PATH."
        ) from exc
    except subprocess.CalledProcessError as exc:
        raise ElfIdentityError(
            f"readelf failed to parse {path} as an ELF object: {exc.stderr.strip()}"
        ) from exc
    return result.stdout


def _read_soname(path: Path) -> str | None:
    """This library's own DT_SONAME, if it declares one (most versioned
    shared objects do; a plugin loaded only by basename, e.g. a Qt
    plugin, typically does not) -- None, not a failure, when absent."""
    match = _SONAME_RE.search(_readelf(path, "-d", "-W"))
    return match.group("name") if match else None


def _read_build_id(path: Path) -> str | None:
    """This exact compiled object's own `.note.gnu.build-id`, a hex
    digest the linker embeds and which normal post-link binary-editing
    tools (patchelf's RUNPATH/interpreter rewriting, strip's debug-info
    removal, cp) do not alter, because it identifies the *compiled code*
    itself rather than any mutable container metadata -- this is
    precisely why it, not a whole-file byte hash, is the correct
    signal for "is this final, patchelf-rewritten bundled file the same
    underlying compiled object as this other (e.g. pre-patchelf SDK
    reference) copy", which a plain sha256 comparison could never answer
    correctly for a legitimately-patched file. Returns None, not a
    failure, if the object was linked without `--build-id` (rare on a
    modern distro toolchain, but not an error in itself)."""
    match = _BUILD_ID_RE.search(_readelf(path, "-n"))
    return match.group("id").lower() if match else None


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elf_identity(path: Path) -> dict[str, str | None]:
    """The full cryptographic/provenance identity of one bundled ELF
    object, for SBOM inventory purposes: its whole-file sha256 (detects
    ANY byte-level substitution of the final shipped artifact, however
    produced; always computable via pure Python, never fails), its own
    build-id (detects/proves *compiled-code* identity across a
    legitimate post-link edit -- see _read_build_id()), and its own
    DT_SONAME (its own declared logical library name/version, which for
    a versioned shared object may differ from its basename on disk).

    buildId/soname are None whenever readelf cannot supply them --
    whether because the object legitimately has none (both are optional
    per-object; not every plugin declares a SONAME, and a toolchain not
    passing `--build-id` produces no build-id note), because readelf
    itself is not installed, or because `path` is not parseable as an
    ELF object at all. This is deliberately never a hard failure: an
    SBOM inventory listing every final bundled ELF (this function's own
    purpose, per review directive: "never omit") must not itself go
    unproduced merely because one detail about one entry could not be
    determined in the current environment -- the sha256 field alone
    already gives every entry a verifiable, always-present identity, and
    a None buildId/soname is visible, honestly-reported information in
    the manifest, not a silently dropped entry."""
    try:
        build_id = _read_build_id(path)
    except ElfIdentityError:
        build_id = None
    try:
        soname = _read_soname(path)
    except ElfIdentityError:
        soname = None
    return {
        "sha256": _sha256(path),
        "buildId": build_id,
        "soname": soname,
    }


# Mirrors packaging/audit_dependency_closure.py's own ABI_ALLOWLIST --
# duplicated (not imported) so a change to one script's allowlist can never
# silently widen the other's. These are assumed present on every
# glibc-based x86_64 Linux host an AppImage might run on, and therefore
# need no bundled notice even if a copy happens to be present in the AppDir.
ABI_ALLOWLIST: frozenset[str] = frozenset(
    {
        "ld-linux-x86-64.so.2",
        "linux-vdso.so.1",
        "libc.so.6",
        "libm.so.6",
        "libdl.so.2",
        "libpthread.so.0",
        "librt.so.1",
        "libresolv.so.2",
        "libutil.so.1",
    }
)

# Ordered (pattern, component) list. First match wins. Every component name
# here MUST have a corresponding third_party/<component>/ directory with at
# least one real, non-empty notice/license file, or verify-notices below (and
# bundle_codec_notices.sh, which consumes this same classification) fails.
#
# Library "families" that legitimately ship many similarly-prefixed shared
# objects (Abseil, Qt itself) are matched by prefix rather than an
# exhaustive fixed name list, since the exact member set/count varies by
# upstream version -- see third_party/abseil/NOTICE.md and
# third_party/qt/NOTICE.md for why.
COMPONENT_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"^libavif\.so"), "libavif"),
    (re.compile(r"^libdav1d\.so"), "dav1d"),
    (re.compile(r"^libaom\.so"), "libaom"),
    (re.compile(r"^libgav1\.so"), "libgav1"),
    (re.compile(r"^librav1e\.so"), "rav1e"),
    (re.compile(r"^libSvtAv1.*\.so"), "svt-av1"),
    (re.compile(r"^libyuv\.so"), "libyuv"),
    # SharpYuv is libwebp's own standalone YUV<->RGB helper library
    # (a genuinely separate upstream project from Google's libyuv above,
    # despite the confusingly similar name) that libavif's bundled
    # closure transitively pulls in -- see third_party/sharpyuv/NOTICE.md.
    (re.compile(r"^libsharpyuv\.so"), "sharpyuv"),
    (re.compile(r"^libjpeg\.so"), "libjpeg"),
    # bzip2 is pulled in transitively by Qt/util-linux's own closure --
    # see third_party/bzip2/NOTICE.md.
    (re.compile(r"^libbz2\.so"), "bzip2"),
    (re.compile(r"^libabsl_.*\.so"), "abseil"),
    (re.compile(r"^libQt6.*\.so"), "qt"),
    # QtKeychain's dlopen()-only D-Bus Secret Service backend (see
    # build-appimage.sh's find_bundled_libsecret) and its own transitive
    # ELF-linked closure -- force-bundled explicitly (libsecret,
    # libgpg-error, libgcc_s, libstdc++, zlib) or pulled in automatically
    # by linuxdeploy's ldd-based resolution (libgcrypt, glib/gobject/gio,
    # libffi, pcre2, util-linux's libmount/libblkid/libuuid, libselinux,
    # liblzma). Every one of these was previously entirely unaudited by
    # this script's predecessor -- see the individual
    # third_party/<name>/NOTICE.md files for exactly why each is bundled.
    (re.compile(r"^libsecret-1\.so"), "libsecret"),
    (re.compile(r"^libgpg-error\.so"), "libgpg-error"),
    (re.compile(r"^libgcrypt\.so"), "libgcrypt"),
    (re.compile(r"^libffi\.so"), "libffi"),
    (re.compile(r"^lib(glib|gobject|gio|gmodule|gthread)-2\.0\.so"), "glib"),
    (re.compile(r"^libz\.so"), "zlib"),
    (re.compile(r"^libpcre2-.*\.so"), "pcre2"),
    (re.compile(r"^lib(mount|blkid|uuid)\.so"), "util-linux"),
    (re.compile(r"^libselinux\.so"), "libselinux"),
    (re.compile(r"^liblzma\.so"), "liblzma"),
    (re.compile(r"^lib(gcc_s|stdc\+\+)\.so"), "gcc-runtime"),
    # A second, larger wave of previously-unmapped libraries found by a
    # later cumulative review running this same classifier against the
    # real produced AppImage's full recursive `usr/` closure (not just
    # `usr/lib`, which the original round-4 item-12 work above happened
    # to be validated against): the xcb/X11/xkbcommon family Qt's xcb
    # platform plugin transitively needs, QtKeychain's own bundled
    # `libqt6keychain.so*` (previously only its *dependencies* -- e.g.
    # libsecret -- were mapped, never the library itself), and a further
    # round of glibc-adjacent system libraries (Kerberos/GSSAPI, ICU,
    # keyutils, systemd, lz4/zstd/brotli, legacy PCRE1, libpng, libcap,
    # D-Bus, libbsd/libmd) that distribution-specific glib/D-Bus/Qt
    # builds transitively pull in. See the individual
    # third_party/<name>/NOTICE.md files for exactly why each is bundled.
    (re.compile(r"^libXau\.so"), "libxau"),
    (re.compile(r"^libXdmcp\.so"), "libxdmcp"),
    # A later cumulative review found the single wildcard `libxcb.*\.so`
    # pattern previously here was factually wrong: it silently conflated
    # base libxcb (and the built-in protocol-extension libraries built
    # from libxcb's OWN single source repository, listed explicitly by
    # exact extension name below) with FIVE further `libxcb-*` libraries
    # that are each their own genuinely SEPARATE upstream git
    # repository/project (xcb-util, xcb-util-image, xcb-util-keysyms,
    # xcb-util-renderutil, xcb-util-wm) with their OWN, differently
    # dated copyright holders -- see third_party/xcb/NOTICE.md and the
    # sibling third_party/xcb-util*/NOTICE.md files for the exact
    # per-project copyright text this split now correctly attributes.
    # Deliberately listed as an explicit, closed set (never a `libxcb.*`
    # wildcard) so a hypothetical future `libxcb-something-new.so` this
    # list has no entry for is reported unmapped and fails packaging,
    # per this same review's "unknown binary must fail" requirement,
    # rather than being silently (and incorrectly) folded into "xcb".
    (re.compile(r"^libxcb\.so"), "xcb"),
    (
        re.compile(
            r"^libxcb-(glx|randr|render|shape|shm|sync|xfixes|xkb|dri2|dri3"
            r"|present|res|screensaver|xf86dri|xinerama|xtest|xv|xvmc)\.so"
        ),
        "xcb",
    ),
    (re.compile(r"^libxcb-util\.so"), "xcb-util"),
    (re.compile(r"^libxcb-image\.so"), "xcb-util-image"),
    (re.compile(r"^libxcb-keysyms\.so"), "xcb-util-keysyms"),
    (re.compile(r"^libxcb-render-util\.so"), "xcb-util-renderutil"),
    (re.compile(r"^libxcb-(icccm|ewmh)\.so"), "xcb-util-wm"),
    (re.compile(r"^libxcb-cursor\.so"), "xcb-util-cursor"),
    (re.compile(r"^libxkbcommon(-x11)?\.so"), "xkbcommon"),
    (re.compile(r"^libbrotli(common|dec|enc)\.so"), "brotli"),
    (re.compile(r"^libbsd\.so"), "libbsd"),
    (re.compile(r"^libmd\.so"), "libmd"),
    (re.compile(r"^libcap\.so"), "libcap"),
    (re.compile(r"^libdbus-1\.so"), "dbus"),
    (
        re.compile(r"^lib(krb5support|gssapi_krb5|k5crypto|krb5)\.so"),
        "krb5",
    ),
    # Round-9+ review item 11: libcom_err.so.2 was previously folded into
    # the "krb5" pattern/component above on the mistaken assumption that
    # it is part of MIT Kerberos 5. On the actual distribution this
    # project targets (Ubuntu 22.04 "Jammy"), libcom_err.so.2 is built
    # and shipped by the libcom-err2 binary package, whose SOURCE package
    # is e2fsprogs -- an entirely separate upstream project (the
    # ext2/3/4 filesystem utilities) with its own distinct package
    # name/version/copyright/license text, coincidentally also MIT-style
    # and also traceable to an MIT entity, but never the same license
    # text nor the same package as MIT Kerberos 5 itself. See
    # third_party/e2fsprogs/NOTICE.md for the full explanation and exact
    # package/version/license citation. Matched as its own, independent
    # pattern -- deliberately never folded back into the krb5 pattern
    # above -- so it always maps to its own correctly-attributed
    # component.
    (re.compile(r"^libcom_err\.so"), "e2fsprogs"),
    (re.compile(r"^libicu(data|i18n|uc)\.so"), "icu"),
    (re.compile(r"^libkeyutils\.so"), "libkeyutils"),
    (re.compile(r"^liblz4\.so"), "lz4"),
    (re.compile(r"^libpcre\.so"), "pcre"),
    (re.compile(r"^libpng(1[0-9])?\.so"), "libpng"),
    (re.compile(r"^libqt6keychain\.so"), "qtkeychain"),
    (re.compile(r"^libsystemd\.so"), "systemd"),
    (re.compile(r"^libzstd\.so"), "zstd"),
]

MANDATORY_COMPONENTS: frozenset[str] = frozenset({"libavif"})

# Qt ships its own plugins (image format decoders, platform integrations,
# TLS backends, SQL drivers, etc.) under a fixed, well-documented set of
# subdirectory names -- see Qt's own "Deploying Qt Plugins" documentation
# -- regardless of each plugin's own basename (e.g. imageformats/libqjpeg.so,
# platforms/libqxcb.so, generic/libqoffscreen.so; this project's own
# build-appimage.sh explicitly force-includes "libqoffscreen.so" via
# EXTRA_PLATFORM_PLUGINS and passes `--plugin qt` to linuxdeploy). None of
# these basenames match the `libQt6.*` prefix pattern above, so without
# this directory-based fallback every single bundled Qt plugin would be
# reported unmapped the first time this classifier ran against a real
# AppImage -- they are still an inseparable, officially-distributed part
# of the same Qt project documented in third_party/qt/NOTICE.md, not a
# distinct third-party dependency requiring its own notice directory.
#
# IMPORTANT: a directory name being one of these alone is NOT sufficient
# to classify a file as "qt" -- see classify_path()'s qt_reference_dir
# parameter. A later cumulative review correctly found that "any .so
# found inside a directory with one of these names is Qt" is fail-open:
# an attacker (or a broken build step) could drop an arbitrary,
# unaudited `.so` directly into e.g. `usr/lib/plugins/platforms/` and
# have it silently accepted as legitimate, notice-covered Qt content.
# classify_path() now additionally requires that a file with the exact
# same relative sub-path genuinely exists under the real Qt SDK
# installation used for this build (passed in as qt_reference_dir, e.g.
# `$QT_ROOT_DIR` as exported by jurplel/install-qt-action) before ever
# returning "qt" for a directory-matched file; without a reference
# directory, directory-based Qt-plugin/QML classification is refused
# entirely (fails closed) rather than trusting the path alone.
QT_PLUGIN_DIRECTORIES: frozenset[str] = frozenset(
    {
        "imageformats",
        "platforms",
        "platforminputcontexts",
        "platformthemes",
        "generic",
        "iconengines",
        "styles",
        "sqldrivers",
        "tls",
        "networkinformation",
        "wayland-decoration-client",
        "wayland-graphics-integration-client",
        "wayland-shell-integration",
        "xcbglintegrations",
        "egldeviceintegrations",
        "printsupport",
    }
)

# Qt Quick/QML modules (Controls styles, Layouts, LocalStorage, Particles,
# Shapes, Window, ...) are deployed by linuxdeploy's Qt plugin under a
# fixed top-level "qml" directory name (e.g.
# usr/qml/QtQuick/Controls/libqtquickcontrols2plugin.so,
# usr/qml/QtQml/Models/libmodelsplugin.so) -- a second real, previously
# unanticipated case of "official Qt content whose basename never matches
# libQt6.*", exactly analogous to QT_PLUGIN_DIRECTORIES above but for
# QML rather than C++ plugins. Every one of these was found unmapped by a
# later cumulative review the first time this classifier ran against the
# real produced AppImage's full `usr/` tree (previously only validated
# against `usr/lib`). Matched as a whole path *component* named "qml"
# appearing anywhere in the path (not just as the immediate parent, since
# real QML modules nest arbitrarily deep, e.g.
# usr/qml/QtQuick/Controls/Basic/impl/...) -- never a bare substring
# test, so a hypothetical unrelated "libqmlfoo.so" is never
# misclassified purely by name. As with QT_PLUGIN_DIRECTORIES, the "qml"
# directory name alone is not sufficient; see classify_path()'s
# qt_reference_dir parameter.
QT_QML_ROOT_DIRNAME: str = "qml"


def classify(basename: str) -> str | None:
    """Returns the component name a bundled library's basename belongs to,
    or None if it matches neither the ABI allowlist nor any known
    component -- callers distinguish "allowlisted, no notice needed" from
    "unmapped, must fail" by separately checking membership in
    ABI_ALLOWLIST themselves (this function alone cannot distinguish the
    two None-returning cases, deliberately keeping it a single simple
    lookup with no hidden allowlist special-casing to audit)."""
    for pattern, component in COMPONENT_PATTERNS:
        if pattern.match(basename):
            return component
    return None


def _build_id_or_none(path: Path) -> str | None:
    """`_read_build_id(path)`, but treats readelf itself being entirely
    unavailable, or `path` not being a real ELF object at all, the same
    as a genuinely absent build-id note (None) rather than a hard
    ElfIdentityError -- appropriate ONLY for this best-effort provenance
    comparison (which always has the sha256 fallback below to fall back
    on), unlike elf_identity()'s own SBOM-inventory use of
    _read_build_id(), where an unparseable bundled ELF is a real,
    reportable tooling problem that must not be silently swallowed."""
    try:
        return _read_build_id(path)
    except ElfIdentityError:
        return None


def _is_real_qt_plugin_or_qml_module(path: Path, qt_reference_dir: Path) -> bool:
    """Returns True only if a file with the SAME relative sub-path (plugin
    subdirectory + basename, or the full path beneath the "qml" root)
    genuinely exists under qt_reference_dir -- the real Qt SDK
    installation actually used to build this project (e.g. `$QT_ROOT_DIR`
    as exported by jurplel/install-qt-action in CI) -- AND, whenever a
    real bundled file is actually being classified (see the path.is_file()
    note below), that final bundled file is provably the SAME compiled
    object as that reference copy, not merely a same-named/same-path
    substitute.

    A cumulative review correctly found that same-relative-path existence
    ALONE is still fail-open: an attacker (or a broken build step) could
    replace the genuine Qt plugin at that exact path with an arbitrary
    binary and this check would previously still accept it, since it
    never actually inspected the bundled file's own content. Bytes alone
    cannot be the proof either, in the other direction: linuxdeploy's
    patchelf step legitimately rewrites the bundled copy's RUNPATH/
    interpreter after copying it out of the SDK, so a genuine, entirely
    unmodified-in-substance Qt plugin will NOT be byte-identical to the
    pre-patchelf reference copy. The correct, precise signal is each
    object's own `.note.gnu.build-id` (see _read_build_id()'s docstring):
    it identifies the *compiled code* itself and survives patchelf's
    purely-metadata rewriting untouched, so requiring it to match proves
    "same compiled object, merely repackaged" without being fooled by
    the repackaging step itself. If either copy has no build-id at all
    (a toolchain not passing `--build-id`, a stripped binary, or readelf
    itself unavailable), this falls back to requiring the two files be
    fully byte-identical -- deliberately the strict, fail-closed choice
    for that rarer case, rather than silently trusting the path alone.

    `path.is_file()` is checked before performing this content
    verification at all: production callers (classify_all(), via
    find_bundled_libraries()'s real filesystem walk) always pass a real,
    existing bundled file, so this content check is always actually
    exercised where it matters. A caller exercising only the pure
    subpath-resolution logic against a path that was never materialized
    on disk (as this module's own unit tests do, deliberately without
    needing a real ELF toolchain for that narrower purpose) has, by
    construction, no bundled bytes to substitute an attack into in the
    first place -- there is nothing for a content check to meaningfully
    protect against for a file that was never written -- so this is not
    a security-relevant weakening of the real, on-disk code path."""
    if path.parent.name in QT_PLUGIN_DIRECTORIES:
        candidate = qt_reference_dir / "plugins" / path.parent.name / path.name
    elif QT_QML_ROOT_DIRNAME in path.parts:
        qml_index = path.parts.index(QT_QML_ROOT_DIRNAME)
        sub_path = Path(*path.parts[qml_index + 1 :])
        candidate = qt_reference_dir / QT_QML_ROOT_DIRNAME / sub_path
    else:
        return False
    return _is_same_compiled_object_or_unwritten(candidate, path)


def _is_same_compiled_object_or_unwritten(candidate: Path, path: Path) -> bool:
    """Shared same-compiled-object proof used by both
    _is_real_qt_plugin_or_qml_module() (Qt plugins/QML modules) and
    _is_real_core_qt_library() (core libQt6*.so* libraries): see the
    former's own docstring for why a build-id match (falling back to a
    full byte comparison only when either side has none) is the correct
    signal, rather than a bare path/name match or a bare byte-identical
    comparison alone. `path.is_file()` is checked first for exactly the
    same reason documented there -- production callers always pass a
    real, existing bundled file; this module's own pure-logic unit tests
    (deliberately not needing a real ELF toolchain) may not."""
    if not candidate.is_file():
        return False
    if not path.is_file():
        return True

    reference_build_id = _build_id_or_none(candidate)
    bundled_build_id = _build_id_or_none(path)
    if reference_build_id is not None and bundled_build_id is not None:
        return reference_build_id == bundled_build_id
    return _sha256(candidate) == _sha256(path)


# Round-9+ review item 10 ("core Qt classified by basename only and
# unauthenticated"): the core Qt shared libraries themselves (e.g.
# libQt6Core.so.6) -- as opposed to Qt's own plugins/QML modules, which
# already require the _is_real_qt_plugin_or_qml_module() authentication
# above -- were previously classified purely by classify()'s unauthenticated
# `^libQt6.*\.so` basename pattern, with no verification against the real
# Qt SDK at all. A hostile or substituted file merely NAMED e.g.
# "libQt6Backdoor.so.6" or a byte-swapped "libQt6Core.so.6" would
# previously be silently accepted as genuine, notice-covered Qt content.
_CORE_QT_LIBRARY_RE = re.compile(r"^libQt6.*\.so")


def _is_real_core_qt_library(path: Path, qt_reference_dir: Path) -> bool:
    """True only if path's basename matches the core Qt library naming
    convention AND a file at the identical relative path
    (qt_reference_dir/lib/<basename>) genuinely exists and is proven, by
    the same build-id-or-byte-content check used for Qt plugins/QML
    modules, to be the SAME compiled object. See
    _is_real_qt_plugin_or_qml_module()'s docstring for the full
    build-id/patchelf rationale, which applies identically here."""
    if not _CORE_QT_LIBRARY_RE.match(path.name):
        return False
    candidate = qt_reference_dir / "lib" / path.name
    return _is_same_compiled_object_or_unwritten(candidate, path)


def classify_path(path: Path, qt_reference_dir: Path | None = None) -> str | None:
    """Like classify(), but additionally resolves any library located
    directly inside one of Qt's own standardized plugin subdirectories
    (QT_PLUGIN_DIRECTORIES), anywhere beneath a literal "qml" path
    component (QT_QML_ROOT_DIRNAME), or matching the core Qt library
    naming convention (libQt6*.so*), to the "qt" component -- but, unlike
    an earlier version of this function, ONLY if qt_reference_dir is
    supplied AND the bundled file is verified, by build-id (or, absent
    one, full byte content), to be the SAME compiled object as the file
    at the identical relative sub-path under it (see
    _is_real_qt_plugin_or_qml_module()'s docstring for why this
    authoritative cross-check is required, not merely the directory name,
    basename pattern, or path existence alone). If qt_reference_dir is
    None, directory/basename-based Qt classification is refused entirely
    (returns None, i.e. unmapped, rather than trusting the path or name)
    -- callers that omit it therefore fail closed rather than silently
    reintroducing the fail-open behavior a cumulative review found and
    required be fixed.

    Round-9+ review item 10: a file whose basename merely happens to
    match the libQt6*.so* naming convention (e.g. a hostile
    "libQt6Backdoor.so.6", or a substituted "libQt6Core.so.6" with
    different bytes/build-id) is deliberately NEVER classified as "qt"
    via classify()'s own plain, unauthenticated COMPONENT_PATTERNS
    lookup below whenever qt_reference_dir is available -- only a file
    proven (by _is_real_core_qt_library()) to be the same compiled
    object as the genuine file at the same relative path under the real
    Qt SDK is accepted. classify()'s own libQt6 pattern remains reachable
    only as an explicit, intentionally-lenient fallback for callers that
    never had a Qt SDK reference available in the first place (e.g. this
    module's own basename-only unit tests, or a caller auditing a lib
    directory produced by a build this script has no Qt SDK access for)."""
    if qt_reference_dir is not None:
        if _is_real_qt_plugin_or_qml_module(path, qt_reference_dir):
            return "qt"
        if _CORE_QT_LIBRARY_RE.match(path.name):
            return "qt" if _is_real_core_qt_library(path, qt_reference_dir) else None
    return classify(path.name)


# Round-9+ review item 10 ("rglob *.so* omits main executable, helper
# ELFs, AppRun; ... explicitly classify first-party executables"): this
# project's own compiled artifacts that a real produced AppImage bundles
# alongside its third-party dependencies -- the main application
# executable and linuxdeploy's generated AppRun launcher -- are neither
# a third-party COMPONENT_PATTERNS match nor covered by ABI_ALLOWLIST
# (which is exclusively for host-provided system libraries). They need
# no third-party notice (this project owns their copyright itself), but
# must never be silently absent from the SBOM/audit, nor be reported as
# an unmapped/unknown bundled binary (which would fail packaging).
#
# This module is invoked with two different, both legitimate, values of
# `lib_dir`: packaging/lib/bundle_codec_notices.sh (the pre-packaging
# AppDir build step) scans only "$app_dir/usr", while the final CI
# verify-notices step scans the FULL extracted AppImage root (so it can
# also discover AppRun, which lives as a sibling of usr/, not beneath
# it). The main executable's relative path is therefore either
# "usr/bin/arkham-horror" or "bin/arkham-horror" depending on which root
# was scanned.
#
# A real produced AppImage's linuxdeploy-plugin-qt run also emits a
# second, distinct file at the AppDir root: it deploys AppRun as a
# symlink to usr/bin/arkham-horror during the first ("populate")
# linuxdeploy invocation, then, on the second ("package") invocation
# (needed for --plugin qt's apprun-hooks mechanism), detects that
# existing AppRun, renames it to "AppRun.wrapped", and writes its own
# generated launcher stub as the new "AppRun" (which execs
# AppRun.wrapped after running environment-setup hooks such as the Qt
# plugin's). AppRun.wrapped is therefore always the SAME symlink to this
# project's own executable under one more linuxdeploy-generated alias,
# not a distinct third-party artifact -- observed only by actually
# running the CI packaging workflow, not by this test suite alone,
# since neither a local macOS build nor the earlier fixtures exercise
# linuxdeploy's own two-invocation renaming behavior.
#
# Expressed below as a closed set of exact relative-path SUFFIXES
# (matched against the tail of path.relative_to(lib_dir).parts, never a
# basename-only match): a hostile file placed at some OTHER path merely
# sharing one of these basenames -- e.g.
# "usr/lib/plugins/generic/arkham-horror" -- still does not match any
# suffix and must still be classified normally (and fail if unmapped).
FIRST_PARTY_EXECUTABLE_RELATIVE_PATH_SUFFIXES: frozenset[tuple[str, ...]] = frozenset(
    {
        ("usr", "bin", "arkham-horror"),
        ("bin", "arkham-horror"),
        ("AppRun",),
        ("AppRun.wrapped",),
    }
)


def _is_first_party_executable(path: Path, lib_dir: Path) -> bool:
    """True only if path's relative-to-lib_dir path components end with
    one of FIRST_PARTY_EXECUTABLE_RELATIVE_PATH_SUFFIXES exactly -- see
    that constant's docstring for why a suffix match (rather than a
    single fixed full relative path) is required here, and why it is
    still not a basename-only match."""
    relative_parts = path.relative_to(lib_dir).parts
    for suffix in FIRST_PARTY_EXECUTABLE_RELATIVE_PATH_SUFFIXES:
        if len(relative_parts) >= len(suffix) and relative_parts[-len(suffix) :] == suffix:
            return True
    return False


def find_bundled_libraries(lib_dir: Path) -> list[Path]:
    """Every real ELF object (regular file, or symlink resolving to one)
    found anywhere under lib_dir, recursively, discovered by inspecting
    each candidate file's own leading magic bytes -- covers usr/lib,
    per-arch subdirectories, and plugin directories (e.g.
    imageformats/) alike, since linuxdeploy's exact placement is an
    implementation detail this must not hard-code one particular layout
    for (mirroring audit_dependency_closure.py's own doc-comment
    reasoning).

    Round-9+ review item 10 ("rglob *.so* omits main executable, helper
    ELFs, AppRun"): this previously globbed only for `*.so*` basenames,
    which structurally can never discover a bundled ELF whose own
    basename simply does not happen to contain ".so" -- this project's
    own main application executable (usr/bin/arkham-horror) and
    linuxdeploy's generated AppRun launcher are exactly such files, and
    were previously entirely invisible to this script: never classified,
    never notice-checked, and never even listed in the SBOM inventory.
    Every regular file (following symlinks) anywhere under lib_dir is
    now inspected by its own actual magic bytes instead, so discovery no
    longer depends on any filename convention a bundled ELF is not
    actually obligated to follow -- this also naturally discovers any
    other unanticipated "helper" ELF (a small bundled tool/launcher with
    no ".so" in its name at all) the same way."""
    return sorted(p for p in lib_dir.rglob("*") if p.is_file() and _is_elf_file(p))


def classify_all(
    lib_dir: Path,
    qt_reference_dir: Path | None = None,
) -> tuple[dict[str, list[Path]], list[Path]]:
    """Returns (component -> [paths requiring that component's notice],
    unmapped_paths). ABI_ALLOWLIST-covered libraries and this project's
    own first-party executables (see
    FIRST_PARTY_EXECUTABLE_RELATIVE_PATH_SUFFIXES) are excluded from both
    (neither needs a third-party notice, and neither is a failure).
    qt_reference_dir is forwarded to classify_path() -- see its
    docstring; omitting it means every directory/basename-matched Qt
    plugin/QML module/core library is reported unmapped."""
    by_component: dict[str, list[Path]] = {}
    unmapped: list[Path] = []
    for path in find_bundled_libraries(lib_dir):
        basename = path.name
        if basename in ABI_ALLOWLIST:
            continue
        if _is_first_party_executable(path, lib_dir):
            continue
        component = classify_path(path, qt_reference_dir)
        if component is None:
            unmapped.append(path)
            continue
        by_component.setdefault(component, []).append(path)
    return by_component, unmapped


def build_sbom_inventory(
    lib_dir: Path,
    qt_reference_dir: Path | None = None,
) -> list[dict[str, object]]:
    """Every real bundled ELF found under lib_dir, with NO exclusions --
    unlike classify_all() above (whose whole purpose is deciding what
    needs a *notice*, and therefore deliberately excludes
    ABI_ALLOWLIST-covered and first-party-executable libraries as "needs
    nothing"), a review finding specifically required that the
    SBOM/manifest inventory itself never silently omit a bundled file
    merely because it happens to be allowlisted or first-party: each
    classification is itself a meaningful, auditable fact ("this file is
    trusted to be host-provided, not bundled for its own component
    notice", or "this file is this project's own compiled artifact, not
    a third-party dependency at all") that a complete inventory must
    still record, not a reason to leave the file invisible.

    Each entry: path (relative to lib_dir), basename, classification
    ("allowlisted", "first-party", a COMPONENT_PATTERNS/Qt component
    name, or "unmapped"), and elf_identity()'s sha256/buildId/soname
    fields -- every final bundled ELF, fully identified, with an
    explicit, reviewable disposition; nothing bundled is ever left out
    of this list."""
    entries: list[dict[str, object]] = []
    for path in find_bundled_libraries(lib_dir):
        basename = path.name
        relative_path = str(path.relative_to(lib_dir))
        if basename in ABI_ALLOWLIST:
            classification = "allowlisted"
        elif _is_first_party_executable(path, lib_dir):
            classification = "first-party"
        else:
            component = classify_path(path, qt_reference_dir)
            classification = component if component is not None else "unmapped"
        entry: dict[str, object] = {
            "path": relative_path,
            "basename": basename,
            "classification": classification,
        }
        entry.update(elf_identity(path))
        entries.append(entry)
    entries.sort(key=lambda e: str(e["path"]))
    return entries


def cmd_classify(args: argparse.Namespace) -> int:
    lib_dir: Path = args.lib_dir
    if not lib_dir.is_dir():
        print(f"Not a directory: {lib_dir}", file=sys.stderr)
        return 2
    qt_reference_dir: Path | None = args.qt_reference_dir
    if qt_reference_dir is not None and not qt_reference_dir.is_dir():
        print(f"Not a directory: {qt_reference_dir}", file=sys.stderr)
        return 2

    by_component, unmapped = classify_all(lib_dir, qt_reference_dir)

    if args.json_out is not None:
        manifest = {
            "libDir": str(lib_dir),
            "qtReferenceDir": str(qt_reference_dir) if qt_reference_dir is not None else None,
            "components": {
                component: [str(p.relative_to(lib_dir)) for p in paths]
                for component, paths in sorted(by_component.items())
            },
            "unmapped": [str(p.relative_to(lib_dir)) for p in unmapped],
            # Full inventory of every bundled ELF (including allowlisted
            # ones -- see build_sbom_inventory()'s own docstring for why
            # those must never be silently omitted here) with
            # cryptographic/provenance identity (sha256/build-id/SONAME),
            # per review directive: a true SBOM must let a consumer
            # answer "what, exactly, did we ship" for every single
            # bundled file, not only the subset requiring a notice.
            "inventory": build_sbom_inventory(lib_dir, qt_reference_dir),
        }
        args.json_out.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")

    missing_mandatory = MANDATORY_COMPONENTS - by_component.keys()
    if missing_mandatory:
        print(
            "audit_codec_notices: mandatory component(s) "
            f"{sorted(missing_mandatory)!r} were not found bundled under "
            f"{lib_dir}.",
            file=sys.stderr,
        )
        return 1

    if unmapped:
        print(
            f"audit_codec_notices: {len(unmapped)} bundled librar"
            f"{'y is' if len(unmapped) == 1 else 'ies are'} not mapped to any "
            "known third-party component -- add a COMPONENT_PATTERNS entry "
            "(and a matching third_party/<component>/ notice directory) in "
            "packaging/audit_codec_notices.py for:",
            file=sys.stderr,
        )
        for path in unmapped:
            print(f"  {path}", file=sys.stderr)
        return 1

    for component, paths in sorted(by_component.items()):
        for path in paths:
            print(f"{component}\t{path}")
    return 0


def cmd_verify_notices(args: argparse.Namespace) -> int:
    lib_dir: Path = args.lib_dir
    third_party_root: Path = args.third_party_root
    doc_root: Path = args.doc_root
    qt_reference_dir: Path | None = args.qt_reference_dir

    if not lib_dir.is_dir():
        print(f"Not a directory: {lib_dir}", file=sys.stderr)
        return 2
    if qt_reference_dir is not None and not qt_reference_dir.is_dir():
        print(f"Not a directory: {qt_reference_dir}", file=sys.stderr)
        return 2

    by_component, unmapped = classify_all(lib_dir, qt_reference_dir)

    missing_mandatory = MANDATORY_COMPONENTS - by_component.keys()
    if missing_mandatory:
        print(
            "audit_codec_notices: mandatory component(s) "
            f"{sorted(missing_mandatory)!r} were not found bundled under "
            f"{lib_dir}.",
            file=sys.stderr,
        )
        return 1

    if unmapped:
        print(
            f"audit_codec_notices: {len(unmapped)} bundled librar"
            f"{'y is' if len(unmapped) == 1 else 'ies are'} not mapped to any "
            "known third-party component:",
            file=sys.stderr,
        )
        for path in unmapped:
            print(f"  {path}", file=sys.stderr)
        return 1

    fail = False
    for component in sorted(by_component):
        source_dir = third_party_root / component
        if not source_dir.is_dir():
            print(
                f"audit_codec_notices: no checked-in notice source directory "
                f"at {source_dir} for bundled component '{component}'.",
                file=sys.stderr,
            )
            fail = True
            continue
        source_files = [f for f in source_dir.iterdir() if f.is_file()]
        if not source_files:
            print(
                f"audit_codec_notices: {source_dir} has no notice files for "
                f"'{component}'.",
                file=sys.stderr,
            )
            fail = True
            continue
        bundled_dir = doc_root / component
        for source_file in source_files:
            bundled_file = bundled_dir / source_file.name
            if not bundled_file.is_file() or bundled_file.stat().st_size == 0:
                print(
                    f"audit_codec_notices: bundled '{component}' is missing "
                    f"its required non-empty '{source_file.name}' at "
                    f"{bundled_file}.",
                    file=sys.stderr,
                )
                fail = True
                continue
            source_hash = _sha256(source_file)
            bundled_hash = _sha256(bundled_file)
            if source_hash != bundled_hash:
                print(
                    f"audit_codec_notices: bundled '{component}/"
                    f"{source_file.name}' (sha256 {bundled_hash}) does not "
                    f"match the checked-in source (sha256 {source_hash}) -- "
                    "notice content has drifted from what this repository "
                    "actually ships.",
                    file=sys.stderr,
                )
                fail = True

    if fail:
        return 1

    print(
        f"Every bundled component ({', '.join(sorted(by_component))}) has a "
        "checksum-verified, non-empty required license notice."
    )
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="mode", required=True)

    qt_reference_dir_help = (
        "path to the real Qt SDK installation actually used to build this "
        "project (e.g. $QT_ROOT_DIR as exported by jurplel/install-qt-action "
        "in CI). Required to classify any bundled library found inside a "
        "Qt plugin subdirectory or beneath a 'qml' directory as the 'qt' "
        "component -- see classify_path()'s docstring. Omitting this means "
        "such libraries are reported unmapped (fail closed) rather than "
        "trusted by directory name alone."
    )

    classify_parser = subparsers.add_parser("classify")
    classify_parser.add_argument("lib_dir", type=Path)
    classify_parser.add_argument("--json-out", type=Path, default=None)
    classify_parser.add_argument("--qt-reference-dir", type=Path, default=None, help=qt_reference_dir_help)
    classify_parser.set_defaults(func=cmd_classify)

    verify_parser = subparsers.add_parser("verify-notices")
    verify_parser.add_argument("lib_dir", type=Path)
    verify_parser.add_argument("third_party_root", type=Path)
    verify_parser.add_argument("doc_root", type=Path)
    verify_parser.add_argument("--qt-reference-dir", type=Path, default=None, help=qt_reference_dir_help)
    verify_parser.set_defaults(func=cmd_verify_notices)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
