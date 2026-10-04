#include "SiteSettingsAssetHostFetcher.h"

#include "AssetLocator.h"
#include "SiteSettings.h"

#include <QByteArray>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <optional>
#include <stdexcept>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

constexpr qsizetype kMaxSiteSettingsResponseBytes = 64 * 1024;

std::optional<QUrl> decodeAssetHost(QByteArrayView body) {
  const std::optional<QString> assetHost = decodeSiteSettingsAssetHost(body);
  if (!assetHost.has_value()) {
    return std::nullopt;
  }

  auto url = AssetLocator::assetBaseUrlFromString(*assetHost);
  if (!url) {
    return std::nullopt;
  }
  return *url;
}

bool contentLengthExceedsCap(QNetworkReply *reply) {
  const QVariant header = reply->header(QNetworkRequest::ContentLengthHeader);
  if (!header.isValid()) {
    return false;
  }

  bool ok = false;
  const qlonglong contentLength = header.toLongLong(&ok);
  return ok && contentLength > kMaxSiteSettingsResponseBytes;
}

} // namespace

SiteSettingsAssetHostFetcher::SiteSettingsAssetHostFetcher(
    QNetworkAccessManager &nam, std::chrono::milliseconds timeout,
    QObject *parent)
    : QObject(parent), m_nam(nam), m_timeout(timeout) {
  if (timeout < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument(
        "site settings asset host timeout cannot be negative");
  }
}

SiteSettingsAssetHostFetcher::~SiteSettingsAssetHostFetcher() {
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

void SiteSettingsAssetHostFetcher::fetch(const ServerProfile &profile) {
  if (!profile.isValid()) {
    emitFinishedQueued();
    return;
  }

  QNetworkRequest request(profile.apiUrl(u"site-settings"));
  request.setRawHeader("Accept", "application/json");
  request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::SameOriginRedirectPolicy);

  QNetworkReply *reply = m_nam.get(request);
  reply->setReadBufferSize(kMaxSiteSettingsResponseBytes + 1);

  QTimer *timer = nullptr;
  if (m_timeout.count() > 0) {
    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this,
            [this, reply]() { completeReply(reply, std::nullopt, true); });
  }
  m_pendingReplies.insert(reply, PendingReply{timer, QByteArray{}});

  connect(reply, &QNetworkReply::metaDataChanged, this,
          [this, reply]() { handleMetadataChanged(reply); });
  connect(reply, &QNetworkReply::readyRead, this,
          [this, reply]() { readAvailable(reply); });
  connect(reply, &QNetworkReply::finished, this,
          [this, reply]() { handleReply(reply); });

  handleMetadataChanged(reply);

  if (timer) {
    timer->start(m_timeout);
  }
}

void SiteSettingsAssetHostFetcher::handleMetadataChanged(QNetworkReply *reply) {
  if (!m_pendingReplies.contains(reply)) {
    return;
  }
  if (contentLengthExceedsCap(reply)) {
    completeReply(reply, std::nullopt, true);
  }
}

bool SiteSettingsAssetHostFetcher::readAvailable(QNetworkReply *reply) {
  while (reply->bytesAvailable() > 0) {
    auto it = m_pendingReplies.find(reply);
    if (it == m_pendingReplies.end()) {
      return false;
    }

    const qsizetype remaining =
        kMaxSiteSettingsResponseBytes - it.value().body.size();
    const QByteArray chunk = reply->read(static_cast<qint64>(remaining) + 1);
    if (chunk.isEmpty()) {
      return true;
    }
    if (chunk.size() > remaining) {
      completeReply(reply, std::nullopt, true);
      return false;
    }
    it.value().body += chunk;
  }
  return true;
}

void SiteSettingsAssetHostFetcher::handleReply(QNetworkReply *reply) {
  handleMetadataChanged(reply);
  if (!m_pendingReplies.contains(reply) || !readAvailable(reply)) {
    return;
  }

  const QVariant statusAttr =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
  if (!statusAttr.isValid()) {
    completeReply(reply, std::nullopt, false);
    return;
  }

  const int status = statusAttr.toInt();
  if (status < 200 || status >= 300) {
    completeReply(reply, std::nullopt, false);
    return;
  }

  if (reply->error() != QNetworkReply::NoError) {
    completeReply(reply, std::nullopt, false);
    return;
  }

  const auto it = m_pendingReplies.constFind(reply);
  if (it == m_pendingReplies.constEnd()) {
    return;
  }
  completeReply(reply, decodeAssetHost(it.value().body), false);
}

void SiteSettingsAssetHostFetcher::completeReply(QNetworkReply *reply,
                                                 std::optional<QUrl> assetHost,
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

  if (assetHost.has_value()) {
    emit assetHostAvailable(*assetHost);
  }
  emit finished();
}

void SiteSettingsAssetHostFetcher::emitFinishedQueued() {
  QPointer<SiteSettingsAssetHostFetcher> self(this);
  QMetaObject::invokeMethod(
      this,
      [self]() {
        if (self) {
          emit self->finished();
        }
      },
      Qt::QueuedConnection);
}

} // namespace Arkham
