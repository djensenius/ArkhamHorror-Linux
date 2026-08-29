# Third-party dependency: libpng

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libpng
- **Upstream:** http://www.libpng.org/pub/png/libpng.html
- **License:** libpng License (a permissive, zlib-style license; see
  `LICENSE`, reproduced verbatim from upstream)
- **Copyright:** Copyright (c) 1995-2026 The PNG Reference Library Authors
  and prior contributors (see `LICENSE`)

## Why this may be bundled

libpng is a transitive dependency of Qt's bundled PNG `imageformats` plugin
and/or bundled fontconfig/freetype; linuxdeploy's ldd-based bundling copies
it automatically when present in that closure. This project's own
AVIF/JPEG decoding path (see `src/AssetAvifDecoder.cpp`) does not use
libpng directly; PNG support, where present, is exercised only via Qt's
own bundled image plugin.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/libpng/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libpng's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
