# Third-party dependency: MIT Kerberos 5

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** MIT Kerberos 5 (libgssapi_krb5, libk5crypto, libkrb5,
  libkrb5support, libcom_err)
- **Upstream:** https://web.mit.edu/kerberos/
- **License:** MIT-krb5 (a customized MIT-style permissive license with
  additional per-contributor notices from RSA, Cygnus, FundsXpress, Red Hat
  and others -- see `NOTICE`, reproduced verbatim from upstream)
- **Copyright:** Copyright (C) 1985-2026 by the Massachusetts Institute of
  Technology; additional portions per `NOTICE`

## Why this may be bundled

The GSSAPI/Kerberos family is a transitive dependency of bundled Qt
Network's (or, independently, bundled D-Bus/glib's) GSSAPI-based
authentication support on distributions that build those components with
Kerberos enabled; linuxdeploy's ldd-based bundling copies whichever of
these five libraries the actual bundled closure needs, matched here by a
single `lib(krb5support|gssapi_krb5|k5crypto|krb5|com_err).so` pattern
rather than independent entries. `libcom_err.so.2` (Kerberos's "common
error" support library, required transitively by bundled
libgssapi_krb5/libkrb5) is, like libgpg-error, excluded from linuxdeploy's
own default bundling blacklist and is instead force-bundled explicitly by
`find_bundled_libcomerr` in `packaging/build-appimage.sh`.

This directory is present unconditionally in the source tree so its notice
is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/krb5/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document MIT Kerberos 5's
own terms for attribution purposes only. ArkhamHorror-Linux itself remains
unlicensed (see the repository root); this file does not grant, and must
not be read as granting, any license to ArkhamHorror-Linux's own source
code.
