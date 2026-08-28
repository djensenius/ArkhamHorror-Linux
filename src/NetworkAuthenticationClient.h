#pragma once

#include "AuthModels.h"
#include "IAuthenticationClient.h"
#include "ServerProfile.h"

#include <QHash>
#include <QObject>
#include <chrono>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Arkham {

// Production IAuthenticationClient backed by QNetworkAccessManager.
//
// The production constructor creates and owns a dedicated
// QNetworkAccessManager (destroyed together with this client), so
// authentication traffic is isolated from any other manager (e.g.
// NetworkCapabilityProbe's) without depending on caller convention. A
// second constructor borrows an externally-owned manager instead, for tests
// that need to inject a fake QNetworkAccessManager subclass; that manager
// must still be isolated and must outlive this client.
//
// Every request explicitly disables cookie load/save and cached
// Authorization reuse, forces cache-bypassing load control
// (CacheLoadControlAttribute = AlwaysNetwork) and disables cache writes
// (CacheSaveControlAttribute = false), and sets
// QNetworkRequest::ManualRedirectPolicy so no 3xx response is ever
// auto-followed; every 3xx is reported as AuthOutcome::UnexpectedStatus.
// authenticate()/registerAccount() never add an Authorization header;
// whoAmI() adds exactly "Authorization: Token <token>".
//
// Every request is also rejected before it is built/sent unless its
// resolved URL is either https, or http restricted to a loopback host --
// exactly the case-insensitive hostname "localhost" (no suffix/trailing
// dot/userinfo), canonical four-component dotted-decimal "127.x.y.z" (no
// leading zeros, octal/hex/shortened forms, or out-of-range components),
// or the exact literal "::1" -- see isCanonicalLoopbackHostText() and
// isSecureOrLoopbackAuthTransport() in AuthTransportSecurity.h/.cpp. This
// is deliberately a strict lexical allow-list rather than delegating to
// QHostAddress::setAddress(), because QHostAddress (like QUrl itself)
// treats many ambiguous/non-canonical numeric spellings (e.g. "127.1",
// octal/hex/single-integer IPv4, IPv4-mapped or expanded IPv6) as
// equivalent to a canonical loopback address; the authoritative rejection
// of such spellings actually happens earlier still, in
// UrlValidator::validateCustomUrl() against the raw pre-QUrl input text,
// before QUrl can canonicalize an unsafe spelling into an accepted one --
// this check here is retained purely as request-time defense-in-depth.
// ServerProfile exposes no public constructor that can carry an unvalidated
// URL (its only public constructor is the argument-less default one, which
// is permanently invalid), and this class additionally rejects any profile
// for which ServerProfile::hasValidatedProvenance() is false, so this
// defense-in-depth layer only matters if some future internal code path
// ever bypassed those structural guarantees. This preserves local
// development/self-hosting over plain HTTP without ever allowing a LAN or
// public host to receive a password or bearer token in cleartext. A
// hostname that merely resembles a loopback address without being one
// (e.g. "localhost.evil.com", "127.0.0.1.evil.com") is a different DNS name
// and is never treated as loopback; userinfo in the URL is rejected
// unconditionally. Because 3xx is never followed, this exception can never
// be leveraged to redirect a loopback request to an insecure remote origin.
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

  // Production constructor: owns a dedicated QNetworkAccessManager.
  explicit NetworkAuthenticationClient(
      std::chrono::milliseconds timeout = kDefaultTimeout,
      QObject *parent = nullptr);

  // Test/advanced constructor: borrows |nam| (see class comment above; must
  // be isolated and must outlive this client).
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

  // Shared implementation constructor. |ownedNam| is non-null only when the
  // production (owning) constructor delegates here, in which case |nam| is
  // ignored; otherwise |nam| is the externally-owned, caller-borrowed
  // manager.
  NetworkAuthenticationClient(std::unique_ptr<QNetworkAccessManager> ownedNam,
                              QNetworkAccessManager *nam,
                              std::chrono::milliseconds timeout,
                              QObject *parent);

  // Emits |result| to |callback| exactly once, asynchronously, guarded by a
  // QPointer so a callback is never invoked after this client is destroyed.
  // Takes both by value and moves them into the queued invocation so the
  // secret-bearing AuthResult<T> (and the callback itself) are never copied.
  template <typename T>
  void emitAsync(std::function<void(AuthResult<T>)> callback,
                 AuthResult<T> result);

  template <typename T>
  AuthRequestHandle
  rejectInvalidInput(std::function<void(AuthResult<T>)> callback,
                     QString diagnostic);

  AuthRequestHandle issueTokenRequest(const ServerProfile &profile,
                                      QStringView path, const QJsonObject &body,
                                      AuthTokenCallback callback);

  std::unique_ptr<QNetworkAccessManager> m_ownedNam; // null when borrowing
  QNetworkAccessManager &m_nam;
  std::chrono::milliseconds m_timeout;
  quint64 m_nextHandle{1};
  QHash<quint64, Pending<AuthToken>> m_pendingTokenRequests;
  QHash<quint64, Pending<CurrentUser>> m_pendingUserRequests;
};

} // namespace Arkham
