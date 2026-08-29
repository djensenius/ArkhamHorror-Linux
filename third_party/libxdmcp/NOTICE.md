# Third-party dependency: libXdmcp

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libXdmcp
- **Upstream:** https://gitlab.freedesktop.org/xorg/lib/libxdmcp
- **License:** MIT/X11 (The Open Group; see `LICENSE`, reproduced verbatim
  from upstream's `COPYING`)
- **Copyright:** Copyright 1989, 1998 The Open Group

## Why this may be bundled

libXdmcp (X Display Manager Control Protocol) is a transitive dependency of
Qt's bundled xcb platform plugin's own libX11/libxcb closure, pulled in
automatically by linuxdeploy's ldd-based bundling for the same reason as
libXau (see `third_party/libxau/`).

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/libxdmcp/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libXdmcp's own
terms for attribution purposes only. ArkhamHorror-Linux itself remains
unlicensed (see the repository root); this file does not grant, and must
not be read as granting, any license to ArkhamHorror-Linux's own source
code.
