#include "AssetRequestCoordinatorTests.h"

#include "AssetCache.h"
#include "AssetLocator.h"
#include "AssetNetworkFetcher.h"
#include "AssetRequestCoordinator.h"
#include "MockHttpServer.h"

#include <QBuffer>
#include <QImage>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QTest>
#include <optional>

using namespace Arkham;

namespace {

QByteArray encodePng(int width, int height) {
  QImage image(width, height, QImage::Format_Mono);
  image.fill(0);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  const bool ok = image.save(&buffer, "png");
  Q_ASSERT(ok);
  Q_UNUSED(ok);
  return bytes;
}

AssetKey makeKey(const QUrl &base,
                 const QString &identifier = QStringLiteral("valid01")) {
  AssetKey key;
  key.assetBase = base;
  key.category = AssetCategory::Card;
  key.identifier = identifier;
  key.side = AssetSide::Front;
  key.format = AssetFormat::Png;
  return key;
}

using Result = AssetOutcome<AssetCache::CachedEntry>;

} // namespace

void AssetRequestCoordinatorTests::init() {
  m_tempDir = std::make_unique<QTemporaryDir>();
  QVERIFY(m_tempDir->isValid());
  m_tempDirPath = m_tempDir->path();
}

void AssetRequestCoordinatorTests::cleanup() { m_tempDir.reset(); }

void AssetRequestCoordinatorTests::coalescesConcurrentIdenticalRequests() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8);
  response.slowDrip = true; // slow enough that both requests overlap in flight
  response.chunkSize = 32;
  response.chunkDelayMs = 20;
  server.setResponse(QStringLiteral("/cards/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  const AssetKey key =
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));

  int completions = 0;
  std::optional<Result> resultA;
  std::optional<Result> resultB;
  coordinator.request(key, [&](Result r) {
    ++completions;
    resultA = std::move(r);
  });
  // Issued immediately after, while the first is still in flight: must
  // coalesce onto the SAME underlying operation rather than issuing a
  // second network fetch.
  coordinator.request(key, [&](Result r) {
    ++completions;
    resultB = std::move(r);
  });

  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 1);

  QVERIFY(QTest::qWaitFor([&]() { return completions == 2; }, 5000));

  QCOMPARE(server.requestCount(QStringLiteral("/cards/valid01.png")), 1);
  QVERIFY(resultA.has_value());
  QVERIFY(resultB.has_value());
  QVERIFY2(bool(*resultA), qPrintable(resultA->error().message));
  QVERIFY2(bool(*resultB), qPrintable(resultB->error().message));
  QCOMPARE((**resultA).encodedBytes, (**resultB).encodedBytes);
}

void AssetRequestCoordinatorTests::cancellingOneConsumerNeverAffectsAnother() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8);
  response.slowDrip = true;
  response.chunkSize = 32;
  response.chunkDelayMs = 20;
  server.setResponse(QStringLiteral("/cards/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  const AssetKey key =
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));

  std::optional<Result> cancelledResult;
  std::optional<Result> survivorResult;
  const auto cancelledHandle = coordinator.request(
      key, [&](Result r) { cancelledResult = std::move(r); });
  coordinator.request(key, [&](Result r) { survivorResult = std::move(r); });

  coordinator.cancel(cancelledHandle);
  QVERIFY(QTest::qWaitFor([&]() { return cancelledResult.has_value(); }, 5000));
  QVERIFY(!bool(*cancelledResult));
  QCOMPARE(cancelledResult->error().code, AssetErrorCode::Cancelled);

  // The surviving consumer's underlying fetch must still be running and
  // must still complete successfully -- one consumer's cancellation must
  // never abort a shared in-flight operation while another consumer
  // remains.
  QVERIFY(!survivorResult.has_value());
  QVERIFY(QTest::qWaitFor([&]() { return survivorResult.has_value(); }, 5000));
  QVERIFY2(bool(*survivorResult), qPrintable(survivorResult->error().message));
}

void AssetRequestCoordinatorTests::
    lastConsumerCancellationAbortsUnderlyingFetch() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(200, 200);
  response.slowDrip = true;
  response.chunkSize = 16;
  response.chunkDelayMs = 30;
  server.setResponse(QStringLiteral("/cards/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  const AssetKey key =
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));

  std::optional<Result> result;
  const auto handle =
      coordinator.request(key, [&](Result r) { result = std::move(r); });
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 1);

  // Wait until the mock server has actually started streaming a real
  // response body (at least one slow-drip chunk flushed) before
  // cancelling, so this test proves cancellation truly interrupts an
  // in-progress transfer -- not merely a request that happened to be
  // cancelled before the underlying TCP connection was even established.
  QVERIFY(QTest::qWaitFor(
      [&]() {
        return server.lastBytesWrittenForSlowDrip(
                   QStringLiteral("/cards/valid01.png")) >= 0;
      },
      5000));

  coordinator.cancel(handle);
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::Cancelled);
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 0);

  // Give the server's writer a moment to notice the disconnect.
  QTest::qWait(80);
  const qint64 flushed =
      server.lastBytesWrittenForSlowDrip(QStringLiteral("/cards/valid01.png"));
  QVERIFY(flushed >= 0);
  QVERIFY2(flushed < response.body.size(),
           qPrintable(QStringLiteral("flushed=%1 total=%2")
                          .arg(flushed)
                          .arg(response.body.size())));
}

void AssetRequestCoordinatorTests::advancesToNextCandidateOnlyOnNotFound() {
  MockHttpServer server;
  MockHttpServer::Response notFound;
  notFound.status = 404;
  notFound.reasonPhrase = "Not Found";
  server.setResponse(QStringLiteral("/cards/valid01.png"), notFound);

  MockHttpServer::Response altFront;
  altFront.contentType = "image/png";
  altFront.body = encodePng(4, 4);
  server.setResponse(QStringLiteral("/cards/valid01a.png"), altFront);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  const AssetKey key =
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE(server.requestCount(QStringLiteral("/cards/valid01.png")), 1);
  QCOMPARE(server.requestCount(QStringLiteral("/cards/valid01a.png")), 1);
}

void AssetRequestCoordinatorTests::nonNotFoundErrorNeverAdvancesCandidate() {
  MockHttpServer server;
  MockHttpServer::Response serverError;
  serverError.status = 500;
  serverError.reasonPhrase = "Internal Server Error";
  server.setResponse(QStringLiteral("/cards/valid01.png"), serverError);
  // Deliberately leave "/cards/valid01a.png" unregistered: if the
  // coordinator ever (incorrectly) advanced to it, the default 200-empty
  // response would be served, which we can detect via requestCount.

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  const AssetKey key =
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::UnexpectedStatus);
  QCOMPARE(server.requestCount(QStringLiteral("/cards/valid01a.png")), 0);
}

void AssetRequestCoordinatorTests::cacheHitShortCircuitsNetworkEntirely() {
  MockHttpServer server;
  // No response registered at all for the candidate path: if the
  // coordinator ever issued a network request despite the cache hit, it
  // would receive the server's default empty 200 body and this test's
  // assertions on the returned bytes would fail.

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);
  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = QByteArrayLiteral("already-cached-bytes");
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  cache.store(cacheKey, preSeeded);

  AssetRequestCoordinator coordinator(cache, fetcher);
  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).encodedBytes, QByteArrayLiteral("already-cached-bytes"));
  QCOMPARE(server.requestCount(QStringLiteral("/cards/valid01.png")), 0);
}

void AssetRequestCoordinatorTests::destructionNeverInvokesStaleCallback() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(200, 200);
  response.slowDrip = true;
  response.chunkSize = 16;
  response.chunkDelayMs = 100;
  server.setResponse(QStringLiteral("/cards/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  bool callbackFired = false;
  const AssetKey key =
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
  {
    AssetRequestCoordinator coordinator(cache, fetcher);
    coordinator.request(key,
                        [&callbackFired](Result) { callbackFired = true; });
    // coordinator destroyed here, mid-flight.
  }

  QTest::qWait(300);
  QVERIFY(!callbackFired);
}

void AssetRequestCoordinatorTests::
    cancellingImmediateCacheHitCompletionSuppressesDelivery() {
  // Even a cache hit (or a pre-network resolution error) must return a
  // VALID handle, and that handle must be able to suppress the queued
  // completion before it runs -- otherwise a QML seam that calls
  // cancel() unconditionally in its destructor (as AssetImageRequest
  // does) would have no way to prevent a completion callback from firing
  // on an object it no longer owns.
  MockHttpServer server; // no responses registered: any network hit fails.

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);
  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = QByteArrayLiteral("cache-hit-bytes");
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  cache.store(cacheKey, preSeeded);

  AssetRequestCoordinator coordinator(cache, fetcher);
  int callbackCount = 0;
  std::optional<Result> result;
  const auto handle = coordinator.request(key, [&](Result r) {
    ++callbackCount;
    result = std::move(r);
  });

  // The whole point of this test: the handle for an immediate cache-hit
  // completion must be valid, not the default-constructed sentinel.
  QVERIFY(handle.isValid());

  // Cancel before the event loop has had any chance to run the queued
  // cache-hit delivery.
  coordinator.cancel(handle);

  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::Cancelled);

  // Give any (incorrectly) still-queued cache-hit delivery a chance to
  // fire before asserting it never did.
  QTest::qWait(100);
  QCOMPARE(callbackCount, 1);
}

void AssetRequestCoordinatorTests::
    diskHitWithValidatorsRevalidatesAndServesStaleOn304() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(32, 32); // must never be served to the caller
  response.etagForConditionalMatch = "\"stale-etag\"";
  server.setResponse(QStringLiteral("/cards/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);
  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = QByteArrayLiteral("stale-but-still-valid-bytes");
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  preSeeded.etag = QStringLiteral("\"stale-etag\"");
  cache.store(cacheKey, preSeeded);

  // A fresh AssetCache instance (simulating a process restart with empty
  // memory) forces this request through the disk-hit path rather than
  // the memory-hit path, which never revalidates.
  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).encodedBytes,
           QByteArrayLiteral("stale-but-still-valid-bytes"));
  QCOMPARE(server.requestCount(QStringLiteral("/cards/valid01.png")), 1);
  QCOMPARE(server.lastRequestHeaders(QStringLiteral("/cards/valid01.png"))
               .value("if-none-match"),
           QByteArrayLiteral("\"stale-etag\""));
}

void AssetRequestCoordinatorTests::
    diskHitRevalidationReplacesEntryOnFresh200() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(32, 32);
  // No etagForConditionalMatch configured: the origin's content genuinely
  // changed, so it answers the conditional GET with a full fresh 200.
  server.setResponse(QStringLiteral("/cards/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);
  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = QByteArrayLiteral("old-bytes-now-outdated");
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  preSeeded.etag = QStringLiteral("\"old-etag\"");
  cache.store(cacheKey, preSeeded);

  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QVERIFY((**result).encodedBytes !=
          QByteArrayLiteral("old-bytes-now-outdated"));
  QCOMPARE(server.requestCount(QStringLiteral("/cards/valid01.png")), 1);

  // The cache must actually be updated with the fresh content, not just
  // the in-memory result returned to this one caller.
  const auto updated = restartedCache.lookupMemory(cacheKey);
  QVERIFY(updated.has_value());
  QVERIFY(updated->encodedBytes != QByteArrayLiteral("old-bytes-now-outdated"));
}

void AssetRequestCoordinatorTests::
    diskHitRevalidationServesStaleOnAnyFailure() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.status = 404;
  response.reasonPhrase = "Not Found";
  server.setResponse(QStringLiteral("/cards/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);
  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = QByteArrayLiteral("still-served-despite-404");
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  preSeeded.lastModified = QStringLiteral("Wed, 01 Jan 2020 00:00:00 GMT");
  cache.store(cacheKey, preSeeded);

  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  // "Stale-if-error": an origin that now 404s a previously-cached
  // candidate must never make already-cached, already-displayed art
  // disappear or error out.
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).encodedBytes,
           QByteArrayLiteral("still-served-despite-404"));
}
