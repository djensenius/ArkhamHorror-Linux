# Third-party dependency: QtKeychain

This directory documents (it does not vendor or build) a third-party
dependency of the ArkhamHorror-Linux client:

- **Name:** QtKeychain
- **Upstream:** https://github.com/frankosterfeld/qtkeychain
- **Version:** 0.17.0
- **Pinned commit:** `875f77d9f61bd97fd84cca47ce3bc71186dfbd09` ("Prepare
  0.17.0 release")
- **License:** BSD-3-Clause (see [`LICENSE`](./LICENSE), reproduced verbatim
  from upstream's `COPYING` file at the pinned commit, with the copyright
  notice from upstream's source file headers, e.g. `qtkeychain/keychain.h`,
  prepended for clarity)
- **Copyright:** (C) 2011-2015 Frank Osterfeld
  <frank.osterfeld@gmail.com>

QtKeychain is fetched reproducibly at build time via CMake `FetchContent`,
pinned to the exact commit above (see the top-level `CMakeLists.txt`). It is
built as a static/shared dependency of the `arkham_foundation` library and is
not modified from upstream.

## Runtime backend requirements

On Linux, QtKeychain's Secret Service backend depends on `libsecret-1` and
`Qt6::DBus` at runtime; `libsecret-1-dev` and `pkg-config` are required at
build time (see `.github/workflows/ci.yml`). QtKeychain loads
`libsecret-1.so.0` at runtime via `QLibrary` (`dlopen`), not as a linked
(`DT_NEEDED`) dependency of `libqt6keychain` -- so it is *not* discovered by
generic `ldd`-based dependency bundling. The AppImage packaging step
(`packaging/build-appimage.sh`) therefore locates the system
`libsecret-1.so.0` explicitly and passes it to `linuxdeploy` via
`--library`, which bundles it (and its own transitive shared-library
dependencies, e.g. glib/gobject/gio) into the AppImage so the backend
remains usable without a host-provided copy. CI's `appimage-smoke` job
extracts the produced AppImage and asserts `libsecret-1.so.0` is present
and resolvable within it.

## No insecure fallback

QtKeychain ships an internal plaintext fallback store
(`plaintextstore.cpp`), gated behind `Job::insecureFallback()`, which
defaults to `false` upstream. This project never enables it:
`QtKeychainJobFactory` (see `src/QtKeychainJobFactory.cpp`) explicitly calls
`setInsecureFallback(false)` on every job it creates, so a missing or
unsupported secure-storage backend always surfaces as a typed
`TokenStoreOutcome::Unavailable` failure -- never a silent plaintext write.

Additionally, a small downstream patch
(`third_party/qtkeychain/patches/0001-disable-insecure-kwallet-fallback-and-error-mapping.patch`,
applied automatically via CMake `FetchContent`'s `PATCH_COMMAND`, on top of
the unmodified pinned commit) closes two upstream KWallet-backend gaps:
the read path unconditionally consulted/migrated legacy plaintext even when
`insecureFallback()` was `false`, and the final KWallet D-Bus
read/write/delete completion handler ignored transport-level D-Bus errors
and reported success regardless. Both are fixed to respect
`insecureFallback(false)` and to surface D-Bus failures as explicit
QtKeychain errors, matching the no-plaintext-fallback and no-false-success
requirements above.

## License scope

This NOTICE and the accompanying `LICENSE` file document QtKeychain's own
BSD-3-Clause terms for attribution purposes only. ArkhamHorror-Linux itself
remains unlicensed (see the repository root); this file does not grant, and
must not be read as granting, any license to ArkhamHorror-Linux's own source
code. Both files are also installed into the distributed AppImage at
`usr/share/doc/ArkhamHorror/third_party/qtkeychain/` (see
`packaging/build-appimage.sh`) so end users receive this attribution
without a network round-trip to this repository.
