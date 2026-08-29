#include "AssetImageRequestTests.h"

#include "AssetCache.h"
#include "AssetImageRequest.h"
#include "AssetNetworkFetcher.h"
#include "AssetRequestCoordinator.h"
#include "MockHttpServer.h"

#include <QBuffer>
#include <QImage>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTest>

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

} // namespace

void AssetImageRequestTests::init() {
  m_tempDir = std::make_unique<QTemporaryDir>();
  QVERIFY(m_tempDir->isValid());
  m_tempDirPath = m_tempDir->path();
}

void AssetImageRequestTests::cleanup() { m_tempDir.reset(); }

void AssetImageRequestTests::successfulLoadTransitionsIdleLoadingReady() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(16, 16);
  server.setResponse(QStringLiteral("/cards/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);
  AssetImageRequest request(coordinator);

  QCOMPARE(request.status(), AssetImageRequest::Status::Idle);

  QSignalSpy statusSpy(&request, &AssetImageRequest::statusChanged);
  QSignalSpy imageSpy(&request, &AssetImageRequest::imageChanged);

  request.load(
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port()))));
  QCOMPARE(request.status(), AssetImageRequest::Status::Loading);

  QVERIFY(QTest::qWaitFor(
      [&]() { return request.status() == AssetImageRequest::Status::Ready; },
      5000));

  QCOMPARE(request.status(), AssetImageRequest::Status::Ready);
  QVERIFY(!request.image().isNull());
  QCOMPARE(request.errorCode(), 0);
  QVERIFY(request.errorString().isEmpty());
  QVERIFY(!request.accessibleDescription().isEmpty());
  QVERIFY(statusSpy.count() >= 2); // Idle->Loading, Loading->Ready
  QVERIFY(imageSpy.count() >= 1);
}

void AssetImageRequestTests::failedLoadTransitionsIdleLoadingError() {
  MockHttpServer server;
  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);
  AssetImageRequest request(coordinator);

  QSignalSpy errorSpy(&request, &AssetImageRequest::errorChanged);

  // "UPPER01" fails AssetLocator's identifier grammar synchronously
  // (before any network I/O), exercising the InvalidIdentifier -> Error
  // path.
  request.load(
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port())),
              QStringLiteral("UPPER01")));
  QCOMPARE(request.status(), AssetImageRequest::Status::Loading);

  QVERIFY(QTest::qWaitFor(
      [&]() { return request.status() == AssetImageRequest::Status::Error; },
      5000));

  QCOMPARE(request.status(), AssetImageRequest::Status::Error);
  QCOMPARE(request.errorCode(),
           static_cast<int>(AssetErrorCode::InvalidIdentifier));
  QVERIFY(!request.errorString().isEmpty());
  QVERIFY(errorSpy.count() >= 1);
}

void AssetImageRequestTests::
    cancelMidFlightReturnsToIdleWithoutFurtherSignals() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(200, 200);
  response.slowDrip = true;
  response.chunkSize = 16;
  response.chunkDelayMs = 60;
  server.setResponse(QStringLiteral("/cards/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);
  AssetImageRequest request(coordinator);

  request.load(
      makeKey(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port()))));
  QCOMPARE(request.status(), AssetImageRequest::Status::Loading);

  request.cancel();
  QCOMPARE(request.status(), AssetImageRequest::Status::Idle);

  QSignalSpy statusSpy(&request, &AssetImageRequest::statusChanged);
  QSignalSpy imageSpy(&request, &AssetImageRequest::imageChanged);
  QSignalSpy errorSpy(&request, &AssetImageRequest::errorChanged);

  // Give the (now-cancelled) in-flight fetch plenty of time to have
  // completed if it were (incorrectly) still going to deliver a stale
  // Ready/Error transition.
  QTest::qWait(400);

  QCOMPARE(request.status(), AssetImageRequest::Status::Idle);
  QCOMPARE(statusSpy.count(), 0);
  QCOMPARE(imageSpy.count(), 0);
  QCOMPARE(errorSpy.count(), 0);
}

void AssetImageRequestTests::destructionMidFlightNeverEmitsAfterDestruction() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(200, 200);
  response.slowDrip = true;
  response.chunkSize = 16;
  response.chunkDelayMs = 60;
  server.setResponse(QStringLiteral("/cards/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  {
    AssetImageRequest request(coordinator);
    request.load(makeKey(
        QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port()))));
    // request destroyed here, mid-flight -- must not crash and must not
    // touch any destroyed state when the underlying fetch eventually
    // completes/aborts.
  }

  QTest::qWait(400); // long enough for the slow drip to otherwise finish
  QVERIFY(true);     // reaching here without a crash is the assertion
}
