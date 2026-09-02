# Third-party dependency: util-linux (libmount / libblkid / libuuid)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** util-linux (libmount / libblkid / libuuid)
- **Upstream:** https://github.com/util-linux/util-linux
- **License:** LGPL-2.1-or-later (see `LICENSE`, reproduced verbatim from upstream)
- **Copyright:** Copyright (C) The util-linux authors

## Why this may be bundled

libmount/libblkid/libuuid are common transitive dependencies of bundled GLib/GIO (used for mount-table and filesystem UUID lookups), themselves pulled in by bundled libsecret's own ELF-linked dependency graph.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/util-linux/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document util-linux (libmount / libblkid / libuuid)'s own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
