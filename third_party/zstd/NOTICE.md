# Third-party dependency: Zstandard (libzstd)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** Zstandard (libzstd)
- **Upstream:** https://github.com/facebook/zstd
- **License:** BSD-3-Clause OR GPL-2.0-only (dual-licensed by upstream, at
  the recipient's option; this project elects to comply under the
  BSD-3-Clause terms since that avoids any GPL source-provision obligation
  for redistributing the library unmodified -- see `LICENSE`; the
  alternate `COPYING` GPLv2 text is included alongside it since upstream
  ships both files side by side)
- **Copyright:** Copyright (c) Meta Platforms, Inc. and affiliates

## Why this may be bundled

libzstd is a transitive dependency of bundled libsystemd (journal
compression) and/or bundled Qt/glib closures; linuxdeploy's ldd-based
bundling copies it automatically when present.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/zstd/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document Zstandard's own
terms for attribution purposes only. ArkhamHorror-Linux itself remains
unlicensed (see the repository root); this file does not grant, and must
not be read as granting, any license to ArkhamHorror-Linux's own source
code.
