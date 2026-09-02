#include "AssetImageRequestTests.h"

#include "AssetCache.h"
#include "AssetImageRequest.h"
#include "AssetNetworkFetcher.h"
#include "AssetRequestCoordinator.h"
#include "MockHttpServer.h"

#include <QBuffer>
#include <QDir>
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
  // Round-9+ review: rooted under home so this exercises AssetCache's
  // real home-anchored directory-resolution code path -- see
  // AssetCacheTests::init()'s comment for the full rationale.
  m_tempDir = std::make_unique<QTemporaryDir>(
      QDir::homePath() + QStringLiteral("/.arkham-asset-image-test-XXXXXX"));
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
    statusChangedObserverSeesFullyConsistentPropertySnapshot() {
  // Copilot review (round-N+): statusChanged() must never fire while
  // errorString()/errorCode()/image()/accessibleDescription() still hold
  // a STALE value from the previous transition -- a directly-connected
  // observer (including a QML binding that reacts to statusChanged() by
  // re-reading those other properties) must always see a fully
  // consistent snapshot. This is forced via a REAL, synchronous,
  // direct-connection signal handler (not a queued/event-loop wait),
  // exercising actual reentrancy into this object's own property
  // getters from within its own signal emission -- not merely a
  // sequential check of before/after.
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

  // First load fails synchronously, populating a non-empty
  // errorString/errorCode this test can later prove is no longer
  // visible the instant statusChanged() reports the NEXT Loading
  // transition.
  request.load(makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
                       QStringLiteral("a/b")));
  QVERIFY(QTest::qWaitFor(
      [&]() { return request.status() == AssetImageRequest::Status::Error; },
      5000));
  QVERIFY(!request.errorString().isEmpty());
  QVERIFY(request.errorCode() != 0);

  struct Snapshot {
    AssetImageRequest::Status status;
    QString errorString;
    int errorCode;
    bool imageIsNull;
    QString accessibleDescription;
  };
  QVector<Snapshot> snapshots;
  QObject::connect(
      &request, &AssetImageRequest::statusChanged, &request,
      [&]() {
        snapshots.push_back(Snapshot{
            request.status(), request.errorString(), request.errorCode(),
            request.image().isNull(), request.accessibleDescription()});
      },
      Qt::DirectConnection);

  request.load(
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));

  // The Idle/Error -> Loading transition's own statusChanged() delivery
  // must already observe the cleared error and image, and the new
  // "Loading ..." accessible description -- never the stale values from
  // the just-superseded failed load().
  QVERIFY(!snapshots.isEmpty());
  const Snapshot &loadingSnapshot = snapshots.first();
  QCOMPARE(loadingSnapshot.status, AssetImageRequest::Status::Loading);
  QVERIFY(loadingSnapshot.errorString.isEmpty());
  QCOMPARE(loadingSnapshot.errorCode, 0);
  QVERIFY(loadingSnapshot.imageIsNull);
  QVERIFY(loadingSnapshot.accessibleDescription.startsWith(
      QStringLiteral("Loading")));

  QVERIFY(QTest::qWaitFor(
      [&]() { return request.status() == AssetImageRequest::Status::Ready; },
      5000));

  // Likewise, the Loading -> Ready transition's statusChanged() delivery
  // must already observe the newly-decoded image, never a null one.
  QVERIFY(snapshots.size() >= 2);
  const Snapshot &readySnapshot = snapshots.last();
  QCOMPARE(readySnapshot.status, AssetImageRequest::Status::Ready);
  QVERIFY(!readySnapshot.imageIsNull);
  QVERIFY(readySnapshot.errorString.isEmpty());
  QCOMPARE(readySnapshot.errorCode, 0);
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

void AssetImageRequestTests::
    reentrantLoadFromWithinProgressChangedLeavesOnlyInnerRequestSurvives() {
  // Review round-N+ finding #6 (AssetImageRequest.cpp): load() emits its
  // Loading-state NOTIFY signals BEFORE registering its own coordinator
  // request. A directly-connected observer that reentrantly calls
  // load() again from inside one of THOSE signal emissions runs the
  // INNER load() to completion -- including registering ITS OWN
  // coordinator handle -- before the OUTER load() call resumes and
  // (without the fix) would unconditionally overwrite m_handle with its
  // own, permanently orphaning the inner one. This forces that exact
  // reentrancy via a REAL, synchronous, direct-connection signal
  // handler (not a queued/event-loop simulation).
  MockHttpServer server;
  MockHttpServer::Response outerResponse;
  outerResponse.contentType = "image/png";
  outerResponse.body = encodePng(16, 16);
  server.setResponse(QStringLiteral("/img/arkham/sets/outer01.png"),
                     outerResponse);

  MockHttpServer::Response innerResponse;
  innerResponse.contentType = "image/png";
  innerResponse.body = encodePng(32, 32);
  server.setResponse(QStringLiteral("/img/arkham/sets/inner01.png"),
                     innerResponse);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);
  AssetImageRequest request(coordinator);

  bool reentered = false;
  QObject::connect(
      &request, &AssetImageRequest::progressChanged, &request,
      [&]() {
        if (reentered) {
          return; // the INNER load()'s own progressChanged(): do not recurse
        }
        reentered = true;
        request.load(
            makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
                    QStringLiteral("inner01")));
      },
      Qt::DirectConnection);

  request.load(makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
                       QStringLiteral("outer01")));

  QVERIFY(reentered);
  QCOMPARE(request.status(), AssetImageRequest::Status::Loading);

  // Coordinator-level assertion, checked IMMEDIATELY after the two fully
  // synchronous, nested load() calls above returned -- before the event
  // loop has run at all, so neither fetch could possibly have completed
  // on its own yet: exactly the survivor (inner) operation remains
  // in-flight. Without the fix, the outer registration is never
  // explicitly cancelled when it discovers it is stale, so BOTH
  // operations would still be in-flight here -- a real, observable
  // resource leak (an abandoned coordinator Operation/fetch that nothing
  // will ever cancel), not merely a discrepancy in this object's own
  // eventually-published image.
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 1);

  QVERIFY(QTest::qWaitFor(
      [&]() { return request.status() == AssetImageRequest::Status::Ready; },
      5000));
  QCOMPARE(request.status(), AssetImageRequest::Status::Ready);
  // The INNER (reentrant) load must be the one that ultimately wins and
  // publishes -- its distinct 32x32 image, never the outer 16x16 one.
  QCOMPARE(request.image().width(), 32);
  QCOMPARE(request.image().height(), 32);

  // Give the (now-cancelled, orphan-free) OUTER fetch plenty of time to
  // have delivered a stale completion if it were (incorrectly) still
  // tracked/published.
  QSignalSpy imageSpy(&request, &AssetImageRequest::imageChanged);
  QSignalSpy statusSpy(&request, &AssetImageRequest::statusChanged);
  QTest::qWait(300);
  QCOMPARE(imageSpy.count(), 0);         // no further, stale publication
  QCOMPARE(statusSpy.count(), 0);        // no further, stale transition
  QCOMPARE(request.image().width(), 32); // still the inner result, unclobbered
  QCOMPARE(request.status(), AssetImageRequest::Status::Ready);
}

void AssetImageRequestTests::
    synchronousDestructionDuringLoadSignalNeverContinuesAfterDestruction() {
  // Review round-N+ finding #6: a directly-connected observer that
  // destroys this AssetImageRequest synchronously from inside one of
  // load()'s own NOTIFY emissions must not leave load()'s remaining body
  // (further member writes/emits, then coordinator registration) running
  // against the now-destroyed object.
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

  auto request = std::make_unique<AssetImageRequest>(coordinator);
  QObject::connect(
      request.get(), &AssetImageRequest::progressChanged, request.get(),
      [&]() { request.reset(); }, Qt::DirectConnection);

  request->load(
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port())));
  // Reaching here at all (rather than crashing/UB-sanitizer-tripping
  // inside load()'s own remaining body) is the primary assertion.
  QVERIFY(request == nullptr);

  QTest::qWait(200); // long enough for any leaked/still-registered fetch
                     // to otherwise have delivered a stale completion
                     // into now-freed memory.
  QVERIFY(true);
}

void AssetImageRequestTests::
    reentrantCancelFromWithinCancelSignalNeverDoubleCorruptsState() {
  // Companion coverage for cancel() itself (load() calls cancel() first,
  // so this exercises the exact same synchronous-reentrancy hazard one
  // level lower): a directly-connected observer that reentrantly calls
  // cancel() again from inside cancel()'s OWN statusChanged()/
  // progressChanged() emission must not corrupt state or double-emit.
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

  int reentrantCancelCount = 0;
  QObject::connect(
      &request, &AssetImageRequest::progressChanged, &request,
      [&]() {
        if (reentrantCancelCount > 0) {
          return; // the reentrant cancel()'s own progressChanged(): stop
        }
        ++reentrantCancelCount;
        request.cancel();
      },
      Qt::DirectConnection);

  request.cancel();
  QCOMPARE(request.status(), AssetImageRequest::Status::Idle);
  QCOMPARE(reentrantCancelCount, 1);

  QSignalSpy statusSpy(&request, &AssetImageRequest::statusChanged);
  QTest::qWait(300); // long enough for the (cancelled) slow-drip fetch to
                     // otherwise have delivered a stale completion
  QCOMPARE(request.status(), AssetImageRequest::Status::Idle);
  QCOMPARE(statusSpy.count(), 0);
}
