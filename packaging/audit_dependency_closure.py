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
"""

from __future__ import annotations

import argparse
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

_NEEDED_RE = re.compile(r"\(NEEDED\)\s+Shared library:\s+\[(?P<name>[^\]]+)\]")


class ClosureAuditError(RuntimeError):
    """Raised when readelf itself cannot be run or a bundled file cannot be
    parsed as an ELF shared object -- distinct from a MISSING dependency,
    which is a normal (if failing) audit outcome, not a tooling error."""


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


def _index_lib_dir(lib_dir: Path) -> dict[str, Path]:
    """Maps every file/symlink basename actually present in lib_dir to its
    path, so a NEEDED entry's exact SONAME string (e.g. "libsecret-1.so.0")
    can be looked up directly -- AppImage bundling copies both a library's
    real file and its SONAME symlink, so exact-name lookup (rather than a
    version-stripping heuristic) is both correct and simple here."""
    index: dict[str, Path] = {}
    for entry in lib_dir.iterdir():
        if entry.is_file() or entry.is_symlink():
            index[entry.name] = entry
    return index


def audit_closure(
    lib_dir: Path, roots: list[str]
) -> tuple[set[str], dict[str, list[str]], set[str]]:
    """Returns (bundled_closure, missing -> [requested-by...], visited_all).

    bundled_closure: every bundled library name found reachable from roots.
    missing: every NEEDED name that was neither bundled nor allowlisted,
      mapped to the list of bundled libraries that required it (for a
      useful error message pinpointing which dependency edge is broken).
    """
    index = _index_lib_dir(lib_dir)

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
            if name not in ABI_ALLOWLIST:
                missing.setdefault(name, [])
            continue

        bundled_closure.add(name)
        resolved_path = index[name]
        if resolved_path.is_symlink():
            resolved_path = (lib_dir / resolved_path.readlink()).resolve()
            if not resolved_path.exists():
                resolved_path = index[name]

        for needed in _readelf_needed(resolved_path):
            queue.append(needed)
            if needed not in index and needed not in ABI_ALLOWLIST:
                missing.setdefault(needed, []).append(name)

    return bundled_closure, missing, seen


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "lib_dir", type=Path, help="AppDir lib directory to audit (e.g. AppDir/usr/lib)"
    )
    parser.add_argument(
        "--root",
        action="append",
        dest="roots",
        default=None,
        help="Root library name to start the closure walk from "
        "(repeatable; default: libsecret-1.so.0)",
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

    roots = args.roots or ["libsecret-1.so.0"]

    if not args.lib_dir.is_dir():
        print(f"Not a directory: {args.lib_dir}", file=sys.stderr)
        return 2

    for root in roots:
        if root not in _index_lib_dir(args.lib_dir):
            print(
                f"Root library '{root}' was not found in {args.lib_dir} at all "
                "-- nothing to audit.",
                file=sys.stderr,
            )
            return 2

    try:
        bundled_closure, missing, _ = audit_closure(args.lib_dir, roots)
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
    required_bundled = bundled_closure - ABI_ALLOWLIST

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
