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
- Separate lint, test, build, and AppImage CI jobs.
- A `mise` entry point for local and CI tasks.

Gameplay, authentication, networking, card assets, and official gaming mats are
not implemented yet. Official artwork is not bundled in this repository.

## Prerequisites

- [mise](https://mise.jdx.dev/)
- CMake and Ninja
- Qt 6.8 or newer with Quick, Quick Controls, and WebSockets
- C++23 compiler

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

- `src/`: C++ application, server profile, and semantic command model.
- `qml/`: declarative Steam Deck-first shell and focus behavior.
- `tests/`: headless Qt Test coverage for foundation behavior.
- `packaging/`: desktop metadata and AppImage scripts.

See the [Linux roadmap](https://github.com/djensenius/ArkhamHorror-Linux/issues/5)
and [backend roadmap](https://github.com/djensenius/ArkhamHorror/issues/5).
