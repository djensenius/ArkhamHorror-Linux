# Third-party dependency: ICU (International Components for Unicode)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** ICU (libicudata, libicui18n, libicuuc)
- **Upstream:** https://icu.unicode.org/
- **License:** Unicode-3.0 (see `LICENSE`, reproduced verbatim from
  upstream; this same file also documents ICU's own historical "ICU
  License" used by older releases for older code, itself equally
  permissive)
- **Copyright:** Copyright © 1991-2026 Unicode, Inc.

## Why this may be bundled

ICU is a transitive dependency of bundled Qt6Core's text/locale/collation
support (`QLocale`, `QCollator`) on distributions where Qt is built with
ICU enabled; linuxdeploy's ldd-based bundling copies all three ICU
libraries automatically when present in that closure.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/icu/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document ICU's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
