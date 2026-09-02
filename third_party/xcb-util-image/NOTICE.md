# Third-party dependency: xcb-util-image (libxcb-image.so)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** xcb-util-image (`libxcb-image.so`)
- **Upstream:** https://gitlab.freedesktop.org/xorg/lib/libxcb-image
- **License:** MIT (see `LICENSE`, reproduced verbatim from upstream's
  `COPYING`)
- **Copyright:** Copyright (C) 2007-2008 Bart Massey; Copyright (C) 2008
  Julien Danjou; Copyright (C) 2008 Keith Packard

## Why this may be bundled

Qt's xcb platform plugin (`libqxcb.so`, force-included via linuxdeploy's
`--plugin qt` since this is the only supported Linux windowing backend for
this project) transitively links against `libxcb-image.so`. xcb-util-image
is a genuinely SEPARATE upstream project/git repository from both base
libxcb and plain xcb-util (see `third_party/xcb/NOTICE.md` and
`third_party/xcb-util/NOTICE.md`), with its own, differently-dated
copyright holders -- it must never be attributed to, or classified as,
either of those (see `COMPONENT_PATTERNS` in
`packaging/audit_codec_notices.py`, which matches this library by its
exact `libxcb-image\.so` basename, never a shared `libxcb.*` wildcard).

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's
`usr/share/doc/ArkhamHorror/third_party/xcb-util-image/` only when this
library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document xcb-util-image's
own terms for attribution purposes only. ArkhamHorror-Linux itself remains
unlicensed (see the repository root); this file does not grant, and must
not be read as granting, any license to ArkhamHorror-Linux's own source
code.
