# Third-party dependency: SVT-AV1

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** SVT-AV1 (Scalable Video Technology for AV1)
- **Upstream:** https://github.com/AOMediaCodec/SVT-AV1
- **License:** BSD-3-Clause-Clear (see [`LICENSE`](./LICENSE), reproduced
  verbatim from upstream's `LICENSE.md`)
- **Copyright:** (c) 2021, Alliance for Open Media. All rights reserved.

## Why this may be bundled

SVT-AV1 is one of the AV1 codec backends the system `libavif` package
(a mandatory build dependency; see `third_party/libavif/NOTICE.md`) may have
been built against on the AppImage build host -- it is never linked directly
by ArkhamHorror-Linux's own code. `packaging/lib/bundle_codec_notices.sh`
scans the populated AppDir for `libSvtAv1*.so*` immediately after
`linuxdeploy`'s automatic `ldd`-based dependency bundling and copies this
notice into the distributed AppImage at
`usr/share/doc/ArkhamHorror/third_party/svt-av1/` only when that library is
actually found bundled -- see `packaging/build-appimage.sh`. This directory
is present unconditionally in the source tree so that notice is always
available to bundle when needed, but the notice itself is only copied into a
given AppImage build if SVT-AV1 is genuinely part of that build's bundled
codec closure.

## License scope

This NOTICE and the accompanying `LICENSE` file document SVT-AV1's own
BSD-3-Clause-Clear terms for attribution purposes only (note: this license
explicitly grants no patent rights -- see the license text itself).
ArkhamHorror-Linux itself remains unlicensed (see the repository root); this
file does not grant, and must not be read as granting, any license to
ArkhamHorror-Linux's own source code.
