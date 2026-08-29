# Third-party dependency: libxcb and its extension libraries

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libxcb and its extension libraries (libxcb-cursor, -glx, -icccm,
  -image, -keysyms, -randr, -render, -render-util, -shape, -shm, -sync,
  -util, -xfixes, -xkb, ...)
- **Upstream:** https://gitlab.freedesktop.org/xorg/lib/libxcb
- **License:** MIT (see `LICENSE`, reproduced verbatim from upstream's
  `COPYING`)
- **Copyright:** Copyright (C) 2001-2006 Bart Massey, Jamey Sharp, and Josh
  Triplett

## Why this may be bundled

Qt's xcb platform plugin (`libqxcb.so`, force-included via linuxdeploy's
`--plugin qt` since this is the only supported Linux windowing backend for
this project) links against the base libxcb library and a family of
similarly-licensed `libxcb-*` extension libraries. All are published from
the same upstream project sharing one license, matched here by a single
`libxcb.*` basename prefix pattern (see `COMPONENT_PATTERNS` in
`packaging/audit_codec_notices.py`) rather than an exhaustive per-extension
name list -- mirroring how the already-documented `libQt6.*`/`libabsl_*`
families are handled.

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
