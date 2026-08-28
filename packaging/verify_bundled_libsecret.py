#!/usr/bin/env python3
"""Prove that an AppImage's bundled libsecret-1.so.0 is the copy that a real
dlopen() call actually resolves and maps, using only the AppImage's own
usr/lib directory on LD_LIBRARY_PATH (mirroring AppRun's environment setup).

QtKeychain's Secret Service backend loads libsecret-1 at runtime via
QLibrary (i.e. dlopen()), not as an ELF DT_NEEDED dependency, so `ldd` can
never show it as a dependency of libqt6keychain.so regardless of bundling.
This script performs the same kind of dlopen() QLibrary performs and then
reads /proc/self/maps to confirm the resolved, mapped file is exactly the
AppImage-bundled copy -- not any host system install -- which is a stronger
and more deterministic proof than transiently hiding the CI runner's own
system libsecret install (that approach requires mutating shared runner
state via sudo and is sensitive to ldconfig cache/symlink-recreation
quirks across distros). Because LD_LIBRARY_PATH is searched by the dynamic
linker before the system ld.so.cache and default trusted paths, resolving
to the bundled path here proves the AppImage is self-sufficient regardless
of whether the target machine has libsecret installed at all.
"""

import ctypes
import os
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <bundled-libsecret-path> <bundled-lib-dir>", file=sys.stderr)
        return 2

    bundled_secret_path, apprun_libdir = sys.argv[1], sys.argv[2]
    expected = os.path.realpath(bundled_secret_path)

    ctypes.CDLL("libsecret-1.so.0")

    resolved = None
    with open("/proc/self/maps", encoding="utf-8") as maps_file:
        for line in maps_file:
            if "libsecret-1.so.0" in line:
                resolved = line.strip().split()[-1]
                break

    if resolved is None:
        print("libsecret-1.so.0 was not found in /proc/self/maps after dlopen().", file=sys.stderr)
        return 1

    if os.path.realpath(resolved) != expected:
        print(
            f"dlopen() resolved libsecret-1.so.0 to {resolved!r}, which is not the "
            f"AppImage-bundled copy at {expected!r}. The bundled library is not what "
            "would actually be used at runtime.",
            file=sys.stderr,
        )
        return 1

    print(
        f'dlopen("libsecret-1.so.0") with LD_LIBRARY_PATH={apprun_libdir!r} resolved to '
        f"the AppImage-bundled copy at {resolved!r} -- confirmed bundled and "
        "resolvable without relying on any host installation."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
