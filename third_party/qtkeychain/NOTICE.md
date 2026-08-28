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
build time (see `.github/workflows/ci.yml`). The AppImage packaging step
(`packaging/build-appimage.sh`) bundles `libqt6keychain` and its shared
library dependencies (including `libsecret-1`) via `linuxdeploy`'s automatic
`ldd`-based dependency resolution.

## No insecure fallback

QtKeychain ships an internal plaintext fallback store
(`plaintextstore.cpp`), gated behind `Job::insecureFallback()`, which
defaults to `false` upstream. This project never enables it:
`QtKeychainJobFactory` (see `src/QtKeychainJobFactory.cpp`) explicitly calls
`setInsecureFallback(false)` on every job it creates, so a missing or
unsupported secure-storage backend always surfaces as a typed
`TokenStoreOutcome::Unavailable` failure -- never a silent plaintext write.

## License scope

This NOTICE and the accompanying `LICENSE` file document QtKeychain's own
BSD-3-Clause terms for attribution purposes only. ArkhamHorror-Linux itself
remains unlicensed (see the repository root); this file does not grant, and
must not be read as granting, any license to ArkhamHorror-Linux's own source
code.
