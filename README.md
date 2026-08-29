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
- A `SessionCoordinator` that composes profile storage, capability probing,
  secure token storage, and the authentication client into one
  QML-observable, deterministic state machine: boot/first-run profile
  seeding, capability probing before any secure-token read, credential
  restore (including durable cleanup of a rejected token), sign-in/register/
  sign-out, and profile switching with generation-based staleness
  protection and per-profile FIFO token operations. A separate per-profile
  credential epoch tracks whether a fresh-token save has already crossed
  the (uncancellable) secure-store boundary when a switch/restart/sign-out
  invalidates the session: if so, a compensating delete is reserved behind
  it in FIFO order so an abandoned token is never left stored, and a
  required deletion that fails is left durably blocking that profile's
  queue (never silently dropped) until a retry succeeds. Each queued token
  operation carries a unique operation ID, and each per-profile dispatch
  tracks an in-flight attempt ID: central dispatch refuses to redispatch a
  profile's head operation while one attempt is already outstanding, and a
  completion is only ever applied to the queue if it matches both IDs --
  so a duplicate retry, an overlapping `retry()` call, or a replayed/stale
  store callback can never redispatch concurrently or corrupt a later,
  unrelated operation's result. A required deletion's actionable `retry()`
  action and its visible `SecureStorageUnavailable` state are installed
  centrally (inside the FIFO dispatcher itself, gated only on whether the
  profile is still the one currently selected), not by the original
  per-call continuation, so a retry does not silently become
  un-actionable merely because the UI switched away and back (or
  restarted) while it was outstanding. `signOut()` reserves (enqueues) its
  durable delete FIRST and only then publishes the explicit `SigningOut`
  state -- and only if still `SignedIn` -- so a directly-connected
  reentrant observer of that transition can never cause the deletion
  itself to be dropped; once published, a reentrant or ordinary duplicate
  `signOut()` call while one is already in flight is a safe no-op rather
  than a second enqueued deletion (this holds even if the reserved delete
  resolves synchronously, reentering from inside `signOut()` itself).
  `start()` and `switchProfile()` assign every observable field of the new
  transition snapshot -- transitional (non-`SignedIn`) state, cleared
  identity, and, for `switchProfile()`, the new selected profile itself
  (persisted to storage first) -- together, BEFORE emitting any of
  `stateChanged()`/`currentUserChanged()`/`selectedProfileChanged()`, with
  destruction check (never a generation check) between each individual
  emission -- see the next paragraph for why. A directly-connected
  reentrant observer of any of these signals therefore only ever sees one
  complete, coherent snapshot (either the full old one, if reentered
  before the transition began, or the full new one) and never a hybrid --
  e.g. never the new profile's identity paired with the old session's
  stale `SignedIn` state, and never a persisted selection that has
  diverged from the in-memory one because a reentrant call superseded the
  transition partway through. Every emission that can reenter the
  coordinator (`stateChanged`, `currentUserChanged`,
  `selectedProfileChanged`) is guarded so a directly-connected reentrant
  `switchProfile()`/`start()`/`signOut()`/destruction during that emission
  can never dispatch a request for an abandoned profile, dereference a
  stale/destroyed probe, delete the wrong profile's token, or resurrect a
  superseded state. Its properties/signals never expose a password,
  token, or `Authorization` header. Once a transition has assigned its
  observable fields, every notify signal it owes (`stateChanged`, and
  `currentUserChanged`/`selectedProfileChanged` where applicable) is
  delivered via a single shared publication helper
  (`publishDirtyProperties()`) that tracks a per-property-group mutation/
  notification **revision** for each of the three groups (`state`+
  `diagnostic`, `currentUser`, `selectedProfile`): a group's mutation
  revision is bumped only when its externally observable getter value
  actually changes, its notification revision is marked equal to the
  current mutation revision immediately BEFORE emitting (never after),
  and a signal fires only when a group's current mutation is still newer
  than its last-notified revision. This is checked only for coordinator
  destruction between emissions, never for a nested reentrant transition
  having changed the generation in the meantime. A nested/reentrant
  transition changing the generation mid-batch only ever gates *further*
  asynchronous side effects afterward (such as starting a new capability
  probe), never whether an already-committed field's change is announced;
  a directly-connected observer can therefore never observe a stale,
  un-notified value merely because another transition interrupted the
  first one's own signal delivery. Because notification revisions (not
  coarse per-batch booleans) are tracked, a nested transition's own
  publication for a property correctly satisfies an outer, not-yet-
  notified obligation for that SAME property exactly once -- an unguarded
  directly-connected handler that reassigns an unchanged value (e.g.
  calling `start()` again whenever it observes `Loading`) creates no new
  obligation and therefore cannot recurse, and a nested transition that
  settles a property on a newer value than the interrupted outer
  transition's own is announced exactly once, for the newest value, never
  duplicated by the outer frame resuming afterward.
  Credential-restore additionally enforces at most one logical restore
  read queued or in-flight per profile, in every FIFO state (not only once
  a delete has already failed): repeated `start()` calls while a restore
  read is merely in-flight, or while an ordinary not-yet-failed delete or
  save is ahead of it in that profile's queue, rebind the single queued
  read's continuation to the newest call rather than enqueueing a
  duplicate read against the OS secret store. The
  production composition (`AppSessionComposition`) and the hermetic
  `--smoke-test` gate (`AppBootstrap`) ensure smoke-test runs never
  construct the coordinator or touch QSettings/the network/the keychain.
- Separate lint, test, build, and AppImage CI jobs.
- A `mise` entry point for local and CI tasks.

These are headless foundation pieces only: there is no final account/
sign-in/server-management QML, WebSockets, password reset, account
deletion, or gameplay wiring yet. Gameplay, card assets, and official
gaming mats are not implemented yet. Official artwork is not bundled in
this repository.

Secret Service/KWallet usability inside SteamOS Gaming Mode has not been
verified on real hardware; do not assume it works there. When a backend is
unavailable or unsupported, `QtKeychainTokenStore` reports an explicit
typed failure rather than silently falling back to an insecure store.

`SessionCoordinator`'s credential-epoch/cleanup durability guarantees hold
only for the coordinator's in-process lifetime: there is no on-disk journal
of in-flight save/delete intent, so an OS-level process kill or crash while
a fresh-token save or a required compensating deletion is in flight can
still leave a stale token in the OS secret store until the next successful
sign-out/cleanup for that profile. This is an accepted, explicitly
documented residual risk (see `SessionCoordinator.h`'s "Durability scope"
comment), not a claim of crash-proof durability.

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
