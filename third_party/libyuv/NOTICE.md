# Third-party dependency: libyuv

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libyuv
- **Upstream:** https://chromium.googlesource.com/libyuv/libyuv
- **License:** BSD-3-Clause (see [`LICENSE`](./LICENSE), reproduced verbatim
  from upstream)
- **Copyright:** 2011 The LibYuv Project Authors. All rights reserved.

## Why this may be bundled

libyuv provides pixel-format conversion routines some AV1 codec backends
(and libavif's own YUV<->RGB conversion path) may link against on the
AppImage build host -- it is never linked directly by ArkhamHorror-Linux's
own code. `packaging/lib/bundle_codec_notices.sh` scans the populated AppDir
for `libyuv.so*` immediately after `linuxdeploy`'s automatic `ldd`-based
dependency bundling and copies this notice into the distributed AppImage at
`usr/share/doc/ArkhamHorror/third_party/libyuv/` only when that library is
actually found bundled -- see `packaging/build-appimage.sh`. This directory
is present unconditionally in the source tree so that notice is always
available to bundle when needed, but the notice itself is only copied into a
given AppImage build if libyuv is genuinely part of that build's bundled
codec closure.

## License scope

This NOTICE and the accompanying `LICENSE` file document libyuv's own
BSD-3-Clause terms for attribution purposes only. ArkhamHorror-Linux itself
remains unlicensed (see the repository root); this file does not grant, and
must not be read as granting, any license to ArkhamHorror-Linux's own source
code.
