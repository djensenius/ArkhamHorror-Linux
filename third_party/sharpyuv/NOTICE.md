# Third-party dependency: sharpyuv (libwebp project)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** SharpYuv (part of the libwebp project)
- **Upstream:** https://chromium.googlesource.com/webm/libwebp
- **License:** BSD-3-Clause (see [`LICENSE`](./LICENSE), reproduced verbatim
  from upstream)
- **Copyright:** 2010 Google Inc. All rights reserved.

## Why this may be bundled

`libsharpyuv.so.0` is libwebp's standalone "sharp" YUV<->RGB conversion
library. It is **not** the same project as Google's separate `libyuv`
(chromium.googlesource.com/libyuv/libyuv, already documented in
`third_party/libyuv/`) despite the similarly named libraries -- SharpYuv is
libavif's AVIF<->RGB colour-space conversion helper, pulled in transitively
by libavif's own bundled closure via linuxdeploy's automatic `ldd`-based
dependency bundling. It is never linked directly by ArkhamHorror-Linux's own
code. `packaging/lib/bundle_codec_notices.sh` scans the populated AppDir for
`libsharpyuv.so*` immediately after linuxdeploy's automatic bundling step and
copies this notice into the distributed AppImage at
`usr/share/doc/ArkhamHorror/third_party/sharpyuv/` only when that library is
actually found bundled -- see `packaging/build-appimage.sh`. This directory
is present unconditionally in the source tree so that notice is always
available to bundle when needed, but the notice itself is only copied into a
given AppImage build if SharpYuv is genuinely part of that build's bundled
codec closure.

## License scope

This NOTICE and the accompanying `LICENSE` file document SharpYuv's own
BSD-3-Clause terms for attribution purposes only. ArkhamHorror-Linux itself
remains unlicensed (see the repository root); this file does not grant, and
must not be read as granting, any license to ArkhamHorror-Linux's own source
code.
