#include "AppSessionComposition.h"

#include "NetworkCapabilityProbe.h"

#include <QNetworkAccessManager>
#include <QPointer>

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

  session->gameNam = std::make_unique<QNetworkAccessManager>();
  session->gameClient = std::make_unique<GameClient>(*session->gameNam);
  QPointer<SessionCoordinator> coordinator(session->coordinator.get());
  session->gameBoard = std::make_unique<GameBoardController>(
      *session->gameClient,
      [coordinator](SessionCoordinator::AccessTokenCallback callback) {
        if (coordinator) {
          coordinator->requestAccessToken(std::move(callback));
        }
      });
  // Leaving SignedIn (sign-out, restart, profile switch) clears the board
  // and drops every fetch still in flight; returning reloads an open screen.
  GameBoardController::bindToSession(*session->coordinator,
                                     *session->gameBoard);

  return session;
}

} // namespace Arkham
