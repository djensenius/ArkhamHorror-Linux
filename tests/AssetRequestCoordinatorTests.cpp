#include "AssetRequestCoordinatorTests.h"

#include "AssetCache.h"
#include "AssetLocator.h"
#include "AssetNetworkFetcher.h"
#include "AssetRequestCoordinator.h"
#include "MockHttpServer.h"

#include <QBuffer>
#include <QCoreApplication>
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

// A minimal, spec-valid ISOBMFF "ftyp" box whose major_brand is "avif":
// enough for AssetNetworkFetcher's magic-byte sniffing to genuinely
// identify these bytes as AVIF (see sniffMagicBytes() in
// AssetNetworkFetcher.cpp), without needing a full, real, pixel-encoded
// AVIF image. This is deliberately used only to exercise the pipeline up
// to (and no further than) the Qt-codec-support gate: this build/CI
// environment has no Qt AVIF decode plugin installed (see issue #17
// review item 4, not yet implemented), so a genuine Card/HomebrewCard
// fetch of real bytes is expected to end in AssetErrorCode::UnsupportedCodec
// here, not a successful decode -- proving every earlier gate (correct
// candidate URL requested, Content-Type match, magic-byte match) passed
// for real AVIF-shaped bytes.
QByteArray minimalAvifFtypBox() {
  QByteArray bytes;
  bytes.append(char(0));
  bytes.append(char(0));
  bytes.append(char(0));
  bytes.append(char(16)); // box size = 16, big-endian
  bytes.append("ftyp");
  bytes.append("avif"); // major_brand
  bytes.append(char(0));
  bytes.append(char(0));
  bytes.append(char(0));
  bytes.append(char(0)); // minor_version = 0
  return bytes;
}

// A real Card AssetKey (unlike makeKey()'s default SetIcon), for the two
// alternate-front-fallback tests below, which exercise Card-only
// candidate-list behaviour (see AssetLocator::resolveCandidates()'s
// alternate-front fallback, only ever produced for Card/HomebrewCard).
AssetKey makeCardKey(const QString &rawBase,
                     const QString &identifier = QStringLiteral("valid01")) {
  const AssetOutcome<ValidatedAssetSource> base =
      ValidatedAssetSource::fromRaw(rawBase);
  if (!base) {
    qFatal("makeCardKey() fixture base URL failed validation: %s",
           qPrintable(base.error().message));
  }
  AssetKey key;
  key.assetBase = *base;
  key.category = AssetCategory::Card;
  key.identifier = identifier;
  key.side = AssetSide::Front;
  key.format = AssetFormat::Avif;
  return key;
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
  // exercises AssetRequestCoordinator's cache/coalescing/fallback-status
  // logic, which (aside from the two alternate-front-fallback tests
  // below, which need real Card semantics) is category-agnostic.
  key.category = AssetCategory::SetIcon;
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
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));

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

  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);
  QVERIFY(resultA.has_value());
  QVERIFY(resultB.has_value());
  QVERIFY2(bool(*resultA), qPrintable(resultA->error().message));
  QVERIFY2(bool(*resultB), qPrintable(resultB->error().message));
  QCOMPARE((**resultA).encodedBytes, (**resultB).encodedBytes);
}

void AssetRequestCoordinatorTests::
    keysDifferingOnlyByAssetBaseTrailingSlashStillCoalesce() {
  // Regression test: ValidatedAssetSource::fromRaw() runs
  // UrlValidator::validateCustomUrl() once, at construction, which strips
  // a trailing slash -- so a raw assetBase string with a trailing slash
  // and one without produce genuinely EQUAL ValidatedAssetSource values
  // (same normalised QUrl), not merely two different-looking-but-
  // equivalent values that some separate coalescing step has to know to
  // treat as the same. This is a strictly stronger guarantee than the
  // historical raw-QUrl field ever provided (where such a pair was NOT
  // operator==-equal and coalescing had to re-normalise independently):
  // by construction, there is no way to end up with two AssetKey values
  // that resolve to the same candidate URL yet compare unequal.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8);
  response.slowDrip = true; // both requests must overlap in flight
  response.chunkSize = 32;
  response.chunkDelayMs = 20;
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  AssetKey keyA =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));

  const AssetOutcome<ValidatedAssetSource> trailingSlashBase =
      ValidatedAssetSource::fromRaw(
          QStringLiteral("http://127.0.0.1:%1/").arg(server.port()));
  QVERIFY2(bool(trailingSlashBase),
           qPrintable(trailingSlashBase.error().message));
  AssetKey keyB = keyA;
  keyB.assetBase = *trailingSlashBase;

  // The two AssetKey values ARE operator==-equal: ValidatedAssetSource
  // normalises at construction, so two raw strings that differ only by a
  // trailing slash produce the identical normalised QUrl. They coalesce
  // onto one operation for the ordinary reason any two equal AssetKey
  // values would.
  QVERIFY(keyA == keyB);

  int completions = 0;
  std::optional<Result> resultA;
  std::optional<Result> resultB;
  coordinator.request(keyA, [&](Result r) {
    ++completions;
    resultA = std::move(r);
  });
  coordinator.request(keyB, [&](Result r) {
    ++completions;
    resultB = std::move(r);
  });

  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 1);

  QVERIFY(QTest::qWaitFor([&]() { return completions == 2; }, 5000));

  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);
  QVERIFY(resultA.has_value());
  QVERIFY(resultB.has_value());
  QVERIFY2(bool(*resultA), qPrintable(resultA->error().message));
  QVERIFY2(bool(*resultB), qPrintable(resultB->error().message));
  QCOMPARE((**resultA).encodedBytes, (**resultB).encodedBytes);
}

void AssetRequestCoordinatorTests::
    keysDifferingOnlyByHostileLocaleContentNeverCoalesce() {
  // Regression test for a HIGH-severity coalescing-key robustness finding:
  // the operation key used to join fields (assetBase, category,
  // identifier, side, locale, format) with a plain separator character.
  // `identifier` is validated by AssetLocator's grammar (so it can never
  // itself contain the separator) and `assetBase` is percent-encoded
  // before joining, but `locale` is caller-supplied free text with NO
  // such validation -- so, unlike every other field, it alone could carry
  // a raw separator character. With every OTHER field's width/content
  // pinned by validation as it is today, that alone does not yet produce
  // a *reachable* full-string collision (this exact input, verified by
  // hand, still distinguished the two operations even before the
  // subsequent length-prefixing fix) -- but that safety is an accidental
  // byproduct of unrelated invariants living in AssetLocator/AssetTypes,
  // not a property of canonicalOperationKey() itself, and would silently
  // break the moment any of those invariants changed (a looser identifier
  // grammar, or AssetCategory/AssetSide/AssetFormat growing to 10+
  // values). The fix makes injectivity unconditional (length-prefixed
  // fields) instead of relying on that coupling, and this test pins down
  // the actual observable contract that must never regress regardless of
  // how the encoding is implemented: two AssetKey values that are NOT
  // operator==-equal (locale differs, here via hostile content) must
  // never be merged into the same in-flight operation. This holds
  // regardless of whether the two locale values happen to resolve to the
  // same candidate URLs (neither "de" nor a hostile variant of it maps to
  // a supported localized variant here, so both fall back to the same
  // English candidate) -- operation-key identity is deliberately
  // independent of resolved-URL identity (see the class doc comment on
  // canonicalOperationKey()).
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8);
  response.slowDrip = true; // both requests must overlap in flight
  response.chunkSize = 32;
  response.chunkDelayMs = 20;
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  AssetKey keyA =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyA.locale = QStringLiteral("de");

  AssetKey keyB = keyA;
  // Embeds the historical separator character plus digits chosen to look
  // like a plausible (but different) trailing field, exercising exactly
  // the collision shape the finding describes. The literal is split into
  // two adjacent string-literal pieces ("...\x1f" followed by "2")
  // rather than written as a single "...\x1f2" literal: `\x` hex escapes
  // are variable-width and greedily consume ALL following hex digits, so
  // a single literal would parse "\x1f2" as one code unit (hex 0x1F2)
  // instead of the intended separator (0x1F) immediately followed by the
  // literal digit '2'. Adjacent string literals are concatenated only
  // AFTER each literal's own escape sequences are fully resolved, so
  // splitting at the literal boundary is what actually produces the
  // separator+digit shape this test is trying to exercise.
  keyB.locale = QStringLiteral("de\x1f") + QStringLiteral("2");

  QVERIFY(!(keyA == keyB));

  int completions = 0;
  std::optional<Result> resultA;
  std::optional<Result> resultB;
  coordinator.request(keyA, [&](Result r) {
    ++completions;
    resultA = std::move(r);
  });
  coordinator.request(keyB, [&](Result r) {
    ++completions;
    resultB = std::move(r);
  });

  // The real assertion: two AssetKeys that differ (only in locale, via
  // hostile content) must occupy two SEPARATE in-flight operations, never
  // one merged operation.
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 2);

  QVERIFY(QTest::qWaitFor([&]() { return completions == 2; }, 5000));
  QVERIFY(resultA.has_value());
  QVERIFY(resultB.has_value());
  QVERIFY2(bool(*resultA), qPrintable(resultA->error().message));
  QVERIFY2(bool(*resultB), qPrintable(resultB->error().message));
}

void AssetRequestCoordinatorTests::cancellingOneConsumerNeverAffectsAnother() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8);
  response.slowDrip = true;
  response.chunkSize = 32;
  response.chunkDelayMs = 20;
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));

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
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));

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
                   QStringLiteral("/img/arkham/sets/valid01.png")) >= 0;
      },
      5000));

  coordinator.cancel(handle);
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::Cancelled);
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 0);

  // Give the server's writer a moment to notice the disconnect.
  QTest::qWait(80);
  const qint64 flushed = server.lastBytesWrittenForSlowDrip(
      QStringLiteral("/img/arkham/sets/valid01.png"));
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
  server.setResponse(QStringLiteral("/img/arkham/cards/valid01.avif"),
                     notFound);

  MockHttpServer::Response altFront;
  altFront.contentType = "image/avif";
  altFront.body = minimalAvifFtypBox();
  server.setResponse(QStringLiteral("/img/arkham/cards/valid01a.avif"),
                     altFront);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  const AssetKey key =
      makeCardKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  // No Qt AVIF decode plugin is installed in this build/CI environment
  // (see issue #17 review item 4, not yet implemented): the FINAL typed
  // outcome for genuinely AVIF-shaped bytes is UnsupportedCodec, not a
  // successful decode. That is expected and, combined with the two
  // requestCount assertions below, still fully proves the fallback
  // mechanism itself: the coordinator correctly advanced past the
  // English candidate's definitive 404 to the alternate-front candidate,
  // requested exactly the right URL there, and got far enough into
  // validating that response (past Content-Type and magic-byte checks)
  // to reach the codec-support gate -- it did not stop early, retry the
  // same candidate, or skip straight past validation.
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::UnsupportedCodec);
  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/cards/valid01.avif")), 1);
  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/cards/valid01a.avif")),
      1);
}

void AssetRequestCoordinatorTests::nonNotFoundErrorNeverAdvancesCandidate() {
  MockHttpServer server;
  MockHttpServer::Response serverError;
  serverError.status = 500;
  serverError.reasonPhrase = "Internal Server Error";
  server.setResponse(QStringLiteral("/img/arkham/cards/valid01.avif"),
                     serverError);
  // Deliberately leave "/img/arkham/cards/valid01a.avif" unregistered: if
  // the coordinator ever (incorrectly) advanced to it, the default
  // 200-empty response would be served, which we can detect via
  // requestCount.

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  const AssetKey key =
      makeCardKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::UnexpectedStatus);
  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/cards/valid01a.avif")),
      0);
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
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);
  AssetCache::CachedEntry preSeeded;
  // Real, decodable PNG bytes: a memory-hit result now always goes
  // through ensureDecoded() (see AssetRequestCoordinator.h), which
  // decodes on demand whenever decodedImage is null -- unlike this test's
  // opaque placeholder bytes of prior rounds, that decode must actually
  // succeed for a genuine cache-hit outcome to be observed here.
  preSeeded.encodedBytes = encodePng(4, 4);
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  cache.store(cacheKey, preSeeded);

  AssetRequestCoordinator coordinator(cache, fetcher);
  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).encodedBytes, preSeeded.encodedBytes);
  QVERIFY(!(**result).decodedImage.isNull());
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           0);
}

void AssetRequestCoordinatorTests::
    cachedEnglishFallbackNeverSkipsUntriedLocalizedCandidate() {
  // Regression for review item 5: a lower-priority candidate (English)
  // that is already cached must never be served ahead of a
  // higher-priority candidate (the localized "it" variant) that has not
  // been tried at all. Real digest data confirms card "01001" has an
  // Italian localized variant (see contracts/asset-locale-digest-
  // sources/ita.json), so AssetLocator::resolveCandidates() proposes,
  // in strict order: [it/cards/01001.avif, cards/01001.avif,
  // cards/01001a.avif (alt-front fallback)].
  MockHttpServer server;
  MockHttpServer::Response localized;
  localized.contentType = "image/avif";
  localized.body = minimalAvifFtypBox();
  server.setResponse(QStringLiteral("/img/arkham/ita/cards/01001.avif"),
                     localized);
  // Deliberately leave the English candidate unregistered: if the
  // coordinator ever (incorrectly) served the pre-cached English entry
  // instead of trying the untried localized candidate first, this test
  // would still "pass" by accident unless we also prove the localized
  // candidate was genuinely requested -- see the requestCount assertion
  // below, which is the actual proof.

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  AssetKey key =
      makeCardKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
                  QStringLiteral("01001"));
  key.locale = QStringLiteral("it");

  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  QCOMPARE(candidates->size(), 3);
  QVERIFY2(candidates->at(1)
               .url.toString(QUrl::FullyEncoded)
               .endsWith(QStringLiteral("/img/arkham/cards/01001.avif")),
           qPrintable(candidates->at(1).url.toString(QUrl::FullyEncoded)));
  const QString englishCacheKey =
      AssetCache::cacheKeyFor(candidates->at(1).url);
  AssetCache::CachedEntry preSeededEnglish;
  preSeededEnglish.encodedBytes = minimalAvifFtypBox();
  preSeededEnglish.contentType = QStringLiteral("image/avif");
  preSeededEnglish.dimensions = QSize(4, 4);
  cache.store(englishCacheKey, preSeededEnglish);

  AssetRequestCoordinator coordinator(cache, fetcher);
  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  // Proof the untried localized candidate was genuinely attempted (not
  // skipped): it was requested exactly once.
  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/ita/cards/01001.avif")),
      1);
  // Proof the pre-cached English entry was never even consulted over the
  // network for this same reason it was never served either: the final
  // outcome is this environment's expected UnsupportedCodec (no Qt AVIF
  // decode plugin -- see minimalAvifFtypBox()'s comment), NOT a success
  // carrying the pre-seeded English bytes. If the coordinator had
  // (incorrectly) shortcut straight to the cached English entry, this
  // would instead be a success whose encodedBytes equal preSeededEnglish.
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::UnsupportedCodec);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/cards/01001.avif")),
           0);
}

void AssetRequestCoordinatorTests::
    confirmedNegative404AuthorizesSkippingCandidate() {
  // Only an exact, authoritative 404 for the EXACT candidate authorizes
  // skipping it on a later logical request; anything else (including no
  // record at all) never does -- see
  // cachedEnglishFallbackNeverSkipsUntriedLocalizedCandidate() above for
  // that side of review item 5's requirement.
  MockHttpServer server;
  MockHttpServer::Response notFound;
  notFound.status = 404;
  notFound.reasonPhrase = "Not Found";
  server.setResponse(QStringLiteral("/img/arkham/ita/cards/01001.avif"),
                     notFound);
  MockHttpServer::Response english;
  english.contentType = "image/avif";
  english.body = minimalAvifFtypBox();
  server.setResponse(QStringLiteral("/img/arkham/cards/01001.avif"), english);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  AssetKey key =
      makeCardKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
                  QStringLiteral("01001"));
  key.locale = QStringLiteral("it");

  std::optional<Result> firstResult;
  coordinator.request(key, [&](Result r) { firstResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return firstResult.has_value(); }, 5000));
  // English is genuinely AVIF-shaped but undecodable in this environment
  // (no Qt AVIF plugin): UnsupportedCodec, not a success -- deterministic
  // and, crucially, never stored in the cache (only a successful outcome
  // is ever cache-stored), so a second identical request cannot shortcut
  // via an English cache hit either -- it can ONLY shortcut via the
  // localized candidate's negative-404 record, which is exactly what this
  // test proves.
  QVERIFY(!bool(*firstResult));
  QCOMPARE(firstResult->error().code, AssetErrorCode::UnsupportedCodec);
  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/ita/cards/01001.avif")),
      1);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/cards/01001.avif")),
           1);

  std::optional<Result> secondResult;
  coordinator.request(key, [&](Result r) { secondResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return secondResult.has_value(); }, 5000));

  QVERIFY(!bool(*secondResult));
  QCOMPARE(secondResult->error().code, AssetErrorCode::UnsupportedCodec);
  // The localized candidate's confirmed negative-404 record authorized
  // skipping it entirely on this second request: its request count did
  // NOT increase, while English (genuinely re-tried, since its own
  // UnsupportedCodec outcome left no cache entry or negative record) did.
  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/ita/cards/01001.avif")),
      1);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/cards/01001.avif")),
           2);
}

void AssetRequestCoordinatorTests::
    allCandidatesNegative404CompletesWithoutNetworkRoundTrip() {
  MockHttpServer server;
  MockHttpServer::Response notFound;
  notFound.status = 404;
  notFound.reasonPhrase = "Not Found";
  server.setResponse(QStringLiteral("/img/arkham/ita/cards/01001.avif"),
                     notFound);
  server.setResponse(QStringLiteral("/img/arkham/cards/01001.avif"), notFound);
  server.setResponse(QStringLiteral("/img/arkham/cards/01001a.avif"), notFound);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  AssetKey key =
      makeCardKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
                  QStringLiteral("01001"));
  key.locale = QStringLiteral("it");

  std::optional<Result> firstResult;
  coordinator.request(key, [&](Result r) { firstResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return firstResult.has_value(); }, 5000));
  QVERIFY(!bool(*firstResult));
  QCOMPARE(firstResult->error().code, AssetErrorCode::NotFound);
  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/ita/cards/01001.avif")),
      1);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/cards/01001.avif")),
           1);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/cards/01001a.avif")),
           1);

  std::optional<Result> secondResult;
  coordinator.request(key, [&](Result r) { secondResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return secondResult.has_value(); }, 5000));
  QVERIFY(!bool(*secondResult));
  QCOMPARE(secondResult->error().code, AssetErrorCode::NotFound);
  // Every candidate carried a confirmed negative-404 record: the second,
  // logically identical request resolves immediately with NO further
  // network round trip for any of them.
  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/ita/cards/01001.avif")),
      1);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/cards/01001.avif")),
           1);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/cards/01001a.avif")),
           1);
}

void AssetRequestCoordinatorTests::destructionNeverInvokesStaleCallback() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(200, 200);
  response.slowDrip = true;
  response.chunkSize = 16;
  response.chunkDelayMs = 100;
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  bool callbackFired = false;
  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
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
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
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
    cancellingAfterCompletionButBeforeQueuedDeliverySuppressesResult() {
  // cancellingImmediateCacheHitCompletionSuppressesDelivery() above covers
  // cancelling BEFORE the event loop has run at all -- i.e. before even
  // completeOperation() (the first of two queued hops for an immediate
  // completion) has run. This test targets the OTHER window the class
  // comment documents: cancel() must ALSO work after completeOperation()
  // has already run (moving the consumer out of m_handleToOperation) but
  // before that specific consumer's own queued delivery inside
  // dispatchToConsumers() has executed. QCoreApplication::sendPostedEvents
  // targeted at exactly this object processes only events already queued
  // at the time of the call -- a new event posted from WITHIN that call
  // (dispatchToConsumers()'s own queued delivery, posted while
  // completeOperation() is running) is deferred to the next call, giving
  // a fully deterministic way to observe the in-between state without
  // any timing-dependent QTest::qWait race.
  MockHttpServer server; // no responses registered: any network hit fails.

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);
  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = encodePng(4, 4);
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
  QVERIFY(handle.isValid());

  // Run exactly the first queued hop (completeOperation()), which moves
  // this consumer's handle out of m_handleToOperation and queues the
  // second hop (the actual delivery) -- but does not run that second hop
  // within this same call.
  QCoreApplication::sendPostedEvents(&coordinator, QEvent::MetaCall);
  QCOMPARE(callbackCount, 0);

  // The handle is no longer "in flight" by any naive definition, yet
  // cancel() here must still suppress the queued (but not yet delivered)
  // success result.
  coordinator.cancel(handle);

  // Now let the second hop run.
  QCoreApplication::sendPostedEvents(&coordinator, QEvent::MetaCall);

  QCOMPARE(callbackCount, 1);
  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::Cancelled);
}

void AssetRequestCoordinatorTests::
    diskHitWithValidatorsRevalidatesAndServesStaleOn304() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(32, 32); // must never be served to the caller
  response.etagForConditionalMatch = "\"stale-etag\"";
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);
  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = encodePng(4, 4);
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
  QCOMPARE((**result).encodedBytes, preSeeded.encodedBytes);
  QVERIFY(!(**result).decodedImage.isNull());
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);
  QCOMPARE(
      server.lastRequestHeaders(QStringLiteral("/img/arkham/sets/valid01.png"))
          .value("if-none-match"),
      QByteArrayLiteral("\"stale-etag\""));
}

void AssetRequestCoordinatorTests::
    notModifiedResponseWithRefreshedValidatorUpdatesCacheEntry() {
  // Regression test: RFC 7232 S4.1 permits a 304 response to carry a
  // refreshed validator even though it has no body -- a server MAY
  // rotate/extend its ETag at revalidation time without re-sending the
  // representation. AssetNetworkFetcher must surface that refreshed
  // validator and AssetRequestCoordinator/AssetCache must persist it, so
  // a LATER revalidation (e.g. after a process restart) sends the NEW
  // validator rather than replaying the original one forever.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(32, 32); // must never be served to the caller
  response.etagForConditionalMatch = "\"stale-etag\"";
  response.etagOn304Override = "\"refreshed-etag\"";
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache seedCache(cacheConfig);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);
  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = encodePng(4, 4);
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  preSeeded.etag = QStringLiteral("\"stale-etag\"");
  seedCache.store(cacheKey, preSeeded);

  // Restart #1 (fresh memory, forces the disk-hit revalidation path):
  // revalidates against the pre-seeded "stale-etag" and receives a 304
  // carrying the server's refreshed "refreshed-etag".
  {
    AssetCache restartedCache(cacheConfig);
    AssetRequestCoordinator coordinator(restartedCache, fetcher);
    std::optional<Result> result;
    coordinator.request(key, [&](Result r) { result = std::move(r); });
    QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));
    QVERIFY2(bool(*result), qPrintable(result->error().message));
    QCOMPARE(
        server
            .lastRequestHeaders(QStringLiteral("/img/arkham/sets/valid01.png"))
            .value("if-none-match"),
        QByteArrayLiteral("\"stale-etag\""));
  }

  // Restart #2 (another fresh AssetCache reading only from disk, after
  // the first coordinator/cache above has gone out of scope): if the
  // refreshed validator from the first 304 was actually persisted, this
  // revalidation attempt must send "refreshed-etag" as If-None-Match --
  // NOT the original "stale-etag" -- proving the refresh survived a
  // process restart rather than only updating an in-memory copy.
  {
    AssetCache restartedCache(cacheConfig);
    AssetRequestCoordinator coordinator(restartedCache, fetcher);
    std::optional<Result> result;
    coordinator.request(key, [&](Result r) { result = std::move(r); });
    QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));
    QVERIFY2(bool(*result), qPrintable(result->error().message));
    QCOMPARE(
        server
            .lastRequestHeaders(QStringLiteral("/img/arkham/sets/valid01.png"))
            .value("if-none-match"),
        QByteArrayLiteral("\"refreshed-etag\""));
  }
}

void AssetRequestCoordinatorTests::
    confirmedNotModifiedPromotesEntryToMemoryForSameProcessShortCircuit() {
  // Regression for a review finding: a successful 304 confirms the
  // disk-cached entry is still current, but touchAfterNotModified() only
  // updates ALREADY memory-resident entries in place (a no-op here, since
  // lookupDisk() intentionally withheld promoting a validator-carrying
  // entry -- see its .cpp comment). Without promoting the now-confirmed
  // entry into memory here, a second same-process request() for the
  // identical key would hit disk again and start a second, entirely
  // redundant conditional GET, defeating "a same-process memory hit
  // short-circuits the network entirely" and repeating the on-demand
  // decode every single time.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(32, 32); // must never be served to the caller
  response.etagForConditionalMatch = "\"stale-etag\"";
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);
  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = encodePng(4, 4);
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  preSeeded.etag = QStringLiteral("\"stale-etag\"");
  cache.store(cacheKey, preSeeded);

  // A fresh AssetCache instance (simulating a process restart with empty
  // memory) forces the FIRST request through the disk-hit-with-validators
  // path.
  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> firstResult;
  coordinator.request(key, [&](Result r) { firstResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return firstResult.has_value(); }, 5000));
  QVERIFY2(bool(*firstResult), qPrintable(firstResult->error().message));
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);

  // The entry must now be memory-resident, with a decoded image, purely
  // from the 304 confirmation above -- no second request() call needed to
  // observe the promotion.
  const auto memoryHit = restartedCache.lookupMemory(cacheKey);
  QVERIFY2(memoryHit.has_value(),
           "a confirmed-current entry must be promoted to memory after a "
           "304, not left disk-only");
  QVERIFY(!memoryHit->decodedImage.isNull());

  // A second request for the identical key must now short-circuit via
  // the memory hit: no additional network request at all.
  std::optional<Result> secondResult;
  coordinator.request(key, [&](Result r) { secondResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return secondResult.has_value(); }, 5000));
  QVERIFY2(bool(*secondResult), qPrintable(secondResult->error().message));
  QVERIFY(!(**secondResult).decodedImage.isNull());
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);
}

void AssetRequestCoordinatorTests::
    diskHitRevalidationReplacesEntryOnFresh200() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(32, 32);
  // No etagForConditionalMatch configured: the origin's content genuinely
  // changed, so it answers the conditional GET with a full fresh 200.
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
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
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);

  // The cache must actually be updated with the fresh content, not just
  // the in-memory result returned to this one caller.
  const auto updated = restartedCache.lookupMemory(cacheKey);
  QVERIFY(updated.has_value());
  QVERIFY(updated->encodedBytes != QByteArrayLiteral("old-bytes-now-outdated"));
}

void AssetRequestCoordinatorTests::
    diskHitRevalidationEvictsEntryAndAdvancesOn404() {
  // Regression for review item 5: a REVALIDATION 404 is the one
  // revalidation failure that must NOT fall back to "stale-if-error" --
  // the origin has authoritatively confirmed this exact candidate is
  // gone, so the stale entry is evicted and the request advances through
  // the remaining candidates exactly like a first-time miss. Uses a real
  // Card key (English + alt-front-fallback candidates) so there IS a
  // remaining candidate to advance to.
  MockHttpServer server;
  MockHttpServer::Response notFound;
  notFound.status = 404;
  notFound.reasonPhrase = "Not Found";
  server.setResponse(QStringLiteral("/img/arkham/cards/valid01.avif"),
                     notFound);
  MockHttpServer::Response altFront;
  altFront.contentType = "image/avif";
  altFront.body = minimalAvifFtypBox();
  server.setResponse(QStringLiteral("/img/arkham/cards/valid01a.avif"),
                     altFront);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeCardKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  QCOMPARE(candidates->size(), 2);
  const QString englishCacheKey =
      AssetCache::cacheKeyFor(candidates->at(0).url);
  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = minimalAvifFtypBox();
  preSeeded.contentType = QStringLiteral("image/avif");
  preSeeded.dimensions = QSize(4, 4);
  preSeeded.lastModified = QStringLiteral("Wed, 01 Jan 2020 00:00:00 GMT");
  cache.store(englishCacheKey, preSeeded);

  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  // The 404'd English candidate was evicted (never resurrected via
  // stale-if-error) and the request genuinely advanced to (and
  // requested) the alt-front-fallback candidate: its own outcome is this
  // environment's expected UnsupportedCodec (no Qt AVIF decode plugin),
  // proving real advancement rather than a hang or a false stale
  // success.
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::UnsupportedCodec);
  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/cards/valid01.avif")), 1);
  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/cards/valid01a.avif")),
      1);
  QVERIFY(!restartedCache.lookupDisk(englishCacheKey).has_value());
}

void AssetRequestCoordinatorTests::
    diskHitRevalidationServesStaleOnNon404Failure() {
  // Every OTHER revalidation failure (transport error, timeout, TLS
  // failure, 5xx, cancellation, integrity/codec failure) is NOT proof
  // the resource is gone: "stale-if-error" still applies, and a briefly-
  // unreachable or since-changed origin can never make previously
  // cached, already-displayed art disappear.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.status = 500;
  response.reasonPhrase = "Internal Server Error";
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);
  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = encodePng(4, 4);
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  preSeeded.lastModified = QStringLiteral("Wed, 01 Jan 2020 00:00:00 GMT");
  cache.store(cacheKey, preSeeded);

  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).encodedBytes, preSeeded.encodedBytes);
  QVERIFY(!(**result).decodedImage.isNull());
  // The stale entry is still on disk, untouched -- a non-404 failure
  // never evicts.
  QVERIFY(restartedCache.lookupDisk(cacheKey).has_value());
}

void AssetRequestCoordinatorTests::
    diskHitRevalidationCoalescesConcurrentIdenticalRequests() {
  // Regression for a review finding: request() started a brand-new
  // revalidation operation unconditionally on every disk-hit-with-
  // validators call, without first checking whether an identical AssetKey
  // was already being revalidated -- bypassing the coordinator's
  // coalescing guarantee and issuing a redundant conditional GET under
  // contention. Two concurrent request() calls for the same key, both
  // landing on the disk-hit-with-validators path, must coalesce onto the
  // SAME underlying revalidation operation, exactly like
  // coalescesConcurrentIdenticalRequests() above proves for the ordinary
  // (no-cache-entry) path.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(32, 32);
  // No etagForConditionalMatch configured: the origin answers the
  // conditional GET with a full fresh 200 (which -- unlike this mock
  // server's synchronous bodyless 304 path -- supports slowDrip), giving
  // a deterministic window in which both request() calls are reliably
  // in flight at once.
  response.slowDrip = true;
  response.chunkSize = 32;
  response.chunkDelayMs = 20;
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);
  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = QByteArrayLiteral("old-bytes-now-outdated");
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  preSeeded.etag = QStringLiteral("\"old-etag\"");
  cache.store(cacheKey, preSeeded);

  // A fresh AssetCache instance forces both requests through the
  // disk-hit path rather than the memory-hit path, which never
  // revalidates.
  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  int completions = 0;
  std::optional<Result> resultA;
  std::optional<Result> resultB;
  coordinator.request(key, [&](Result r) {
    ++completions;
    resultA = std::move(r);
  });
  // Issued immediately after, while the first revalidation is still in
  // flight: must coalesce onto the SAME underlying operation rather than
  // issuing a second conditional GET.
  coordinator.request(key, [&](Result r) {
    ++completions;
    resultB = std::move(r);
  });

  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 1);

  QVERIFY(QTest::qWaitFor([&]() { return completions == 2; }, 5000));

  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);
  QVERIFY(resultA.has_value());
  QVERIFY(resultB.has_value());
  QVERIFY2(bool(*resultA), qPrintable(resultA->error().message));
  QVERIFY2(bool(*resultB), qPrintable(resultB->error().message));
  QVERIFY((**resultA).encodedBytes !=
          QByteArrayLiteral("old-bytes-now-outdated"));
  QCOMPARE((**resultA).encodedBytes, (**resultB).encodedBytes);
}

void AssetRequestCoordinatorTests::
    diskHitAfterRestartDecodesOnDemandAndPublishesToMemory() {
  // Regression for a review finding: AssetCache never persists a decoded
  // QImage to disk (only encodedBytes/metadata are), so a CachedEntry
  // served from a disk hit -- e.g. after a simulated process restart --
  // always arrives with decodedImage null. Before ensureDecoded() existed,
  // AssetRequestCoordinator handed such an entry straight to the caller as
  // a SUCCESSFUL result, which would let AssetImageRequest reach
  // Status::Ready with an empty QImage. This test proves the disk-hit (no
  // validators) path now decodes on demand and never surfaces a null
  // image, AND that the freshly-decoded image is published back into the
  // memory cache so a subsequent lookupMemory() hit for the same key is
  // already decoded too.
  MockHttpServer server;
  // No response registered: this request must be served entirely from
  // the disk cache, with no network round trip at all.

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = encodePng(6, 6);
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(6, 6);
  // No etag/lastModified: this is the plain disk-hit-no-validators path,
  // not a revalidation.
  cache.store(cacheKey, preSeeded);
  // decodedImage is deliberately left null in the just-stored memory
  // entry too, to exactly simulate a post-restart disk hit: store()
  // itself never decodes, so the memory entry it just inserted also
  // carries a null decodedImage until something decodes it on demand.

  // A fresh AssetCache instance pointed at the same directory (empty
  // memory) forces this request through the disk-hit path.
  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QVERIFY2(!(**result).decodedImage.isNull(),
           "a successful disk-hit result must never carry a null "
           "decodedImage");
  QCOMPARE((**result).decodedImage.size(), QSize(6, 6));
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           0);

  // The decoded image must also have been published back into the memory
  // cache, so a subsequent lookupMemory() hit is already decoded.
  const auto memoryHit = restartedCache.lookupMemory(cacheKey);
  QVERIFY(memoryHit.has_value());
  QVERIFY2(!memoryHit->decodedImage.isNull(),
           "the just-decoded image must be published back into the "
           "memory cache");
}

void AssetRequestCoordinatorTests::
    malformedDiskEntryIsQuarantinedAndRefetchedFromNetwork() {
  // Review item 9 (self-consistent invalid disk entry poisons forever):
  // a disk hit whose stored bytes can no longer actually decode (magic-
  // byte mismatch here; corruption the payload's own sha256 check does
  // not itself catch, since that check only proves internal
  // self-consistency, not that the bytes are a genuine image) must NOT
  // simply surface a typed error and leave the same doomed entry cached
  // forever -- it must be quarantined (evicted from both memory and
  // disk) and the SAME candidate retried as a genuine network miss. When
  // that retry succeeds, the fresh bytes replace the quarantined ones.
  MockHttpServer server;
  MockHttpServer::Response fresh;
  fresh.contentType = "image/png";
  fresh.body = encodePng(6, 6);
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), fresh);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  AssetCache::CachedEntry preSeeded;
  // Not real PNG bytes: on-demand decode must fail, not silently succeed
  // with a null image.
  preSeeded.encodedBytes = QByteArrayLiteral("not-actually-a-png");
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  cache.store(cacheKey, preSeeded);

  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  // The retry genuinely happened over the network exactly once, and
  // succeeded with the FRESH bytes -- never the original malformed ones,
  // and never a hang/hard failure.
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);
  QVERIFY(bool(*result));
  QCOMPARE((**result).dimensions, QSize(6, 6));
  QVERIFY(!(**result).decodedImage.isNull());

  // The quarantined malformed bytes were genuinely replaced on disk, not
  // merely masked by a memory-only success.
  const auto onDisk = restartedCache.lookupDisk(cacheKey);
  QVERIFY(onDisk.has_value());
  QCOMPARE(onDisk->dimensions, QSize(6, 6));
  QVERIFY(onDisk->encodedBytes != preSeeded.encodedBytes);
}

void AssetRequestCoordinatorTests::
    quarantineRefetchFailureSurfacesFreshErrorWithoutLooping() {
  // Companion to the success case above: if the network refetch
  // triggered by quarantining a malformed disk entry ALSO fails (here,
  // a definitive 404), the operation completes with THAT fresh, genuine
  // outcome -- never the stale decode error, and never a second retry
  // (no infinite quarantine loop): exactly one network request is made.
  MockHttpServer server;
  MockHttpServer::Response notFound;
  notFound.status = 404;
  notFound.reasonPhrase = "Not Found";
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), notFound);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = QByteArrayLiteral("not-actually-a-png");
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  cache.store(cacheKey, preSeeded);

  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);
  QVERIFY(!bool(*result));
  // The fresh network outcome (NotFound), not the original decode error
  // (MagicBytesMismatch), is what the caller observes.
  QCOMPARE(result->error().code, AssetErrorCode::NotFound);
  QVERIFY(!restartedCache.lookupDisk(cacheKey).has_value());
}

void AssetRequestCoordinatorTests::
    unsupportedCodecDecodeFailureNeverQuarantinesValidBytes() {
  // Review item 9's explicit carve-out: AssetErrorCode::UnsupportedCodec
  // means the cached bytes are still perfectly valid -- this process
  // simply has no installed decoder for them right now (this build/CI
  // environment has no Qt AVIF plugin; see minimalAvifFtypBox()'s
  // comment) -- so, unlike a genuine integrity/format/limits failure,
  // this must NEVER evict the entry or trigger a network retry.
  MockHttpServer server;

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  const AssetKey key =
      makeCardKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  AssetCache::CachedEntry preSeeded;
  // Genuinely AVIF-shaped (passes magic-byte sniffing) but undecodable in
  // this environment -- NOT malformed/corrupt bytes.
  preSeeded.encodedBytes = minimalAvifFtypBox();
  preSeeded.contentType = QStringLiteral("image/avif");
  preSeeded.dimensions = QSize(4, 4);
  cache.store(cacheKey, preSeeded);

  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::UnsupportedCodec);
  // No network round trip at all: the cache hit's own decode failure was
  // never treated as grounds for a retry.
  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/cards/valid01.avif")), 0);
  // The still-valid bytes were never evicted.
  const auto onDisk = restartedCache.lookupDisk(cacheKey);
  QVERIFY(onDisk.has_value());
  QCOMPARE(onDisk->encodedBytes, preSeeded.encodedBytes);
}
