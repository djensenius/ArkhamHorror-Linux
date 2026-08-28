# ArkhamHorror-Linux

Steam Deck-first native client for the
[Arkham Horror LCG backend](https://github.com/djensenius/ArkhamHorror).

The application uses Qt Quick for a controller-first, semantic digital card
game. It will not emulate a mouse cursor or reproduce tabletop-simulator object
physics. Rules and outcomes remain authoritative on the existing Haskell
backend.

## Foundation scope

The current walking skeleton establishes:

- Qt Quick/QML presentation with a typed C++ core.
- Semantic focus commands for controller, keyboard, touch, and pointer inputs.
- A server-profile model ready for hosted and self-hosted deployments.
- A secure, per-profile authentication-token store (`ITokenStore` /
  `QtKeychainTokenStore`) backed by the OS secret store (Secret
  Service/libsecret or KWallet on Linux, Keychain on macOS) via QtKeychain --
  never QSettings, a file, an environment variable, or plaintext.
- An injectable, redirect-safe authentication HTTP client
  (`IAuthenticationClient` / `NetworkAuthenticationClient`) for the backend's
  `/authenticate`, `/register`, and `/whoami` endpoints. HTTPS is required
  for any host; a narrow, explicitly tested loopback-only HTTP exception
  (`localhost` or a loopback IPv4/IPv6 literal) preserves local
  development/self-hosting without ever sending a password or bearer token
  over cleartext HTTP to a LAN or public host.
- Separate lint, test, build, and AppImage CI jobs.
- A `mise` entry point for local and CI tasks.

These are headless foundation pieces only: there is no account/sign-in UI,
app-level session coordinator, or gameplay wiring yet. Gameplay, card
assets, and official gaming mats are not implemented yet. Official artwork
is not bundled in this repository.

Secret Service/KWallet usability inside SteamOS Gaming Mode has not been
verified on real hardware; do not assume it works there. When a backend is
unavailable or unsupported, `QtKeychainTokenStore` reports an explicit
typed failure rather than silently falling back to an insecure store.

## Prerequisites

- [mise](https://mise.jdx.dev/)
- CMake and Ninja
- Qt 6.8 or newer with Quick, Quick Controls, Network, and WebSockets
- C++23 compiler
- On Linux only: `libsecret-1-dev` and `pkg-config`, required to build
  QtKeychain's Secret Service backend (see `.github/workflows/ci.yml`).
  QtKeychain itself is fetched reproducibly via CMake `FetchContent`, pinned
  to an exact upstream commit (see `CMakeLists.txt` and
  `third_party/qtkeychain/NOTICE.md`); it is not vendored in this
  repository. A small, reviewable downstream patch closes two upstream
  KWallet-backend gaps (an unconditional legacy-plaintext read/migration
  path, and a completion handler that ignored failed D-Bus replies) -- see
  `third_party/qtkeychain/patches/` and `third_party/qtkeychain/NOTICE.md`.
  The AppImage explicitly bundles `libsecret-1.so.0`, since QtKeychain loads
  it at runtime via `QLibrary` rather than as a linked dependency (see
  `packaging/build-appimage.sh`).

On macOS, `mise run setup:macos` installs the Homebrew dependencies. On Linux,
install the equivalent Qt development packages or use CI as the reference. CI
currently pins Qt 6.11.1 while CMake retains 6.8 as the supported minimum.

## Commands

```sh
mise run setup:macos
mise run format:check
mise run lint
mise run test
mise run build
```

The AppImage task runs on x86_64 Linux:

```sh
mise run package:appimage
```

## Architecture

- `src/`: C++ application, server profile, secure token store, and
  authentication client, plus the semantic command model.
- `qml/`: declarative Steam Deck-first shell and focus behavior.
- `tests/`: headless Qt Test coverage for foundation behavior.
- `packaging/`: desktop metadata and AppImage scripts.
- `third_party/qtkeychain/`: BSD-3-Clause attribution and pin notes for the
  QtKeychain dependency (see above); no vendored source.

See the [Linux roadmap](https://github.com/djensenius/ArkhamHorror-Linux/issues/5)
and [backend roadmap](https://github.com/djensenius/ArkhamHorror/issues/5).
