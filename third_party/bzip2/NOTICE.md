# Third-party dependency: bzip2

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** bzip2 / libbzip2
- **Upstream:** https://sourceware.org/bzip2/
- **License:** bzip2 (BSD-style, see [`LICENSE`](./LICENSE), reproduced
  verbatim from upstream)
- **Copyright:** 1996-2019 Julian R Seward. All rights reserved.

## Why this may be bundled

`libbz2.so.1.0` is pulled in transitively by bundled Qt (QtCore's compression
support) and/or util-linux's `libmount`/`libblkid` closure via linuxdeploy's
own `ldd`-based automatic dependency bundling -- it is never linked directly
by ArkhamHorror-Linux's own code. `packaging/lib/bundle_codec_notices.sh`
scans the populated AppDir for `libbz2.so*` immediately after linuxdeploy's
automatic bundling step and copies this notice into the distributed AppImage
at `usr/share/doc/ArkhamHorror/third_party/bzip2/` only when that library is
actually found bundled -- see `packaging/build-appimage.sh`. This directory
is present unconditionally in the source tree so that notice is always
available to bundle when needed, but the notice itself is only copied into a
given AppImage build if bzip2 is genuinely part of that build's bundled
library closure.

## License scope

This NOTICE and the accompanying `LICENSE` file document bzip2's own
BSD-style terms for attribution purposes only. ArkhamHorror-Linux itself
remains unlicensed (see the repository root); this file does not grant, and
must not be read as granting, any license to ArkhamHorror-Linux's own source
code.
