# Third-party dependency: xcb-util-renderutil (libxcb-render-util.so)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** xcb-util-renderutil (`libxcb-render-util.so`)
- **Upstream:** https://gitlab.freedesktop.org/xorg/lib/libxcb-render-util
- **License:** MIT (see `LICENSE`, reproduced verbatim from upstream's
  `COPYING`)
- **Copyright:** Copyright (C) 2000 Keith Packard; Copyright (C) 2006 Jamey
  Sharp; Copyright (C) 2006 Ian Osgood

## Why this may be bundled

Qt's xcb platform plugin (`libqxcb.so`, force-included via linuxdeploy's
`--plugin qt` since this is the only supported Linux windowing backend for
this project) transitively links against `libxcb-render-util.so`.
xcb-util-renderutil is a genuinely SEPARATE upstream project/git repository
from base libxcb and the other xcb-util-* components (see
`third_party/xcb/NOTICE.md` and sibling `third_party/xcb-util*/`
directories), with its own, differently-dated copyright holders -- it must
never be attributed to, or classified as, any of those (see
`COMPONENT_PATTERNS` in `packaging/audit_codec_notices.py`, which matches
this library by its exact `libxcb-render-util\.so` basename, never a
shared `libxcb.*` wildcard).

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's
`usr/share/doc/ArkhamHorror/third_party/xcb-util-renderutil/` only when
this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document
xcb-util-renderutil's own terms for attribution purposes only.
ArkhamHorror-Linux itself remains unlicensed (see the repository root);
this file does not grant, and must not be read as granting, any license
to ArkhamHorror-Linux's own source code.
