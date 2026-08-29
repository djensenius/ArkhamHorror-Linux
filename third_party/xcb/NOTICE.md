# Third-party dependency: libxcb (base library and built-in extensions)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libxcb, and the X11 protocol-extension libraries built from the
  SAME upstream source repository (libxcb-glx, -randr, -render, -shape,
  -shm, -sync, -xfixes, -xkb, -dri2, -dri3, -present, -res, -screensaver,
  -xf86dri, -xinerama, -xtest, -xv, -xvmc)
- **Upstream:** https://gitlab.freedesktop.org/xorg/lib/libxcb
- **License:** MIT (see `LICENSE`, reproduced verbatim from upstream's
  `COPYING`)
- **Copyright:** Copyright (C) 2001-2006 Bart Massey, Jamey Sharp, and Josh
  Triplett

## Why this may be bundled

Qt's xcb platform plugin (`libqxcb.so`, force-included via linuxdeploy's
`--plugin qt` since this is the only supported Linux windowing backend for
this project) links against the base libxcb library and the X11 protocol
extensions listed above, all of which are built from libxcb's own single
source repository and share exactly one license/copyright. Matched here by
an explicit `libxcb\.so` pattern plus a fixed, explicit list of these
specific built-in extension basenames (see `COMPONENT_PATTERNS` in
`packaging/audit_codec_notices.py`) -- never a bare `libxcb.*` wildcard,
which would incorrectly also match the genuinely SEPARATE, differently
copyrighted `xcb-util`/`xcb-util-image`/`xcb-util-keysyms`/
`xcb-util-renderutil`/`xcb-util-wm`/`xcb-util-cursor` projects documented
in their own `third_party/xcb-util*/` directories -- a misattribution a
later cumulative review specifically found and required be split apart.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/xcb/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libxcb's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
