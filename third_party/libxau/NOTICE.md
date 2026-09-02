# Third-party dependency: libXau

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libXau
- **Upstream:** https://gitlab.freedesktop.org/xorg/lib/libxau
- **License:** MIT/X11 (The Open Group; see `LICENSE`, reproduced verbatim
  from upstream's `COPYING`)
- **Copyright:** Copyright 1988, 1993, 1994, 1998 The Open Group

## Why this may be bundled

libXau (X11 authorization/"cookie" parsing) is a transitive dependency of
Qt's bundled xcb platform plugin (`libqxcb.so`, linked via libX11/libxcb's
own closure); linuxdeploy's ldd-based bundling copies it automatically
alongside the other X11 client libraries the xcb platform plugin needs.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/libxau/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libXau's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
