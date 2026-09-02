# Third-party dependency: Brotli (libbrotlicommon / libbrotlidec)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** Brotli (libbrotlicommon, libbrotlidec)
- **Upstream:** https://github.com/google/brotli
- **License:** MIT (see `LICENSE`, reproduced verbatim from upstream)
- **Copyright:** Copyright (c) 2009, 2010, 2013-2016 by the Brotli Authors

## Why this may be bundled

libbrotlicommon/libbrotlidec are a transitive dependency of bundled Qt
Network's TLS/HTTP compression support (and, independently, of some system
freetype/fontconfig builds); linuxdeploy's ldd-based bundling copies
whichever of these the actual bundled Qt6Network/Qt6Core build was linked
against.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/brotli/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document Brotli's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
