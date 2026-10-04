#pragma once

#include "ServerProfile.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QUrl>

#include <chrono>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Arkham {

// Fetches the public /site-settings endpoint and emits a validated assetHost
// override when the server provides one. Missing, empty, malformed, insecure,
// or failed responses intentionally produce only finished(), leaving callers on
// AssetLocator::defaultAssetBaseUrl() or whatever fallback they configured.
class SiteSettingsAssetHostFetcher final : public QObject {
  Q_OBJECT

public:
  static constexpr std::chrono::seconds kDefaultTimeout{30};

  explicit SiteSettingsAssetHostFetcher(
      QNetworkAccessManager &nam,
      std::chrono::milliseconds timeout = kDefaultTimeout,
      QObject *parent = nullptr);
  ~SiteSettingsAssetHostFetcher() override;

  void fetch(const ServerProfile &profile);

signals:
  void assetHostAvailable(const QUrl &assetHost);
  void finished();

private:
  struct PendingReply {
    QTimer *timer{nullptr};
    QByteArray body;
  };

  void handleReply(QNetworkReply *reply);
  void handleMetadataChanged(QNetworkReply *reply);
  bool readAvailable(QNetworkReply *reply);
  void completeReply(QNetworkReply *reply, std::optional<QUrl> assetHost,
                     bool abortReply);
  void emitFinishedQueued();

  QNetworkAccessManager &m_nam;
  std::chrono::milliseconds m_timeout;
  QHash<QNetworkReply *, PendingReply> m_pendingReplies;
};

} // namespace Arkham
