#include "AssetNetworkFetcher.h"

#include "AuthTransportSecurity.h"

#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QVariant>

#include <limits>
#include <stdexcept>
#include <utility>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

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

bool redirectTargetAllowed(const QUrl &from, const QUrl &to) {
  if (sameOrigin(from, to)) {
    return true;
  }
  if (to.scheme() == "https"_L1) {
    return true;
  }
  return to.scheme() == "http"_L1 && isSecureOrLoopbackAuthTransport(to);
}

bool isRedirectStatus(int status) { return status >= 300 && status < 400; }

void applyAssetRequestPolicy(QNetworkRequest &request) {
  request.setRawHeader("Accept",
                       "image/avif,image/jpeg,image/png,image/*;q=0.8");
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
  startRequest(url, 0, std::move(callback));
}

void AssetNetworkFetcher::startRequest(const QUrl &url, int redirectCount,
                                       FetchCallback callback) {
  QNetworkRequest request(url);
  applyAssetRequestPolicy(request);

  QNetworkReply *reply = m_networkAccessManager.get(request);
  auto *timer = new QTimer(this);
  timer->setSingleShot(true);

  m_pendingRequests.insert(
      reply,
      PendingRequest{std::move(callback), QByteArray{}, timer, redirectCount});

  connect(timer, &QTimer::timeout, this, [this, reply]() {
    failReply(reply,
              AssetError{AssetErrorCode::Timeout,
                         QStringLiteral("asset fetch timed out"), 0,
                         reply ? reply->url() : QUrl{}},
              true);
  });
  connect(reply, &QNetworkReply::metaDataChanged, this,
          [this, reply]() { checkContentLength(reply); });
  connect(reply, &QIODevice::readyRead, this,
          [this, reply]() { handleReadyRead(reply); });
  connect(reply, &QNetworkReply::finished, this,
          [this, reply]() { handleFinished(reply); });

  timer->start(m_timeout);
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

  if (isRedirectStatus(status)) {
    if (pending.redirectCount >= m_limits.maxRedirects) {
      deliver(std::move(pending.callback),
              AssetError{AssetErrorCode::TooManyRedirects,
                         QStringLiteral("asset redirect limit exceeded"),
                         status, reply->url()});
      return;
    }
    const QVariant redirectAttribute =
        reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    const QUrl redirectUrl = reply->url().resolved(redirectAttribute.toUrl());
    const auto validation = validateFetchUrl(redirectUrl);
    if (!validation || !redirectTargetAllowed(reply->url(), redirectUrl)) {
      deliver(
          std::move(pending.callback),
          AssetError{AssetErrorCode::RedirectRejected,
                     QStringLiteral("asset redirect target is not permitted"),
                     status, redirectUrl});
      return;
    }
    startRequest(redirectUrl, pending.redirectCount + 1,
                 std::move(pending.callback));
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
