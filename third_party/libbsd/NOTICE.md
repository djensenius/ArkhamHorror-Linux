# Third-party dependency: libbsd

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libbsd
- **Upstream:** https://libbsd.freedesktop.org/wiki/
- **License:** BSD-3-Clause AND BSD-2-Clause AND ISC (mixed permissive per
  upstream file; see `COPYING`, reproduced verbatim from upstream in
  Debian-copyright-format, which lists every file/license/copyright triple)
- **Copyright:** Copyright © 2004-2024 Guillem Jover and other BSD-licensed
  original authors per `COPYING`

## Why this may be bundled

libbsd provides BSD-origin compatibility routines (e.g. `arc4random`,
`strlcpy`) that some transitive dependency in the bundled closure (commonly
glib or D-Bus builds on Debian/Ubuntu-derived distributions) is linked
against; linuxdeploy's ldd-based bundling copies it automatically when
present in that closure.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/libbsd/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libbsd's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
