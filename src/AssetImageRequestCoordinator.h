#pragma once

#include "AssetCache.h"
#include "AssetLocator.h"
#include "AssetNetworkFetcher.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QUrl>

#include <chrono>
#include <functional>
#include <memory>

class QNetworkAccessManager;

namespace Arkham {

// Thread-affine coordinator for card image requests. Callers may keep this on
// a dedicated worker thread so AssetCache disk/decode work and QNetworkAccess-
// Manager activity never run in the QML/GUI thread. Identical in-flight URLs
// are coalesced; cancelled waiters are detached so late fetches cannot complete
// dropped QML image responses; 404s are remembered briefly to avoid refetching
// known-missing art while still allowing newly-published assets to appear.
class AssetImageRequestCoordinator final : public QObject {
public:
  using ImageCallback = std::function<void(AssetOutcome<QImage>)>;

  struct Config {
    QUrl assetBaseUrl{AssetLocator::defaultAssetBaseUrl()};
    AssetCache::Config cacheConfig{};
    AssetFetchLimits fetchLimits{};
    std::chrono::milliseconds fetchTimeout{
        AssetNetworkFetcher::kDefaultTimeout};
    std::chrono::milliseconds negativeCacheTtl{std::chrono::seconds(30)};
  };

  explicit AssetImageRequestCoordinator(QObject *parent = nullptr);
  explicit AssetImageRequestCoordinator(Config config,
                                        QObject *parent = nullptr);
  ~AssetImageRequestCoordinator() override;

  [[nodiscard]] quint64 requestCardImage(const AssetLocator::CardImageKey &key,
                                         ImageCallback callback);
  void requestCardImage(quint64 requestId,
                        const AssetLocator::CardImageKey &key,
                        ImageCallback callback);
  void cancel(quint64 requestId);
  void cancelAll();

private:
  struct Waiter {
    quint64 requestId{0};
    ImageCallback callback;
  };

  struct Flight {
    QUrl url;
    AssetNetworkFetcher::RequestId fetchRequestId{0};
    QList<Waiter> waiters;
  };

  [[nodiscard]] static QString normalizedRequestKey(const QUrl &url);
  [[nodiscard]] quint64 nextRequestId();
  void ensureInitialized();
  void requestUrl(const QUrl &url, quint64 requestId, ImageCallback callback);
  void completeOne(ImageCallback callback, AssetOutcome<QImage> result);
  void completeWaiters(QList<Waiter> waiters, AssetOutcome<QImage> result);
  void handleFetchFinished(const QString &requestKey,
                           AssetOutcome<AssetFetchResult> result);
  [[nodiscard]] bool isNegativeCached(const QString &requestKey);
  void rememberNotFound(const QString &requestKey);

  Config m_config;
  std::unique_ptr<QNetworkAccessManager> m_networkAccessManager;
  std::unique_ptr<AssetNetworkFetcher> m_fetcher;
  std::unique_ptr<AssetCache> m_cache;
  QHash<QString, Flight> m_flights;
  QHash<quint64, QString> m_requestKeys;
  QHash<QString, std::chrono::steady_clock::time_point> m_negativeCache;
  quint64 m_nextRequestId{1};
};

} // namespace Arkham
