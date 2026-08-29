# Third-party dependency: liblzma (XZ Utils)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** liblzma (XZ Utils)
- **Upstream:** https://github.com/tukaani-project/xz
- **License:** 0BSD (see `COPYING`, `COPYING.0BSD`, reproduced verbatim from upstream)
- **Copyright:** Copyright (C) The XZ Utils authors and contributors

## Why this may be bundled

liblzma is a common transitive dependency of bundled GLib/GIO (compressed-data support), itself pulled in by bundled libsecret's own ELF-linked dependency graph.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/liblzma/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document liblzma (XZ Utils)'s own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
