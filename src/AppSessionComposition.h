#pragma once

#include "IAuthenticationClient.h"
#include "IProfileStore.h"
#include "ITokenStore.h"
#include "NetworkAuthenticationClient.h"
#include "QSettingsProfileStore.h"
#include "QtKeychainTokenStore.h"
#include "SessionCoordinator.h"

#include <memory>

class QNetworkAccessManager;

namespace Arkham {

// Owns the full production dependency graph wired into SessionCoordinator:
// a dedicated QNetworkAccessManager for capability probing (isolated from
// NetworkAuthenticationClient's own internally owned manager -- see its
// production constructor), a QSettingsProfileStore, a QtKeychainTokenStore,
// a NetworkAuthenticationClient, and the SessionCoordinator itself.
//
// Field declaration order is deliberate and load-bearing: C++ destroys
// members in REVERSE declaration order, and SessionCoordinator only
// borrows references to everything above it, so it must be destroyed
// before any of them -- hence it is declared LAST, which means it is
// destroyed FIRST.
struct ProductionSession {
  std::unique_ptr<QNetworkAccessManager> capabilityNam;
  std::unique_ptr<QSettingsProfileStore> profileStore;
  std::unique_ptr<QtKeychainTokenStore> tokenStore;
  std::unique_ptr<NetworkAuthenticationClient> authClient;
  std::unique_ptr<SessionCoordinator> coordinator;
};

// Constructs the full production dependency graph and a SessionCoordinator
// over it, but never calls SessionCoordinator::start(): the caller decides
// if and when to begin any I/O. This function itself performs real I/O as
// soon as it runs (QSettings access, keychain backend discovery, and a
// dedicated QNetworkAccessManager), so it must only ever be invoked from
// the |composeSession| callback passed to bootstrapSession() with
// ProcessMode::Normal (see AppBootstrap.h) -- never unconditionally, and
// never for --smoke-test.
//
// The returned SessionCoordinator is deliberately unparented (its QObject
// parent is nullptr): ProductionSession's unique_ptr is its sole owner, and
// field declaration order (see above) is what guarantees it is destroyed
// before the dependencies it only borrows by reference. Parenting it to
// another QObject (e.g. a QQmlApplicationEngine) in addition to owning it
// here would double-delete it; do not do that.
[[nodiscard]] std::unique_ptr<ProductionSession> composeProductionSession();

} // namespace Arkham
