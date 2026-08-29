# Third-party dependency: libkeyutils

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** libkeyutils
- **Upstream:** https://git.kernel.org/pub/scm/linux/kernel/git/dhowells/keyutils.git
- **License:** LGPL-2.1-or-later (see `LICENSE`, the canonical FSF LGPLv2.1
  text)
- **Copyright:** Copyright (C) David Howells and the keyutils contributors

## Why this may be bundled

libkeyutils is a transitive dependency of bundled libkrb5 (MIT Kerberos's
kernel-keyring-backed credential-cache support, see `third_party/krb5/`);
linuxdeploy's ldd-based bundling copies it automatically alongside the
krb5 family.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/libkeyutils/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document libkeyutils's own
terms for attribution purposes only. ArkhamHorror-Linux itself remains
unlicensed (see the repository root); this file does not grant, and must
not be read as granting, any license to ArkhamHorror-Linux's own source
code.
