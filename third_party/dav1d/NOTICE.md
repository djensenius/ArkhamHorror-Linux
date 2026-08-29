# Third-party dependency: dav1d

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** dav1d
- **Upstream:** https://code.videolan.org/videolan/dav1d
- **License:** BSD-2-Clause (see [`LICENSE`](./LICENSE), reproduced verbatim
  from upstream's `COPYING` file)
- **Copyright:** 2018-2025, VideoLAN and dav1d authors. All rights reserved.

## Why this may be bundled

dav1d is one of the AV1 codec backends the system `libavif` package
(a mandatory build dependency; see `third_party/libavif/NOTICE.md`) may have
been built against on the AppImage build host -- it is never linked directly
by ArkhamHorror-Linux's own code. `packaging/lib/bundle_codec_notices.sh`
scans the populated AppDir for `libdav1d.so*` immediately after
`linuxdeploy`'s automatic `ldd`-based dependency bundling and copies this
notice into the distributed AppImage at
`usr/share/doc/ArkhamHorror/third_party/dav1d/` only when that library is
actually found bundled -- see `packaging/build-appimage.sh`. This directory
is present unconditionally in the source tree so that notice is always
available to bundle when needed, but the notice itself is only copied into a
given AppImage build if dav1d is genuinely part of that build's bundled
codec closure.

## License scope

This NOTICE and the accompanying `LICENSE` file document dav1d's own
BSD-2-Clause terms for attribution purposes only. ArkhamHorror-Linux itself
remains unlicensed (see the repository root); this file does not grant, and
must not be read as granting, any license to ArkhamHorror-Linux's own source
code.
