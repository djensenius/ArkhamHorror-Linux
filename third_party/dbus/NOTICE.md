# Third-party dependency: D-Bus (libdbus-1)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** D-Bus (libdbus-1)
- **Upstream:** https://www.freedesktop.org/wiki/Software/dbus/
- **License:** AFL-2.1 OR GPL-2.0-or-later (dual-licensed by upstream, at
  the recipient's option; this project elects to comply under the
  GPL-2.0-or-later terms -- see `LICENSE`, the canonical FSF GPLv2 text)
- **Copyright:** Copyright (C) The D-Bus contributors

## Why this may be bundled

libdbus-1 is bundled as a transitive dependency of QtKeychain's
dlopen()-only libsecret D-Bus Secret Service backend's own closure (see
`third_party/libsecret/`); linuxdeploy's ldd-based bundling copies it
automatically when present.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/dbus/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document D-Bus's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
