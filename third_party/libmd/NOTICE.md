# Third-party dependency: libmd

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libmd
- **Upstream:** https://www.hadrons.org/software/libmd/
- **License:** BSD-3-Clause (see `COPYING`, reproduced verbatim from
  upstream in Debian-copyright-format)
- **Copyright:** Copyright 2009-2026 Guillem Jover; portions Copyright
  2000-2001 Aaron D. Gifford (see `COPYING`)

## Why this may be bundled

libmd (standalone message-digest routines split out of libbsd) is a
transitive dependency of bundled libbsd on Debian/Ubuntu-derived build
hosts; linuxdeploy's ldd-based bundling copies it automatically alongside
libbsd (see `third_party/libbsd/`).

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/libmd/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libmd's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
