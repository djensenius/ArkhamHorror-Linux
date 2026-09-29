#pragma once

#include "AssetTypes.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
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
  using RequestId = quint64;

  static constexpr std::chrono::seconds kDefaultTimeout{30};

  explicit AssetNetworkFetcher(
      QNetworkAccessManager &networkAccessManager, AssetFetchLimits limits = {},
      std::chrono::milliseconds timeout = kDefaultTimeout,
      QObject *parent = nullptr);
  ~AssetNetworkFetcher() override;

  RequestId fetch(const QUrl &url, FetchCallback callback);
  void cancel(RequestId requestId);

private:
  struct PendingRequest {
    RequestId requestId{0};
    FetchCallback callback;
    QByteArray bytes;
    QTimer *timer{nullptr};
    int redirectCount{0};
    QUrl originalUrl;
    std::chrono::steady_clock::time_point deadline;
  };

  void startRequest(RequestId requestId, const QUrl &url,
                    const QUrl &originalUrl, int redirectCount,
                    std::chrono::steady_clock::time_point deadline,
                    FetchCallback callback);
  void handleReadyRead(QNetworkReply *reply);
  void handleFinished(QNetworkReply *reply);
  void checkContentLength(QNetworkReply *reply);
  void failReply(QNetworkReply *reply, AssetError error, bool abortReply);
  void handleReplyDestroyed(QNetworkReply *reply);
  void handleNetworkManagerDestroyed();
  void deliver(FetchCallback callback, AssetOutcome<AssetFetchResult> result);

  QPointer<QNetworkAccessManager> m_networkAccessManager;
  AssetFetchLimits m_limits;
  std::chrono::milliseconds m_timeout;
  QHash<QNetworkReply *, PendingRequest> m_pendingRequests;
  QHash<RequestId, QNetworkReply *> m_repliesByRequestId;
  RequestId m_nextRequestId{1};
};

[[nodiscard]] AssetDecodeOutcome<QImage>
decodeFetchedAssetImage(const AssetFetchResult &fetchResult,
                        const AssetDecodeLimits &limits = AssetDecodeLimits{});

} // namespace Arkham
