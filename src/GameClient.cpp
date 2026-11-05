#include "GameClient.h"

#include "AuthTransportSecurity.h"
#include "Games.h"
#include "TokenValidation.h"

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

namespace Arkham {

namespace {

// The same request settings NetworkAuthenticationClient applies to every
// token-bearing request (its applyCommonRequestSettings() is file-local,
// and that file's layout is pinned by the encoder-hygiene check, so it is
// mirrored here rather than moved): no cookies, no cached authorization,
// no redirects followed (a 3xx is a typed HttpError, so the Authorization
// header is never replayed to another origin), and no HTTP cache.
void applyAuthenticatedRequestSettings(QNetworkRequest &request) {
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

bool contentLengthExceedsCap(QNetworkReply *reply, qsizetype cap) {
  const QVariant header = reply->header(QNetworkRequest::ContentLengthHeader);
  if (!header.isValid()) {
    return false;
  }
  bool ok = false;
  const qlonglong contentLength = header.toLongLong(&ok);
  return ok && contentLength > cap;
}

GameRequestStatus transportFailure(QString diagnostic, int httpStatus = 0) {
  return GameRequestStatus{GameRequestOutcome::Transport, std::move(diagnostic),
                           httpStatus, std::nullopt};
}

GameRequestStatus oversizedResponse() {
  return transportFailure(GameClient::oversizedResponseDiagnostic());
}

} // namespace

QString GameClient::oversizedResponseDiagnostic() {
  return QStringLiteral("response body exceeds the size limit");
}

GameClient::GameClient(QNetworkAccessManager &nam,
                       std::chrono::milliseconds timeout, QObject *parent)
    : QObject(parent), m_nam(nam), m_timeout(timeout) {
  if (timeout < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("game client timeout cannot be negative");
  }
}

void GameClient::setMaxResponseBytes(qsizetype maxResponseBytes) {
  if (maxResponseBytes <= 0) {
    throw std::invalid_argument("game client response cap must be positive");
  }
  m_maxResponseBytes = maxResponseBytes;
}

GameClient::~GameClient() {
  for (auto it = m_pendingReplies.begin(); it != m_pendingReplies.end(); ++it) {
    if (QTimer *timer = it.value().timer) {
      timer->stop();
    }
    QNetworkReply *reply = it.key();
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }
  m_pendingReplies.clear();
}

void GameClient::listGames(const ServerProfile &profile, const QString &token,
                           GameListCallback callback) {
  issue(profile, QStringLiteral("arkham/games"), token,
        QStringLiteral("games list not found"),
        [callback = std::move(callback)](const GameRequestStatus &status,
                                         const QByteArray &body) {
          if (status.outcome != GameRequestOutcome::Success) {
            callback(status, nullptr);
            return;
          }
          const auto rows = decodeGameListFromRawBytes(body, u"games");
          if (!rows) {
            callback(GameRequestStatus{GameRequestOutcome::DecodeFailed,
                                       rows.error(), status.httpStatus,
                                       std::nullopt},
                     nullptr);
            return;
          }
          callback(status, &*rows);
        });
}

void GameClient::getGame(const ServerProfile &profile, const QString &gameId,
                         const QString &token, GameCallback callback) {
  const auto id = GameId::parse(gameId);
  if (!id) {
    rejectQueued([callback](const GameRequestStatus &status,
                            const QByteArray &) { callback(status, nullptr); },
                 QStringLiteral("game ID is not a valid UUID"));
    return;
  }
  issue(profile, QStringLiteral("arkham/games/") + id->value(), token,
        QStringLiteral("game not found"),
        [callback = std::move(callback)](const GameRequestStatus &status,
                                         const QByteArray &body) {
          if (status.outcome != GameRequestOutcome::Success) {
            callback(status, nullptr);
            return;
          }
          const auto game = GetGame::fromRawBytes(body, u"getGame");
          if (!game) {
            callback(GameRequestStatus{GameRequestOutcome::DecodeFailed,
                                       game.error(), status.httpStatus,
                                       std::nullopt},
                     nullptr);
            return;
          }
          callback(status, &*game);
        });
}

void GameClient::issue(const ServerProfile &profile, const QString &path,
                       const QString &token, QString notFoundDiagnostic,
                       Completion completion) {
  // Same request-time guards as NetworkAuthenticationClient::whoAmI(): a
  // validated profile, a token that is safe to place in a header, and a
  // secure (or loopback-only cleartext) transport.
  if (!profile.isValid() || !profile.hasValidatedProvenance()) {
    rejectQueued(std::move(completion),
                 QStringLiteral("server profile is invalid; cannot issue "
                                "request"));
    return;
  }
  if (!isValidTokenContent(token)) {
    rejectQueued(std::move(completion),
                 QStringLiteral("token is not a usable token and cannot be "
                                "used in an HTTP header"));
    return;
  }
  const QUrl url = profile.apiUrl(path);
  if (!isSecureOrLoopbackAuthTransport(url)) {
    rejectQueued(std::move(completion),
                 QStringLiteral("cleartext HTTP is only permitted to a "
                                "loopback host; use HTTPS for any other "
                                "server"));
    return;
  }

  QNetworkRequest request(url);
  applyAuthenticatedRequestSettings(request);
  // isValidTokenContent() restricts the token to visible ASCII, so this is
  // exactly one well-formed header with no CR/LF smuggling (see the
  // matching audit comment in NetworkAuthenticationClient::whoAmI()).
  request.setRawHeader("Authorization", ("Token " + token).toUtf8());

  QNetworkReply *reply = m_nam.get(request);
  reply->setReadBufferSize(m_maxResponseBytes + 1);

  QTimer *timer = nullptr;
  if (m_timeout.count() > 0) {
    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, reply]() {
      complete(reply, transportFailure(QStringLiteral("request timed out")),
               true);
    });
  }
  m_pendingReplies.insert(
      reply, PendingReply{timer, QByteArray{}, std::move(notFoundDiagnostic),
                          std::move(completion), m_maxResponseBytes});

  connect(reply, &QNetworkReply::metaDataChanged, this,
          [this, reply]() { handleMetadataChanged(reply); });
  connect(reply, &QNetworkReply::readyRead, this,
          [this, reply]() { readAvailable(reply); });
  connect(reply, &QNetworkReply::finished, this,
          [this, reply]() { handleFinished(reply); });

  if (timer) {
    timer->start(m_timeout);
  }
}

void GameClient::rejectQueued(Completion completion, QString diagnostic) {
  QPointer<GameClient> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, completion = std::move(completion),
       diagnostic = std::move(diagnostic)]() {
        if (self) {
          completion(GameRequestStatus{GameRequestOutcome::InvalidRequest,
                                       diagnostic, 0, std::nullopt},
                     QByteArray{});
        }
      },
      Qt::QueuedConnection);
}

void GameClient::handleMetadataChanged(QNetworkReply *reply) {
  const auto it = m_pendingReplies.constFind(reply);
  if (it != m_pendingReplies.cend() &&
      contentLengthExceedsCap(reply, it.value().maxResponseBytes)) {
    complete(reply, oversizedResponse(), true);
  }
}

bool GameClient::readAvailable(QNetworkReply *reply) {
  while (reply->bytesAvailable() > 0) {
    auto it = m_pendingReplies.find(reply);
    if (it == m_pendingReplies.end()) {
      return false;
    }
    const qsizetype remaining =
        it.value().maxResponseBytes - it.value().body.size();
    const QByteArray chunk = reply->read(static_cast<qint64>(remaining) + 1);
    if (chunk.isEmpty()) {
      return true;
    }
    if (chunk.size() > remaining) {
      complete(reply, oversizedResponse(), true);
      return false;
    }
    it.value().body += chunk;
  }
  return true;
}

void GameClient::handleFinished(QNetworkReply *reply) {
  handleMetadataChanged(reply);
  if (!m_pendingReplies.contains(reply) || !readAvailable(reply)) {
    return;
  }

  const QVariant statusAttr =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
  if (!statusAttr.isValid()) {
    complete(reply, transportFailure(QStringLiteral("network transport error")),
             false);
    return;
  }

  const int status = statusAttr.toInt();
  if (status == 401) {
    complete(reply,
             GameRequestStatus{GameRequestOutcome::Unauthorized,
                               QStringLiteral("server rejected the request "
                                              "as unauthorized"),
                               status, std::nullopt},
             false);
    return;
  }
  if (status == 404) {
    complete(reply,
             GameRequestStatus{GameRequestOutcome::NotFound,
                               m_pendingReplies.value(reply).notFoundDiagnostic,
                               status, std::nullopt},
             false);
    return;
  }
  // Mirrors NetworkCapabilityProbe: only a 503 carrying the server's
  // maintenance body is Maintenance; any other 503 stays HttpError.
  if (status == 503) {
    const QByteArray body = m_pendingReplies.value(reply).body;
    if (auto maintenance = ServerMaintenance::fromResponse(
            status, body, reply->rawHeader("Retry-After"))) {
      const QString notice = maintenance->notice();
      complete(reply,
               GameRequestStatus{GameRequestOutcome::Maintenance, notice,
                                 status, std::move(maintenance)},
               false);
      return;
    }
  }
  if (status < 200 || status >= 300) {
    complete(reply,
             GameRequestStatus{
                 GameRequestOutcome::HttpError,
                 QStringLiteral("unexpected HTTP status %1").arg(status),
                 status, std::nullopt},
             false);
    return;
  }
  // 2xx with a transport-level error: the body may be incomplete.
  if (reply->error() != QNetworkReply::NoError) {
    complete(
        reply,
        transportFailure(QStringLiteral("network transport error"), status),
        false);
    return;
  }
  complete(reply,
           GameRequestStatus{GameRequestOutcome::Success, QString{}, status,
                             std::nullopt},
           false);
}

void GameClient::complete(QNetworkReply *reply, GameRequestStatus status,
                          bool abortReply) {
  auto it = m_pendingReplies.find(reply);
  if (it == m_pendingReplies.end()) {
    return;
  }
  PendingReply pending = std::move(it.value());
  m_pendingReplies.erase(it);

  if (pending.timer) {
    pending.timer->stop();
    pending.timer->deleteLater();
  }
  QObject::disconnect(reply, nullptr, this, nullptr);
  if (abortReply) {
    reply->abort();
  }
  reply->deleteLater();

  // Last statement: the callback may destroy this client, so no member is
  // touched after it runs (the timeout path ends here too).
  if (pending.completion) {
    pending.completion(status, pending.body);
  }
}

} // namespace Arkham
