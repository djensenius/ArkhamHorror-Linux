# Third-party dependency: libcap (POSIX capabilities)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libcap
- **Upstream:** https://git.kernel.org/pub/scm/libs/libcap/libcap.git
- **License:** BSD-3-Clause OR GPL-2.0-only (dual-licensed by upstream, at
  the recipient's option; this project elects to comply under the
  BSD-3-Clause terms since that avoids any GPL source-provision obligation
  for redistributing the library unmodified -- see `License`, reproduced
  verbatim from upstream, which contains both offered license texts)
- **Copyright:** see `License`

## Why this may be bundled

libcap is a transitive dependency of bundled libsystemd (and, on some
distributions, PAM-aware D-Bus builds); linuxdeploy's ldd-based bundling
copies it automatically when present in that closure.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/libcap/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libcap's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
