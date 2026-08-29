# Third-party dependency: LZ4

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** LZ4
- **Upstream:** https://github.com/lz4/lz4
- **License:** BSD-2-Clause (see `LICENSE`, reproduced verbatim from
  upstream)
- **Copyright:** Copyright (c) 2011-2020, Yann Collet

## Why this may be bundled

liblz4 is a transitive dependency of bundled libsystemd (journal
compression) and/or bundled glib's own closure; linuxdeploy's ldd-based
bundling copies it automatically when present.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/lz4/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document LZ4's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
