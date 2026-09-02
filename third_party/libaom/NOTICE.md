# Third-party dependency: libaom (AV1 reference codec)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libaom (aom)
- **Upstream:** https://aomedia.googlesource.com/aom
- **License:** BSD-2-Clause (see [`LICENSE`](./LICENSE), reproduced verbatim
  from upstream), plus the Alliance for Open Media Patent License 1.0 (see
  [`PATENTS`](./PATENTS), reproduced verbatim from upstream)
- **Copyright:** (c) 2016, Alliance for Open Media. All rights reserved.

## Why this may be bundled

libaom is one of the AV1 codec backends the system `libavif` package
(a mandatory build dependency; see `third_party/libavif/NOTICE.md`) may have
been built against on the AppImage build host -- it is never linked directly
by ArkhamHorror-Linux's own code. `packaging/lib/bundle_codec_notices.sh`
scans the populated AppDir for `libaom.so*` immediately after `linuxdeploy`'s
automatic `ldd`-based dependency bundling and copies this notice (both the
copyright license and the patent grant) into the distributed AppImage at
`usr/share/doc/ArkhamHorror/third_party/libaom/` only when that library is
actually found bundled -- see `packaging/build-appimage.sh`. This directory
is present unconditionally in the source tree so that notice is always
available to bundle when needed, but the notice itself is only copied into a
given AppImage build if libaom is genuinely part of that build's bundled
codec closure.

## License scope

This NOTICE and the accompanying `LICENSE`/`PATENTS` files document libaom's
own terms for attribution purposes only. ArkhamHorror-Linux itself remains
unlicensed (see the repository root); this file does not grant, and must not
be read as granting, any license to ArkhamHorror-Linux's own source code.
