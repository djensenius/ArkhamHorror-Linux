# Third-party dependency: e2fsprogs (lib/et "common error" library)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** e2fsprogs's `lib/et` "common error" library (`libcom_err.so.2`)
- **Upstream:** https://e2fsprogs.sourceforge.net/ (source repository:
  https://git.kernel.org/pub/scm/fs/ext2/e2fsprogs.git)
- **Distribution package:** Ubuntu 22.04 "Jammy" `libcom-err2`, version
  `1.46.5-2ubuntu1.1`, built from the `e2fsprogs` source package (see
  `https://packages.ubuntu.com/jammy/libcom-err2` and
  `https://git.launchpad.net/ubuntu/+source/e2fsprogs/tree/debian/copyright?h=ubuntu/jammy`)
- **License:** MIT-style (the e2fsprogs `lib/et`/`lib/ss` license -- see
  `NOTICE`, reproduced verbatim from Ubuntu's `debian/copyright` for the
  e2fsprogs source package). This is a genuinely DIFFERENT license text,
  from a genuinely different upstream project, than the similarly-named
  "MIT-krb5" license documented in `../krb5/NOTICE.md` -- see "Why this is
  its own component" below.
- **Copyright:** Copyright 1987 by the Student Information Processing
  Board of the Massachusetts Institute of Technology

## Why this may be bundled

`libcom_err.so.2` is required transitively by bundled `libgssapi_krb5`/
`libkrb5` (see `../krb5/NOTICE.md`) and, like `libgpg-error`, is excluded
from linuxdeploy's own default bundling blacklist and is instead
force-bundled explicitly by `find_bundled_libcomerr` in
`packaging/build-appimage.sh`.

## Why this is its own component, separate from `../krb5/`

A cumulative review (round-9+ item 11) found this project's own
`packaging/audit_codec_notices.py` previously matched `libcom_err.so`
together with the four genuine MIT Kerberos 5 libraries under one single
`krb5` component/notice, on the mistaken assumption that "libcom_err is
part of Kerberos". This is factually wrong on the actual Linux
distribution this project targets: on Ubuntu 22.04 "Jammy" (and Debian's
own e2fsprogs packaging, which Ubuntu inherits `debian/copyright`
verbatim from), `libcom_err.so.2` is built and shipped by the
**`libcom-err2`** binary package, whose *source* package is **`e2fsprogs`**
-- an entirely separate upstream project (the ext2/3/4 filesystem
utilities, authored by Theodore Ts'o) from MIT Kerberos 5, with its own
distinct package name, version, copyright holder, and license text. The
similarity is coincidental: MIT Kerberos 5's own libraries are also
copyrighted by "the Massachusetts Institute of Technology", and
e2fsprogs's `lib/et`/`lib/ss` license text is *also* MIT-style and *also*
traces back to an MIT entity (the Student Information Processing Board)
-- but they are two different license texts from two different MIT
groups, packaged and versioned entirely independently by the
distribution. Continuing to fold `libcom_err.so.2`'s attribution into the
`krb5` component would misrepresent both its actual upstream source
package/version and its actual license text to anyone inspecting the
bundled AppImage's notices or SBOM.

`packaging/audit_codec_notices.py`'s `COMPONENT_PATTERNS` accordingly
matches `libcom_err.so` on its own, independent
`^libcom_err\.so` pattern, mapped to this `e2fsprogs` component -- never
folded back into the `krb5` component's own
`lib(krb5support|gssapi_krb5|k5crypto|krb5)\.so` pattern.

This directory is present unconditionally in the source tree so its
notice is always available to bundle when needed; `packaging/audit_codec_notices.py`
(driven by `packaging/lib/bundle_codec_notices.sh`) copies it into the
distributed AppImage's `usr/share/doc/ArkhamHorror/third_party/e2fsprogs/`
only when this library is actually found bundled in a given build.

## License scope

This NOTICE and the accompanying license file(s) document e2fsprogs's own
terms for attribution purposes only. ArkhamHorror-Linux itself remains
unlicensed (see the repository root); this file does not grant, and must
not be read as granting, any license to ArkhamHorror-Linux's own source
code.
