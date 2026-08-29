# Third-party dependency: libgcc_s / libstdc++ (GCC runtime libraries)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libgcc_s / libstdc++ (GCC runtime libraries)
- **Upstream:** https://gcc.gnu.org/
- **License:** GPL-3.0-or-later WITH GCC-exception-3.1 (see `COPYING.RUNTIME` (GCC Runtime Library Exception) and `third_party/qt/LICENSE.GPL3` (GPLv3 base text), reproduced verbatim from upstream)
- **Copyright:** Copyright (C) Free Software Foundation, Inc.

## Why this may be bundled

libgcc_s and libstdc++ are the GCC C/C++ runtime support libraries. They are excluded from linuxdeploy's automatic bundling by its own default blacklist (treated as "always present"), which is unsafe for a portable AppImage since their C++ ABI is not guaranteed compatible across distros/ages -- so `build-appimage.sh` force-bundles both explicitly via `find_bundled_libgccs`/`find_bundled_libstdcxx`. The GCC Runtime Library Exception (reproduced in `COPYING.RUNTIME`) is an additional permission under GPLv3 (see `third_party/qt/LICENSE.GPL3` for the full GPLv3 text this exception applies on top of) that explicitly permits linking non-GPL, including proprietary, programs against these libraries.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/gcc-runtime/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libgcc_s / libstdc++ (GCC runtime libraries)'s own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
