#pragma once

#include "AssetTypes.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QUrl>

#include <chrono>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Arkham {

struct AssetFetchLimits {
  qint64 maxResponseBytes{16 * 1024 * 1024};
  int maxRedirects{5};
};

struct AssetFetchResult {
  QByteArray bytes;
  QUrl finalUrl;
  QString contentType;
  int httpStatus{0};
};

class AssetNetworkFetcher final : public QObject {
public:
  using FetchCallback = std::function<void(AssetOutcome<AssetFetchResult>)>;

  static constexpr std::chrono::seconds kDefaultTimeout{30};

  explicit AssetNetworkFetcher(
      QNetworkAccessManager &networkAccessManager, AssetFetchLimits limits = {},
      std::chrono::milliseconds timeout = kDefaultTimeout,
      QObject *parent = nullptr);
  ~AssetNetworkFetcher() override;

  void fetch(const QUrl &url, FetchCallback callback);

private:
  struct PendingRequest {
    FetchCallback callback;
    QByteArray bytes;
    QTimer *timer{nullptr};
    int redirectCount{0};
  };

  void startRequest(const QUrl &url, int redirectCount, FetchCallback callback);
  void handleReadyRead(QNetworkReply *reply);
  void handleFinished(QNetworkReply *reply);
  void checkContentLength(QNetworkReply *reply);
  void failReply(QNetworkReply *reply, AssetError error, bool abortReply);
  void deliver(FetchCallback callback, AssetOutcome<AssetFetchResult> result);

  QNetworkAccessManager &m_networkAccessManager;
  AssetFetchLimits m_limits;
  std::chrono::milliseconds m_timeout;
  QHash<QNetworkReply *, PendingRequest> m_pendingRequests;
};

[[nodiscard]] AssetDecodeOutcome<QImage>
decodeFetchedAssetImage(const AssetFetchResult &fetchResult,
                        const AssetDecodeLimits &limits = AssetDecodeLimits{});

} // namespace Arkham
