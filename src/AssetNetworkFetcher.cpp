#include "AssetNetworkFetcher.h"

#include "AuthTransportSecurity.h"

#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

constexpr qint64 kReadBufferCapBytes = 64 * 1024;

bool parseContentLength(const QByteArray &header, qint64 *value) {
  if (header.isEmpty()) {
    return false;
  }
  qint64 parsed = 0;
  for (const char c : header.trimmed()) {
    if (c < '0' || c > '9') {
      return false;
    }
    const int digit = c - '0';
    if (parsed > (std::numeric_limits<qint64>::max() - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  *value = parsed;
  return true;
}

QString normalizedContentType(const QByteArray &rawHeader) {
  QString value = QString::fromLatin1(rawHeader);
  const qsizetype semi = value.indexOf(u';');
  if (semi >= 0) {
    value = value.left(semi);
  }
  return value.trimmed().toLower();
}

bool isAcceptedContentType(const QString &contentType) {
  // image/svg+xml is intentionally accepted by this coarse HTTP sanity check
  // because it is an image/* media type. The separable decodeAssetImage()
  // helper still rejects SVG bytes as UnrecognizedFormat because only PNG,
  // JPEG, and AVIF magic bytes are supported by the native decoder.
  return contentType.startsWith("image/"_L1) ||
         contentType == "application/octet-stream"_L1;
}

int defaultPortForScheme(const QString &scheme) {
  if (scheme == "https"_L1) {
    return 443;
  }
  if (scheme == "http"_L1) {
    return 80;
  }
  return -1;
}

bool sameOrigin(const QUrl &left, const QUrl &right) {
  if (left.scheme() != right.scheme() || left.host() != right.host()) {
    return false;
  }
  return left.port(defaultPortForScheme(left.scheme())) ==
         right.port(defaultPortForScheme(right.scheme()));
}

AssetOutcome<bool> validateFetchUrl(const QUrl &url) {
  if (!url.isValid() || url.host().isEmpty()) {
    return AssetError{
        AssetErrorCode::InvalidFetchUrl,
        QStringLiteral("asset URL must be absolute and include a host"), 0,
        url};
  }
  if (url.scheme() != "https"_L1 && url.scheme() != "http"_L1) {
    return AssetError{AssetErrorCode::UnsupportedScheme,
                      QStringLiteral("asset URL must use https, except http "
                                     "loopback for local development"),
                      0, url};
  }
  if (!url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
    return AssetError{
        AssetErrorCode::InvalidFetchUrl,
        QStringLiteral(
            "asset URL must not contain credentials, query, or fragment"),
        0, url};
  }
  if (!isSecureOrLoopbackAuthTransport(url)) {
    return AssetError{
        AssetErrorCode::InsecureTransport,
        QStringLiteral("asset URL requires https unless the host is loopback"),
        0, url};
  }
  return true;
}

bool redirectTargetAllowed(const QUrl &original, const QUrl &from,
                           const QUrl &to) {
  if (sameOrigin(from, to)) {
    return true;
  }
  if (to.scheme() == "https"_L1) {
    return true;
  }
  return to.scheme() == "http"_L1 && isCanonicalLoopbackHostText(to.host()) &&
         isCanonicalLoopbackHostText(original.host());
}

bool isRedirectStatus(int status) { return status >= 300 && status < 400; }

std::chrono::milliseconds
remainingTimeout(std::chrono::steady_clock::time_point deadline) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  return std::max(remaining, std::chrono::milliseconds::zero());
}

void applyAssetRequestPolicy(QNetworkRequest &request) {
  request.setRawHeader("Accept",
                       "image/avif,image/jpeg,image/png,image/*;q=0.8");
  // Prefer identity transfer coding so byte limits apply before any avoidable
  // decompression buffer growth. If a server ignores this, the readyRead path
  // still counts the bytes Qt delivers before appending them to our buffer.
  request.setRawHeader("Accept-Encoding", "identity");
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

} // namespace

AssetNetworkFetcher::AssetNetworkFetcher(
    QNetworkAccessManager &networkAccessManager, AssetFetchLimits limits,
    std::chrono::milliseconds timeout, QObject *parent)
    : QObject(parent), m_networkAccessManager(networkAccessManager),
      m_limits(limits), m_timeout(timeout) {
  if (m_limits.maxResponseBytes <= 0) {
    throw std::invalid_argument("asset response byte limit must be positive");
  }
  if (m_limits.maxRedirects < 0) {
    throw std::invalid_argument("asset redirect limit must not be negative");
  }
  if (m_timeout <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("asset fetch timeout must be positive");
  }
}

AssetNetworkFetcher::~AssetNetworkFetcher() {
  for (auto it = m_pendingRequests.begin(); it != m_pendingRequests.end();
       ++it) {
    if (it.value().timer) {
      it.value().timer->stop();
      it.value().timer->deleteLater();
    }
    QNetworkReply *reply = it.key();
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }
  m_pendingRequests.clear();
}

void AssetNetworkFetcher::fetch(const QUrl &url, FetchCallback callback) {
  const auto validation = validateFetchUrl(url);
  if (!validation) {
    deliver(std::move(callback), validation.error());
    return;
  }
  startRequest(url, url, 0, std::chrono::steady_clock::now() + m_timeout,
               std::move(callback));
}

void AssetNetworkFetcher::startRequest(
    const QUrl &url, const QUrl &originalUrl, int redirectCount,
    std::chrono::steady_clock::time_point deadline, FetchCallback callback) {
  const std::chrono::milliseconds remaining = remainingTimeout(deadline);
  if (remaining <= std::chrono::milliseconds::zero()) {
    deliver(std::move(callback),
            AssetError{AssetErrorCode::Timeout,
                       QStringLiteral("asset fetch timed out"), 0, url});
    return;
  }

  QNetworkRequest request(url);
  applyAssetRequestPolicy(request);

  QNetworkReply *reply = m_networkAccessManager.get(request);
  const qint64 plusOne =
      m_limits.maxResponseBytes == std::numeric_limits<qint64>::max()
          ? std::numeric_limits<qint64>::max()
          : m_limits.maxResponseBytes + 1;
  reply->setReadBufferSize(qMin(plusOne, kReadBufferCapBytes));

  auto *timer = new QTimer(this);
  timer->setSingleShot(true);

  m_pendingRequests.insert(
      reply, PendingRequest{std::move(callback), QByteArray{}, timer,
                            redirectCount, originalUrl, deadline});

  QPointer<QNetworkReply> guardedReply(reply);
  connect(timer, &QTimer::timeout, this, [this, guardedReply]() {
    if (!guardedReply) {
      return;
    }
    failReply(guardedReply,
              AssetError{AssetErrorCode::Timeout,
                         QStringLiteral("asset fetch timed out"), 0,
                         guardedReply->url()},
              true);
  });
  connect(reply, &QObject::destroyed, this, [this, reply]() {
    auto it = m_pendingRequests.find(reply);
    if (it == m_pendingRequests.end()) {
      return;
    }
    if (it.value().timer) {
      it.value().timer->stop();
      it.value().timer->deleteLater();
    }
    m_pendingRequests.erase(it);
  });
  connect(reply, &QNetworkReply::metaDataChanged, this, [this, guardedReply]() {
    if (guardedReply) {
      checkContentLength(guardedReply);
    }
  });
  connect(reply, &QIODevice::readyRead, this, [this, guardedReply]() {
    if (guardedReply) {
      handleReadyRead(guardedReply);
    }
  });
  connect(reply, &QNetworkReply::finished, this, [this, guardedReply]() {
    if (guardedReply) {
      handleFinished(guardedReply);
    }
  });

  timer->start(remaining);
}

void AssetNetworkFetcher::checkContentLength(QNetworkReply *reply) {
  const auto it = m_pendingRequests.find(reply);
  if (it == m_pendingRequests.end()) {
    return;
  }
  qint64 contentLength = 0;
  if (parseContentLength(reply->rawHeader("Content-Length"), &contentLength) &&
      contentLength > m_limits.maxResponseBytes) {
    failReply(reply,
              AssetError{AssetErrorCode::ResponseTooLarge,
                         QStringLiteral("asset response declares %1 bytes, "
                                        "exceeding the configured cap of %2")
                             .arg(contentLength)
                             .arg(m_limits.maxResponseBytes),
                         0, reply->url()},
              true);
  }
}

void AssetNetworkFetcher::handleReadyRead(QNetworkReply *reply) {
  auto it = m_pendingRequests.find(reply);
  if (it == m_pendingRequests.end()) {
    return;
  }
  QByteArray chunk = reply->readAll();
  if (it.value().bytes.size() + chunk.size() > m_limits.maxResponseBytes) {
    failReply(
        reply,
        AssetError{AssetErrorCode::ResponseTooLarge,
                   QStringLiteral(
                       "asset response exceeded the configured cap of %1 bytes")
                       .arg(m_limits.maxResponseBytes),
                   0, reply->url()},
        true);
    return;
  }
  it.value().bytes += chunk;
}

void AssetNetworkFetcher::handleFinished(QNetworkReply *reply) {
  handleReadyRead(reply);
  auto it = m_pendingRequests.find(reply);
  if (it == m_pendingRequests.end()) {
    return;
  }

  PendingRequest pending = std::move(it.value());
  m_pendingRequests.erase(it);
  if (pending.timer) {
    pending.timer->stop();
    pending.timer->deleteLater();
  }
  reply->deleteLater();

  const QVariant statusAttribute =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
  if (!statusAttribute.isValid()) {
    deliver(std::move(pending.callback),
            AssetError{AssetErrorCode::NetworkError, reply->errorString(), 0,
                       reply->url()});
    return;
  }
  const int status = statusAttribute.toInt();

  if (status == 304) {
    deliver(
        std::move(pending.callback),
        AssetError{AssetErrorCode::NotModifiedWithoutCache,
                   QStringLiteral(
                       "asset fetch does not support bodyless 304 responses"),
                   status, reply->url()});
    return;
  }

  if (isRedirectStatus(status)) {
    const QVariant redirectAttribute =
        reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    if (!redirectAttribute.isValid() || redirectAttribute.toUrl().isEmpty()) {
      deliver(
          std::move(pending.callback),
          AssetError{AssetErrorCode::MissingRedirectLocation,
                     QStringLiteral("asset redirect response has no Location"),
                     status, reply->url()});
      return;
    }
    if (pending.redirectCount >= m_limits.maxRedirects) {
      deliver(std::move(pending.callback),
              AssetError{AssetErrorCode::TooManyRedirects,
                         QStringLiteral("asset redirect limit exceeded"),
                         status, reply->url()});
      return;
    }
    const QUrl redirectUrl = reply->url().resolved(redirectAttribute.toUrl());
    const auto validation = validateFetchUrl(redirectUrl);
    if (!validation || !redirectTargetAllowed(pending.originalUrl, reply->url(),
                                              redirectUrl)) {
      deliver(
          std::move(pending.callback),
          AssetError{AssetErrorCode::RedirectRejected,
                     QStringLiteral("asset redirect target is not permitted"),
                     status, redirectUrl});
      return;
    }
    startRequest(redirectUrl, pending.originalUrl, pending.redirectCount + 1,
                 pending.deadline, std::move(pending.callback));
    return;
  }

  if (status == 404) {
    deliver(std::move(pending.callback),
            AssetError{AssetErrorCode::NotFound,
                       QStringLiteral("asset was not found"), status,
                       reply->url()});
    return;
  }
  if (status != 200) {
    deliver(std::move(pending.callback),
            AssetError{
                AssetErrorCode::HttpError,
                QStringLiteral("unexpected asset HTTP status %1").arg(status),
                status, reply->url()});
    return;
  }
  if (reply->error() != QNetworkReply::NoError) {
    deliver(std::move(pending.callback),
            AssetError{AssetErrorCode::NetworkError, reply->errorString(),
                       status, reply->url()});
    return;
  }

  const QString contentType =
      normalizedContentType(reply->rawHeader("Content-Type"));
  if (!isAcceptedContentType(contentType)) {
    deliver(std::move(pending.callback),
            AssetError{AssetErrorCode::UnsupportedContentType,
                       QStringLiteral("unsupported asset content type: %1")
                           .arg(contentType),
                       status, reply->url()});
    return;
  }

  deliver(std::move(pending.callback),
          AssetFetchResult{std::move(pending.bytes), reply->url(), contentType,
                           status});
}

void AssetNetworkFetcher::failReply(QNetworkReply *reply, AssetError error,
                                    bool abortReply) {
  auto it = m_pendingRequests.find(reply);
  if (it == m_pendingRequests.end()) {
    return;
  }
  PendingRequest pending = std::move(it.value());
  m_pendingRequests.erase(it);
  if (pending.timer) {
    pending.timer->stop();
    pending.timer->deleteLater();
  }
  QObject::disconnect(reply, nullptr, this, nullptr);
  if (abortReply) {
    reply->abort();
  }
  reply->deleteLater();
  deliver(std::move(pending.callback), std::move(error));
}

void AssetNetworkFetcher::deliver(FetchCallback callback,
                                  AssetOutcome<AssetFetchResult> result) {
  QPointer<AssetNetworkFetcher> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, callback = std::move(callback),
       result = std::move(result)]() mutable {
        if (self && callback) {
          callback(std::move(result));
        }
      },
      Qt::QueuedConnection);
}

AssetDecodeOutcome<QImage>
decodeFetchedAssetImage(const AssetFetchResult &fetchResult,
                        const AssetDecodeLimits &limits) {
  return decodeAssetImage(fetchResult.bytes, limits);
}

} // namespace Arkham
