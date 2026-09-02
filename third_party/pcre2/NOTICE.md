# Third-party dependency: PCRE2

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** PCRE2
- **Upstream:** https://github.com/PCRE2Project/pcre2
- **License:** BSD-3-Clause WITH PCRE2-exception (see `LICENCE`, reproduced verbatim from upstream)
- **Copyright:** Copyright (c) various; see `LICENCE`

## Why this may be bundled

PCRE2 is a common transitive dependency of bundled GLib (used for GRegex), itself pulled in by bundled libsecret's own ELF-linked dependency graph.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/pcre2/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document PCRE2's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
