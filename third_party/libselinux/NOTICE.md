# Third-party dependency: libselinux

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libselinux
- **Upstream:** https://github.com/SELinuxProject/selinux
- **License:** Public domain (NSA) (see `LICENSE`, reproduced verbatim from upstream)
- **Copyright:** Public domain; see `LICENSE`

## Why this may be bundled

libselinux is a common transitive dependency of bundled GLib/GIO on SELinux-capable distributions, itself pulled in by bundled libsecret's own ELF-linked dependency graph.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/libselinux/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libselinux's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
