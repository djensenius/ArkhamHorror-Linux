# Third-party dependency: zlib

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** zlib
- **Upstream:** https://zlib.net/
- **License:** zlib (see `LICENSE`, reproduced verbatim from upstream)
- **Copyright:** (C) 1995-2026 Jean-loup Gailly and Mark Adler

## Why this may be bundled

zlib is required transitively by both bundled Qt and bundled libsecret's own closure, and is force-bundled explicitly (see `find_bundled_libz` in `build-appimage.sh`) since it is not part of glibc and is excluded from linuxdeploy's automatic bundling by its own default blacklist.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/zlib/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document zlib's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
