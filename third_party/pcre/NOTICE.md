# Third-party dependency: PCRE (legacy, pre-PCRE2)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** PCRE (legacy PCRE1 ABI; libpcre.so.3)
- **Upstream:** https://www.pcre.org/
- **License:** BSD-3-Clause (see `LICENSE`, reproduced verbatim from
  upstream)
- **Copyright:** Copyright (c) 1997-2018 University of Cambridge

## Why this may be bundled

libpcre.so.3 (the legacy PCRE1 ABI, distinct from the already-documented
`third_party/pcre2/`) is a transitive dependency some distributions'
glib/D-Bus builds still link against for backward compatibility;
linuxdeploy's ldd-based bundling copies it automatically when present in
that closure.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/pcre/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document PCRE's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
