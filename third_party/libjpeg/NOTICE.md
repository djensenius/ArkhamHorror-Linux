# Third-party dependency: libjpeg

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libjpeg (in practice, libjpeg-turbo on Debian/Ubuntu, the ABI- and
  API-compatible drop-in implementation virtually every distribution ships as
  their `libjpeg.so.*`)
- **Upstream:** https://github.com/libjpeg-turbo/libjpeg-turbo
- **License:** Dual-licensed -- the IJG (Independent JPEG Group) License (see
  [`README.ijg`](./README.ijg), reproduced verbatim from upstream) for the
  libjpeg API library itself, and the Modified (3-clause) BSD License (see
  [`LICENSE.md`](./LICENSE.md), reproduced verbatim from upstream's
  `LICENSE.md`) for the TurboJPEG API library, associated programs, and the
  build/test system. Both texts are reproduced together, unaltered, per the
  IJG License's own condition 1 (the README file must be included with its
  copyright/license text unaltered).
- **Copyright:** 1991-2020, Thomas G. Lane, Guido Vollbeding (IJG portions);
  2009-2026 D. R. Commander, 2018-2023 Randy \<randy408@protonmail.com\>
  (libjpeg-turbo/TurboJPEG portions)

## Why this is bundled

Qt's `libqjpeg` image-format plugin (bundled into every produced AppImage;
see the "Verify the JPEG Qt plugin is bundled and loadable" CI step) links
against the system `libjpeg.so` as an ordinary ELF `DT_NEEDED` dependency,
which `linuxdeploy`'s automatic `ldd`-based dependency resolution follows and
copies into the AppImage the same way it does for `libavif` and its AV1 codec
backends -- but, as with those, `linuxdeploy` has no notion of license
attribution, so nothing previously arranged for libjpeg's required IJG/BSD
notice text to ship alongside it.

`packaging/audit_codec_notices.py`'s full recursive ELF-closure classifier
(review round-4 item 12) requires every bundled non-ABI-allowlisted `.so`
found anywhere under the AppDir to resolve to a known component with a
notice source directory here under `third_party/`; libjpeg is one of those
components. `packaging/lib/bundle_codec_notices.sh` copies this notice into
the distributed AppImage at
`usr/share/doc/ArkhamHorror/third_party/libjpeg/` -- see
`packaging/build-appimage.sh`.

## License scope

This NOTICE and the accompanying `LICENSE.md`/`README.ijg` files document
libjpeg-turbo's own dual licensing terms for attribution purposes only.
ArkhamHorror-Linux itself remains unlicensed (see the repository root); this
file does not grant, and must not be read as granting, any license to
ArkhamHorror-Linux's own source code.
