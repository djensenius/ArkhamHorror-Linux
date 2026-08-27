# Contributing

ArkhamHorror-Linux is being built in reviewable vertical slices against the
[Linux roadmap](https://github.com/djensenius/ArkhamHorror-Linux/issues/5).
The Haskell backend remains authoritative for game rules.

## Before opening a pull request

- Search existing issues and discuss substantial product or protocol decisions
  before implementation.
- Report vulnerabilities through
  [private vulnerability reporting](https://github.com/djensenius/ArkhamHorror-Linux/security/advisories/new).
- Do not commit official card art, official gaming-mat art, authentication
  tokens, or private game exports.
- This repository does not yet have an open-source license. Do not assume
  permission to redistribute its source or assets.

## Local workflow

Install the pinned tools on every development platform:

```sh
mise install
```

On macOS, install the Homebrew Qt development dependencies:

```sh
mise run setup:macos
```

On Linux, install equivalent Qt development packages, then run:

```sh
mise run format:check
mise run lint
mise run test
mise run build
```

Use `mise run package:appimage` on x86_64 Linux when packaging changes.

## Pull requests

- Branch from the current `main`.
- Keep hand-written changes near 20 files and 2,000 lines or less.
- Put typed models, networking, storage, and input adapters in C++; keep
  presentation and focus behavior declarative in QML.
- Use semantic commands and native Qt focus. Controller input must not emulate
  a mouse cursor.
- Add focused tests for protocol parsing, state transitions, input mapping, and
  other high-risk behavior before visual hardening.
- State which Steam Deck, Gamescope/Wayland, X11, or offscreen environments were
  exercised.
