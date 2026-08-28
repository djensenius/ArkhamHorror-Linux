#include "NetworkAuthenticationClient.h"

#include "AuthTransportSecurity.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <stdexcept>
#include <utility>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// Classifies a finished QNetworkReply into a typed, secret-free AuthResult.
// |decode| parses the 2xx JSON body into T; its ValueOrError<T>::error()
// only ever names a missing/mismatched field (see AuthModels.cpp), never the
// offending value, so it is safe to surface here.
template <typename T, typename Decode>
AuthResult<T> classifyReply(QNetworkReply *reply, Decode decode) {
  const QVariant statusAttr =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);

  if (!statusAttr.isValid()) {
    if (reply->error() != QNetworkReply::NoError) {
      return AuthResult<T>{AuthOutcome::Transport,
                           QStringLiteral("network transport error"), 0,
                           std::nullopt};
    }
    return AuthResult<T>{AuthOutcome::NonHttpResponse,
                         QStringLiteral("no HTTP response was received"), 0,
                         std::nullopt};
  }

  const int status = statusAttr.toInt();

  if (status == 401) {
    return AuthResult<T>{
        AuthOutcome::Unauthorized,
        QStringLiteral("server rejected the request as unauthorized"), status,
        std::nullopt};
  }

  if (status >= 300 && status < 400) {
    // Never auto-followed (ManualRedirectPolicy); every 3xx is an explicit
    // failure so a password body or Authorization header can never be
    // replayed to another origin.
    return AuthResult<T>{
        AuthOutcome::UnexpectedStatus,
        QStringLiteral(
            "server responded with a redirect; redirects are never followed"),
        status, std::nullopt};
  }

  if (status < 200 || status >= 300) {
    return AuthResult<T>{
        AuthOutcome::UnexpectedStatus,
        QStringLiteral("server responded with an unexpected HTTP status"),
        status, std::nullopt};
  }

  // 2xx with a transport-level error: the body may be incomplete, so do not
  // attempt to decode it.
  if (reply->error() != QNetworkReply::NoError) {
    return AuthResult<T>{AuthOutcome::Transport,
                         QStringLiteral("network transport error"), status,
                         std::nullopt};
  }

  const QByteArray body = reply->readAll();
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    return AuthResult<T>{AuthOutcome::MalformedPayload,
                         QStringLiteral("response body is not a valid JSON "
                                        "object"),
                         status, std::nullopt};
  }

  auto decoded = decode(doc.object());
  if (!decoded.has_value()) {
    return AuthResult<T>{AuthOutcome::MalformedPayload, decoded.error(), status,
                         std::nullopt};
  }

  return AuthResult<T>{AuthOutcome::Success, QString{}, status, *decoded};
}

// Returns the request attributes/headers common to every authentication
// request: no cookie load/save, no cached-authorization reuse, a manual
// redirect policy so no 3xx is ever auto-followed, and cache bypass on
// both read and write so a cached response/credential is never reused or
// persisted to disk.
void applyCommonRequestSettings(QNetworkRequest &request) {
  request.setRawHeader("Accept", "application/json");
  request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                       QNetworkRequest::AlwaysNetwork);
  request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
}

// A bearer token is placed verbatim into a raw "Authorization: Token <...>"
// header value. HTTP header field-values must not contain CR/LF (doing so
// would allow header/request splitting or injection of extra headers) or
// other control characters, so any such token must be rejected before a
// request is ever constructed -- never sanitized/stripped, since that would
// silently send a different token than the caller supplied.
bool containsHeaderUnsafeCharacter(const QString &token) {
  for (const QChar ch : token) {
    if (ch.category() == QChar::Other_Control) {
      return true;
    }
  }
  return false;
}

} // namespace

NetworkAuthenticationClient::NetworkAuthenticationClient(
    std::chrono::milliseconds timeout, QObject *parent)
    : NetworkAuthenticationClient(std::make_unique<QNetworkAccessManager>(),
                                  nullptr, timeout, parent) {}

NetworkAuthenticationClient::NetworkAuthenticationClient(
    QNetworkAccessManager &nam, std::chrono::milliseconds timeout,
    QObject *parent)
    : NetworkAuthenticationClient(nullptr, &nam, timeout, parent) {}

NetworkAuthenticationClient::NetworkAuthenticationClient(
    std::unique_ptr<QNetworkAccessManager> ownedNam, QNetworkAccessManager *nam,
    std::chrono::milliseconds timeout, QObject *parent)
    : QObject(parent), m_ownedNam(std::move(ownedNam)),
      m_nam(m_ownedNam ? *m_ownedNam : *nam), m_timeout(timeout) {
  if (timeout < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument(
        "authentication client timeout cannot be negative");
  }
}

NetworkAuthenticationClient::~NetworkAuthenticationClient() {
  // Abort and disconnect every outstanding request without invoking its
  // callback: destruction must never deliver a stale completion.
  for (auto it = m_pendingTokenRequests.begin();
       it != m_pendingTokenRequests.end(); ++it) {
    if (QTimer *timer = it.value().timer) {
      timer->stop();
    }
    QNetworkReply *reply = it.value().reply;
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }
  m_pendingTokenRequests.clear();

  for (auto it = m_pendingUserRequests.begin();
       it != m_pendingUserRequests.end(); ++it) {
    if (QTimer *timer = it.value().timer) {
      timer->stop();
    }
    QNetworkReply *reply = it.value().reply;
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }
  m_pendingUserRequests.clear();
}

template <typename T>
void NetworkAuthenticationClient::emitAsync(
    std::function<void(AuthResult<T>)> callback, AuthResult<T> result) {
  QPointer<NetworkAuthenticationClient> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, callback = std::move(callback),
       result = std::move(result)]() mutable {
        if (self) {
          std::move(callback)(std::move(result));
        }
      },
      Qt::QueuedConnection);
}

template <typename T>
AuthRequestHandle NetworkAuthenticationClient::rejectInvalidInput(
    std::function<void(AuthResult<T>)> callback, QString diagnostic) {
  emitAsync<T>(std::move(callback),
               AuthResult<T>{AuthOutcome::InvalidInput, std::move(diagnostic),
                             0, std::nullopt});
  return AuthRequestHandle{};
}

AuthRequestHandle NetworkAuthenticationClient::issueTokenRequest(
    const ServerProfile &profile, QStringView path, const QJsonObject &body,
    AuthTokenCallback callback) {
  // Reject not only an invalid profile but also one lacking validated
  // provenance (i.e. not produced by ServerProfile::hostedDefault(),
  // custom(), or customWithId(), each of which validates its URL against
  // UrlValidator::validateCustomUrl() before returning). Ordinary calling
  // code cannot construct such a profile at all -- ServerProfile no longer
  // exposes a raw-QUrl constructor -- so this is defense-in-depth against
  // any future internal code path that might otherwise bypass validation.
  if (!profile.isValid() || !profile.hasValidatedProvenance()) {
    return rejectInvalidInput<AuthToken>(
        std::move(callback),
        QStringLiteral("server profile is invalid; cannot issue request"));
  }

  const QUrl url = profile.apiUrl(path);
  if (!isSecureOrLoopbackAuthTransport(url)) {
    return rejectInvalidInput<AuthToken>(
        std::move(callback),
        QStringLiteral("cleartext HTTP is only permitted to a loopback "
                       "host; use HTTPS for any other server"));
  }

  QNetworkRequest request(url);
  applyCommonRequestSettings(request);
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/json"));

  const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
  QNetworkReply *reply = m_nam.post(request, payload);

  const quint64 handle = m_nextHandle++;
  QTimer *timer = nullptr;
  if (m_timeout.count() > 0) {
    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, handle, reply, timer]() {
      auto it = m_pendingTokenRequests.find(handle);
      if (it == m_pendingTokenRequests.end()) {
        return; // already handled before the timer fired
      }
      auto cb = std::move(it.value().callback);
      QObject::disconnect(reply, nullptr, this, nullptr);
      m_pendingTokenRequests.erase(it);
      timer->deleteLater();
      reply->abort();
      reply->deleteLater();
      emitAsync<AuthToken>(
          std::move(cb),
          AuthResult<AuthToken>{AuthOutcome::Transport,
                                QStringLiteral("request timed out"), 0,
                                std::nullopt});
    });
  }

  m_pendingTokenRequests.insert(
      handle, Pending<AuthToken>{reply, timer, std::move(callback)});

  connect(reply, &QNetworkReply::finished, this, [this, handle, reply]() {
    auto it = m_pendingTokenRequests.find(handle);
    if (it == m_pendingTokenRequests.end()) {
      return; // already handled (e.g. by cancel() or the timeout timer)
    }
    if (QTimer *t = it.value().timer) {
      t->stop();
      t->deleteLater();
    }
    auto cb = std::move(it.value().callback);
    m_pendingTokenRequests.erase(it);
    reply->deleteLater();
    AuthResult<AuthToken> result =
        classifyReply<AuthToken>(reply, &AuthToken::fromJson);
    emitAsync<AuthToken>(std::move(cb), std::move(result));
  });

  if (timer) {
    timer->start(m_timeout);
  }

  return AuthRequestHandle{handle};
}

AuthRequestHandle
NetworkAuthenticationClient::authenticate(const ServerProfile &profile,
                                          const AuthenticateRequest &request,
                                          AuthTokenCallback callback) {
  return issueTokenRequest(profile, u"authenticate", request.toJson(),
                           std::move(callback));
}

AuthRequestHandle
NetworkAuthenticationClient::registerAccount(const ServerProfile &profile,
                                             const RegisterRequest &request,
                                             AuthTokenCallback callback) {
  return issueTokenRequest(profile, u"register", request.toJson(),
                           std::move(callback));
}

AuthRequestHandle
NetworkAuthenticationClient::whoAmI(const ServerProfile &profile,
                                    const QString &token,
                                    CurrentUserCallback callback) {
  // See the matching comment in issueTokenRequest(): reject any profile
  // lacking validated provenance as well as any profile that is merely
  // structurally invalid.
  if (!profile.isValid() || !profile.hasValidatedProvenance()) {
    return rejectInvalidInput<CurrentUser>(
        std::move(callback),
        QStringLiteral("server profile is invalid; cannot issue request"));
  }
  if (token.trimmed().isEmpty()) {
    return rejectInvalidInput<CurrentUser>(
        std::move(callback),
        QStringLiteral("token must not be empty or whitespace-only"));
  }
  if (containsHeaderUnsafeCharacter(token)) {
    return rejectInvalidInput<CurrentUser>(
        std::move(callback),
        QStringLiteral("token contains control characters and cannot be "
                       "used in an HTTP header"));
  }

  const QUrl url = profile.apiUrl(u"whoami");
  if (!isSecureOrLoopbackAuthTransport(url)) {
    return rejectInvalidInput<CurrentUser>(
        std::move(callback),
        QStringLiteral("cleartext HTTP is only permitted to a loopback "
                       "host; use HTTPS for any other server"));
  }

  QNetworkRequest request(url);
  applyCommonRequestSettings(request);
  request.setRawHeader("Authorization", ("Token " + token).toUtf8());

  QNetworkReply *reply = m_nam.get(request);

  const quint64 handle = m_nextHandle++;
  QTimer *timer = nullptr;
  if (m_timeout.count() > 0) {
    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, handle, reply, timer]() {
      auto it = m_pendingUserRequests.find(handle);
      if (it == m_pendingUserRequests.end()) {
        return;
      }
      auto cb = std::move(it.value().callback);
      QObject::disconnect(reply, nullptr, this, nullptr);
      m_pendingUserRequests.erase(it);
      timer->deleteLater();
      reply->abort();
      reply->deleteLater();
      emitAsync<CurrentUser>(
          std::move(cb),
          AuthResult<CurrentUser>{AuthOutcome::Transport,
                                  QStringLiteral("request timed out"), 0,
                                  std::nullopt});
    });
  }

  m_pendingUserRequests.insert(
      handle, Pending<CurrentUser>{reply, timer, std::move(callback)});

  connect(reply, &QNetworkReply::finished, this, [this, handle, reply]() {
    auto it = m_pendingUserRequests.find(handle);
    if (it == m_pendingUserRequests.end()) {
      return;
    }
    if (QTimer *t = it.value().timer) {
      t->stop();
      t->deleteLater();
    }
    auto cb = std::move(it.value().callback);
    m_pendingUserRequests.erase(it);
    reply->deleteLater();
    AuthResult<CurrentUser> result =
        classifyReply<CurrentUser>(reply, &CurrentUser::fromJson);
    emitAsync<CurrentUser>(std::move(cb), std::move(result));
  });

  if (timer) {
    timer->start(m_timeout);
  }

  return AuthRequestHandle{handle};
}

void NetworkAuthenticationClient::cancel(AuthRequestHandle handle) {
  if (handle.id == 0) {
    return; // stale/default handle: safe no-op
  }

  if (auto it = m_pendingTokenRequests.find(handle.id);
      it != m_pendingTokenRequests.end()) {
    if (QTimer *timer = it.value().timer) {
      timer->stop();
      timer->deleteLater();
    }
    QNetworkReply *reply = it.value().reply;
    auto cb = std::move(it.value().callback);
    QObject::disconnect(reply, nullptr, this, nullptr);
    m_pendingTokenRequests.erase(it);
    reply->abort();
    reply->deleteLater();
    emitAsync<AuthToken>(
        std::move(cb),
        AuthResult<AuthToken>{AuthOutcome::Cancelled,
                              QStringLiteral("request was cancelled"), 0,
                              std::nullopt});
    return;
  }

  if (auto it = m_pendingUserRequests.find(handle.id);
      it != m_pendingUserRequests.end()) {
    if (QTimer *timer = it.value().timer) {
      timer->stop();
      timer->deleteLater();
    }
    QNetworkReply *reply = it.value().reply;
    auto cb = std::move(it.value().callback);
    QObject::disconnect(reply, nullptr, this, nullptr);
    m_pendingUserRequests.erase(it);
    reply->abort();
    reply->deleteLater();
    emitAsync<CurrentUser>(
        std::move(cb),
        AuthResult<CurrentUser>{AuthOutcome::Cancelled,
                                QStringLiteral("request was cancelled"), 0,
                                std::nullopt});
    return;
  }
  // Stale or already-completed handle: safe no-op.
}

} // namespace Arkham
