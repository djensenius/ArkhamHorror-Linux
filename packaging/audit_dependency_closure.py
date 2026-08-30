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
name) and adds each one's basename as an additional root, so the walk is
rooted at literally everything the AppImage will ever actually try to
load, not only the two libraries one prior incident happened to name.

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

_NEEDED_RE = re.compile(r"\(NEEDED\)\s+Shared library:\s+\[(?P<name>[^\]]+)\]")
_ELF_MAGIC = b"\x7fELF"


class ClosureAuditError(RuntimeError):
    """Raised when readelf itself cannot be run, a bundled file cannot be
    parsed as an ELF shared object, or a bundled SONAME symlink resolves
    outside the AppDir lib directory being audited -- all distinct from a
    MISSING dependency, which is a normal (if failing) audit outcome, not a
    tooling/integrity error."""


def _readelf_needed(path: Path) -> list[str]:
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

    return [m.group("name") for m in _NEEDED_RE.finditer(result.stdout)]


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


def _index_lib_dir(lib_dir: Path) -> dict[str, Path]:
    """Maps every real ELF shared-object basename (or symlink resolving to
    one) actually present anywhere under lib_dir (recursively) to its
    path, so a NEEDED entry's exact SONAME string (e.g.
    "libsecret-1.so.0") can be looked up directly -- AppImage bundling
    copies both a library's real file and its SONAME symlink, so exact-name
    lookup (rather than a version-stripping heuristic) is both correct and
    simple here.

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
    would make this basename-uniqueness check fire on totally unrelated
    files that a NEEDED lookup could never resolve to in the first place.

    Raises ClosureAuditError if two indexed (ELF) entries anywhere in the
    tree share an exact basename but resolve to genuinely different file
    content (by sha256, not merely a different path -- an
    independently-copied, byte-identical duplicate is harmless and common
    in real bundling, so is not treated as ambiguous). A real content
    difference could otherwise cause a NEEDED lookup to silently resolve
    to the wrong one of two same-named files, masking a genuine
    substitution risk.
    """
    index: dict[str, Path] = {}
    for entry in lib_dir.rglob("*"):
        if not _is_elf_file(entry):
            continue
        existing = index.get(entry.name)
        if existing is not None and _file_digest(existing) != _file_digest(entry):
            raise ClosureAuditError(
                f"Ambiguous duplicate basename '{entry.name}' found at both "
                f"{existing} and {entry} with different real content -- "
                "refusing to guess which one a NEEDED lookup should resolve "
                "to."
            )
        index[entry.name] = entry
    return index


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


def _discover_elf_roots(search_dir: Path) -> list[str]:
    """Recursively finds every real ELF file under search_dir (the app's own
    executable, every Qt plugin, every QML module's native library, and
    every already-bundled library alike) and returns their basenames,
    de-duplicated and sorted for deterministic output. This is what lets
    --auto-roots prove closure completeness against literally everything
    the packaged AppImage might ever try to load, not only a hand-picked
    subset some prior incident happened to name."""
    names: set[str] = set()
    for entry in search_dir.rglob("*"):
        if _is_elf_file(entry):
            names.add(entry.name)
    return sorted(names)


def audit_closure(
    lib_dir: Path,
    roots: list[str],
    extra_allowlist: frozenset[str] = frozenset(),
) -> tuple[set[str], dict[str, list[str]], set[str]]:
    """Returns (bundled_closure, missing -> [requested-by...], visited_all).

    bundled_closure: every bundled library name found reachable from roots.
    missing: every NEEDED name that was neither bundled nor allowlisted,
      mapped to the list of bundled libraries that required it (for a
      useful error message pinpointing which dependency edge is broken).
    extra_allowlist: an additional, separately-documented allowlist unioned
      with ABI_ALLOWLIST for this call only (e.g. X11_DESKTOP_ABI_ALLOWLIST,
      opted into explicitly via --allow-x11-desktop-stack) -- kept as an
      explicit parameter rather than silently merged into ABI_ALLOWLIST
      itself so a caller auditing e.g. the narrow libsecret closure never
      unintentionally also trusts the X11/GL desktop-stack assumption.
    """
    allowlist = ABI_ALLOWLIST | extra_allowlist
    index = _index_lib_dir(lib_dir)
    lib_dir_resolved = lib_dir.resolve()

    bundled_closure: set[str] = set()
    missing: dict[str, list[str]] = {}
    queue: list[str] = list(roots)
    seen: set[str] = set()

    while queue:
        name = queue.pop()
        if name in seen:
            continue
        seen.add(name)

        if name not in index:
            if name not in allowlist:
                missing.setdefault(name, [])
            continue

        bundled_closure.add(name)
        resolved_path = index[name]
        if resolved_path.is_symlink():
            # AppImage bundling always produces SONAME symlinks that point
            # at a sibling file inside the same directory (e.g.
            # "libfoo.so.1 -> libfoo.so.1.2.3"). A symlink resolving
            # *outside* lib_dir -- whether via an absolute target or a
            # "../" escape -- is never something legitimate bundling would
            # produce. Tolerating it would let a library that merely
            # happens to exist at that path on the machine running the
            # audit (not inside the AppDir at all) be silently treated as
            # "bundled", defeating this script's entire host-independence
            # guarantee. Fail closed instead of following the symlink.
            #
            # readlink() targets are resolved relative to the symlink's own
            # containing directory, not lib_dir itself -- with the
            # recursive index this may be a nested subdirectory (e.g. a Qt
            # plugin directory), so resolved_path.parent (not lib_dir) is
            # the correct base for a relative target.
            link_target = (resolved_path.parent / resolved_path.readlink()).resolve()
            if not link_target.is_relative_to(lib_dir_resolved):
                raise ClosureAuditError(
                    f"{name} in {lib_dir} is a symlink resolving outside "
                    f"the AppDir lib directory (to {link_target}) -- "
                    "refusing to follow it, since a library present at "
                    "that path on the machine running this audit (rather "
                    "than inside the AppDir) could otherwise be "
                    "misidentified as bundled and silently pass."
                )
            if link_target.exists():
                resolved_path = link_target
            else:
                resolved_path = index[name]

        for needed in _readelf_needed(resolved_path):
            queue.append(needed)
            if needed not in index and needed not in allowlist:
                missing.setdefault(needed, []).append(name)

    return bundled_closure, missing, seen


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
        "--list-only",
        action="store_true",
        help="On success, print only the resolved closure's bundled library "
        "names (one per line, no other text) instead of the human-readable "
        "report -- intended for scripts (e.g. mutation-test harnesses) that "
        "need to enumerate closure members programmatically. Failure output "
        "is unchanged.",
    )
    args = parser.parse_args(argv)

    if not args.lib_dir.is_dir():
        print(f"Not a directory: {args.lib_dir}", file=sys.stderr)
        return 2

    auto_roots: list[str] = []
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

    explicit_roots = args.roots or ([] if auto_roots else ["libsecret-1.so.0"])
    # Deduplicate while keeping output deterministic (auto-discovered roots
    # are already sorted; explicit roots are appended after, then the whole
    # list is de-duplicated by insertion order).
    roots: list[str] = []
    for root in [*auto_roots, *explicit_roots]:
        if root not in roots:
            roots.append(root)

    extra_allowlist = X11_DESKTOP_ABI_ALLOWLIST if args.allow_x11_desktop_stack else frozenset()
    allowlist = ABI_ALLOWLIST | extra_allowlist

    try:
        index = _index_lib_dir(args.lib_dir)
        for root in roots:
            if root not in index:
                print(
                    f"Root library '{root}' was not found in {args.lib_dir} at all "
                    "-- nothing to audit.",
                    file=sys.stderr,
                )
                return 2

        bundled_closure, missing, _ = audit_closure(args.lib_dir, roots, extra_allowlist)
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
        print(f"Resolved {len(required_bundled)} required bundled librar{'y' if len(required_bundled) == 1 else 'ies'} in the closure of {roots}:")
        for name in sorted(required_bundled):
            print(f"  {name}")

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
