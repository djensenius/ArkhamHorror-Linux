# Third-party dependency: libsecret

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libsecret
- **Upstream:** https://gitlab.gnome.org/GNOME/libsecret
- **License:** LGPL-2.1-or-later (see `LICENSE`, reproduced verbatim from upstream)
- **Copyright:** Copyright (C) The GNOME Project / libsecret contributors

## Why this may be bundled

libsecret is the D-Bus Secret Service client library QtKeychain (see `third_party/qtkeychain/`) dlopen()s at runtime to reach the host's OS keychain/credential store; `build-appimage.sh` force-bundles it via `find_bundled_libsecret` because linuxdeploy's ldd-based bundling never discovers dlopen()-only dependencies on its own.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/libsecret/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libsecret's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
