#pragma once

#include "AuthModels.h"
#include "IAuthenticationClient.h"
#include "ServerProfile.h"

#include <QHash>
#include <QObject>
#include <chrono>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Arkham {

// Production IAuthenticationClient backed by QNetworkAccessManager.
//
// The |nam| reference is borrowed; the caller must ensure it outlives this
// client. It must be an isolated QNetworkAccessManager dedicated to
// authentication traffic -- never the shared manager also used by
// NetworkCapabilityProbe or any other capability -- so cookies, cached
// authorization, and connection state from one use can never leak into or
// out of authentication requests.
//
// Every request explicitly disables cookie load/save and cached
// Authorization reuse, and sets QNetworkRequest::ManualRedirectPolicy so no
// 3xx response is ever auto-followed; every 3xx is reported as
// AuthOutcome::UnexpectedStatus. authenticate()/registerAccount() never add
// an Authorization header; whoAmI() adds exactly
// "Authorization: Token <token>".
//
// The optional |timeout| caps the wall-clock time to wait for each response.
// Defaults to 30 s in production; inject a shorter value in tests. Pass
// std::chrono::milliseconds(0) to disable per-request deadlines --
// documented as a test-only sentinel; production code must not disable the
// deadline. Negative durations are rejected with std::invalid_argument.
//
// Outstanding requests are aborted and disconnected when the client is
// destroyed; per-request timers are stopped before replies are aborted, and
// no callback is invoked after destruction.
class NetworkAuthenticationClient final : public QObject,
                                          public IAuthenticationClient {
  Q_OBJECT
public:
  static constexpr std::chrono::seconds kDefaultTimeout{30};

  explicit NetworkAuthenticationClient(
      QNetworkAccessManager &nam,
      std::chrono::milliseconds timeout = kDefaultTimeout,
      QObject *parent = nullptr);
  ~NetworkAuthenticationClient() override;

  AuthRequestHandle authenticate(const ServerProfile &profile,
                                 const AuthenticateRequest &request,
                                 AuthTokenCallback callback) override;
  AuthRequestHandle registerAccount(const ServerProfile &profile,
                                    const RegisterRequest &request,
                                    AuthTokenCallback callback) override;
  AuthRequestHandle whoAmI(const ServerProfile &profile, const QString &token,
                           CurrentUserCallback callback) override;
  void cancel(AuthRequestHandle handle) override;

private:
  template <typename T> struct Pending {
    QNetworkReply *reply{nullptr};
    QTimer *timer{nullptr}; // nullptr when the deadline is disabled
    std::function<void(AuthResult<T>)> callback;
  };

  // Emits |result| to |callback| exactly once, asynchronously, guarded by a
  // QPointer so a callback is never invoked after this client is destroyed.
  template <typename T>
  void emitAsync(const std::function<void(AuthResult<T>)> &callback,
                 AuthResult<T> result);

  template <typename T>
  AuthRequestHandle
  rejectInvalidInput(std::function<void(AuthResult<T>)> callback,
                     QString diagnostic);

  AuthRequestHandle issueTokenRequest(const ServerProfile &profile,
                                      QStringView path, const QJsonObject &body,
                                      AuthTokenCallback callback);

  QNetworkAccessManager &m_nam;
  std::chrono::milliseconds m_timeout;
  quint64 m_nextHandle{1};
  QHash<quint64, Pending<AuthToken>> m_pendingTokenRequests;
  QHash<quint64, Pending<CurrentUser>> m_pendingUserRequests;
};

} // namespace Arkham
