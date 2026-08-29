#include "AppSessionComposition.h"

#include "NetworkCapabilityProbe.h"

#include <QNetworkAccessManager>

namespace Arkham {

std::unique_ptr<ProductionSession> composeProductionSession() {
  auto session = std::make_unique<ProductionSession>();

  session->capabilityNam = std::make_unique<QNetworkAccessManager>();
  session->profileStore = std::make_unique<QSettingsProfileStore>(
      QStringLiteral("djensenius"), QStringLiteral("Arkham Horror"));
  session->tokenStore = std::make_unique<QtKeychainTokenStore>();
  // Production constructor: owns its own dedicated QNetworkAccessManager,
  // isolated from the capability probe's manager above.
  session->authClient = std::make_unique<NetworkAuthenticationClient>();

  QNetworkAccessManager *capabilityNam = session->capabilityNam.get();
  session->coordinator = std::make_unique<SessionCoordinator>(
      *session->profileStore,
      [capabilityNam] {
        return std::make_unique<NetworkCapabilityProbe>(*capabilityNam);
      },
      *session->tokenStore, *session->authClient);

  return session;
}

} // namespace Arkham
