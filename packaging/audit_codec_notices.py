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
import sys
from pathlib import Path

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
    (re.compile(r"^libjpeg\.so"), "libjpeg"),
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
    (re.compile(r"^lib(krb5support|gssapi_krb5|k5crypto|krb5)\.so"), "krb5"),
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


def _is_real_qt_plugin_or_qml_module(path: Path, qt_reference_dir: Path) -> bool:
    """Returns True only if a file with the SAME relative sub-path (plugin
    subdirectory + basename, or the full path beneath the "qml" root)
    genuinely exists under qt_reference_dir -- the real Qt SDK
    installation actually used to build this project (e.g. `$QT_ROOT_DIR`
    as exported by jurplel/install-qt-action in CI). This is what turns
    the directory-name-based Qt-plugin/QML fallback in classify_path()
    from a fail-open "trust the path" check into one anchored to an
    authoritative, independently-obtained source: an attacker who can
    merely place a file inside the AppDir's imageformats/ or qml/
    directory cannot also cause a same-named, same-subpath file to
    already exist inside the pinned Qt SDK download this function
    consults, so an unrecognized/attacker-supplied binary is refused
    here and falls through to classify_path()'s unmapped return."""
    if path.parent.name in QT_PLUGIN_DIRECTORIES:
        candidate = qt_reference_dir / "plugins" / path.parent.name / path.name
        return candidate.is_file()
    if QT_QML_ROOT_DIRNAME in path.parts:
        qml_index = path.parts.index(QT_QML_ROOT_DIRNAME)
        sub_path = Path(*path.parts[qml_index + 1 :])
        candidate = qt_reference_dir / QT_QML_ROOT_DIRNAME / sub_path
        return candidate.is_file()
    return False


def classify_path(path: Path, qt_reference_dir: Path | None = None) -> str | None:
    """Like classify(), but additionally resolves any library located
    directly inside one of Qt's own standardized plugin subdirectories
    (QT_PLUGIN_DIRECTORIES), or anywhere beneath a literal "qml" path
    component (QT_QML_ROOT_DIRNAME), to the "qt" component -- but, unlike
    an earlier version of this function, ONLY if qt_reference_dir is
    supplied AND a file with the identical relative sub-path is verified
    to genuinely exist under it (see
    _is_real_qt_plugin_or_qml_module()'s docstring for why this
    authoritative cross-check is required, not merely the directory name
    itself). If qt_reference_dir is None, directory-based Qt
    classification is refused entirely (returns None, i.e. unmapped,
    rather than trusting the path) -- callers that omit it therefore
    fail closed rather than silently reintroducing the fail-open
    behavior a cumulative review found and required be fixed."""
    if qt_reference_dir is not None and _is_real_qt_plugin_or_qml_module(path, qt_reference_dir):
        return "qt"
    return classify(path.name)


def find_bundled_libraries(lib_dir: Path) -> list[Path]:
    """Every `.so` or `.so.<version...>` regular file or symlink found
    anywhere under lib_dir, recursively -- covers usr/lib, per-arch
    subdirectories, and plugin directories (e.g. imageformats/) alike,
    since linuxdeploy's exact placement is an implementation detail this
    must not hard-code one particular layout for (mirroring
    audit_dependency_closure.py's own doc-comment reasoning)."""
    return sorted(p for p in lib_dir.rglob("*.so*") if p.is_file() or p.is_symlink())


def classify_all(
    lib_dir: Path,
    qt_reference_dir: Path | None = None,
) -> tuple[dict[str, list[Path]], list[Path]]:
    """Returns (component -> [paths requiring that component's notice],
    unmapped_paths). ABI_ALLOWLIST-covered libraries are excluded from
    both (they need no notice and are not a failure). qt_reference_dir is
    forwarded to classify_path() -- see its docstring; omitting it means
    every directory-matched Qt plugin/QML module is reported unmapped."""
    by_component: dict[str, list[Path]] = {}
    unmapped: list[Path] = []
    for path in find_bundled_libraries(lib_dir):
        basename = path.name
        if basename in ABI_ALLOWLIST:
            continue
        component = classify_path(path, qt_reference_dir)
        if component is None:
            unmapped.append(path)
            continue
        by_component.setdefault(component, []).append(path)
    return by_component, unmapped


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
