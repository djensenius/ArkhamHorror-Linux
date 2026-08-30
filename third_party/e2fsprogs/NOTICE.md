# Third-party dependency: e2fsprogs (lib/et "common error" library)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** e2fsprogs's `lib/et` "common error" library (`libcom_err.so.2`)
- **Upstream:** https://e2fsprogs.sourceforge.net/ (source repository:
  https://git.kernel.org/pub/scm/fs/ext2/e2fsprogs.git)
- **Distribution package:** Ubuntu 22.04 "Jammy" `libcom-err2`, built from
  the `e2fsprogs` source package (see
  `https://packages.ubuntu.com/jammy/libcom-err2` and
  `https://git.launchpad.net/ubuntu/+source/e2fsprogs/tree/debian/copyright?h=ubuntu/jammy`).
  The exact point-release *version* (last independently confirmed:
  `1.46.5-2ubuntu1.2`) is deliberately **not** pinned as a hardcoded fact
  in this file -- a routine Ubuntu security update to this project's own
  pinned `ubuntu-22.04` CI runner (see `.github/workflows/ci.yml`'s
  `appimage-smoke` job) legitimately bumps it from time to time, and a
  frozen version string here would silently go stale the moment that
  happens, with nothing to ever catch the drift. Instead,
  `packaging/audit_codec_notices.py`'s `capture_package_provenance()` /
  `COMPONENT_EXPECTED_SOURCE_PACKAGES` re-derive and authenticate the
  REAL, currently-installed `libcom-err2` version/source-package identity
  directly from the live `dpkg` database at real build/audit time (see
  "Provenance is verified dynamically, not hardcoded" below), and the
  final produced AppImage's own SBOM (`--json-out`'s `inventory[].
  packageProvenance`) always records the exact version that was actually
  used for that specific build -- the durable, always-current source of
  truth for "which exact version shipped", rather than this document.
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

## Provenance is verified dynamically, not hardcoded

A cumulative review (round-N+, MEDIUM) correctly found that hardcoding one
exact Ubuntu package *version* string in this document (as an earlier
revision of this file did) is itself a latent defect: this project's real
CI `apt-get install` steps are deliberately left unpinned to a specific
package version (only the Ubuntu *release* -- "22.04" -- is pinned, via
`runs-on: ubuntu-22.04` in `.github/workflows/ci.yml`), so the actual
installed `libcom-err2` version can legitimately advance via routine Ubuntu
security updates between CI runs, silently making a frozen version string
here stale/wrong with nothing to ever notice.

Rather than hardcode a version doomed to eventually drift, `packaging/
audit_codec_notices.py` authenticates this component's real provenance
DYNAMICALLY at build/audit time instead:

- `capture_package_provenance("libcom_err.so.2")` finds the real,
  currently-installed system copy of this library on the CI runner (or any
  other Debian/Ubuntu host running the audit) and queries the live `dpkg`
  database for its exact installed binary package name, version, and
  source package.
- `COMPONENT_EXPECTED_SOURCE_PACKAGES["e2fsprogs"]` records only the
  STABLE fact that this component's real Debian *source package* name is
  `e2fsprogs` -- something that does not change across ordinary point
  releases -- and `validate_component_package_provenance()` fails
  `packaging/audit_codec_notices.py classify` outright if the real,
  installed system copy's own source package ever disagrees (e.g. a
  future regression re-introducing the "libcom_err is part of krb5"
  mistake this same review round's own history already once made -- see
  "Why this is its own component" below).
- The full captured `{package, version, sourcePackage}` record for every
  bundled distro library this mechanism can verify is emitted into the
  final SBOM/manifest (`packaging/audit_codec_notices.py classify
  --json-out`'s `inventory[].packageProvenance` field), giving any
  consumer of a specific, real, produced AppImage the exact version that
  build actually shipped -- accurate by construction, unlike a hand
  maintained prose string in this document ever could be.

This deliberately never requires editing this file merely because Ubuntu
shipped a routine point-release security update.

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
