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
    if (QTimer *timer = it.value()) {
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

  QTimer *timer = nullptr;
  if (m_timeout.count() > 0) {
    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, reply, timer]() {
      if (!m_pendingReplies.contains(reply)) {
        return;
      }
      QObject::disconnect(reply, nullptr, this, nullptr);
      m_pendingReplies.remove(reply);
      timer->deleteLater();
      reply->abort();
      reply->deleteLater();
      emitFinishedQueued();
    });
  }
  m_pendingReplies.insert(reply, timer);

  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    if (QTimer *timer = m_pendingReplies.value(reply)) {
      timer->stop();
      timer->deleteLater();
    }
    m_pendingReplies.remove(reply);
    reply->deleteLater();
    handleReply(reply);
  });

  if (timer) {
    timer->start(m_timeout);
  }
}

void SiteSettingsAssetHostFetcher::handleReply(QNetworkReply *reply) {
  const QVariant statusAttr =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
  if (!statusAttr.isValid()) {
    emit finished();
    return;
  }

  const int status = statusAttr.toInt();
  if (status < 200 || status >= 300) {
    emit finished();
    return;
  }

  if (reply->error() != QNetworkReply::NoError) {
    emit finished();
    return;
  }

  const QByteArray body = reply->readAll();
  const std::optional<QUrl> assetHost = decodeAssetHost(body);
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
