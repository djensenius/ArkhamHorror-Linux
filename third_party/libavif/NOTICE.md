# Third-party dependency: libavif

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libavif
- **Upstream:** https://github.com/AOMediaCodec/libavif
- **License:** BSD-2-Clause (see [`LICENSE`](./LICENSE), reproduced verbatim
  from upstream's `LICENSE` file; it additionally embeds a small BSD-2-Clause
  notice covering one file derived from dav1d, and a short IJG-style notice
  for a `third_party/iccjpeg` helper libavif itself carries -- both are part
  of the same upstream file and are reproduced unmodified along with it)
- **Copyright:** 2019 Joe Drago. All rights reserved.

## Why this is bundled

AVIF card art (djensenius/ArkhamHorror-Linux#17) is decoded directly against
libavif's own C API (see `src/AssetAvifDecoder.cpp`) rather than through any
Qt image-format plugin -- Qt has no official AVIF plugin as of this Qt
release. `libavif` is consequently a mandatory build dependency
(`pkg_check_modules(LIBAVIF REQUIRED ...)` in the top-level `CMakeLists.txt`)
and is linked as an ordinary ELF `DT_NEEDED` dependency of
`arkham_foundation`/`arkham-horror`, so `packaging/build-appimage.sh`'s
`linuxdeploy` invocation bundles `libavif.so` into the AppImage automatically
via its standard `ldd`-based dependency resolution (no explicit `--library`
flag is needed for it, unlike libsecret elsewhere in this file's sibling
directories).

`packaging/lib/bundle_codec_notices.sh`'s `bundle_codec_notices` function
copies this notice (and the sibling AV1 codec-backend notices below it) into
the distributed AppImage at
`usr/share/doc/ArkhamHorror/third_party/libavif/` -- see
`packaging/build-appimage.sh` -- so end users receive accurate attribution
for every codec library actually bundled, without a network round-trip to
this repository. `libavif` is unconditionally required to be found bundled;
the build fails loudly if it is missing rather than silently shipping without
attribution.

## AV1 codec backend

libavif itself only implements the AVIF container format; actual AV1
encoding/decoding is delegated to whichever backend the system `libavif`
package was built against (commonly `dav1d` and/or `libaom` on
Debian/Ubuntu; `libgav1`, `rav1e`, `SVT-AV1`, and `libyuv` are also possible
depending on distro/version). See the sibling `third_party/dav1d/`,
`third_party/libaom/`, `third_party/libgav1/`, `third_party/rav1e/`,
`third_party/svt-av1/`, and `third_party/libyuv/` directories for their own
license notices -- `bundle_codec_notices` bundles each one's notice only when
its corresponding library is actually found bundled inside the AppDir, since
which backend(s) are present is not fixed at build time.

## License scope

This NOTICE and the accompanying `LICENSE` file document libavif's own
BSD-2-Clause terms for attribution purposes only. ArkhamHorror-Linux itself
remains unlicensed (see the repository root); this file does not grant, and
must not be read as granting, any license to ArkhamHorror-Linux's own source
code.
