#!/usr/bin/env python3
"""Recursively audit a bundled shared library's full ELF DT_NEEDED closure
against an AppImage's AppDir lib directory.

This is a static, purely file-based proof, independent of whatever shared
libraries happen to already be resolvable/preloaded in whatever environment
runs it (unlike a dlopen()-based check, which can be fooled into "passing"
by a host or container that happens to already have the same transitive
libraries installed system-wide -- see verify_bundled_libsecret.py's
docstring for that check's own, narrower, complementary purpose). Starting
from one or more root libraries (by default, libsecret-1.so.0, the
dlopen()-only dependency QtKeychain's Secret Service backend loads -- see
build-appimage.sh), this walks every DT_NEEDED entry read via `readelf -d`
and requires each one to either:

  1. resolve to a file actually present in the AppDir lib directory (in
     which case its own DT_NEEDED entries are recursively walked too), or
  2. appear in the ABI_ALLOWLIST below.

Any dependency satisfying neither condition is reported as MISSING and
causes a non-zero exit: it is a library the packaged AppImage would fail
to dlopen()/link against on a target host that does not already happen to
have it installed, which is exactly the class of bug (bundled
libgcrypt.so.20 transitively requiring libgpg-error.so.0, which
linuxdeploy's own default blacklist excludes from automatic bundling)
this script exists to catch deterministically and by name.

ABI_ALLOWLIST is intentionally narrow: only the dynamic loader itself and
the small set of "core" glibc component libraries that are effectively
guaranteed to exist, at a compatible ABI, on any glibc-based x86_64 Linux
distribution an AppImage might run on (this is the same baseline AppImages
in general already rely on glibc itself not being bundled). Anything not
on this list must be bundled explicitly; nothing on this list is ever
treated as "found" by searching the AppDir -- it is presumed to come from
the target host's own loader/libc, which is the one dependency an AppImage
cannot avoid relying on.

Reporting note: the human-readable summary and --list-only output both
report only the *required* bundled closure, i.e. ABI_ALLOWLIST names are
excluded even if a copy incidentally happens to be present in the AppDir
(as is common, since many packaging tools copy the full ldd-resolved
closure rather than the minimal non-ABI subset). This keeps --list-only
safe to use directly as a mutation-testing victim list: every name it
prints is one whose removal is expected to make the audit fail, never one
the allowlist would silently continue to cover.

Root discovery note (--auto-roots): a hand-picked --root list only proves
completeness for the specific libraries it names -- it can never notice a
*different* bundled ELF (the app's own executable, or one of Qt's
plugins/QML modules under usr/lib/plugins or usr/qml, none of which are
transitively reachable from libsecret-1.so.0 or libqt6keychain.so at all)
requiring something that was never bundled. --auto-roots recursively
scans a given directory for every real ELF file (detected by its own
magic bytes, "\x7fELF", never by trusting a file extension or directory
name) and adds each one's own exact, real discovered path as an
additional root, so the walk is rooted at literally everything the
AppImage will ever actually try to load, not only the two libraries one
prior incident happened to name. Two different discovered files sharing
the same bare basename (e.g. two independently-built same-named plugins
in different directories) are both preserved as distinct roots, each
audited via its own real RUNPATH/RPATH/$ORIGIN context -- never collapsed
to a single "first match wins" entry per name.

X11 desktop-stack note (--allow-x11-desktop-stack): a separate,
explicitly-opt-in allowlist (X11_DESKTOP_ABI_ALLOWLIST) for base X11/xcb/
xkbcommon/GL-EGL client libraries, which linuxdeploy's own default
blacklist already refuses to auto-bundle on the assumption that any
X11/XWayland desktop session capable of hosting an xcb-platform Qt
application already implies a compatible copy of them. Kept deliberately
separate from ABI_ALLOWLIST (a different kind of guarantee: "glibc is
always present" vs. "a running windowing session implies these"), and
never implied by ABI_ALLOWLIST or --auto-roots alone -- a caller auditing
a closure where this assumption is inappropriate (e.g. the narrower
libsecret-only audit) must never have it silently applied.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

# The dynamic loader and the small set of "core" glibc libraries every
# glibc-based x86_64 Linux host is guaranteed to provide at a compatible
# ABI. Nothing else may be silently assumed to be present on the target;
# everything else must resolve from within the AppDir itself.
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

# Round-N+ review (HIGH, "auditor can disagree with kernel/glibc: ...
# allows nonexistent paths by basename"): the dynamic loader's BASENAME
# alone is not enough to trust a recorded PT_INTERP value -- a basename
# match only proves the string ends the same way, never that the
# absolute path is one the kernel could actually open on a real glibc
# x86_64 host. glibc's own ABI fixes the loader's install location at
# exactly this one canonical absolute path on every glibc-based x86_64
# Linux distribution (this is the same path stock, unpatched
# linuxdeploy/gcc/ld always records in every executable this project's
# own build produces) -- an interpreter string that merely shares this
# basename while naming some OTHER directory (e.g. a build-host-only
# path, or a typo) would exec() successfully only by accident on THIS
# specific host, and would simply fail outright on a real target with
# no reason to have anything at that other path at all.
LOADER_CANONICAL_PATH: dict[str, str] = {
    "ld-linux-x86-64.so.2": "/lib64/ld-linux-x86-64.so.2",
}

# A second, separately-documented allowlist for the base X11/xcb/GL
# "desktop stack" -- deliberately kept apart from ABI_ALLOWLIST above (which
# is reserved for the dynamic loader and core glibc-provided libraries)
# because the rationale for trusting these is different in kind: it is not
# "glibc guarantees this exists", it is "a running X11/XWayland desktop
# session capable of hosting an xcb-platform Qt application already implies
# a compatible copy of these libraries, because the windowing session
# itself cannot exist without them".
#
# This project already relied on exactly this assumption implicitly and
# inconsistently before this list existed: linuxdeploy's own default
# blacklist already refuses to auto-bundle base X11/xcb libraries (verified
# against linuxdeploy's public documentation/behavior: base X11 client
# libraries are treated as "always already present on the target", the same
# category as glibc, precisely because bundling a mismatched xcb/X11 stack
# alongside a *different* host X server can break in the opposite, non-obvious
# direction -- protocol/ABI skew between client and server), and this
# project's own "Launch the packaged AppImage headlessly to verify real
# startup" CI step already `apt-get install`s libxkbcommon0/libgl1/
# libopengl0/libegl1 into an intentionally libsecret/glib-less container
# specifically because they are assumed host-provided, not bundled. What was
# missing was ever making that assumption explicit, complete, and auditable
# in one place -- a narrower closure audit rooted only at libsecret/
# libqt6keychain never actually walked into Qt's own xcb platform plugin or
# GL/EGL support libraries at all, so a *real* gap in this same category
# (e.g. base libxcb.so.1 itself, needed only by the bundled xcb platform
# plugin, which is not reachable from either of those two roots) could have
# gone silently unaudited. Anything in this list is asserted, by name, to be
# a library SteamOS/Gamescope (an XWayland-backed compositor) and any
# ordinary X11/Wayland-with-XWayland Linux desktop is expected to already
# provide at a compatible ABI; it is never bundled by build-appimage.sh, and
# nothing here is ever treated as "found" by searching the AppDir.
X11_DESKTOP_ABI_ALLOWLIST: frozenset[str] = frozenset(
    {
        # Base X11 protocol/xcb client libraries (part of libxcb's own repo,
        # not one of the separate xcb-util-* projects -- see
        # third_party/xcb/NOTICE.md).
        "libxcb.so.1",
        "libxcb-render.so.0",
        "libxcb-shm.so.0",
        "libxcb-sync.so.1",
        "libxcb-xfixes.so.0",
        "libxcb-shape.so.0",
        "libxcb-randr.so.0",
        "libxcb-glx.so.0",
        "libxcb-present.so.0",
        "libxcb-dri2.so.0",
        "libxcb-dri3.so.0",
        "libxcb-xkb.so.1",
        # The separate xcb-util-* project libraries Qt's xcb platform
        # plugin itself links against (see third_party/xcb-util*/NOTICE.md
        # for why each is a distinct upstream project from base libxcb).
        "libxcb-util.so.1",
        "libxcb-image.so.0",
        "libxcb-keysyms.so.1",
        "libxcb-render-util.so.0",
        "libxcb-icccm.so.4",
        "libxcb-ewmh.so.2",
        "libxcb-cursor.so.0",
        # Core Xlib and its xcb bridge/support libraries.
        "libX11.so.6",
        "libX11-xcb.so.1",
        "libXau.so.6",
        "libXdmcp.so.6",
        "libXext.so.6",
        # Keyboard-mapping support, used regardless of xcb vs. Wayland QPA.
        "libxkbcommon.so.0",
        "libxkbcommon-x11.so.0",
        # GL/EGL dispatch libraries: Qt Gui/OpenGL support is ELF-linked
        # against these unconditionally (see the first-frame smoke test
        # step in .github/workflows/ci.yml for the concrete confirmed
        # errors from omitting any one of these), independent of whether a
        # GPU/EGL context is ever actually created at runtime.
        "libGL.so.1",
        "libGLX.so.0",
        "libOpenGL.so.0",
        "libEGL.so.1",
        "libGLdispatch.so.0",
    }
)

# A third, separately-documented allowlist for the host's own font-rendering
# stack (fontconfig/FreeType), kept apart from both ABI_ALLOWLIST (glibc)
# and X11_DESKTOP_ABI_ALLOWLIST (X11/xcb/GL protocol libraries) because the
# rationale for trusting these is different again in kind from either:
# bundling one's own fontconfig/FreeType inside a portable AppImage is a
# well-known font-rendering compatibility hazard, distinct from an ABI
# guarantee -- fontconfig's on-disk cache format and its config/cache file
# *paths* (e.g. `/etc/fonts`, `~/.cache/fontconfig`) are version- and
# host-specific, so a bundled fontconfig can silently read/write a cache
# the *host's* fontconfig does not understand (or vice versa), corrupting
# font rendering for the AppImage, the host, or both, in a way a simple
# missing-symbol ABI break never would. Verified directly against a real
# linuxdeploy run's own "Skipping deployment of blacklisted library"
# diagnostic: linuxdeploy's default blacklist already excludes both
# libfontconfig and libfreetype from automatic bundling for exactly this
# reason. This project's own CI already relies on this assumption in
# practice -- its own "Launch the packaged AppImage headlessly to verify
# real startup" smoke-test steps `apt-get install fontconfig
# fonts-dejavu-core` into the bare container specifically so Qt Gui's text
# layout has a real font-rendering stack to use, alongside the X11/GL
# stack -- what was missing was ever making that assumption explicit,
# complete, and auditable in this same closure audit. Gated behind its own
# explicit --allow-font-rendering-stack flag, never implied by
# --allow-x11-desktop-stack or ABI_ALLOWLIST alone, so a caller auditing a
# closure where this assumption is inappropriate (e.g. the narrower
# libsecret-only/libavif-only audits) never has it silently applied.
FONT_RENDERING_ABI_ALLOWLIST: frozenset[str] = frozenset(
    {
        "libfontconfig.so.1",
        "libfreetype.so.6",
    }
)

_NEEDED_RE = re.compile(r"\(NEEDED\)\s+Shared library:\s+\[(?P<name>[^\]]+)\]")
_RUNPATH_RE = re.compile(r"\(RUNPATH\)\s+Library runpath:\s+\[(?P<value>[^\]]*)\]")
_RPATH_RE = re.compile(r"\(RPATH\)\s+Library rpath:\s+\[(?P<value>[^\]]*)\]")
_ELF_MAGIC = b"\x7fELF"


class ClosureAuditError(RuntimeError):
    """Raised when readelf itself cannot be run, a bundled file cannot be
    parsed as an ELF shared object, or a bundled SONAME symlink resolves
    outside the AppDir lib directory being audited -- all distinct from a
    MISSING dependency, which is a normal (if failing) audit outcome, not a
    tooling/integrity error."""


def _readelf_dynamic_text(path: Path) -> str:
    """Runs `readelf -d -W` on `path` exactly once and returns its raw
    stdout -- both _parse_needed() and _parse_search_path_entries() below
    parse this same text, so each bundled ELF is only ever actually
    invoked through readelf a single time per audit, regardless of how
    many distinct DT_* tags are read from it."""
    try:
        result = subprocess.run(
            ["readelf", "-d", "-W", str(path)],
            check=True,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError as exc:
        raise ClosureAuditError(
            "readelf is required (part of binutils) but was not found on PATH."
        ) from exc
    except subprocess.CalledProcessError as exc:
        raise ClosureAuditError(
            f"readelf failed to parse {path} as an ELF shared object: {exc.stderr.strip()}"
        ) from exc
    return result.stdout


def _parse_needed(dynamic_text: str) -> list[str]:
    return [m.group("name") for m in _NEEDED_RE.finditer(dynamic_text)]


# Round-N+ review (HIGH, "auditor can disagree with kernel/glibc: reads
# `.interp` section rather than PT_INTERP"): the KERNEL exec() path (and
# every dynamic loader) resolves the program interpreter exclusively via
# the PT_INTERP PROGRAM header -- section headers (including a
# conventionally-named `.interp` SECTION) are pure debugging/linking
# metadata the kernel never even looks at, and a real toolchain is free
# to drop the section-header table entirely (e.g. via a strip variant
# that zeroes e_shnum/e_shstrndx) while leaving every program header,
# including PT_INTERP and its underlying bytes, perfectly intact and
# exec()-able. `readelf -l` walks the PROGRAM header table (e_phoff/
# e_phnum), which is independent of section headers, and annotates a
# PT_INTERP entry with exactly this line.
_PT_INTERP_ANNOTATION_RE = re.compile(
    r"Requesting program interpreter:\s*(?P<value>\S+)\s*\]"
)


def _read_interp(path: Path) -> str | None:
    """Returns the ELF interpreter path recorded in `path`'s own PT_INTERP
    PROGRAM header (e.g. "/lib64/ld-linux-x86-64.so.2"), or None if this
    ELF file carries no PT_INTERP segment at all.

    Presence of PT_INTERP is itself the real, on-disk distinction between
    "this file is directly exec()'d by the kernel, which will hand it to
    exactly this recorded interpreter" (an actual executable) and "this
    file is only ever dlopen()/dlsym()'d by a process that already has
    its own interpreter" (an ordinary shared library) -- never a
    name/extension guess, matching every other detection in this script.
    Deliberately reads this via `readelf -l` (program headers), NEVER
    `readelf -p .interp` (a SECTION-header-table lookup by name): the
    latter silently reports "no such section" -- indistinguishable from
    "not an executable" -- for any ELF whose section-header table has
    been stripped/zeroed, even though such a file's PT_INTERP program
    header (which is what the kernel and every dynamic loader actually
    consult) remains completely intact and this is still a directly
    exec()able program interpreter binding this audit must validate.
    """
    result = subprocess.run(
        ["readelf", "-l", "-W", str(path)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    match = _PT_INTERP_ANNOTATION_RE.search(result.stdout)
    return match.group("value") if match is not None else None


def validate_interp(path: Path, allowlist: frozenset[str]) -> str | None:
    """Round-N+ review (HIGH, "validate PT_INTERP for executables"):
    returns None if `path` is not a directly-executable ELF at all (no
    PT_INTERP program header), or if it is one whose own recorded
    interpreter is genuinely the EXACT, canonical, host-guaranteed
    absolute path for one of the loader names in `allowlist` --
    otherwise returns a human-readable description of the problem.

    Deliberately a SEPARATE, narrower check from the DT_NEEDED closure
    walk above, not folded into it: PT_INTERP resolution is an entirely
    different mechanism (the kernel opens the exact absolute path
    recorded in the ELF header at exec() time -- it never consults
    DT_RUNPATH/DT_RPATH/LD_LIBRARY_PATH, which only govern how THAT
    interpreter subsequently resolves this object's own DT_NEEDED
    entries), so treating it as just another same-shaped dependency edge
    would silently apply reachability semantics that do not actually
    describe how a real kernel resolves it.

    Round-N+ review (HIGH, "... allows nonexistent paths by basename"):
    checking only `Path(interp).name` would accept ANY directory at all,
    provided the final path component happened to match -- including a
    build-host-only path that would simply not exist on a real target.
    The interpreter string must equal LOADER_CANONICAL_PATH's exact,
    documented absolute path for an allowlisted loader name, never
    merely share its basename.

    This project's own build (packaging/build-appimage.sh) relies
    entirely on stock, unpatched linuxdeploy output: every bundled
    executable's interpreter is always left as the plain host loader,
    with LD_LIBRARY_PATH (not a rewritten PT_INTERP) doing the work of
    finding bundled libraries. A bundled executable whose own interpreter
    names anything else is therefore either packaging corruption or an
    unreviewed build-tooling change -- either way, something this audit
    must not silently pass, since a target host has no reason to have a
    compatible loader at any OTHER path, and this project deliberately
    never bundles a replacement loader of its own."""
    interp = _read_interp(path)
    if interp is None:
        return None
    name = Path(interp).name
    if name in allowlist and name in LOADER_CANONICAL_PATH:
        if interp == LOADER_CANONICAL_PATH[name]:
            return None
        return (
            f"{path} records program interpreter '{interp}', whose final "
            f"path component ('{name}') matches an allowlisted loader "
            f"name, but whose full path is not the exact canonical "
            f"absolute path ('{LOADER_CANONICAL_PATH[name]}') every real "
            "glibc-based x86_64 host guarantees it at -- refusing to "
            "assume a same-named directory elsewhere will resolve on a "
            "target host."
        )
    return (
        f"{path} records program interpreter '{interp}', which is neither "
        "the host-guaranteed dynamic loader (ABI_ALLOWLIST) nor anything "
        "this project's own build ever bundles -- refusing to assume it "
        "will resolve on a target host."
    )


def _parse_search_path_kind_and_entries(
    dynamic_text: str,
) -> tuple[str | None, list[str]]:
    """Returns (kind, entries): `kind` is "runpath" if this object carries
    a DT_RUNPATH tag, "rpath" if it carries a DT_RPATH tag instead (never
    both -- DT_RUNPATH, when present, entirely REPLACES DT_RPATH for
    resolving this object's own direct DT_NEEDED entries; DT_RPATH is
    only ever consulted when DT_RUNPATH is completely absent), or None if
    neither tag is present at all. `entries` is the colon-separated list
    from whichever tag was found (empty entries dropped), or an empty
    list if `kind` is None. Round-9+ review (HIGH): the distinction
    between the two kinds matters beyond merely "which one wins" -- see
    _effective_search_dirs()'s own comment on why DT_RUNPATH and DT_RPATH
    each have a DIFFERENT real precedence relative to LD_LIBRARY_PATH."""
    match = _RUNPATH_RE.search(dynamic_text)
    if match is not None:
        return "runpath", [entry for entry in match.group("value").split(":") if entry]
    match = _RPATH_RE.search(dynamic_text)
    if match is not None:
        return "rpath", [entry for entry in match.group("value").split(":") if entry]
    return None, []


def _expand_origin(entry: str, origin_dir: Path) -> Path:
    """Expands a single RUNPATH/RPATH entry's $ORIGIN (or ${ORIGIN})
    token(s) to `origin_dir` -- the requesting object's OWN containing
    directory, exactly as ld.so defines $ORIGIN -- then resolves the
    result (following any further symlinked ancestor directories, exactly
    like the real loader would) to an absolute, canonical directory path.
    A non-$ORIGIN relative entry (rare, but permitted by the format) is
    resolved relative to origin_dir too, matching ld.so's own documented
    behavior for a relative RUNPATH/RPATH entry."""
    expanded = entry.replace("$ORIGIN", str(origin_dir)).replace(
        "${ORIGIN}", str(origin_dir)
    )
    candidate = Path(expanded)
    if not candidate.is_absolute():
        candidate = origin_dir / candidate
    return candidate.resolve()


def _effective_search_dirs(
    requester_path: Path,
    dynamic_text: str,
    global_search_dirs: list[Path],
    lib_dir_resolved: Path,
) -> list[Path]:
    """The ordered list of real directories a load of `requester_path`'s
    own DT_NEEDED entries would actually search, per real ld.so
    precedence -- which, contrary to a naive "RUNPATH/RPATH always wins"
    assumption, genuinely DIFFERS depending on which of the two tags this
    object carries:

      - DT_RPATH (only consulted when DT_RUNPATH is absent): searched
        BEFORE LD_LIBRARY_PATH -- this is the legacy behavior glibc
        preserves for backwards compatibility.
      - DT_RUNPATH: searched AFTER LD_LIBRARY_PATH -- the modern tag is
        deliberately overridable by the environment, exactly the
        opposite precedence from DT_RPATH.

    `global_search_dirs` represents this project's AppRun-exported
    LD_LIBRARY_PATH, applying uniformly to every bundled object
    regardless of its own individual RUNPATH/RPATH (see main()'s
    --global-search-dir). A directory named by more than one of these
    sources is only ever searched once, at its EARLIEST (highest-
    precedence) position for the kind of tag actually present.

    Round-9+ review (HIGH, "boolean resolver permits external path"): a
    RUNPATH/RPATH entry (after $ORIGIN expansion) that resolves OUTSIDE
    `lib_dir_resolved` -- the AppDir actually being audited -- is never
    included at all, regardless of what may or may not happen to exist
    at that absolute path on the specific machine running this audit.

    Round-N+ review (MEDIUM, "silently drops external DT_RPATH that
    runtime may search before LD_LIBRARY_PATH"): merely OMITTING such an
    entry from the returned search-dir list is not enough -- a real
    ld.so still SEARCHES that exact external directory (and, for a
    legacy DT_RPATH, searches it BEFORE this project's own bundled
    LD_LIBRARY_PATH entries, so if some ABI-incompatible or malicious
    library of the identical SONAME happens to sit there on a given
    target machine, the REAL runtime resolves to THAT one, never this
    audit's own carefully-verified bundled copy, regardless of what this
    audit itself concludes). Silently proceeding as though the entry
    were simply absent would let this function report a dependency as
    "reachable via our own bundled directory" even though a real load on
    some target machine could resolve to a completely different,
    unaudited file first. An external RPATH/RUNPATH entry is therefore
    always treated as a hard packaging defect -- this project's own
    build must never emit one at all -- and raises immediately rather
    than being tolerated in either direction (neither trusted, per the
    comment above, nor quietly ignored).
    Trusting such a path would let a dependency be misreported as
    "reachable" purely because the AUDIT HOST happens to have a
    same-named library sitting there -- exactly the host-dependent
    leakage this script's docstring and every other check in it
    (_index_lib_dir's ambiguous-duplicate guard, the symlink-escape
    check in audit_closure()) are designed to prevent. A real target
    machine (e.g. a SteamOS Deck) has no reason to have anything at that
    same absolute path at all; only directories genuinely inside the
    AppDir itself can ever make a dependency truly self-contained.
    global_search_dirs is not filtered the same way here: it is always
    supplied by this script's own caller (main()'s --global-search-dir),
    which is expected to only ever pass real AppDir-relative directories
    (e.g. the AppDir's own usr/lib) -- see main()'s own argument help
    text -- so it is trusted as given, exactly as before.
    """
    origin_dir = requester_path.parent.resolve()
    kind, entries = _parse_search_path_kind_and_entries(dynamic_text)
    own_dirs: list[Path] = []
    for entry in entries:
        expanded = _expand_origin(entry, origin_dir)
        if not expanded.is_relative_to(lib_dir_resolved):
            raise ClosureAuditError(
                f"{requester_path} carries a {'DT_RPATH' if kind == 'rpath' else 'DT_RUNPATH'} "
                f"entry ('{entry}', resolving to {expanded}) outside the "
                f"AppDir being audited ({lib_dir_resolved}). A real "
                "loader on some target machine would still search this "
                "exact external directory -- and, for a legacy RPATH, "
                "before this project's own bundled search directories -- "
                "so this can never be silently ignored: fix the build so "
                "no bundled object ever records an external RPATH/RUNPATH "
                "entry at all."
            )
        if expanded not in own_dirs:
            own_dirs.append(expanded)

    ordered: list[Path] = []
    if kind == "rpath":
        # Legacy DT_RPATH: searched BEFORE LD_LIBRARY_PATH.
        for d in own_dirs:
            if d not in ordered:
                ordered.append(d)
        for candidate in global_search_dirs:
            if candidate not in ordered:
                ordered.append(candidate)
    else:
        # DT_RUNPATH (or neither tag present, in which case own_dirs is
        # empty and this ordering is moot): LD_LIBRARY_PATH is searched
        # BEFORE the object's own RUNPATH.
        for candidate in global_search_dirs:
            if candidate not in ordered:
                ordered.append(candidate)
        for d in own_dirs:
            if d not in ordered:
                ordered.append(d)
    return ordered


def _file_digest(path: Path) -> str | None:
    """Returns a content hash (sha256) of the file a path resolves to
    (following symlinks), or None if it cannot be read/does not exist
    (e.g. a dangling symlink, handled elsewhere as its own distinct
    error). Used only to compare two same-basename entries for genuine
    content equality -- two independently-copied, byte-identical files at
    different paths are not an ambiguity (either resolves to the same
    readelf result), only genuinely differing content is."""
    try:
        resolved = path.resolve()
        if not resolved.is_file():
            return None
        digest = hashlib.sha256()
        with resolved.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def _index_lib_dir(lib_dir: Path) -> dict[str, list[Path]]:
    """Maps every real ELF shared-object basename (or symlink resolving to
    one) actually present anywhere under lib_dir (recursively) to EVERY
    distinct path it is found at, so a NEEDED entry's exact SONAME string
    (e.g. "libsecret-1.so.0") can be looked up directly -- AppImage
    bundling copies both a library's real file and its SONAME symlink, so
    exact-name lookup (rather than a version-stripping heuristic) is both
    correct and simple here.

    Recursive (rglob) rather than the original flat iterdir() so a single
    invocation can be pointed at an entire AppDir usr/ tree (usr/bin, usr/lib,
    usr/lib/plugins/**, usr/qml/**) at once -- required for --auto-roots to
    resolve dependencies of an app executable or Qt plugin/QML module that
    do not live directly in lib_dir itself. Strictly a superset of what the
    old flat iterdir() found, so existing single-directory callers are
    unaffected other than also seeing any genuine subdirectory contents.

    Only entries that are themselves a real ELF shared object (checked via
    `_is_elf_file()`'s own magic-byte sniff, which transparently follows a
    symlink to its ultimate regular-file target -- never a filename/
    extension guess) are indexed. A DT_NEEDED tag can, by the ELF/ld.so
    SONAME convention, only ever name a real shared object -- never an
    arbitrary data file -- so any other file under this tree (Qt QML
    modules' own "plugins.qmltypes" metadata, ".qml"/".json"/license-notice
    files, etc.) is irrelevant to a NEEDED lookup and must not be indexed
    at all: real AppDir trees legitimately contain many same-basename,
    genuinely-different non-library files (every bundled QML module ships
    its own "plugins.qmltypes", "qmldir", etc.), and indexing those too
    would make an unrelated ambiguity check fire on totally unrelated
    files that a NEEDED lookup could never resolve to in the first place.

    Round-N+ review (HIGH, "index/root still one path per basename"):
    deliberately returns EVERY distinct occurrence rather than collapsing
    to a single "canonical" path -- a real AppDir can legitimately contain
    two different, non-interchangeable files sharing the same basename in
    different directories (e.g. a Qt plugin's own private copy of a
    dependency versus the main lib directory's copy, each correctly
    resolved only via ITS OWN requester's real RUNPATH/RPATH/search
    context, per audit_closure()'s edge resolution). This function itself
    never judges that ambiguous -- only a caller that must pick a SINGLE
    occurrence without any real per-requester search context to
    disambiguate (a bare root name, or a best-effort fallback
    representative for an edge that resolved via no search directory at
    all) needs -- and gets, via _select_unambiguous_occurrence() below --
    that stricter guarantee.
    """
    index: dict[str, list[Path]] = {}
    for entry in sorted(lib_dir.rglob("*")):
        if not _is_elf_file(entry):
            continue
        index.setdefault(entry.name, []).append(entry)
    return index


def _select_unambiguous_occurrence(
    name: str, candidates: list[Path], lib_dir: Path
) -> Path:
    """Returns the single occurrence to use when no real per-requester
    search context is available at all -- a root's own bare name (roots
    are exempt from reachability entirely; see audit_closure()'s own
    docstring), or the best-effort representative used to keep exploring
    an edge that could not be resolved via ANY of its requester's real
    search directories (already reported unreachable; this is purely to
    surface further, unrelated problems in the same audit run).

    Two or more occurrences are only genuinely ambiguous if their real
    content actually differs (by sha256, not merely a different path --
    an independently-copied, byte-identical duplicate is harmless and
    common in real bundling, so is never treated as ambiguous). Raises
    ClosureAuditError for a genuine content difference: silently picking
    one could resolve to the wrong file entirely, masking a genuine
    substitution risk exactly like the pre-existing symlink-escape check
    above guards against."""
    first = candidates[0]
    for other in candidates[1:]:
        if _file_digest(first) != _file_digest(other):
            raise ClosureAuditError(
                f"Ambiguous duplicate basename '{name}' found at both "
                f"{first} and {other} with different real content, and no "
                "real per-requester search context is available here to "
                "disambiguate them (a root name, or an edge already "
                "unresolved via any real search directory) -- refusing to "
                "guess which one should be used."
            )
    return first


def _is_elf_file(path: Path) -> bool:
    """Detects a real ELF file by its own magic bytes ("\\x7fELF"), never
    by trusting a file extension or the name/location of its containing
    directory -- this is the same host-independence discipline the rest of
    this script already applies to dependency resolution, extended to root
    discovery itself."""
    try:
        if not path.is_file():
            return False
        with path.open("rb") as handle:
            return handle.read(len(_ELF_MAGIC)) == _ELF_MAGIC
    except OSError:
        return False


def _discover_elf_roots(search_dir: Path) -> list[Path]:
    """Recursively finds EVERY real ELF file under search_dir (the app's
    own executable, every Qt plugin, every QML module's native library,
    and every already-bundled library alike) and returns each one's real,
    exact, distinct path -- never collapsed by basename.

    Round-N+ review (HIGH, "auto roots still dict[basename,Path], losing
    duplicates/location"): a real AppDir tree can legitimately contain two
    *different* ELF files sharing the same basename in different
    directories (a plugin's own private copy of a helper, or two
    independently-built variants of the same-named tool) -- collapsing
    them to a single "first match wins" Path per basename silently drops
    the second one from the audit's own root set entirely, meaning its
    real dependency edges (resolved via ITS OWN, different, real
    location's RUNPATH/RPATH/$ORIGIN context -- see
    _effective_search_dirs()) are never audited at all. Returning every
    discovered path, keyed by nothing, is what lets audit_closure() (see
    its own "process_root" step) treat each one as an independent root
    with its own known, exact, already-resolved location -- never
    re-derived via a name-only flat-index lookup that could not tell
    these apart either."""
    discovered: list[Path] = []
    for entry in sorted(search_dir.rglob("*")):
        if _is_elf_file(entry):
            discovered.append(entry)
    return discovered


def audit_closure(
    lib_dir: Path,
    roots: list[str | tuple[str, Path]],
    extra_allowlist: frozenset[str] = frozenset(),
    global_search_dirs: list[Path] | None = None,
) -> tuple[set[str], dict[str, list[str]], dict[str, list[str]], set[str]]:
    """Returns (bundled_closure, missing -> [requested-by...],
    unreachable -> [requested-by...], visited_all).

    bundled_closure: every bundled library name found reachable from roots
      -- both actually present in the AppDir AND actually resolvable via
      its specific requester's own real loader search context (see
      unreachable below); a name that is merely present somewhere in the
      tree but never actually reachable is NOT included here.
    missing: every NEEDED name that was neither bundled anywhere in the
      tree nor allowlisted, mapped to the list of bundled libraries that
      required it (for a useful error message pinpointing which
      dependency edge is broken).
    unreachable: every NEEDED name that IS bundled somewhere in the tree
      (and is not allowlisted) but is NOT reachable from the specific
      requester(s) that named it -- i.e. neither that requester's own
      $ORIGIN-expanded DT_RUNPATH/DT_RPATH nor any global_search_dirs
      entry actually contains it directly. A real ld.so load of that
      requester would fail to resolve this exact dependency exactly as
      surely as if it were entirely absent from the AppDir, regardless of
      which unrelated, unsearched directory elsewhere in the tree happens
      to also contain a same-named file -- see the module docstring's
      RUNPATH/RPATH/$ORIGIN/LD_LIBRARY_PATH loader-search-awareness
      rationale. Mapped to the same "requested by" list shape as missing.
    global_search_dirs: directories searched for EVERY requester,
      regardless of its own RUNPATH/RPATH -- representing this project's
      AppRun-exported LD_LIBRARY_PATH (see main()'s --global-search-dir).
      Defaults to [lib_dir] alone, preserving this function's original,
      pre-loader-search-aware behavior for every existing single-flat-
      directory caller that never passes this parameter.
    extra_allowlist: an additional, separately-documented allowlist unioned
      with ABI_ALLOWLIST for this call only (e.g. X11_DESKTOP_ABI_ALLOWLIST,
      opted into explicitly via --allow-x11-desktop-stack) -- kept as an
      explicit parameter rather than silently merged into ABI_ALLOWLIST
      itself so a caller auditing e.g. the narrow libsecret closure never
      unintentionally also trusts the X11/GL desktop-stack assumption.

    Round-N+ review (HIGH, "auto roots still dict[basename,Path], losing
    duplicates/location ... traversal omits inherited legacy DT_RPATH
    context"): each entry of `roots` may be either a bare name (str --
    the pre-existing --root convention: "the name as found anywhere in
    the indexed tree", never claiming any particular real location) or a
    (name, Path) tuple carrying that root's own already-known, exact,
    real discovered location (--auto-roots' own per-file discovery; see
    _discover_elf_roots()). Two *different* roots that happen to share a
    bare basename (a genuine, if rare, real-world AppDir shape) are
    THEREFORE both preserved and independently processed here -- each via
    its own real path, its own real RUNPATH/RPATH, its own real $ORIGIN --
    never collapsed to a single flat-index lookup that could not tell
    them apart. A root's own resolved path becomes the requester for ITS
    OWN dependency edges exactly like any other object; only an actual
    DT_NEEDED dependency EDGE (which always has a concrete, known
    requester) is ever reachability-checked -- a root itself never is,
    since it is presumed loaded directly (execve()'d, or --auto-roots'
    own discovery), never resolved BY NAME through some other object's
    search path.
    """
    allowlist = ABI_ALLOWLIST | extra_allowlist
    index = _index_lib_dir(lib_dir)
    lib_dir_resolved = lib_dir.resolve()
    if global_search_dirs is None:
        global_search_dirs = [lib_dir_resolved]
    else:
        global_search_dirs = [d.resolve() for d in global_search_dirs]

    # Normalize every root entry to (name, known_path | None): a bare str
    # keeps the pre-existing "resolve by name in the flat index" behavior
    # (--root's documented convention); a (name, Path) tuple carries an
    # already-known, exact, real location (--auto-roots' own discovery)
    # that must be used AS-IS, never re-derived from the flat index (see
    # this function's own docstring).
    normalized_roots: list[tuple[str, Path | None]] = [
        (root, None) if isinstance(root, str) else (root[0], root[1])
        for root in roots
    ]

    bundled_closure: set[str] = set()
    missing: dict[str, list[str]] = {}
    unreachable: dict[str, list[str]] = {}
    # Each queue entry is (name, requester_path), always a REAL, resolved
    # requester Path: roots are seeded directly below (see process_root())
    # rather than ever being pushed onto this queue as a bare name with no
    # requester, so this queue exclusively models actual DT_NEEDED
    # dependency edges (each with a concrete, known requester).
    queue: list[tuple[str, Path]] = []
    # Round-9+ review (HIGH, "seen keyed only SONAME skips same dependency
    # from different requester/RPATH contexts" / "roots collapsed
    # basename"): deduplication is keyed by the EDGE -- (name, requester)
    # -- never by `name` alone. Two DIFFERENT requesters naming the same
    # dependency name each have their OWN real search context and must be
    # independently reachability-checked: one requester's RUNPATH/RPATH
    # reaching a bundled library says nothing about whether a SECOND,
    # differently-located requester's own RUNPATH/RPATH also reaches it.
    # Keying by name alone previously let the FIRST requester's outcome
    # silently stand in for every later requester of the identical name --
    # including a root (requester=None, exempt from reachability entirely)
    # incorrectly "vouching" for an unrelated later dependency edge that
    # happens to share its bare basename, and a genuinely unreachable edge
    # from one requester being masked by an earlier, differently-located,
    # successful edge for the same name. A per-resolved-file readelf-
    # output cache (below) keeps this from being a real performance
    # regression: only the (cheap, in-memory) reachability check repeats
    # per distinct requester, never a redundant subprocess invocation.
    seen_edges: set[tuple[str, str]] = set()
    # Round-N+ review (HIGH, "auto roots still dict[basename,Path], losing
    # duplicates/location"): roots are deduplicated by (name, EXACT known
    # path) -- never by name alone -- so two different roots that happen
    # to share a bare basename (e.g. two distinct plugin copies discovered
    # by --auto-roots at different real locations) are both independently
    # processed, each via its own real path/RUNPATH/$ORIGIN context.
    seen_roots: set[tuple[str, str | None]] = set()
    dynamic_text_cache: dict[Path, str] = {}

    def cached_dynamic_text(path: Path) -> str:
        cached = dynamic_text_cache.get(path)
        if cached is None:
            cached = _readelf_dynamic_text(path)
            dynamic_text_cache[path] = cached
        return cached

    def resolve_symlink_within_appdir(candidate: Path, name: str) -> Path:
        """AppImage bundling always produces SONAME symlinks that point at
        a sibling file inside the same directory (e.g. "libfoo.so.1 ->
        libfoo.so.1.2.3"). A symlink resolving *outside* lib_dir --
        whether via an absolute target or a "../" escape -- is never
        something legitimate bundling would produce. Tolerating it would
        let a library that merely happens to exist at that path on the
        machine running the audit (not inside the AppDir at all) be
        silently treated as "bundled", defeating this script's entire
        host-independence guarantee. Fail closed instead of following the
        symlink.

        readlink() targets are resolved relative to the symlink's own
        containing directory, not lib_dir itself -- with the recursive
        index this may be a nested subdirectory (e.g. a Qt plugin
        directory), so candidate.parent (not lib_dir) is the correct base
        for a relative target."""
        if not candidate.is_symlink():
            return candidate
        link_target = (candidate.parent / candidate.readlink()).resolve()
        if not link_target.is_relative_to(lib_dir_resolved):
            raise ClosureAuditError(
                f"{name} in {lib_dir} is a symlink resolving outside "
                f"the AppDir lib directory (to {link_target}) -- "
                "refusing to follow it, since a library present at "
                "that path on the machine running this audit (rather "
                "than inside the AppDir) could otherwise be "
                "misidentified as bundled and silently pass."
            )
        return link_target if link_target.exists() else candidate

    def enqueue_dependencies_of(resolved_path: Path, requester_name: str) -> None:
        bundled_closure.add(requester_name)
        for needed in _parse_needed(cached_dynamic_text(resolved_path)):
            queue.append((needed, resolved_path))
            if needed not in index and needed not in allowlist:
                missing.setdefault(needed, []).append(requester_name)

    def process_root(name: str, known_path: Path | None) -> None:
        """A root is never subjected to the reachability check: it is
        presumed loaded by its own real, already-known path (execve()'d
        directly for the app's own executable/plugins discovered by
        --auto-roots, or --root's documented convention of naming
        something expected to live directly in lib_dir), never resolved
        BY NAME through some other object's own search path in the first
        place. Once its own resolved path is known, that path becomes the
        requester for ITS OWN dependency edges exactly like any other
        object's."""
        root_key = (name, str(known_path) if known_path is not None else None)
        if root_key in seen_roots:
            return
        seen_roots.add(root_key)
        if known_path is not None:
            resolved_path = resolve_symlink_within_appdir(known_path, name)
        else:
            if name not in index:
                if name not in allowlist:
                    missing.setdefault(name, [])
                return
            resolved_path = resolve_symlink_within_appdir(
                _select_unambiguous_occurrence(name, index[name], lib_dir), name
            )
        enqueue_dependencies_of(resolved_path, name)

    for name, known_path in normalized_roots:
        process_root(name, known_path)

    while queue:
        name, requester_path = queue.pop()
        edge_key = (name, str(requester_path))
        if edge_key in seen_edges:
            continue
        seen_edges.add(edge_key)

        if name not in index:
            if name not in allowlist:
                missing.setdefault(name, [])
            continue

        requester_dynamic_text = cached_dynamic_text(requester_path)
        effective_dirs = _effective_search_dirs(
            requester_path, requester_dynamic_text, global_search_dirs,
            lib_dir_resolved,
        )
        # Round-N+ review (HIGH, "index/root still one path per
        # basename; reachability boolean then recursion chooses
        # index[name], not loader-selected duplicate"): when the same
        # basename exists at more than one path in the tree (e.g. two
        # copies of a plugin's private dependency, one genuinely
        # reachable via this requester's own RUNPATH, one that only
        # happens to sit elsewhere), the file that gets explored for
        # FURTHER dependencies must be the EXACT path a real loader
        # would actually select for THIS requester -- never whichever
        # single occurrence `index[name]` happens to remember (which
        # reflects only rglob's own traversal order, not real loader
        # precedence, and previously could silently substitute an
        # entirely unrelated copy's own DT_NEEDED/RUNPATH into this
        # requester's closure). Resolve the exact reachable file
        # directly from `effective_dirs`, in real search-order
        # precedence, rather than ever falling back to the flat index
        # for a requester-specific edge.
        resolved_path: Path | None = None
        for search_dir in effective_dirs:
            candidate = search_dir / name
            if _is_elf_file(candidate):
                resolved_path = resolve_symlink_within_appdir(candidate, name)
                break
        if resolved_path is None:
            # Bundled somewhere in the tree, but not anywhere a real
            # load of `requester_path` would actually search for it --
            # exactly as fatal as a genuinely missing dependency (see
            # this function's own docstring). Still recurse into its
            # own further DT_NEEDED entries below (using SOME
            # occurrence, purely so a single audit run can still
            # surface other real problems transitively below an
            # already-broken edge) so a single audit run reports every
            # real problem, not just the first one; never add it to
            # bundled_closure, since this edge was never validly
            # resolved.
            unreachable.setdefault(name, []).append(requester_path.name)
            representative = resolve_symlink_within_appdir(
                _select_unambiguous_occurrence(name, index[name], lib_dir), name
            )
            for needed in _parse_needed(cached_dynamic_text(representative)):
                queue.append((needed, representative))
            continue

        enqueue_dependencies_of(resolved_path, name)

    return bundled_closure, missing, unreachable, (
        {name for name, _ in seen_edges} | {name for name, _ in seen_roots}
    )



def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "lib_dir",
        type=Path,
        help="AppDir directory to audit (e.g. AppDir/usr/lib, or the whole "
        "AppDir/usr tree when combined with --auto-roots) -- searched "
        "recursively",
    )
    parser.add_argument(
        "--root",
        action="append",
        dest="roots",
        default=None,
        help="Root library name to start the closure walk from "
        "(repeatable; default: libsecret-1.so.0, unless --auto-roots is "
        "also given, in which case explicit --root values are additive to "
        "the auto-discovered roots)",
    )
    parser.add_argument(
        "--auto-roots",
        type=Path,
        default=None,
        help="Recursively discover every real ELF file (by magic bytes, "
        "not extension) under this directory and add each one's basename "
        "as an additional root -- e.g. the whole extracted AppDir/usr tree, "
        "so the app's own executable and every Qt plugin/QML module are "
        "proven complete too, not only whatever a hand-picked --root list "
        "happens to name. Typically the same directory as lib_dir itself.",
    )
    parser.add_argument(
        "--allow-x11-desktop-stack",
        action="store_true",
        help="Additionally treat X11_DESKTOP_ABI_ALLOWLIST (base X11/xcb/"
        "xkbcommon/GL-EGL client libraries) as satisfied by the host, on "
        "the same 'guaranteed already present' basis as ABI_ALLOWLIST -- "
        "an explicit, separate opt-in so a narrower audit (e.g. the "
        "libsecret-only closure) never silently inherits this broader, "
        "differently-justified assumption.",
    )
    parser.add_argument(
        "--allow-font-rendering-stack",
        action="store_true",
        help="Additionally treat FONT_RENDERING_ABI_ALLOWLIST "
        "(libfontconfig/libfreetype) as satisfied by the host -- a "
        "separate opt-in from --allow-x11-desktop-stack since the "
        "rationale differs (font-cache/config-path compatibility, not "
        "windowing-protocol ABI); never implied by --allow-x11-desktop-"
        "stack or ABI_ALLOWLIST alone.",
    )
    parser.add_argument(
        "--list-only",
        action="store_true",
        help="On success, print only the resolved closure's bundled library "
        "names (one per line, no other text) instead of the human-readable "
        "report -- intended for scripts (e.g. mutation-test harnesses) that "
        "need to enumerate closure members programmatically. Failure output "
        "is unchanged.",
    )
    parser.add_argument(
        "--global-search-dir",
        action="append",
        dest="global_search_dirs",
        default=None,
        type=Path,
        help="A directory searched for every bundled dependency edge, "
        "regardless of the requesting object's own RUNPATH/RPATH "
        "(repeatable) -- representing this project's own AppRun-exported "
        "LD_LIBRARY_PATH. Defaults to [lib_dir] alone if never given, "
        "matching every existing single-flat-directory invocation. Pass "
        "the real 'usr/lib' directory explicitly when lib_dir is a wider "
        "tree (e.g. the whole 'usr/' root, as --auto-roots typically "
        "audits), since lib_dir itself is otherwise not a real load-time "
        "search directory in that case.",
    )
    args = parser.parse_args(argv)

    if not args.lib_dir.is_dir():
        print(f"Not a directory: {args.lib_dir}", file=sys.stderr)
        return 2

    auto_roots: list[Path] = []
    if args.auto_roots is not None:
        if not args.auto_roots.is_dir():
            print(f"Not a directory: {args.auto_roots}", file=sys.stderr)
            return 2
        auto_roots = _discover_elf_roots(args.auto_roots)
        if not auto_roots:
            print(
                f"--auto-roots found no ELF files at all under {args.auto_roots} "
                "-- nothing to audit.",
                file=sys.stderr,
            )
            return 2

    explicit_root_names = args.roots or ([] if auto_roots else ["libsecret-1.so.0"])
    # Round-N+ review (HIGH, "auto roots still dict[basename,Path], losing
    # duplicates/location"): every auto-discovered root keeps its own
    # exact, real Path (never collapsed to a single "first match wins"
    # entry per basename -- see _discover_elf_roots()'s own docstring), so
    # two different roots that happen to share a bare basename are BOTH
    # preserved and independently audited via their own real location.
    # Deduplicated by the full (name, path) identity -- auto-discovered
    # roots are already sorted by real path; explicit --root names (which
    # carry no known path of their own) are appended after, deduplicated
    # only against an identical bare-name entry.
    roots: list[str | tuple[str, Path]] = []
    seen_root_identities: set[tuple[str, str | None]] = set()
    for path in auto_roots:
        identity = (path.name, str(path))
        if identity not in seen_root_identities:
            seen_root_identities.add(identity)
            roots.append((path.name, path))
    for name in explicit_root_names:
        identity = (name, None)
        if identity not in seen_root_identities:
            seen_root_identities.add(identity)
            roots.append(name)
    root_names_for_display = [
        root if isinstance(root, str) else root[0] for root in roots
    ]

    extra_allowlist = frozenset().union(
        X11_DESKTOP_ABI_ALLOWLIST if args.allow_x11_desktop_stack else frozenset(),
        FONT_RENDERING_ABI_ALLOWLIST if args.allow_font_rendering_stack else frozenset(),
    )
    allowlist = ABI_ALLOWLIST | extra_allowlist

    try:
        index = _index_lib_dir(args.lib_dir)
        # Only a bare --root NAME (no already-known path of its own) needs
        # this existence check: an --auto-roots-discovered root's exact
        # path was already proven to exist (and to be a real ELF file) by
        # _discover_elf_roots()'s own rglob/_is_elf_file() sniff.
        for name in explicit_root_names:
            if name not in index:
                print(
                    f"Root library '{name}' was not found in {args.lib_dir} at all "
                    "-- nothing to audit.",
                    file=sys.stderr,
                )
                return 2

        # Round-N+ review (HIGH, "validate PT_INTERP for executables"):
        # only meaningful for files whose real, on-disk location is
        # actually known -- --auto-roots' own discovered paths -- since a
        # bare --root NAME is only ever a library name by this script's
        # pre-existing convention, never a claim about which exact file
        # in a possibly-ambiguous tree is "the" executable to inspect.
        interp_problems = [
            problem
            for path in auto_roots
            if (problem := validate_interp(path, allowlist)) is not None
        ]
        if interp_problems:
            for problem in interp_problems:
                print(f"error: {problem}", file=sys.stderr)
            return 2

        bundled_closure, missing, unreachable, _ = audit_closure(
            args.lib_dir,
            roots,
            extra_allowlist,
            global_search_dirs=args.global_search_dirs,
        )
    except ClosureAuditError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    # Some AppDirs incidentally also bundle a copy of an ABI-allowlisted
    # library (e.g. libc.so.6) alongside everything else linuxdeploy
    # copied in; that is harmless but never *required* -- the ABI
    # allowlist already guarantees it either way -- so it is excluded from
    # what is reported/listed as the closure's actual bundling requirement.
    # This keeps `--list-only` a correct enumeration of "libraries whose
    # removal should legitimately fail this audit", for mutation testing.
    required_bundled = bundled_closure - allowlist

    if not args.list_only:
        print(f"Resolved {len(required_bundled)} required bundled librar{'y' if len(required_bundled) == 1 else 'ies'} in the closure of {root_names_for_display}:")
        for name in sorted(required_bundled):
            print(f"  {name}")

    if unreachable:
        print(
            "\nBundled elsewhere in the AppDir, but NOT reachable via the "
            "requesting object's own RUNPATH/RPATH ($ORIGIN-expanded) or "
            "any --global-search-dir -- a real dynamic loader would fail "
            "to resolve this dependency exactly as if it were entirely "
            "absent (see the module docstring's loader-search-awareness "
            "rationale):",
            file=sys.stderr,
        )
        for name, requested_by in sorted(unreachable.items()):
            requesters = ", ".join(sorted(set(requested_by))) or "(root)"
            print(f"  {name}  (required by: {requesters})", file=sys.stderr)

    if missing:
        print(
            "\nMissing non-ABI transitive dependencies (present in neither "
            f"{args.lib_dir} nor the ABI allowlist):",
            file=sys.stderr,
        )
        for name, requested_by in sorted(missing.items()):
            requesters = ", ".join(sorted(set(requested_by))) or "(root)"
            print(f"  {name}  (required by: {requesters})", file=sys.stderr)
        return 1

    if unreachable:
        return 1

    if args.list_only:
        for name in sorted(required_bundled):
            print(name)
    else:
        print(
            "\nEvery non-ABI transitive dependency resolved inside the AppDir. "
            "No host fallback is required to satisfy this closure."
        )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
