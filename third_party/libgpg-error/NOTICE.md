# Third-party dependency: libgpg-error

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libgpg-error
- **Upstream:** https://gnupg.org/software/libgpg-error/index.html
- **License:** LGPL-2.1-or-later (see `LICENSE`, reproduced verbatim from upstream)
- **Copyright:** Copyright (C) The GnuPG Project

## Why this may be bundled

libgpg-error is a transitive dependency of bundled libgcrypt, itself pulled in by libsecret's GNOME-Keyring/GPG-Agent integration; linuxdeploy's default blacklist excludes it from automatic bundling, so `build-appimage.sh` force-bundles it explicitly via `find_bundled_libgpgerror`.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/libgpg-error/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libgpg-error's own terms
for attribution purposes only. ArkhamHorror-Linux itself remains unlicensed
(see the repository root); this file does not grant, and must not be read as
granting, any license to ArkhamHorror-Linux's own source code.
