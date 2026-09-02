# Third-party dependency: GLib / GObject / GIO

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** GLib / GObject / GIO
- **Upstream:** https://gitlab.gnome.org/GNOME/glib
- **License:** LGPL-2.1-or-later (see `LICENSE`, reproduced verbatim from upstream)
- **Copyright:** Copyright (C) The GNOME Project

## Why this may be bundled

GLib, GObject, and GIO are transitive dependencies of bundled libsecret (see `build-appimage.sh`'s own comment on libsecret's transitive closure) and are bundled the same way as any other DT_NEEDED dependency by linuxdeploy's automatic closure resolution.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/glib/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document GLib / GObject / GIO's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
