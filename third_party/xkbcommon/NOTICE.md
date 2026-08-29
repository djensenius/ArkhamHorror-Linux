# Third-party dependency: libxkbcommon

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libxkbcommon (and its X11-specific companion
  libxkbcommon-x11)
- **Upstream:** https://github.com/xkbcommon/libxkbcommon
- **License:** MIT AND ISC (mixed permissive; multiple copyright holders
  across the project's own source tree -- see `LICENSE`, reproduced
  verbatim from upstream's own aggregate license-statement file, which
  upstream itself keeps as the canonical list of every copyright/license
  pairing used anywhere in the tree)
- **Copyright:** see `LICENSE` for the full per-file copyright list

## Why this may be bundled

libxkbcommon implements XKB keymap compilation and keyboard-state tracking
for Qt's xcb platform plugin, which cannot handle keyboard input on
X11/XWayland without it; linuxdeploy bundles both libxkbcommon and
libxkbcommon-x11 automatically as part of the xcb platform plugin's own
resolved dependency closure.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/xkbcommon/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libxkbcommon's own
terms for attribution purposes only. ArkhamHorror-Linux itself remains
unlicensed (see the repository root); this file does not grant, and must
not be read as granting, any license to ArkhamHorror-Linux's own source
code.
