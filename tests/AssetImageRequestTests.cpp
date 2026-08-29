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
  // Copilot review: Q_ASSERT compiles out in release builds, which would
  // silently turn a fixture-encoding failure here into a confusing
  // downstream test failure instead of a clear, immediate diagnosis.
  // qFatal() is enforced in every build configuration.
  if (!ok) {
    qFatal("encodePng() failed to encode a %dx%d test fixture image", width,
           height);
  }
  return bytes;
}

AssetKey makeKey(const QString &rawBase,
                 const QString &identifier = QStringLiteral("valid01")) {
  const AssetOutcome<ValidatedAssetSource> base =
      ValidatedAssetSource::fromRaw(rawBase);
  // Copilot review: Q_ASSERT compiles out in release builds, which would
  // silently turn a fixture-encoding failure here into a confusing
  // downstream test failure instead of a clear, immediate diagnosis.
  // qFatal() is enforced in every build configuration.
  if (!base) {
    qFatal("makeKey() fixture base URL failed validation: %s",
           qPrintable(base.error().message));
  }
  AssetKey key;
  key.assetBase = *base;
  // SetIcon (not Card) so this file's PNG-encoded fixtures match the
  // real, category-fixed format AssetLocator now enforces (Card is
  // always AVIF; see AssetLocator::canonicalFormatFor()) -- this suite
  // exercises AssetImageRequest's lifecycle/state machine, which is
  // category-agnostic, not card-art-specific path shape.
  key.category = AssetCategory::SetIcon;
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
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

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
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
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

void AssetImageRequestTests::
    callerProvidedAccessibleDescriptionIsCarriedThroughAllStates() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(16, 16);
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);
  AssetImageRequest request(coordinator);

  const QString callerDescription = QStringLiteral("Zoey Samaras, front");

  // Per djensenius/ArkhamHorror-Linux#17, this seam must carry a
  // caller-provided accessible description rather than always inventing
  // its own generic text -- verify the caller's exact text is present
  // (not necessarily verbatim, since Loading/Error prefix/suffix status
  // text around it) at every state, including the terminal Ready state
  // where it must appear unchanged.
  request.load(
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port())),
      callerDescription);
  QVERIFY(request.accessibleDescription().contains(callerDescription));

  QVERIFY(QTest::qWaitFor(
      [&]() { return request.status() == AssetImageRequest::Status::Ready; },
      5000));
  QCOMPARE(request.accessibleDescription(), callerDescription);
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

  // "a/b" fails AssetLocator's identifier grammar synchronously (before
  // any network I/O) via its embedded path separator, exercising the
  // InvalidIdentifier -> Error path. (Uppercase letters, e.g. "UPPER01",
  // are valid identifier characters -- real pinned digest sources contain
  // uppercase segments in official card codes/mutationIds -- so uppercase
  // alone is no longer a synchronous-rejection fixture.)
  request.load(makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
                       QStringLiteral("a/b")));
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
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);
  AssetImageRequest request(coordinator);

  request.load(
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
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
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  {
    AssetImageRequest request(coordinator);
    request.load(
        makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
    // request destroyed here, mid-flight -- must not crash and must not
    // touch any destroyed state when the underlying fetch eventually
    // completes/aborts.
  }

  QTest::qWait(400); // long enough for the slow drip to otherwise finish
  QVERIFY(true);     // reaching here without a crash is the assertion
}

void AssetImageRequestTests::
    destructionImmediatelyAfterImmediateCompletionNeverCrashes() {
  MockHttpServer server; // never actually contacted: this identifier is
                         // rejected synchronously by AssetLocator.
  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  {
    AssetImageRequest request(coordinator);
    // "a/b" fails AssetLocator's identifier grammar synchronously (via
    // its embedded path separator), so AssetRequestCoordinator::request()
    // queues an immediate error completion (via QMetaObject::invokeMethod)
    // rather than starting any network fetch. Destroying `request` right
    // here -- before that queued completion has run -- is exactly the
    // scenario that used to use-after-free: an invalid RequestHandle meant
    // this object's destructor could never suppress the queued delivery.
    request.load(
        makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
                QStringLiteral("a/b")));
    // request destroyed here, before the event loop has run at all.
  }

  QTest::qWait(200);
  QVERIFY(true); // reaching here without a crash is the assertion
}

void AssetImageRequestTests::
    reloadingWithNewKeyClearsPreviousImageDuringLoading() {
  MockHttpServer server;
  MockHttpServer::Response firstResponse;
  firstResponse.contentType = "image/png";
  firstResponse.body = encodePng(16, 16);
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"),
                     firstResponse);

  MockHttpServer::Response secondResponse;
  secondResponse.contentType = "image/png";
  secondResponse.body = encodePng(200, 200);
  secondResponse.slowDrip = true;
  secondResponse.chunkSize = 16;
  secondResponse.chunkDelayMs = 60;
  server.setResponse(QStringLiteral("/img/arkham/sets/valid02.png"),
                     secondResponse);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);
  AssetImageRequest request(coordinator);

  request.load(
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
  QVERIFY(QTest::qWaitFor(
      [&]() { return request.status() == AssetImageRequest::Status::Ready; },
      5000));
  QVERIFY(!request.image().isNull());

  // Switch to a second, slow-to-arrive image: the FIRST image must not
  // still be visible while the second load is in its Loading phase.
  request.load(makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
                       QStringLiteral("valid02")));
  QCOMPARE(request.status(), AssetImageRequest::Status::Loading);
  QVERIFY(request.image().isNull());

  QVERIFY(QTest::qWaitFor(
      [&]() { return request.status() == AssetImageRequest::Status::Ready; },
      5000));
  QVERIFY(!request.image().isNull());
}

void AssetImageRequestTests::
    reloadingAfterErrorEmitsErrorChangedWhenClearingStaleError() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(16, 16);
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);
  AssetImageRequest request(coordinator);

  // First load fails synchronously (before any network I/O): "a/b"
  // fails AssetLocator's identifier grammar (via its embedded path
  // separator), exercising the InvalidIdentifier -> Error path and
  // populating errorString/errorCode.
  request.load(makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
                       QStringLiteral("a/b")));
  QVERIFY(QTest::qWaitFor(
      [&]() { return request.status() == AssetImageRequest::Status::Error; },
      5000));
  QVERIFY(!request.errorString().isEmpty());
  QVERIFY(request.errorCode() != 0);

  QSignalSpy errorSpy(&request, &AssetImageRequest::errorChanged);

  // Second load reuses the same object with a valid identifier. Clearing
  // the stale error state at the start of load() must itself emit
  // errorChanged() -- a QML binding to errorString()/errorCode() must
  // never keep showing the first load's error throughout the second
  // load's Loading phase.
  request.load(
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
  QCOMPARE(request.status(), AssetImageRequest::Status::Loading);
  QVERIFY(request.errorString().isEmpty());
  QCOMPARE(request.errorCode(), 0);
  QVERIFY(errorSpy.count() >= 1);

  QVERIFY(QTest::qWaitFor(
      [&]() { return request.status() == AssetImageRequest::Status::Ready; },
      5000));
  QVERIFY(!request.image().isNull());
}
