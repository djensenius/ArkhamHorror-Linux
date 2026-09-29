#include "AssetImageRequestCoordinator.h"

#include <QNetworkAccessManager>
#include <QTimer>

#include <utility>

namespace Arkham {

AssetImageRequestCoordinator::AssetImageRequestCoordinator(QObject *parent)
    : AssetImageRequestCoordinator(Config{}, parent) {}

AssetImageRequestCoordinator::AssetImageRequestCoordinator(Config config,
                                                           QObject *parent)
    : QObject(parent), m_config(std::move(config)) {
  if (!m_config.assetBaseUrl.isValid() ||
      m_config.assetBaseUrl.host().isEmpty()) {
    m_config.assetBaseUrl = AssetLocator::defaultAssetBaseUrl();
  }
}

AssetImageRequestCoordinator::~AssetImageRequestCoordinator() { cancelAll(); }

quint64 AssetImageRequestCoordinator::requestCardImage(
    const AssetLocator::CardImageKey &key, ImageCallback callback) {
  const quint64 requestId = nextRequestId();
  requestCardImage(requestId, key, std::move(callback));
  return requestId;
}

void AssetImageRequestCoordinator::requestCardImage(
    quint64 requestId, const AssetLocator::CardImageKey &key,
    ImageCallback callback) {
  if (!callback) {
    return;
  }

  const auto url = AssetLocator::buildCardImageUrl(m_config.assetBaseUrl, key);
  if (!url) {
    completeOne(std::move(callback), url.error());
    return;
  }

  requestUrl(*url, requestId, std::move(callback));
}

void AssetImageRequestCoordinator::cancel(quint64 requestId) {
  const auto keyIt = m_requestKeys.find(requestId);
  if (keyIt == m_requestKeys.end()) {
    return;
  }

  const QString requestKey = keyIt.value();
  m_requestKeys.erase(keyIt);
  auto flightIt = m_flights.find(requestKey);
  if (flightIt == m_flights.end()) {
    return;
  }

  auto &waiters = flightIt->waiters;
  for (auto waiterIt = waiters.begin(); waiterIt != waiters.end(); ++waiterIt) {
    if (waiterIt->requestId == requestId) {
      waiters.erase(waiterIt);
      break;
    }
  }
  if (waiters.isEmpty()) {
    if (m_fetcher && flightIt->fetchRequestId != 0) {
      m_fetcher->cancel(flightIt->fetchRequestId);
    }
    m_flights.erase(flightIt);
  }
}

void AssetImageRequestCoordinator::cancelAll() {
  if (m_fetcher) {
    for (const Flight &flight : std::as_const(m_flights)) {
      if (flight.fetchRequestId != 0) {
        m_fetcher->cancel(flight.fetchRequestId);
      }
    }
  }
  m_requestKeys.clear();
  m_flights.clear();
}

QString AssetImageRequestCoordinator::normalizedRequestKey(const QUrl &url) {
  return QString::fromLatin1(
      url.adjusted(QUrl::NormalizePathSegments).toEncoded(QUrl::FullyEncoded));
}

quint64 AssetImageRequestCoordinator::nextRequestId() {
  if (m_nextRequestId == 0) {
    m_nextRequestId = 1;
  }
  return m_nextRequestId++;
}

void AssetImageRequestCoordinator::ensureInitialized() {
  if (m_fetcher && m_cache) {
    return;
  }

  m_networkAccessManager = std::make_unique<QNetworkAccessManager>();
  m_fetcher = std::make_unique<AssetNetworkFetcher>(
      *m_networkAccessManager, m_config.fetchLimits, m_config.fetchTimeout);
  m_cache = std::make_unique<AssetCache>(m_config.cacheConfig);
}

void AssetImageRequestCoordinator::requestUrl(const QUrl &url,
                                              quint64 requestId,
                                              ImageCallback callback) {
  ensureInitialized();

  const QString requestKey = normalizedRequestKey(url);
  if (isNegativeCached(requestKey)) {
    completeOne(std::move(callback),
                AssetError{AssetErrorCode::NotFound,
                           QStringLiteral("asset was recently not found"), 404,
                           url});
    return;
  }

  const AssetCache::LookupResult cached = m_cache->lookup(url);
  if (cached.hit()) {
    completeOne(std::move(callback), cached.image);
    return;
  }

  m_requestKeys.insert(requestId, requestKey);
  auto flightIt = m_flights.find(requestKey);
  if (flightIt != m_flights.end()) {
    flightIt->waiters.push_back(Waiter{requestId, std::move(callback)});
    return;
  }

  Flight flight;
  flight.url = url;
  flight.waiters.push_back(Waiter{requestId, std::move(callback)});
  m_flights.insert(requestKey, std::move(flight));

  auto insertedFlightIt = m_flights.find(requestKey);
  insertedFlightIt->fetchRequestId = m_fetcher->fetch(
      url, [this, requestKey](AssetOutcome<AssetFetchResult> result) {
        handleFetchFinished(requestKey, std::move(result));
      });
}

void AssetImageRequestCoordinator::completeOne(ImageCallback callback,
                                               AssetOutcome<QImage> result) {
  if (callback) {
    callback(std::move(result));
  }
}

void AssetImageRequestCoordinator::completeWaiters(
    QList<Waiter> waiters, AssetOutcome<QImage> result) {
  for (Waiter &waiter : waiters) {
    m_requestKeys.remove(waiter.requestId);
    completeOne(std::move(waiter.callback), result);
  }
}

void AssetImageRequestCoordinator::handleFetchFinished(
    const QString &requestKey, AssetOutcome<AssetFetchResult> result) {
  auto flightIt = m_flights.find(requestKey);
  if (flightIt == m_flights.end()) {
    return;
  }

  Flight flight = std::move(flightIt.value());
  m_flights.erase(flightIt);

  if (!result) {
    if (result.error().code == AssetErrorCode::NotFound) {
      rememberNotFound(requestKey);
    }
    completeWaiters(std::move(flight.waiters), result.error());
    return;
  }

  auto stored = m_cache->store(flight.url, result->bytes, result->contentType);
  if (!stored) {
    completeWaiters(std::move(flight.waiters),
                    AssetError{AssetErrorCode::DecodeError,
                               stored.error().message, 0, flight.url});
    return;
  }

  completeWaiters(std::move(flight.waiters), *stored);
}

bool AssetImageRequestCoordinator::isNegativeCached(const QString &requestKey) {
  const auto it = m_negativeCache.find(requestKey);
  if (it == m_negativeCache.end()) {
    return false;
  }
  if (std::chrono::steady_clock::now() < it.value()) {
    return true;
  }
  m_negativeCache.erase(it);
  return false;
}

void AssetImageRequestCoordinator::rememberNotFound(const QString &requestKey) {
  if (m_config.negativeCacheTtl <= std::chrono::milliseconds::zero()) {
    return;
  }
  m_negativeCache.insert(requestKey, std::chrono::steady_clock::now() +
                                         m_config.negativeCacheTtl);
}

} // namespace Arkham
