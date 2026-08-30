#include "AssetRequestCoordinatorTests.h"

#include "AssetCache.h"
#include "AssetLocator.h"
#include "AssetNetworkFetcher.h"
#include "AssetRequestCoordinator.h"
#include "MockHttpServer.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QTest>
#include <avif/avif.h>
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
// to (and no further than) the magic-byte sniff gate: since AVIF decode
// is now unconditional (real libavif, not an optional Qt plugin -- see
// review item 4), a genuine Card/HomebrewCard fetch of these bytes ends
// in AssetErrorCode::MalformedImage (libavif's own parse step correctly
// rejects a bare ftyp box with no meta/mdat as a structurally invalid
// container), not a successful decode -- proving every earlier gate
// (correct candidate URL requested, Content-Type match, magic-byte
// match) passed for real AVIF-shaped bytes.
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

// A tiny, genuinely valid, original (never-shipped) AVIF fixture encoded
// at test-runtime via libavif's own encoder API -- mirrors
// AssetNetworkFetcherTests.cpp's encodeAvifFixture() exactly (this file
// needs its own copy: AssetNetworkFetcherTests.cpp's is file-local to its
// own anonymous namespace). Unlike minimalAvifFtypBox() above (a bare
// container with no actual image content, used only to reach past the
// magic-byte sniff gate and then fail decode), this produces bytes that
// genuinely decode -- required for the round-3 items 12/14/15 tests
// below, which need a cache entry that can actually complete a real
// ensureDecoded() call rather than failing at MalformedImage.
QByteArray encodeAvifFixture(int width, int height) {
  avifImage *image = avifImageCreate(static_cast<uint32_t>(width),
                                     static_cast<uint32_t>(height),
                                     /*depth=*/8, AVIF_PIXEL_FORMAT_YUV420);
  if (image == nullptr) {
    qFatal("encodeAvifFixture() failed to allocate an avifImage");
  }

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  rgb.depth = 8;
  // avifRGBImageAllocatePixels() returns void in some libavif releases
  // this project must support (e.g. Ubuntu 22.04's packaged 0.9.3) and
  // avifResult in later ones -- its return value is therefore never
  // captured, for portability across both signatures. A null `pixels`
  // after the call is the one failure signal valid under every version.
  (void)avifRGBImageAllocatePixels(&rgb);
  if (rgb.pixels == nullptr) {
    avifImageDestroy(image);
    qFatal("encodeAvifFixture() failed to allocate RGB pixels");
  }
  for (uint32_t row = 0; row < rgb.height; ++row) {
    uint8_t *line = rgb.pixels + static_cast<size_t>(row) * rgb.rowBytes;
    for (uint32_t col = 0; col < rgb.width; ++col) {
      uint8_t *pixel = line + static_cast<size_t>(col) * 4;
      pixel[0] = 0x40;
      pixel[1] = 0x80;
      pixel[2] = 0xC0;
      pixel[3] = 0xFF;
    }
  }

  const avifResult toYuvResult = avifImageRGBToYUV(image, &rgb);
  avifRGBImageFreePixels(&rgb);
  if (toYuvResult != AVIF_RESULT_OK) {
    avifImageDestroy(image);
    qFatal("encodeAvifFixture() failed to convert RGB to YUV: %s",
           avifResultToString(toYuvResult));
  }

  avifEncoder *encoder = avifEncoderCreate();
  if (encoder == nullptr) {
    avifImageDestroy(image);
    qFatal("encodeAvifFixture() failed to allocate an avifEncoder");
  }
  encoder->speed = AVIF_SPEED_FASTEST;

  avifRWData output = AVIF_DATA_EMPTY;
  const avifResult writeResult = avifEncoderWrite(encoder, image, &output);
  avifEncoderDestroy(encoder);
  avifImageDestroy(image);
  if (writeResult != AVIF_RESULT_OK) {
    qFatal("encodeAvifFixture() failed to encode: %s",
           avifResultToString(writeResult));
  }

  QByteArray bytes(reinterpret_cast<const char *>(output.data),
                   static_cast<int>(output.size));
  avifRWDataFree(&output);
  return bytes;
}

// A real, validly-encoded multi-image ("avis"-brand) AVIF fixture --
// mirrors AssetNetworkFetcherTests.cpp's encodeAvifSequenceFixture()
// exactly (this file needs its own copy: that one is file-local to its
// own anonymous namespace). Used by
// diskCachedAvifSequenceIsQuarantinedAndRefetchedFromNetwork() below
// (review round-4 item 10) to prove that a disk-cached entry whose bytes
// decode to a multi-image AVIF is quarantined (MalformedImage -- see
// AssetAvifDecoder.cpp's imageCount != 1 branch) and refetched over the
// network exactly like any other malformed disk entry, rather than
// permanently poisoning the key or -- wrongly, before round-4 item 10 --
// being reported as UnsupportedCodec and never quarantined at all despite
// the bytes being perfectly decodable by this build's libavif backend.
QByteArray encodeAvifSequenceFixture(int frameCount) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  avifImage *image =
      avifImageCreate(kWidth, kHeight, /*depth=*/8, AVIF_PIXEL_FORMAT_YUV420);
  if (image == nullptr) {
    qFatal("encodeAvifSequenceFixture() failed to allocate an avifImage");
  }
  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  rgb.depth = 8;
  (void)avifRGBImageAllocatePixels(&rgb);
  if (rgb.pixels == nullptr) {
    avifImageDestroy(image);
    qFatal("encodeAvifSequenceFixture() failed to allocate RGB pixels");
  }
  for (uint32_t row = 0; row < rgb.height; ++row) {
    uint8_t *line = rgb.pixels + static_cast<size_t>(row) * rgb.rowBytes;
    for (uint32_t col = 0; col < rgb.width; ++col) {
      uint8_t *pixel = line + static_cast<size_t>(col) * 4;
      pixel[0] = 0x10;
      pixel[1] = 0x20;
      pixel[2] = 0x30;
      pixel[3] = 0xFF;
    }
  }
  const avifResult toYuvResult = avifImageRGBToYUV(image, &rgb);
  avifRGBImageFreePixels(&rgb);
  if (toYuvResult != AVIF_RESULT_OK) {
    avifImageDestroy(image);
    qFatal("encodeAvifSequenceFixture() failed to convert RGB to YUV: %s",
           avifResultToString(toYuvResult));
  }

  avifEncoder *encoder = avifEncoderCreate();
  if (encoder == nullptr) {
    avifImageDestroy(image);
    qFatal("encodeAvifSequenceFixture() failed to allocate an avifEncoder");
  }
  encoder->speed = AVIF_SPEED_FASTEST;
  encoder->timescale = 30;

  avifResult addResult = AVIF_RESULT_OK;
  for (int frame = 0; frame < frameCount; ++frame) {
    addResult = avifEncoderAddImage(encoder, image, /*durationInTimescales=*/1,
                                    AVIF_ADD_IMAGE_FLAG_NONE);
    if (addResult != AVIF_RESULT_OK) {
      break;
    }
  }
  avifImageDestroy(image);
  if (addResult != AVIF_RESULT_OK) {
    avifEncoderDestroy(encoder);
    qFatal("encodeAvifSequenceFixture() failed to add a frame: %s",
           avifResultToString(addResult));
  }

  avifRWData output = AVIF_DATA_EMPTY;
  const avifResult finishResult = avifEncoderFinish(encoder, &output);
  avifEncoderDestroy(encoder);
  if (finishResult != AVIF_RESULT_OK) {
    qFatal("encodeAvifSequenceFixture() failed to finish: %s",
           avifResultToString(finishResult));
  }
  QByteArray bytes(reinterpret_cast<const char *>(output.data),
                   static_cast<int>(output.size));
  avifRWDataFree(&output);
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
  // Round-9+ review: rooted under home so this exercises AssetCache's
  // real home-anchored directory-resolution code path -- see
  // AssetCacheTests::init()'s comment for the full rationale.
  m_tempDir = std::make_unique<QTemporaryDir>(
      QDir::homePath() +
      QStringLiteral("/.arkham-asset-coordinator-test-XXXXXX"));
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

void AssetRequestCoordinatorTests::
    keysDifferingOnlyByBackIdentityFieldsNeverCoalesce() {
  // Fresh-cumulative-review regression: canonicalOperationKey() joined
  // assetBase/category/identifier/side/locale/homebrewNamespace/
  // mutationId/format, but omitted backKind/otherSideIdentifier/
  // customBackFilename entirely -- even though AssetKey::operator==
  // compares all three (see AssetTypes.h). Two AssetSide::Back requests
  // with identical everything else but a DIFFERENT explicit other-side
  // card code would therefore incorrectly coalesce onto the same
  // in-flight operation, and whichever request's candidate the shared
  // operation actually resolved/fetched would silently be delivered to
  // BOTH consumers -- one of them receiving the wrong card's back art
  // entirely.
  //
  // This uses CardBackKind::ExplicitOtherSide with two different
  // `otherSideIdentifier` values (identifier itself is empty for this
  // backKind, so it alone could never distinguish the two keys): the
  // resulting AssetKeys are NOT operator==-equal, and -- unlike the
  // locale-only regression above, which deliberately keeps both
  // candidates resolving to the SAME URL to isolate the operation-key
  // encoding itself -- these two also resolve to two genuinely DIFFERENT
  // candidate URLs/card art, so a coalescing bug here would be
  // observable as one consumer receiving the wrong image's bytes, not
  // merely a bookkeeping-only discrepancy.
  MockHttpServer server;

  MockHttpServer::Response responseOne;
  responseOne.contentType = "image/avif";
  responseOne.body = encodeAvifFixture(8, 8);
  responseOne.slowDrip = true; // both requests must overlap in flight
  responseOne.chunkSize = 64;
  responseOne.chunkDelayMs = 20;
  server.setResponse(QStringLiteral("/img/arkham/cards/01001.avif"),
                     responseOne);

  MockHttpServer::Response responseTwo;
  responseTwo.contentType = "image/avif";
  responseTwo.body = encodeAvifFixture(16, 16);
  responseTwo.slowDrip = true;
  responseTwo.chunkSize = 64;
  responseTwo.chunkDelayMs = 20;
  server.setResponse(QStringLiteral("/img/arkham/cards/01002.avif"),
                     responseTwo);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  const QString base = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
  const AssetOutcome<ValidatedAssetSource> validatedBase =
      ValidatedAssetSource::fromRaw(base);
  QVERIFY2(bool(validatedBase), qPrintable(validatedBase.error().message));

  AssetKey keyOne;
  keyOne.assetBase = *validatedBase;
  keyOne.category = AssetCategory::Card;
  keyOne.side = AssetSide::Back;
  keyOne.backKind = CardBackKind::ExplicitOtherSide;
  keyOne.otherSideIdentifier = QStringLiteral("01001");
  keyOne.format = AssetFormat::Avif;

  AssetKey keyTwo = keyOne;
  keyTwo.otherSideIdentifier = QStringLiteral("01002");

  QVERIFY(!(keyOne == keyTwo));

  const auto candidatesOne = AssetLocator::resolveCandidates(keyOne);
  QVERIFY2(bool(candidatesOne), qPrintable(candidatesOne.error().message));
  const auto candidatesTwo = AssetLocator::resolveCandidates(keyTwo);
  QVERIFY2(bool(candidatesTwo), qPrintable(candidatesTwo.error().message));
  QVERIFY(candidatesOne->first().url != candidatesTwo->first().url);

  int completions = 0;
  std::optional<Result> resultOne;
  std::optional<Result> resultTwo;
  coordinator.request(keyOne, [&](Result r) {
    ++completions;
    resultOne = std::move(r);
  });
  coordinator.request(keyTwo, [&](Result r) {
    ++completions;
    resultTwo = std::move(r);
  });

  // The real assertion: two AssetKeys that differ only by back-identity
  // fields must occupy two SEPARATE in-flight operations, never one
  // merged operation.
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 2);

  QVERIFY(QTest::qWaitFor([&]() { return completions == 2; }, 5000));
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/cards/01001.avif")),
           1);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/cards/01002.avif")),
           1);

  QVERIFY2(bool(*resultOne), qPrintable(resultOne->error().message));
  QVERIFY2(bool(*resultTwo), qPrintable(resultTwo->error().message));
  // Each consumer must receive exactly the bytes for ITS OWN requested
  // card, never the other's -- the failure mode a coalescing bug here
  // would actually produce.
  QCOMPARE((**resultOne).encodedBytes, responseOne.body);
  QCOMPARE((**resultTwo).encodedBytes, responseTwo.body);
  QCOMPARE((**resultOne).dimensions, QSize(8, 8));
  QCOMPARE((**resultTwo).dimensions, QSize(16, 16));
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

  // minimalAvifFtypBox() is a bare "ftyp" box only (no meta/mdat), so
  // libavif's own parse step rejects it as a structurally invalid
  // container once past magic-byte sniffing: the FINAL typed outcome for
  // this genuinely AVIF-shaped-but-empty body is MalformedImage, not a
  // successful decode. That is expected and, combined with the two
  // requestCount assertions below, still fully proves the fallback
  // mechanism itself: the coordinator correctly advanced past the
  // English candidate's definitive 404 to the alternate-front candidate,
  // requested exactly the right URL there, and got far enough into
  // validating that response (past Content-Type and magic-byte checks)
  // to reach the real decode attempt -- it did not stop early, retry the
  // same candidate, or skip straight past validation.
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
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
  // outcome is MalformedImage (minimalAvifFtypBox() is a bare, content-
  // less "ftyp" box that libavif's own parse step rejects once past
  // magic-byte sniffing), NOT a success carrying the pre-seeded English
  // bytes. If the coordinator had (incorrectly) shortcut straight to the
  // cached English entry, this would instead be a success whose
  // encodedBytes equal preSeededEnglish.
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
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
  // English is genuinely AVIF-shaped but has no actual image content
  // (minimalAvifFtypBox() is a bare "ftyp" box): MalformedImage, not a
  // success -- deterministic and, crucially, never stored in the cache
  // (only a successful outcome is ever cache-stored), so a second
  // identical request cannot shortcut via an English cache hit either --
  // it can ONLY shortcut via the localized candidate's negative-404
  // record, which is exactly what this test proves.
  QVERIFY(!bool(*firstResult));
  QCOMPARE(firstResult->error().code, AssetErrorCode::MalformedImage);
  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/ita/cards/01001.avif")),
      1);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/cards/01001.avif")),
           1);

  std::optional<Result> secondResult;
  coordinator.request(key, [&](Result r) { secondResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return secondResult.has_value(); }, 5000));

  QVERIFY(!bool(*secondResult));
  QCOMPARE(secondResult->error().code, AssetErrorCode::MalformedImage);
  // The localized candidate's confirmed negative-404 record authorized
  // skipping it entirely on this second request: its request count did
  // NOT increase, while English (genuinely re-tried, since its own
  // MalformedImage outcome left no cache entry or negative record) did.
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
  {
    // Cumulative review (independent re-review, HIGH, "shared root
    // authority incomplete"): every same-root AssetCache sibling now
    // genuinely shares ONE memory cache for as long as any of them is
    // still alive -- see RootAuthority's own comment -- so this seeding
    // instance MUST go out of scope (releasing its share of the shared
    // authority) before constructing "restartedCache" below, or
    // restartedCache would simply join the still-live authority and
    // inherit this seeded entry already memory-resident, never
    // exercising the disk-hit-with-validators path this test exists to
    // exercise at all.
    AssetCache seedCache(cacheConfig);
    seedCache.store(cacheKey, preSeeded);
  }

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
  {
    // See diskHitWithValidatorsRevalidatesAndServesStaleOn304()'s
    // identical comment: this seeding instance must go out of scope
    // before any "restarted" instance is constructed, or the latter
    // would join the still-live shared authority and inherit this
    // entry already memory-resident.
    AssetCache seedCache(cacheConfig);
    seedCache.store(cacheKey, preSeeded);
  }

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
  {
    // See diskHitWithValidatorsRevalidatesAndServesStaleOn304()'s
    // identical comment: this seeding instance must go out of scope
    // before "restartedCache" is constructed, or the latter would join
    // the still-live shared authority and inherit this entry already
    // memory-resident.
    AssetCache seedCache(cacheConfig);
    seedCache.store(cacheKey, preSeeded);
  }

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
  // requested) the alt-front-fallback candidate: its own outcome is
  // MalformedImage (minimalAvifFtypBox() has no actual image content),
  // proving real advancement rather than a hang or a false stale
  // success.
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
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
    concurrentIdenticalRequestsForADiskHitCoalesceIntoASingleDecode() {
  // Review round-4 item 6: previously, coalescing (findInFlightOperation())
  // was only ever consulted on the revalidation and network-fetch paths;
  // a plain cache hit (memory OR disk-with-no-validators) unconditionally
  // created its OWN new Operation and queued its OWN independent
  // ensureDecoded() call for every single request() call, even when
  // several concurrent calls named the exact same AssetKey. This
  // reproduces exactly that burst -- two request() calls for the same
  // key, issued back-to-back before either's queued completion has run,
  // against a disk-only entry (no decodedImage yet, simulating a
  // post-restart disk hit exactly like
  // diskHitAfterRestartDecodesOnDemandAndPublishesToMemory() above) --
  // and asserts there is only ONE in-flight operation (and therefore only
  // one decode) shared by both consumers, not two independent ones.
  MockHttpServer server;
  // No response registered: this request must be served entirely from
  // the disk cache, with no network round trip at all -- if coalescing
  // regressed such that either consumer somehow fell through to a
  // network fetch, MockHttpServer would report a 404 for the
  // unregistered path and this test would fail via a non-decoded error
  // result.

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
  cache.store(cacheKey, preSeeded);

  // A fresh AssetCache instance pointed at the same directory (empty
  // memory) forces both requests through the disk-hit path.
  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  int completions = 0;
  std::optional<Result> resultA;
  std::optional<Result> resultB;
  coordinator.request(key, [&](Result r) {
    ++completions;
    resultA = std::move(r);
  });
  // Issued immediately after, before either request's queued completion
  // has run: must join the SAME underlying operation rather than
  // performing a second, independent disk read + decode.
  coordinator.request(key, [&](Result r) {
    ++completions;
    resultB = std::move(r);
  });

  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 1);

  QVERIFY(QTest::qWaitFor([&]() { return completions == 2; }, 5000));

  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           0);
  QVERIFY2(bool(*resultA), qPrintable(resultA->error().message));
  QVERIFY2(bool(*resultB), qPrintable(resultB->error().message));
  QVERIFY2(!(**resultA).decodedImage.isNull(),
           "a successful disk-hit result must never carry a null "
           "decodedImage");
  QVERIFY2(!(**resultB).decodedImage.isNull(),
           "a successful disk-hit result must never carry a null "
           "decodedImage");
  QCOMPARE((**resultA).decodedImage.size(), QSize(6, 6));
  QCOMPARE((**resultB).decodedImage.size(), QSize(6, 6));
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
    diskMetadataDimensionMismatchIsQuarantinedAndRefetched() {
  // Round-4 review item 9: the encoded bytes themselves are perfectly
  // valid and self-consistent (a genuine 6x6 PNG whose sha256Hex/
  // encodedSize the payload-integrity check in AssetCache::lookupDisk()
  // has no reason to reject) -- but the metadata describing them claims
  // a dimension (40x40) that does not match what those bytes actually,
  // truly decode to. This is exactly the "well-formed but wrong"
  // metadata case AssetCache::lookupDisk()'s own payload-hash check
  // cannot catch (it only proves the PAYLOAD matches its own hash, never
  // that the METADATA agrees with the payload's real decoded content).
  // ensureDecoded()'s cross-validation must catch this on the very first
  // read, quarantine the entry, and transparently refetch exactly once.
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
  // Genuinely valid, decodable 6x6 PNG bytes -- only the persisted
  // dimensions metadata is wrong/stale/tampered.
  preSeeded.encodedBytes = encodePng(6, 6);
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(40, 40); // does not match the real bytes
  cache.store(cacheKey, preSeeded);

  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).dimensions, QSize(6, 6));

  const auto onDisk = restartedCache.lookupDisk(cacheKey);
  QVERIFY(onDisk.has_value());
  QCOMPARE(onDisk->dimensions, QSize(6, 6));
}

void AssetRequestCoordinatorTests::
    diskMetadataContentTypeMismatchIsQuarantinedAndRefetched() {
  // Round-4 review item 9: same principle as the dimension-mismatch test
  // above, but for the persisted contentType field -- genuinely valid
  // PNG bytes whose metadata claims a JPEG contentType. The payload's
  // own magic bytes still match the candidate's actual (PNG) format, so
  // decodeAndValidate() succeeds; only the separately-persisted metadata
  // field disagrees, which only ensureDecoded()'s explicit cross-check
  // can catch.
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
  preSeeded.encodedBytes = encodePng(6, 6);
  preSeeded.contentType = QStringLiteral("image/jpeg"); // wrong on purpose
  preSeeded.dimensions = QSize(6, 6);
  cache.store(cacheKey, preSeeded);

  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).dimensions, QSize(6, 6));

  const auto onDisk = restartedCache.lookupDisk(cacheKey);
  QVERIFY(onDisk.has_value());
  QCOMPARE(onDisk->contentType, QStringLiteral("image/png"));
}

void AssetRequestCoordinatorTests::
    diskCachedAvifSequenceIsQuarantinedAndRefetchedFromNetwork() {
  // Review round-4 item 10: a disk-cached AVIF entry whose bytes decode
  // to a valid multi-image ("avis"-brand) container must be classified
  // MalformedImage (see AssetAvifDecoder.cpp's imageCount != 1 branch,
  // fixed this round), which IS quarantine-worthy (isQuarantineWorthy()),
  // NOT UnsupportedCodec (which never quarantines valid-but-undecodable-
  // by-this-build bytes). Before this fix, a multi-image AVIF wrongly
  // classified as UnsupportedCodec would never be quarantined/evicted --
  // this same corrupt-shaped entry would poison every future request for
  // this key forever, since isQuarantineWorthy() explicitly never treats
  // UnsupportedCodec as eligible for eviction+retry. This proves it is
  // instead evicted and the exact same candidate retried as a genuine
  // network miss, exactly like any other malformed disk entry.
  MockHttpServer server;
  MockHttpServer::Response fresh;
  fresh.contentType = "image/avif";
  fresh.body = encodeAvifFixture(6, 6);
  server.setResponse(QStringLiteral("/img/arkham/cards/valid01.avif"), fresh);

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
  preSeeded.encodedBytes = encodeAvifSequenceFixture(3);
  preSeeded.contentType = QStringLiteral("image/avif");
  preSeeded.dimensions = QSize(4, 4);
  cache.store(cacheKey, preSeeded);

  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/cards/valid01.avif")), 1);
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).dimensions, QSize(6, 6));
  QVERIFY(!(**result).decodedImage.isNull());

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
    delayedStaleFetchSuccessNeverOverwritesNewerCrossLogicalKeyCacheEntry() {
  // Round-6 item 8 ("coalescing by logical AssetKey duplicates
  // network/decode for aliases resolving to same candidate/cache key").
  // This test previously proved the round-3-item-14 CAS by forcing two
  // DIFFERENT AssetKeys (differing only in `locale`, which SetIcon
  // ignores when resolving candidates) that resolve to the SAME
  // candidate/cache key to issue TWO independent, genuinely concurrent
  // network fetches with a rigged completion order. That scenario is no
  // longer constructible: startCandidate() now coalesces any second
  // operation reaching the identical (cacheKey, format, no-conditional-
  // headers) combination onto the FIRST operation's already in-flight
  // CandidateAttempt (see AssetRequestCoordinator.h's CandidateAttempt
  // comment) instead of issuing a second HTTP request at all -- so there
  // is no longer a "late, superseded" completion to race against a
  // "first-published" one for this exact scenario.
  //
  // This test now proves that coalescing directly: two different logical
  // AssetKeys requested while genuinely still in flight together (the
  // response is slow-dripped specifically so both requests are issued
  // before either can complete) share exactly ONE underlying
  // CandidateAttempt and result in exactly ONE HTTP request, yet BOTH
  // consumers still receive their own independent, correct completion
  // with the identical fetched bytes, and the disk cache ends up holding
  // exactly those bytes -- never split-brained between two different
  // outcomes.
  MockHttpServer server;
  const QString path = QStringLiteral("/img/arkham/sets/valid01.png");

  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8);
  response.slowDrip = true;
  response.chunkSize = 32;
  response.chunkDelayMs = 20;
  server.setResponse(path, response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  AssetKey keyOld =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyOld.locale = QString();
  AssetKey keyNew = keyOld;
  keyNew.locale = QStringLiteral("fr");
  QVERIFY(!(keyOld == keyNew));

  const auto candidatesOld = AssetLocator::resolveCandidates(keyOld);
  QVERIFY(bool(candidatesOld));
  const auto candidatesNew = AssetLocator::resolveCandidates(keyNew);
  QVERIFY(bool(candidatesNew));
  QCOMPARE(candidatesOld->first().url, candidatesNew->first().url);
  const QString cacheKey = AssetCache::cacheKeyFor(candidatesOld->first().url);

  std::optional<Result> resultOld;
  std::optional<Result> resultNew;
  coordinator.request(keyOld, [&](Result r) { resultOld = std::move(r); });
  coordinator.request(keyNew, [&](Result r) { resultNew = std::move(r); });

  // Both operations are genuinely in flight, sharing exactly one
  // CandidateAttempt -- the decisive proof that this is coalesced
  // transport, not two independent fetches racing.
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 2);
  QCOMPARE(coordinator.candidateAttemptCountForTesting(), 1);
  QCOMPARE(coordinator.candidateAttemptSubscriberCountForTesting(cacheKey), 2);

  QVERIFY(QTest::qWaitFor(
      [&]() { return resultOld.has_value() && resultNew.has_value(); }, 5000));

  // Exactly one HTTP request reached the server -- the second logical
  // key's request never issued its own.
  QCOMPARE(server.requestCount(path), 1);
  QCOMPARE(coordinator.candidateAttemptCountForTesting(), 0);

  QVERIFY2(bool(*resultOld), qPrintable(resultOld->error().message));
  QVERIFY2(bool(*resultNew), qPrintable(resultNew->error().message));
  QCOMPARE((**resultOld).encodedBytes, response.body);
  QCOMPARE((**resultNew).encodedBytes, response.body);

  const auto onDisk = cache.lookupDisk(cacheKey);
  QVERIFY(onDisk.has_value());
  QCOMPARE(onDisk->encodedBytes, response.body);
}

void AssetRequestCoordinatorTests::
    delayedStaleRevalidationSuccessAfterDefinitive404NeverResurrectsEvictedEntry() {
  // Round-6 item 8 (see the identical comment on the test above this
  // one): two DIFFERENT logical AssetKeys (differing only in `locale`)
  // revalidating the SAME pre-seeded disk entry -- same cacheKey, same
  // format, and (since both read the identical on-disk etag) the same
  // conditional-validator snapshot -- now coalesce onto exactly one
  // shared conditional GET (see startRevalidation()'s CandidateAttempt
  // join) instead of racing two independent ones. This test proves that
  // coalescing for the revalidation path specifically, then reuses the
  // original test's tail to prove the round-4-item-5 invalidate()-on-404
  // behavior (eviction, negative-404 recording, TTL, and restart
  // durability) still holds exactly as before -- now driven by ONE
  // shared 404 response instead of a racing pair.
  MockHttpServer server;
  const QString path = QStringLiteral("/img/arkham/sets/valid01.png");

  MockHttpServer::Response notFoundResponse;
  notFoundResponse.status = 404;
  notFoundResponse.reasonPhrase = "Not Found";
  notFoundResponse.slowDrip = true;
  notFoundResponse.chunkSize = 4;
  notFoundResponse.chunkDelayMs = 20;
  server.setResponse(path, notFoundResponse);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;

  AssetKey keyFresh =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyFresh.locale = QString();
  AssetKey keyNotFound = keyFresh;
  keyNotFound.locale = QStringLiteral("fr");
  QVERIFY(!(keyFresh == keyNotFound));

  const auto candidates = AssetLocator::resolveCandidates(keyFresh);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  // Written through a throwaway AssetCache instance, then read back
  // through a BRAND-NEW instance pointed at the same directory (a
  // simulated restart) so the entry exists ONLY on disk, never in this
  // test's own actual AssetCache's in-process memory cache. A memory
  // hit is served immediately with no revalidation at all (see
  // request()'s own comment) -- exercising that path here would
  // silently bypass startRevalidation() (and this coalescing fix)
  // entirely.
  {
    AssetCache seedCache(cacheConfig);
    AssetCache::CachedEntry preSeeded;
    preSeeded.encodedBytes = QByteArrayLiteral("seed-bytes-being-revalidated");
    preSeeded.contentType = QStringLiteral("image/png");
    preSeeded.dimensions = QSize(4, 4);
    preSeeded.etag = QStringLiteral("\"shared-etag\"");
    seedCache.store(cacheKey, preSeeded);
  }
  AssetCache cache(cacheConfig);
  QVERIFY(cache.lookupDisk(cacheKey).has_value());
  QVERIFY(!cache.lookupMemory(cacheKey).has_value());

  AssetRequestCoordinator coordinator(cache, fetcher);

  std::optional<Result> resultFresh;
  std::optional<Result> resultNotFound;
  coordinator.request(keyFresh, [&](Result r) { resultFresh = std::move(r); });
  coordinator.request(keyNotFound,
                      [&](Result r) { resultNotFound = std::move(r); });

  // Both operations share exactly one coalesced revalidation attempt.
  QCOMPARE(coordinator.candidateAttemptCountForTesting(), 1);
  QCOMPARE(coordinator.candidateAttemptSubscriberCountForTesting(cacheKey), 2);

  QVERIFY(QTest::qWaitFor(
      [&]() { return resultFresh.has_value() && resultNotFound.has_value(); },
      5000));

  // Exactly one conditional GET reached the server.
  QCOMPARE(server.requestCount(path), 1);

  // Both consumers observe the SAME definitive 404 -- coalescing means
  // there is no "fresh" vs "not found" split outcome anymore for this
  // exact scenario; both share the one real response.
  QVERIFY(!bool(*resultFresh));
  QCOMPARE(resultFresh->error().code, AssetErrorCode::NotFound);
  QVERIFY(!bool(*resultNotFound));
  QCOMPARE(resultNotFound->error().code, AssetErrorCode::NotFound);

  // The decisive assertion: the pre-seeded entry was evicted by the
  // shared 404's invalidate() call (round-4 item 5), never left as an
  // untouched stale success masked only by the negative-404 TTL.
  QVERIFY(!cache.lookupDisk(cacheKey).has_value());
  QVERIFY(!cache.lookupMemory(cacheKey).has_value());

  // The negative-404 record survives intact: a third request for the
  // SAME cache key completes as NotFound with NO further network round
  // trip at all.
  AssetKey keyThird = keyFresh;
  keyThird.locale = QStringLiteral("de");
  std::optional<Result> resultThird;
  coordinator.request(keyThird, [&](Result r) { resultThird = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return resultThird.has_value(); }, 5000));
  QVERIFY(!bool(*resultThird));
  QCOMPARE(resultThird->error().code, AssetErrorCode::NotFound);
  QCOMPARE(server.requestCount(path), 1);
}

void AssetRequestCoordinatorTests::unsupportedCodecIsNeverQuarantineWorthy() {
  // Review item 9's explicit carve-out: AssetErrorCode::UnsupportedCodec
  // means cached bytes are still perfectly valid -- this process simply
  // has no installed decoder for them right now -- so, unlike a genuine
  // integrity/format/limits failure, it must never evict the entry or
  // trigger a network retry. This directly and deterministically tests
  // isQuarantineWorthy()'s classification for every AssetErrorCode value
  // that ensureDecoded()/decodeAndValidate() can actually produce, rather
  // than relying on some specific crafted byte sequence to organically
  // provoke each code through a real decode attempt: since AVIF decode is
  // now unconditional (real libavif, not an optional Qt plugin -- see
  // review item 4), UnsupportedCodec for AVIF can now only arise from a
  // genuinely broken/incomplete libavif build (missing AV1 codec backend
  // entirely), which is not something a portable, deterministic test
  // fixture can reliably provoke -- but the classification itself is
  // pure, stateless, and fully specified, so it is tested directly here.
  QVERIFY(!AssetRequestCoordinator::isQuarantineWorthy(
      AssetErrorCode::UnsupportedCodec));

  // Every genuine integrity/format/limits failure IS quarantine-worthy:
  // the cached bytes themselves are now known bad against a fresh,
  // current-limits re-check.
  QVERIFY(AssetRequestCoordinator::isQuarantineWorthy(
      AssetErrorCode::MagicBytesMismatch));
  QVERIFY(AssetRequestCoordinator::isQuarantineWorthy(
      AssetErrorCode::MalformedImage));
  QVERIFY(AssetRequestCoordinator::isQuarantineWorthy(
      AssetErrorCode::DimensionTooLarge));
  QVERIFY(AssetRequestCoordinator::isQuarantineWorthy(
      AssetErrorCode::PixelBudgetExceeded));
  QVERIFY(AssetRequestCoordinator::isQuarantineWorthy(
      AssetErrorCode::CacheCorrupt));

  // Transport-class codes are neither reachable from ensureDecoded() nor
  // quarantine-worthy: a cache-sourced entry that fails to decode never
  // surfaces these, but the classification must still fail closed (never
  // quarantine) for any code it was not explicitly designed for.
  QVERIFY(
      !AssetRequestCoordinator::isQuarantineWorthy(AssetErrorCode::Transport));
  QVERIFY(!AssetRequestCoordinator::isQuarantineWorthy(
      AssetErrorCode::ContentTypeMismatch));
}

void AssetRequestCoordinatorTests::
    localized404AdvanceServesAlreadyCachedEnglishCandidateWithoutNetwork() {
  // Review round-3 item 12: startCandidate()'s/startRevalidation()'s
  // 404-driven advance to the next candidate used to jump straight to a
  // fresh network fetch, bypassing the negative-404/memory/disk cache
  // checks that request()'s OWN initial scan already performs for the
  // first candidate -- so a localized candidate confirmed absent
  // (genuine 404) mid-operation could never discover that the NEXT
  // candidate (English) was already validly cached, needlessly
  // refetching it over the network instead. advanceCandidates() is the
  // single shared path both the initial attempt and every later
  // 404-driven advance now route through, closing that gap.
  //
  // The English candidate's path is deliberately left UNREGISTERED on
  // the mock server: if the coordinator (incorrectly) fell through to a
  // fresh network fetch for it instead of discovering the pre-seeded
  // cache entry, its request count would be nonzero below.
  MockHttpServer server;
  MockHttpServer::Response notFound;
  notFound.status = 404;
  notFound.reasonPhrase = "Not Found";
  server.setResponse(QStringLiteral("/img/arkham/ita/cards/01001.avif"),
                     notFound);

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
  QVERIFY2(candidates->at(1)
               .url.toString(QUrl::FullyEncoded)
               .endsWith(QStringLiteral("/img/arkham/cards/01001.avif")),
           qPrintable(candidates->at(1).url.toString(QUrl::FullyEncoded)));
  const QString englishCacheKey =
      AssetCache::cacheKeyFor(candidates->at(1).url);

  AssetCache::CachedEntry preSeededEnglish;
  preSeededEnglish.encodedBytes = encodeAvifFixture(32, 24);
  preSeededEnglish.contentType = QStringLiteral("image/avif");
  preSeededEnglish.dimensions = QSize(32, 24);
  cache.store(englishCacheKey, preSeededEnglish);

  AssetRequestCoordinator coordinator(cache, fetcher);
  std::optional<Result> result;
  coordinator.request(key, [&](Result r) { result = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).encodedBytes, preSeededEnglish.encodedBytes);
  QVERIFY(!(**result).decodedImage.isNull());

  QCOMPARE(
      server.requestCount(QStringLiteral("/img/arkham/ita/cards/01001.avif")),
      1);
  // The decisive assertion: the already-cached English candidate was
  // served from cache, never re-requested over the network.
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/cards/01001.avif")),
           0);
}

void AssetRequestCoordinatorTests::
    negative404RecordExpiresAfterTtlAndIsRefetched() {
  // Review round-3 item 13: a confirmed negative-404 record is no
  // longer permanent -- it expires after a bounded TTL
  // (kNegative404TtlMs), using an injectable monotonic clock
  // (setMonotonicNowForTesting()) so this test can deterministically
  // simulate the passage of time without a real sleep.
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
  AssetRequestCoordinator coordinator(cache, fetcher);

  qint64 fakeNowMs = 1'000'000;
  coordinator.setMonotonicNowForTesting([&]() { return fakeNowMs; });

  AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));

  std::optional<Result> firstResult;
  coordinator.request(key, [&](Result r) { firstResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return firstResult.has_value(); }, 5000));
  QVERIFY(!bool(*firstResult));
  QCOMPARE(firstResult->error().code, AssetErrorCode::NotFound);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);

  // Still well within the TTL: the negative record authorizes skipping
  // the candidate entirely -- no new network round trip.
  fakeNowMs += 60'000; // +1 minute, TTL is 5 minutes
  std::optional<Result> secondResult;
  coordinator.request(key, [&](Result r) { secondResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return secondResult.has_value(); }, 5000));
  QVERIFY(!bool(*secondResult));
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);

  // Past the TTL: the negative record has lazily expired, so this
  // candidate must be genuinely re-tried over the network.
  fakeNowMs += 6 * 60'000; // +6 more minutes -- 7 total, past the 5-minute TTL
  std::optional<Result> thirdResult;
  coordinator.request(key, [&](Result r) { thirdResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return thirdResult.has_value(); }, 5000));
  QVERIFY(!bool(*thirdResult));
  QCOMPARE(thirdResult->error().code, AssetErrorCode::NotFound);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           2);
}

void AssetRequestCoordinatorTests::
    cancellingOneCoalescedCrossLogicalKeySubscriberNeverAbortsAnother() {
  // Round-6 item 8's explicit "preserving independent fallback/cancel
  // semantics" requirement, for the cross-logical-key coalescing case
  // specifically (cancellingOneConsumerNeverAffectsAnother above only
  // covers two requests for the IDENTICAL AssetKey, which was already
  // coalesced before this round). Two DIFFERENT logical AssetKeys
  // (differing only in `locale`) resolve to the same candidate/cache key
  // and are issued while genuinely still in flight together, sharing one
  // CandidateAttempt. Cancelling ONE of them must not abort the shared
  // fetch: the other subscriber must still complete successfully, and
  // only cancelling BOTH may the underlying fetch actually be aborted.
  MockHttpServer server;
  const QString path = QStringLiteral("/img/arkham/sets/valid01.png");
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8);
  response.slowDrip = true;
  response.chunkSize = 32;
  response.chunkDelayMs = 20;
  server.setResponse(path, response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  AssetKey keyA =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyA.locale = QString();
  AssetKey keyB = keyA;
  keyB.locale = QStringLiteral("fr");
  QVERIFY(!(keyA == keyB));

  const auto candidates = AssetLocator::resolveCandidates(keyA);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  std::optional<Result> cancelledResult;
  std::optional<Result> survivorResult;
  const auto cancelledHandle = coordinator.request(
      keyA, [&](Result r) { cancelledResult = std::move(r); });
  coordinator.request(keyB, [&](Result r) { survivorResult = std::move(r); });

  QCOMPARE(coordinator.candidateAttemptCountForTesting(), 1);
  QCOMPARE(coordinator.candidateAttemptSubscriberCountForTesting(cacheKey), 2);

  coordinator.cancel(cancelledHandle);
  QVERIFY(QTest::qWaitFor([&]() { return cancelledResult.has_value(); }, 5000));
  QVERIFY(!bool(*cancelledResult));
  QCOMPARE(cancelledResult->error().code, AssetErrorCode::Cancelled);

  // The shared attempt survives with exactly one remaining subscriber --
  // one consumer's cancellation must never tear down the group.
  QCOMPARE(coordinator.candidateAttemptCountForTesting(), 1);
  QCOMPARE(coordinator.candidateAttemptSubscriberCountForTesting(cacheKey), 1);

  QVERIFY(!survivorResult.has_value());
  QVERIFY(QTest::qWaitFor([&]() { return survivorResult.has_value(); }, 5000));
  QVERIFY2(bool(*survivorResult), qPrintable(survivorResult->error().message));
  QCOMPARE((**survivorResult).encodedBytes, response.body);

  // Exactly one HTTP request served both the cancelled and surviving
  // subscribers -- cancelling one never caused a second, independent
  // fetch to be issued for the survivor.
  QCOMPARE(server.requestCount(path), 1);
  QCOMPARE(coordinator.candidateAttemptCountForTesting(), 0);
}

void AssetRequestCoordinatorTests::
    coalescedRevalidationAppliesFreshReplaceOnceForAllSubscribers() {
  // Round-6 item 8: proves the "duplicates network/decode" defect is
  // fixed for the OTHER revalidation verdict not covered by
  // delayedStaleRevalidationSuccessAfterDefinitive404NeverResurrectsEvictedEntry
  // (which covers the definitive-404 verdict) -- namely, the origin
  // sending a fresh 200 body despite conditional headers. Two DIFFERENT
  // logical AssetKeys revalidating the SAME pre-seeded stale disk entry
  // (same cacheKey, same on-disk etag, so the same validator snapshot)
  // must coalesce onto exactly one conditional GET and exactly one
  // decode/store of the fresh body, never a per-subscriber duplicate,
  // and the memory cache must end up holding exactly the fresh entry --
  // never any remnant of the pre-seeded stale one.
  MockHttpServer server;
  const QString path = QStringLiteral("/img/arkham/sets/valid01.png");
  MockHttpServer::Response freshResponse;
  freshResponse.contentType = "image/png";
  freshResponse.body = encodePng(16, 16);
  freshResponse.slowDrip = true;
  freshResponse.chunkSize = 32;
  freshResponse.chunkDelayMs = 20;
  server.setResponse(path, freshResponse);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;

  AssetKey keyA =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyA.locale = QString();
  AssetKey keyB = keyA;
  keyB.locale = QStringLiteral("fr");
  QVERIFY(!(keyA == keyB));

  const auto candidates = AssetLocator::resolveCandidates(keyA);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  // See the identical restart-simulation comment in
  // delayedStaleRevalidationSuccessAfterDefinitive404NeverResurrectsEvictedEntry:
  // written through a throwaway instance, then read back through a
  // brand-new one so the stale entry exists ONLY on disk, never in this
  // test's own AssetCache's in-process memory cache (a memory hit would
  // otherwise be served immediately with no revalidation at all).
  {
    AssetCache seedCache(cacheConfig);
    AssetCache::CachedEntry staleSeed;
    staleSeed.encodedBytes = encodePng(4, 4);
    staleSeed.contentType = QStringLiteral("image/png");
    staleSeed.dimensions = QSize(4, 4);
    staleSeed.etag = QStringLiteral("\"shared-etag\"");
    seedCache.store(cacheKey, staleSeed);
  }
  AssetCache cache(cacheConfig);
  QVERIFY(cache.lookupDisk(cacheKey).has_value());
  QVERIFY(!cache.lookupMemory(cacheKey).has_value());

  AssetRequestCoordinator coordinator(cache, fetcher);

  std::optional<Result> resultA;
  std::optional<Result> resultB;
  coordinator.request(keyA, [&](Result r) { resultA = std::move(r); });
  coordinator.request(keyB, [&](Result r) { resultB = std::move(r); });

  QCOMPARE(coordinator.candidateAttemptCountForTesting(), 1);
  QCOMPARE(coordinator.candidateAttemptSubscriberCountForTesting(cacheKey), 2);

  QVERIFY(QTest::qWaitFor(
      [&]() { return resultA.has_value() && resultB.has_value(); }, 5000));

  // Exactly one conditional GET served both subscribers.
  QCOMPARE(server.requestCount(path), 1);
  QCOMPARE(coordinator.candidateAttemptCountForTesting(), 0);

  QVERIFY2(bool(*resultA), qPrintable(resultA->error().message));
  QVERIFY2(bool(*resultB), qPrintable(resultB->error().message));
  QCOMPARE((**resultA).encodedBytes, freshResponse.body);
  QCOMPARE((**resultB).encodedBytes, freshResponse.body);
  QCOMPARE((**resultA).decodedImage.size(), QSize(16, 16));
  QCOMPARE((**resultB).decodedImage.size(), QSize(16, 16));

  // The memory cache holds exactly one entry for this key: the fresh
  // 16x16 bytes, never a duplicated/independently-decoded copy and never
  // any remnant of the pre-seeded stale 4x4 bytes.
  const auto memoryHit = cache.lookupMemory(cacheKey);
  QVERIFY(memoryHit.has_value());
  QCOMPARE(memoryHit->encodedBytes, freshResponse.body);
  QVERIFY(!memoryHit->decodedImage.isNull());
  QCOMPARE(memoryHit->decodedImage.size(), QSize(16, 16));
}

void AssetRequestCoordinatorTests::
    newer404TombstonesOlderCachedEntryAcrossTtlExpiryAndRestart() {
  // Review round-4 item 5 ("newer unconditional 404 records negative
  // but doesn't invalidate older cached 200; after TTL old
  // resurrects"), now revisited for round-6 item 8. The original
  // scenario here forced two DIFFERENT logical keys resolving to the
  // SAME candidate/cache key to race as two INDEPENDENT concurrent
  // fetches with a rigged completion order (one 200, one later 404).
  // With startCandidate()'s coalescing (item 8), that exact scenario is
  // now structurally impossible for an unconditional (no-validator)
  // fetch: any two requests for the identical candidate while genuinely
  // still in flight together always share ONE CandidateAttempt and
  // therefore ONE outcome -- there is no longer a way for one subscriber
  // to observe a 200 while another subscriber of the exact same attempt
  // observes a 404. (A stale ALREADY-cached 200 entry with no validators
  // is also unreachable here: request() serves such an entry immediately
  // -- see its own comment -- so it can never even reach startCandidate()
  // again to race against a later 404 in the first place.)
  //
  // This test therefore now proves the surviving, still-load-bearing
  // part of the original fix: startCandidate()'s definitive-404 branch's
  // m_cache.invalidate(cacheKey) call (a no-op here, since nothing was
  // ever cached, but exercised all the same) plus the negative-404
  // TTL/restart durability, driven by a coalesced 404 shared by two
  // DIFFERENT logical keys instead of by a single key.
  MockHttpServer server;
  const QString path = QStringLiteral("/img/arkham/sets/valid01.png");
  MockHttpServer::Response notFound;
  notFound.status = 404;
  notFound.reasonPhrase = "Not Found";
  notFound.slowDrip = true;
  notFound.chunkSize = 4;
  notFound.chunkDelayMs = 20;
  server.setResponse(path, notFound);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  auto cache = std::make_unique<AssetCache>(cacheConfig);
  auto coordinator = std::make_unique<AssetRequestCoordinator>(*cache, fetcher);

  qint64 fakeNowMs = 1'000'000;
  coordinator->setMonotonicNowForTesting([&]() { return fakeNowMs; });

  AssetKey keyA =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyA.locale = QString();
  AssetKey keyB = keyA;
  keyB.locale = QStringLiteral("fr");
  QVERIFY(!(keyA == keyB));

  const auto candidatesA = AssetLocator::resolveCandidates(keyA);
  QVERIFY(bool(candidatesA));
  const QString cacheKey = AssetCache::cacheKeyFor(candidatesA->first().url);

  std::optional<Result> resultA;
  std::optional<Result> resultB;
  coordinator->request(keyA, [&](Result r) { resultA = std::move(r); });
  coordinator->request(keyB, [&](Result r) { resultB = std::move(r); });

  // Both operations share exactly one coalesced attempt: there is no
  // longer any way for A and B to observe different outcomes for the
  // identical candidate.
  QCOMPARE(coordinator->candidateAttemptCountForTesting(), 1);
  QCOMPARE(coordinator->candidateAttemptSubscriberCountForTesting(cacheKey), 2);

  QVERIFY(QTest::qWaitFor(
      [&]() { return resultA.has_value() && resultB.has_value(); }, 5000));
  QCOMPARE(server.requestCount(path), 1);

  QVERIFY(!bool(*resultA));
  QCOMPARE(resultA->error().code, AssetErrorCode::NotFound);
  QVERIFY(!bool(*resultB));
  QCOMPARE(resultB->error().code, AssetErrorCode::NotFound);

  // Nothing was ever cached: the shared 404's invalidate() call is a
  // harmless no-op here, but must not itself misbehave (e.g. crash or
  // leave a partial entry) when there was nothing to remove.
  QVERIFY(!cache->lookupDisk(cacheKey).has_value());

  // Still well within the negative-404 TTL: a third, later request for
  // the SAME cache key observes NotFound from the negative record, no
  // new network round trip.
  fakeNowMs += 60'000; // +1 minute, TTL is 5 minutes
  std::optional<Result> resultAAgain;
  coordinator->request(keyA, [&](Result r) { resultAAgain = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return resultAAgain.has_value(); }, 5000));
  QVERIFY(!bool(*resultAAgain));
  QCOMPARE(resultAAgain->error().code, AssetErrorCode::NotFound);
  QCOMPARE(server.requestCount(path), 1); // no new network round trip

  // Past the negative-404 TTL: the record has lazily expired, so this
  // must genuinely re-check the network -- and must never resurrect any
  // cached success (there never was one, but the invalidated/absent
  // state must still be observed correctly after expiry).
  fakeNowMs += 6 * 60'000; // +6 more minutes -- past the 5-minute TTL
  std::optional<Result> resultAPastTtl;
  coordinator->request(keyA, [&](Result r) { resultAPastTtl = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return resultAPastTtl.has_value(); }, 5000));
  QVERIFY(!bool(*resultAPastTtl));
  QCOMPARE(resultAPastTtl->error().code, AssetErrorCode::NotFound);
  QCOMPARE(server.requestCount(path), 2); // genuinely re-tried

  // Simulated restart: a brand-new AssetCache/AssetRequestCoordinator
  // pair backed by the SAME on-disk directory must never find a
  // resurrected entry either.
  coordinator.reset();
  cache.reset();
  AssetCache restartedCache(cacheConfig);
  QVERIFY(!restartedCache.lookupDisk(cacheKey).has_value());
}

void AssetRequestCoordinatorTests::
    negative404AndGenerationStateStayBoundedUnderHighCardinality() {
  // Review round-4 item 7 ("m_negative404, issuedGeneration/
  // currentGeneration retain every candidate forever"). Previously
  // hasNegative404() was a purely lazy, read-only check: an
  // expired/superseded record was simply left in m_negative404 forever
  // (never swept), and m_cacheKeyGeneration/m_cacheKeyIssuedGeneration
  // gained one permanent entry per DISTINCT cache key ever observed for
  // the coordinator's entire process lifetime. A long play session
  // touching many thousands of distinct card arts would grow all three
  // maps without limit.
  //
  // This drives many DISTINCT candidates (distinct identifiers => distinct
  // resolved URLs => distinct cache keys) to a definitive 404, one at a
  // time (each fully completed before the next is issued, so none is
  // ever "in flight" and pinned by activeInFlightCacheKeys() by the time
  // the next one starts), then asserts the coordinator's own observable
  // counters never grow anywhere near the full candidate count -- proving
  // pruneStaleCacheKeyState() (called opportunistically at the end of
  // every completeOperation()) actually bounds these maps rather than
  // merely documenting an intent to.
  //
  // Every candidate's negative-404 TTL is still fully alive throughout
  // (the fake monotonic clock never advances), so it is specifically the
  // HARD CAP (kMaxTrackedNegative404Entries) -- not lazy TTL-based
  // expiry -- that must bound growth here. The production cap (4096) is
  // deliberately overridden down to a small test value via
  // setMaxTrackedNegative404EntriesForTesting() so this test can drive
  // enough distinct candidates to exceed it using a handful of fast
  // local round trips rather than thousands of real ones.
  MockHttpServer server;
  MockHttpServer::Response notFound;
  notFound.status = 404;
  notFound.reasonPhrase = "Not Found";

  constexpr int kTestCap = 5;
  constexpr int kCandidateCount = kTestCap * 4;
  for (int i = 0; i < kCandidateCount; ++i) {
    server.setResponse(QStringLiteral("/img/arkham/sets/icon%1.png").arg(i),
                       notFound);
  }

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);
  coordinator.setMaxTrackedNegative404EntriesForTesting(kTestCap);

  qint64 fakeNowMs = 1'000'000;
  coordinator.setMonotonicNowForTesting([&]() { return fakeNowMs; });

  for (int i = 0; i < kCandidateCount; ++i) {
    const AssetKey key =
        makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
                QStringLiteral("icon%1").arg(i));
    std::optional<Result> result;
    coordinator.request(key, [&](Result r) { result = std::move(r); });
    QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));
    QVERIFY(!bool(*result));
    QCOMPARE(result->error().code, AssetErrorCode::NotFound);
    // Advance the fake clock by a tiny, strictly-increasing amount
    // between iterations (still nowhere near kNegative404TtlMs) purely
    // so each record's expiresAtMonotonicMs is distinct and strictly
    // increasing with insertion order -- making the cap's "evict
    // soonest-to-expire first" tie-breaking deterministic (earliest
    // inserted == soonest to expire == first evicted) for this test's
    // later assertion about specifically icon0.
    ++fakeNowMs;
    // completeOperation() prunes opportunistically after every single
    // completion above, so the cap is enforced continuously, never only
    // at the very end.
    QVERIFY2(coordinator.negative404RecordCountForTesting() <= kTestCap,
             qPrintable(QStringLiteral("negative404 record count %1 exceeded "
                                       "the %2 test cap after candidate %3")
                            .arg(coordinator.negative404RecordCountForTesting())
                            .arg(kTestCap)
                            .arg(i)));
  }

  QCOMPARE(coordinator.negative404RecordCountForTesting(), kTestCap);
  // Cumulative review (independent re-review, HIGH, "negative 404 is
  // coordinator-local and can hide sibling-populated cache"): the
  // negative-404 record itself now lives entirely in the shared
  // AssetCache authority (see AssetCache::recordNegative404()'s own
  // bounded-pruning/hard-cap enforcement), never pinning this
  // coordinator's own m_cacheKeyGeneration/m_cacheKeyIssuedGeneration
  // maps at all -- so once every operation above has fully completed
  // (none is in flight, and pruneStaleCacheKeyState() has run after
  // each), nothing pins these maps and they are fully empty.
  QCOMPARE(coordinator.cacheKeyGenerationStateCountForTesting(), 0);

  // Stale-completion safety after eviction: a fresh request for one of
  // the EARLIEST (necessarily evicted, since only the most recent
  // kTestCap records can possibly have survived) candidates must still
  // behave correctly -- it re-checks the network (its negative record
  // was evicted by the cap) and gets a fresh, correct NotFound, never a
  // crash or a wrongly-resurrected/duplicated result.
  const AssetKey earlyKeyAgain =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
              QStringLiteral("icon0"));
  QVERIFY(!coordinator.hasNegative404ForTesting(AssetCache::cacheKeyFor(
      AssetLocator::resolveCandidates(earlyKeyAgain)->first().url)));
  std::optional<Result> earlyResultAgain;
  coordinator.request(earlyKeyAgain,
                      [&](Result r) { earlyResultAgain = std::move(r); });
  QVERIFY(
      QTest::qWaitFor([&]() { return earlyResultAgain.has_value(); }, 5000));
  QVERIFY(!bool(*earlyResultAgain));
  QCOMPARE(earlyResultAgain->error().code, AssetErrorCode::NotFound);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/icon0.png")),
           2); // once originally, once again after its record was evicted
}

void AssetRequestCoordinatorTests::
    soleConsumerCancellationPrunesIssuanceStateUnderHighCardinality() {
  // Round-6 item 7 ("sole-consumer cancellation erases op/aborts but
  // never prunes issuance state, causing unbounded
  // m_cacheKeyIssuedGeneration for unique requests"). Previously
  // pruneStaleCacheKeyState() was only ever called from
  // completeOperation() -- the last-consumer branch of cancel() erased
  // the Operation from m_operations directly and returned, never
  // pruning. A network-bound candidate mints its
  // m_cacheKeyIssuedGeneration entry as soon as it is ISSUED (see
  // startCandidate()/issueCacheKeyGeneration()'s comment), strictly
  // before any response arrives -- so a caller that starts a request
  // and cancels it (as the sole consumer) before it ever completes,
  // repeated across many DISTINCT cache keys (e.g. a user rapidly
  // scrolling past many different card arts, each request cancelled the
  // instant it scrolls off-screen), previously grew
  // m_cacheKeyIssuedGeneration/m_cacheKeyGeneration without bound for
  // the coordinator's entire process lifetime, since none of these
  // cancelled candidates ever reaches completeOperation() at all.
  //
  // This drives many distinct, slow-drip candidates, cancelling each as
  // the sole consumer immediately after confirming its fetch has
  // actually started (so cancellation genuinely races a real in-flight
  // operation, not one that never began), and asserts
  // cacheKeyGenerationStateCountForTesting() never grows past a small
  // bound -- proving the fix actually prunes on the cancellation path,
  // not merely on completion.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8);
  response.slowDrip = true;
  response.chunkSize = 8;
  response.chunkDelayMs = 200;

  constexpr int kCandidateCount = 25;
  for (int i = 0; i < kCandidateCount; ++i) {
    server.setResponse(QStringLiteral("/img/arkham/sets/icon%1.png").arg(i),
                       response);
  }

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);
  AssetRequestCoordinator coordinator(cache, fetcher);

  for (int i = 0; i < kCandidateCount; ++i) {
    const QString path = QStringLiteral("/img/arkham/sets/icon%1.png").arg(i);
    const AssetKey key =
        makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()),
                QStringLiteral("icon%1").arg(i));
    std::optional<Result> result;
    const auto handle =
        coordinator.request(key, [&](Result r) { result = std::move(r); });

    // Confirm the underlying fetch has genuinely started streaming
    // before cancelling -- this proves cancellation is racing a real
    // in-flight operation (which really did mint an issuance-generation
    // entry) rather than one that happened to be cancelled before the
    // TCP connection was even established.
    QVERIFY(QTest::qWaitFor(
        [&]() { return server.lastBytesWrittenForSlowDrip(path) >= 0; }, 5000));

    coordinator.cancel(handle);
    QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));
    QVERIFY(!bool(*result));
    QCOMPARE(result->error().code, AssetErrorCode::Cancelled);
    QCOMPARE(coordinator.inFlightOperationCountForTesting(), 0);

    // The fix prunes opportunistically inside cancel() itself, so this
    // must hold true after EVERY single cancellation, not only at the
    // very end -- a coordinator that only pruned lazily on some later,
    // unrelated event would still (harmlessly, but incorrectly for this
    // test's purpose) pass a check made only after the loop.
    QVERIFY2(coordinator.cacheKeyGenerationStateCountForTesting() <= 1,
             qPrintable(
                 QStringLiteral("issuance-generation state count %1 "
                                "was not pruned after cancelling "
                                "candidate %2")
                     .arg(coordinator.cacheKeyGenerationStateCountForTesting())
                     .arg(i)));
  }

  // Final state: nothing is in flight and no negative-404 record was
  // ever recorded (every one of these was cancelled, never actually
  // completed with a network result) -- so no cache key has any reason
  // to still be pinned.
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 0);
  QCOMPARE(coordinator.cacheKeyGenerationStateCountForTesting(), 0);
}

void AssetRequestCoordinatorTests::
    failedDurableInvalidationOnDefinitive404NeverRecordsNegativeAndFailsClosed() {
  // Round-6 item 6 ("definitive 404 invalidation ignores delete failure
  // and manifest unlink lacks directory fsync; old 200 can revive after
  // TTL/restart/crash"). Simulate a genuine on-disk deletion failure
  // (revoking write permission on the cache directory -- unlinkat()
  // requires write permission on the CONTAINING directory, not the file
  // itself, so this deterministically fails the manifest unlink
  // regardless of platform/privilege) at the exact moment a definitive
  // 404 (from a conditional GET revalidation) tries to tombstone an
  // already-cached 200 entry for the same cache key. The fix must never
  // record a negative-404 over an entry it could not actually confirm
  // gone -- doing so would let the never-actually-deleted stale entry
  // resurface once that record's bounded TTL lazily expired. Instead the
  // operation must fail closed with a typed, observable
  // CachePersistenceFailed error.
  MockHttpServer server;
  const QString path = QStringLiteral("/img/arkham/sets/valid01.png");

  // An ETag is required so the SECOND request below revalidates over a
  // real conditional GET (startRevalidation()) rather than being served
  // straight from the memory/disk cache with no network round trip at
  // all -- see request()'s own cache-hit branches.
  MockHttpServer::Response fastOk;
  fastOk.contentType = "image/png";
  fastOk.body = encodePng(8, 8);
  fastOk.extraHeaders.append(
      qMakePair(QByteArray("ETag"), QByteArray("\"v1\"")));
  server.setResponse(path, fastOk);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  auto cache = std::make_unique<AssetCache>(cacheConfig);
  auto coordinator = std::make_unique<AssetRequestCoordinator>(*cache, fetcher);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  // First request: an ordinary 200, durably stored to disk with its ETag.
  std::optional<Result> firstResult;
  coordinator->request(key, [&](Result r) { firstResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return firstResult.has_value(); }, 5000));
  QVERIFY2(bool(*firstResult), qPrintable(firstResult->error().message));
  const auto storedEntry = cache->lookupDisk(cacheKey);
  QVERIFY(storedEntry.has_value());
  QCOMPARE(storedEntry->etag, QStringLiteral("\"v1\""));

  // Simulated restart with a BRAND NEW cache/coordinator pair pointed at
  // the same directory: the in-memory cache starts cold, so the second
  // request below is a genuine DISK hit carrying validators, which
  // request() revalidates over a real conditional GET
  // (startRevalidation()) rather than serving straight from a still-warm
  // memory entry with no network round trip at all (see request()'s own
  // memory-hit branch, which never revalidates).
  coordinator.reset();
  cache.reset();
  cache = std::make_unique<AssetCache>(cacheConfig);
  coordinator = std::make_unique<AssetRequestCoordinator>(*cache, fetcher);

  // Now revoke write permission on the cache directory -- ALL of this
  // entry's data is already safely on disk at this point, so nothing
  // about the request above is affected; only a SUBSEQUENT unlink
  // attempt is.
  struct ScopedDirectoryPermissionLock {
    QString path;
    ~ScopedDirectoryPermissionLock() {
      QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner |
                                      QFile::ExeOwner);
    }
  } permissionGuard{m_tempDirPath};
  QVERIFY(QFile::setPermissions(
      m_tempDirPath, QFile::ReadOwner | QFile::ExeOwner)); // r-x, no write

  MockHttpServer::Response notFound;
  notFound.status = 404;
  notFound.reasonPhrase = "Not Found";
  server.setResponse(path, notFound);

  // Second request for the SAME key: the cached entry carries an ETag,
  // so this is a real conditional GET (startRevalidation()), which the
  // server above answers with an unconditional (not-304) 404.
  std::optional<Result> secondResult;
  coordinator->request(key, [&](Result r) { secondResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return secondResult.has_value(); }, 5000));

  // The decisive assertions for the fix: this candidate transition must
  // fail closed with the typed persistence error -- NOT report a
  // (misleadingly successful-looking) NotFound, and NOT record a
  // negative-404 for a key whose still-live disk entry could not
  // actually be confirmed removed.
  QVERIFY(!bool(*secondResult));
  QCOMPARE(secondResult->error().code, AssetErrorCode::CachePersistenceFailed);
  QVERIFY(!coordinator->hasNegative404ForTesting(cacheKey));

  // Sanity: restoring write permission and letting the guard's own
  // destructor run at scope exit (below) leaves the directory in a
  // normal, writable state again -- this test's cleanup() (which resets
  // m_tempDir) still works normally afterward.
}

void AssetRequestCoordinatorTests::
    concurrentAliasedMemoryHitRequestsCoalesceIntoASingleDecode() {
  // Round-7/8 item 6 ("cache-hit read/decode occurs before operation
  // coalescing"): two DIFFERENT logical AssetKeys (differing only in
  // `locale`, which has no effect on a SetIcon candidate -- see
  // makeKey()'s comment) resolve to the identical candidate/cache key
  // and are issued back-to-back, before either's queued decode has run,
  // against an already-warm MEMORY entry. Both must share exactly one
  // real decode, not one each -- a burst of several simultaneous QML
  // Image elements for the same card must never each independently
  // decode a near-32-megapixel image.
  MockHttpServer server;
  // No response registered: a memory hit must never touch the network
  // at all.

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  AssetKey keyA =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyA.locale = QString();
  AssetKey keyB = keyA;
  keyB.locale = QStringLiteral("fr");
  QVERIFY(!(keyA == keyB));

  const auto candidates = AssetLocator::resolveCandidates(keyA);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = encodePng(8, 8);
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(8, 8);
  cache.store(cacheKey, preSeeded);

  AssetRequestCoordinator coordinator(cache, fetcher);

  std::optional<Result> resultA;
  std::optional<Result> resultB;
  coordinator.request(keyA, [&](Result r) { resultA = std::move(r); });
  coordinator.request(keyB, [&](Result r) { resultB = std::move(r); });

  // Both requests genuinely joined ONE shared pending decode group before
  // either's queued decode ran -- checked synchronously, before the event
  // loop has had a chance to run the queued completeCoalescedCacheDecode()
  // call at all.
  QCOMPARE(coordinator.pendingCacheDecodeGroupCountForTesting(), 1);
  QCOMPARE(coordinator.pendingCacheDecodeWaiterCountForTesting(
               cacheKey, AssetFormat::Png),
           2);

  QVERIFY(QTest::qWaitFor(
      [&]() { return resultA.has_value() && resultB.has_value(); }, 5000));

  QVERIFY2(bool(*resultA), qPrintable(resultA->error().message));
  QVERIFY2(bool(*resultB), qPrintable(resultB->error().message));
  QVERIFY(!(**resultA).decodedImage.isNull());
  QVERIFY(!(**resultB).decodedImage.isNull());
  QCOMPARE((**resultA).decodedImage.size(), QSize(8, 8));
  QCOMPARE((**resultB).decodedImage.size(), QSize(8, 8));

  // The decisive assertion: exactly ONE real decode ran for both waiters.
  QCOMPARE(coordinator.realDecodeCallCountForTesting(), 1);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           0);
  QCOMPARE(coordinator.pendingCacheDecodeGroupCountForTesting(), 0);
}

void AssetRequestCoordinatorTests::
    concurrentAliasedDiskHitRequestsCoalesceIntoASingleDecode() {
  // Same scenario as the memory-hit test above, but against a genuine
  // DISK-only entry (no decodedImage yet, forced via a fresh AssetCache
  // instance pointed at the same directory -- exactly like
  // concurrentIdenticalRequestsForADiskHitCoalesceIntoASingleDecode()
  // above, but for two DIFFERENT aliased logical keys instead of the
  // identical one).
  MockHttpServer server;

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  AssetKey keyA =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyA.locale = QString();
  AssetKey keyB = keyA;
  keyB.locale = QStringLiteral("fr");
  QVERIFY(!(keyA == keyB));

  const auto candidates = AssetLocator::resolveCandidates(keyA);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = encodePng(6, 6);
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(6, 6);
  cache.store(cacheKey, preSeeded);

  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> resultA;
  std::optional<Result> resultB;
  coordinator.request(keyA, [&](Result r) { resultA = std::move(r); });
  coordinator.request(keyB, [&](Result r) { resultB = std::move(r); });

  QCOMPARE(coordinator.pendingCacheDecodeGroupCountForTesting(), 1);
  QCOMPARE(coordinator.pendingCacheDecodeWaiterCountForTesting(
               cacheKey, AssetFormat::Png),
           2);

  QVERIFY(QTest::qWaitFor(
      [&]() { return resultA.has_value() && resultB.has_value(); }, 5000));

  QVERIFY2(bool(*resultA), qPrintable(resultA->error().message));
  QVERIFY2(bool(*resultB), qPrintable(resultB->error().message));
  QCOMPARE((**resultA).decodedImage.size(), QSize(6, 6));
  QCOMPARE((**resultB).decodedImage.size(), QSize(6, 6));

  QCOMPARE(coordinator.realDecodeCallCountForTesting(), 1);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           0);
}

void AssetRequestCoordinatorTests::
    corruptCoalescedCacheHitInvalidatesExactlyOnceAndEachWaiterIndependentlyRefetches() {
  // Round-7/8 item 6: when the single shared decode a coalesced group is
  // waiting on turns out to be quarantine-worthy (the cached bytes no
  // longer actually decode), the group must invalidate the cache key
  // EXACTLY ONCE -- never once per waiter -- and every waiter whose own
  // CAS still applies must independently retry the SAME candidate; those
  // independent retries must themselves share a single HTTP request via
  // the pre-existing CandidateAttempt network-level coalescing (see
  // startCandidate()'s comment), never one round trip per waiter either.
  MockHttpServer server;
  const QString path = QStringLiteral("/img/arkham/sets/valid01.png");
  MockHttpServer::Response fresh;
  fresh.contentType = "image/png";
  fresh.body = encodePng(6, 6);
  server.setResponse(path, fresh);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  AssetKey keyA =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyA.locale = QString();
  AssetKey keyB = keyA;
  keyB.locale = QStringLiteral("fr");
  QVERIFY(!(keyA == keyB));

  const auto candidates = AssetLocator::resolveCandidates(keyA);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  AssetCache::CachedEntry preSeeded;
  // Not real PNG bytes: on-demand decode must fail for both waiters
  // sharing this one entry.
  preSeeded.encodedBytes = QByteArrayLiteral("not-actually-a-png");
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(4, 4);
  cache.store(cacheKey, preSeeded);

  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> resultA;
  std::optional<Result> resultB;
  coordinator.request(keyA, [&](Result r) { resultA = std::move(r); });
  coordinator.request(keyB, [&](Result r) { resultB = std::move(r); });

  QCOMPARE(coordinator.pendingCacheDecodeGroupCountForTesting(), 1);
  QCOMPARE(coordinator.pendingCacheDecodeWaiterCountForTesting(
               cacheKey, AssetFormat::Png),
           2);

  QVERIFY(QTest::qWaitFor(
      [&]() { return resultA.has_value() && resultB.has_value(); }, 5000));

  QVERIFY2(bool(*resultA), qPrintable(resultA->error().message));
  QVERIFY2(bool(*resultB), qPrintable(resultB->error().message));
  QCOMPARE((**resultA).dimensions, QSize(6, 6));
  QCOMPARE((**resultB).dimensions, QSize(6, 6));

  // Exactly one invalidate() call for the whole group, and exactly one
  // real HTTP request serving both waiters' independent retries.
  QCOMPARE(restartedCache.invalidateCallCountForTesting(), 1);
  QCOMPARE(server.requestCount(path), 1);

  const auto onDisk = restartedCache.lookupDisk(cacheKey);
  QVERIFY(onDisk.has_value());
  QCOMPARE(onDisk->dimensions, QSize(6, 6));
}

void AssetRequestCoordinatorTests::
    cancellingOneWaiterInACoalescedCacheDecodeGroupNeverAffectsAnother() {
  // Round-7/8 item 6's explicit "canceled waiter separate" requirement:
  // cancelling ONE waiter of a shared pending cache-hit decode group
  // before its single queued decode has run must never affect any
  // surviving sibling waiter -- the cancelled waiter is silently skipped
  // during delivery (see completeCacheReadGroupOrQuarantine()'s comment),
  // while the survivor still receives the shared decode's outcome
  // normally.
  MockHttpServer server;

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  AssetKey keyA =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyA.locale = QString();
  AssetKey keyB = keyA;
  keyB.locale = QStringLiteral("fr");
  QVERIFY(!(keyA == keyB));

  const auto candidates = AssetLocator::resolveCandidates(keyA);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = encodePng(8, 8);
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(8, 8);
  cache.store(cacheKey, preSeeded);

  AssetRequestCoordinator coordinator(cache, fetcher);

  std::optional<Result> cancelledResult;
  std::optional<Result> survivorResult;
  const auto cancelledHandle = coordinator.request(
      keyA, [&](Result r) { cancelledResult = std::move(r); });
  coordinator.request(keyB, [&](Result r) { survivorResult = std::move(r); });

  QCOMPARE(coordinator.pendingCacheDecodeGroupCountForTesting(), 1);
  QCOMPARE(coordinator.pendingCacheDecodeWaiterCountForTesting(
               cacheKey, AssetFormat::Png),
           2);

  // Cancel the FIRST (leader) waiter before the shared queued decode has
  // had any chance to run at all.
  coordinator.cancel(cancelledHandle);

  QVERIFY(QTest::qWaitFor([&]() { return survivorResult.has_value(); }, 5000));
  QVERIFY2(bool(*survivorResult), qPrintable(survivorResult->error().message));
  QVERIFY(!(**survivorResult).decodedImage.isNull());
  QCOMPARE((**survivorResult).decodedImage.size(), QSize(8, 8));

  // The cancelled waiter still receives cancel()'s own Cancelled
  // delivery (exactly like any other cancelled consumer -- see
  // cancellingOneCoalescedCrossLogicalKeySubscriberNeverAbortsAnother()
  // above) -- but it must never receive the shared decode's own
  // outcome, and must never affect the survivor's own delivery: the
  // shared decode's delivery loop skips an operationId already absent
  // from m_operations (removed by cancel() itself once its last
  // consumer left) cleanly, without erroring out or corrupting the
  // survivor's own delivery.
  QVERIFY(QTest::qWaitFor([&]() { return cancelledResult.has_value(); }, 5000));
  QVERIFY(!bool(*cancelledResult));
  QCOMPARE(cancelledResult->error().code, AssetErrorCode::Cancelled);

  // Exactly one real decode still ran (for the surviving waiter) -- the
  // cancelled waiter never caused the group to skip decoding altogether,
  // nor did it cause a second, independent decode.
  QCOMPARE(coordinator.realDecodeCallCountForTesting(), 1);
}

void AssetRequestCoordinatorTests::
    cancellingEveryWaiterInACoalescedCacheDecodeGroupPreventsTheDecodeEntirely() {
  // Round-9+ review item 3/7 ("fully cancelled PendingCacheDecode groups
  // retained and still decode"): unlike the test above (one survivor
  // remains), THIS test cancels BOTH waiters of a shared cache-hit
  // decode group before its single queued decode has had any chance to
  // run at all. The group must be pruned entirely by cancel() itself
  // (see pruneCancelledPendingCacheDecodeWaiter()'s comment) -- the
  // decode this group was formed for must never run at all, not even
  // once, since delivering it to anyone is now impossible.
  MockHttpServer server;

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cache(cacheConfig);

  AssetKey keyA =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyA.locale = QString();
  AssetKey keyB = keyA;
  keyB.locale = QStringLiteral("fr");
  QVERIFY(!(keyA == keyB));

  const auto candidates = AssetLocator::resolveCandidates(keyA);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  AssetCache::CachedEntry preSeeded;
  preSeeded.encodedBytes = encodePng(8, 8);
  preSeeded.contentType = QStringLiteral("image/png");
  preSeeded.dimensions = QSize(8, 8);
  cache.store(cacheKey, preSeeded);

  AssetRequestCoordinator coordinator(cache, fetcher);

  std::optional<Result> resultA;
  std::optional<Result> resultB;
  const auto handleA =
      coordinator.request(keyA, [&](Result r) { resultA = std::move(r); });
  const auto handleB =
      coordinator.request(keyB, [&](Result r) { resultB = std::move(r); });

  QCOMPARE(coordinator.pendingCacheDecodeGroupCountForTesting(), 1);
  QCOMPARE(coordinator.pendingCacheDecodeWaiterCountForTesting(
               cacheKey, AssetFormat::Png),
           2);

  // Cancel BOTH waiters before the shared queued decode has run.
  coordinator.cancel(handleA);
  coordinator.cancel(handleB);

  // The group must already be gone -- pruned synchronously by the
  // second cancel(), which emptied its waiter list.
  QCOMPARE(coordinator.pendingCacheDecodeGroupCountForTesting(), 0);

  QVERIFY(QTest::qWaitFor(
      [&]() { return resultA.has_value() && resultB.has_value(); }, 5000));
  QVERIFY(!bool(*resultA));
  QCOMPARE(resultA->error().code, AssetErrorCode::Cancelled);
  QVERIFY(!bool(*resultB));
  QCOMPARE(resultB->error().code, AssetErrorCode::Cancelled);

  // The decisive assertion: zero real decodes ever ran. The group's own
  // queued completeCoalescedCacheDecode() closure still runs (it was
  // already scheduled before either cancellation), but finds nothing in
  // m_pendingCacheDecodes and is a genuine no-op.
  QCOMPARE(coordinator.realDecodeCallCountForTesting(), 0);
}

void AssetRequestCoordinatorTests::
    concurrentAliasedRevalidationStaleIfErrorRequestsCoalesceIntoASingleDecode() {
  // Round-9+ review item 3/7 ("aliases coalesce network but not cached/
  // 304 decode"): two aliased logical keys (differing only in `locale`)
  // both revalidate the identical pre-seeded disk entry (same cacheKey,
  // same on-disk etag, so the same coalesced conditional GET -- see
  // diskHitRevalidationCoalescesConcurrentIdenticalRequests()) against
  // an origin that answers with a transport failure (500): the
  // "stale-if-error" verdict must decode the served-stale entry exactly
  // ONCE for the whole coalesced group, never once per aliased
  // subscriber.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.status = 500;
  response.reasonPhrase = "Internal Server Error";
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;

  AssetKey keyA =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyA.locale = QString();
  AssetKey keyB = keyA;
  keyB.locale = QStringLiteral("fr");
  QVERIFY(!(keyA == keyB));

  const auto candidates = AssetLocator::resolveCandidates(keyA);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  {
    AssetCache seedCache(cacheConfig);
    AssetCache::CachedEntry preSeeded;
    preSeeded.encodedBytes = encodePng(8, 8);
    preSeeded.contentType = QStringLiteral("image/png");
    preSeeded.dimensions = QSize(8, 8);
    preSeeded.etag = QStringLiteral("\"shared-etag\"");
    seedCache.store(cacheKey, preSeeded);
  }
  // A fresh AssetCache instance (simulating a restart) forces both
  // requests through the disk-hit-with-validators/revalidation path --
  // a memory hit would skip revalidation (and this coalescing) entirely.
  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> resultA;
  std::optional<Result> resultB;
  coordinator.request(keyA, [&](Result r) { resultA = std::move(r); });
  coordinator.request(keyB, [&](Result r) { resultB = std::move(r); });

  // Both operations share exactly one coalesced revalidation attempt.
  QCOMPARE(coordinator.candidateAttemptCountForTesting(), 1);
  QCOMPARE(coordinator.candidateAttemptSubscriberCountForTesting(cacheKey), 2);

  QVERIFY(QTest::qWaitFor(
      [&]() { return resultA.has_value() && resultB.has_value(); }, 5000));

  QVERIFY2(bool(*resultA), qPrintable(resultA->error().message));
  QVERIFY2(bool(*resultB), qPrintable(resultB->error().message));
  QCOMPARE((**resultA).encodedBytes, (**resultB).encodedBytes);
  QVERIFY(!(**resultA).decodedImage.isNull());
  QVERIFY(!(**resultB).decodedImage.isNull());
  QCOMPARE((**resultA).decodedImage.size(), QSize(8, 8));
  QCOMPARE((**resultB).decodedImage.size(), QSize(8, 8));
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);

  // The decisive assertion: exactly ONE real decode ran for both
  // aliased subscribers sharing the one served-stale entry.
  QCOMPARE(coordinator.realDecodeCallCountForTesting(), 1);
}

void AssetRequestCoordinatorTests::
    concurrentAliasedRevalidationNotModifiedRequestsCoalesceIntoASingleDecode() {
  // Same principle as the StaleIfError test above, but for a confirmed
  // 304: two aliased logical keys share one coalesced conditional GET
  // that comes back Not Modified, and must share exactly one decode of
  // the now-promoted entry.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(32, 32); // must never be served to the caller
  response.etagForConditionalMatch = "\"shared-etag\"";
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;

  AssetKey keyA =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  keyA.locale = QString();
  AssetKey keyB = keyA;
  keyB.locale = QStringLiteral("fr");
  QVERIFY(!(keyA == keyB));

  const auto candidates = AssetLocator::resolveCandidates(keyA);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  {
    AssetCache seedCache(cacheConfig);
    AssetCache::CachedEntry preSeeded;
    preSeeded.encodedBytes = encodePng(8, 8);
    preSeeded.contentType = QStringLiteral("image/png");
    preSeeded.dimensions = QSize(8, 8);
    preSeeded.etag = QStringLiteral("\"shared-etag\"");
    seedCache.store(cacheKey, preSeeded);
  }
  AssetCache restartedCache(cacheConfig);
  AssetRequestCoordinator coordinator(restartedCache, fetcher);

  std::optional<Result> resultA;
  std::optional<Result> resultB;
  coordinator.request(keyA, [&](Result r) { resultA = std::move(r); });
  coordinator.request(keyB, [&](Result r) { resultB = std::move(r); });

  QCOMPARE(coordinator.candidateAttemptCountForTesting(), 1);
  QCOMPARE(coordinator.candidateAttemptSubscriberCountForTesting(cacheKey), 2);

  QVERIFY(QTest::qWaitFor(
      [&]() { return resultA.has_value() && resultB.has_value(); }, 5000));

  QVERIFY2(bool(*resultA), qPrintable(resultA->error().message));
  QVERIFY2(bool(*resultB), qPrintable(resultB->error().message));
  QVERIFY(!(**resultA).decodedImage.isNull());
  QVERIFY(!(**resultB).decodedImage.isNull());
  QCOMPARE((**resultA).decodedImage.size(), QSize(8, 8));
  QCOMPARE((**resultB).decodedImage.size(), QSize(8, 8));
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           1);

  // Both entries were promoted to memory by the shared 304 confirmation.
  QVERIFY(restartedCache.lookupMemory(cacheKey).has_value());

  // The decisive assertion: exactly ONE real decode ran for both
  // aliased subscribers sharing the confirmed-current entry.
  QCOMPARE(coordinator.realDecodeCallCountForTesting(), 1);
}

void AssetRequestCoordinatorTests::
    staleCancelledAttemptCallbackNeverCorruptsReplacementNetworkFetch() {
  // Round-9+ review (HIGH): see CandidateAttempt::token's declaration
  // comment and this test's declaration comment in the header. Cancel a
  // sole consumer's unconditional network fetch (which synchronously
  // erases its CandidateAttempt from m_candidateAttempts and calls
  // AssetNetworkFetcher::cancel() on the underlying fetch -- whose own
  // Cancelled completion is always dispatched later via a queued
  // invocation, never synchronously), then, in the SAME call stack,
  // before that queued Cancelled callback has any chance to run, issue a
  // brand new request for the IDENTICAL candidate. That new request
  // necessarily reuses the exact same string-keyed attemptKey (same
  // cacheKey/format/empty-validators). Pre-fix, the stale callback --
  // when it eventually ran -- found the NEW attempt under that key,
  // erased it, and dispatched its own stale Cancelled result to the new
  // request's subscriber; the new attempt's real, later network
  // completion then found nothing left in the map to dispatch to and
  // was silently dropped.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8);
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
  const auto cancelledHandle = coordinator.request(
      key, [&](Result r) { cancelledResult = std::move(r); });
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 1);

  // Sole consumer: synchronously erases the CandidateAttempt and calls
  // m_fetcher.cancel() on its fetch handle.
  coordinator.cancel(cancelledHandle);
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 0);

  std::optional<Result> replacementResult;
  coordinator.request(key, [&](Result r) { replacementResult = std::move(r); });

  // The replacement request must observe its OWN real, correct
  // completion -- never the first (cancelled) attempt's stale Cancelled
  // result, and never be silently dropped.
  QVERIFY(
      QTest::qWaitFor([&]() { return replacementResult.has_value(); }, 5000));
  QVERIFY2(bool(*replacementResult),
           qPrintable(replacementResult->error().message));
  QVERIFY(!(**replacementResult).decodedImage.isNull());
  QCOMPARE((**replacementResult).decodedImage.size(), QSize(8, 8));

  // The original (cancelled) consumer still receives cancel()'s own
  // Cancelled delivery, exactly as always.
  QVERIFY(QTest::qWaitFor([&]() { return cancelledResult.has_value(); }, 5000));
  QVERIFY(!bool(*cancelledResult));
  QCOMPARE(cancelledResult->error().code, AssetErrorCode::Cancelled);
}

void AssetRequestCoordinatorTests::
    staleCancelledAttemptCallbackNeverCorruptsReplacementRevalidation() {
  // Round-9+ review (HIGH): identical race to the test above, but for
  // the conditional-revalidation path (startRevalidation()) -- proving
  // CandidateAttempt::token protects both call sites independently.
  MockHttpServer server;
  const QString path = QStringLiteral("/img/arkham/sets/valid01.png");
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8);
  response.extraHeaders.append(
      qMakePair(QByteArray("ETag"), QByteArray("\"replacement-fresh-etag\"")));
  server.setResponse(path, response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  // Pre-seed a stale disk-only entry (no memory hit) carrying a
  // validator, exactly like the other revalidation tests above, so
  // request() takes the conditional-revalidation branch rather than an
  // unconditional network fetch.
  {
    AssetCache seedCache(cacheConfig);
    AssetCache::CachedEntry preSeeded;
    preSeeded.encodedBytes = encodePng(4, 4);
    preSeeded.contentType = QStringLiteral("image/png");
    preSeeded.dimensions = QSize(4, 4);
    preSeeded.etag = QStringLiteral("\"stale-etag\"");
    seedCache.store(cacheKey, preSeeded);
  }
  AssetCache cache(cacheConfig);
  QVERIFY(cache.lookupDisk(cacheKey).has_value());
  QVERIFY(!cache.lookupMemory(cacheKey).has_value());

  AssetRequestCoordinator coordinator(cache, fetcher);

  std::optional<Result> cancelledResult;
  const auto cancelledHandle = coordinator.request(
      key, [&](Result r) { cancelledResult = std::move(r); });
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 1);

  // Sole consumer: synchronously erases the CandidateAttempt and calls
  // m_fetcher.cancel() on the underlying conditional GET.
  coordinator.cancel(cancelledHandle);
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 0);

  std::optional<Result> replacementResult;
  coordinator.request(key, [&](Result r) { replacementResult = std::move(r); });

  // The replacement request must observe its OWN real, correct fresh-200
  // completion (carrying the NEW etag) -- never the first (cancelled)
  // attempt's stale Cancelled result, and never be silently dropped.
  QVERIFY(
      QTest::qWaitFor([&]() { return replacementResult.has_value(); }, 5000));
  QVERIFY2(bool(*replacementResult),
           qPrintable(replacementResult->error().message));
  QVERIFY(!(**replacementResult).decodedImage.isNull());
  QCOMPARE((**replacementResult).decodedImage.size(), QSize(8, 8));
  const auto freshDiskEntry = cache.lookupDisk(cacheKey);
  QVERIFY(freshDiskEntry.has_value());
  QCOMPARE(freshDiskEntry->etag, QStringLiteral("\"replacement-fresh-etag\""));

  // The original (cancelled) consumer still receives cancel()'s own
  // Cancelled delivery, exactly as always.
  QVERIFY(QTest::qWaitFor([&]() { return cancelledResult.has_value(); }, 5000));
  QVERIFY(!bool(*cancelledResult));
  QCOMPARE(cancelledResult->error().code, AssetErrorCode::Cancelled);
}

void AssetRequestCoordinatorTests::
    requestAgainstAnInvalidlyConfiguredCacheFailsImmediatelyWithoutNetworkAccess() {
  // Round-N+ review (MEDIUM, repeat finding, "invalid cache limits
  // publicly constructible"): a negative diskMaxBytes/memoryMaxCostBytes
  // makes the WHOLE AssetCache::Config invalid (see
  // AssetCache::validateConfiguration()) -- previously, an
  // AssetRequestCoordinator built against such a cache would still
  // dispatch a real network fetch and only discover the misconfiguration
  // much later, indirectly, as a confusing CachePersistenceFailed once
  // store()/lookupDisk() silently failed. This proves the fixed,
  // immediate-diagnosis behaviour: request() itself refuses to proceed
  // at all.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8);
  server.setResponse(QStringLiteral("/img/arkham/sets/valid01.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  cacheConfig.diskMaxBytes = -1;
  QVERIFY(AssetCache::validateConfiguration(cacheConfig).has_value());
  AssetCache cache(cacheConfig);
  QVERIFY(!cache.isValid());
  AssetRequestCoordinator coordinator(cache, fetcher);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));

  std::optional<Result> result;
  int completions = 0;
  const AssetRequestCoordinator::RequestHandle handle =
      coordinator.request(key, [&](Result r) {
        ++completions;
        result = std::move(r);
      });
  QVERIFY(handle.isValid());

  // Completion is still delivered asynchronously-deferred (never
  // synchronously from inside request() itself), exactly like every
  // other immediate-completion path (e.g. InvalidIdentifier) -- but it
  // must arrive promptly, without ever waiting on any real network I/O.
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 2000));
  QCOMPARE(completions, 1);
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::InvalidConfiguration);

  // No network request was ever issued for this key.
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           0);
  QCOMPARE(coordinator.inFlightOperationCountForTesting(), 0);

  // A second, concurrent request against the same invalid cache must
  // independently fail the same way -- this is not accidentally
  // "coalescing" onto some leftover state from the first call.
  std::optional<Result> secondResult;
  coordinator.request(key, [&](Result r) { secondResult = std::move(r); });
  QVERIFY(QTest::qWaitFor([&]() { return secondResult.has_value(); }, 2000));
  QVERIFY(!bool(*secondResult));
  QCOMPARE(secondResult->error().code, AssetErrorCode::InvalidConfiguration);
  QCOMPARE(server.requestCount(QStringLiteral("/img/arkham/sets/valid01.png")),
           0);
}

void AssetRequestCoordinatorTests::
    crossInstanceSiblingDefinitiveInvalidateDuringInFlightFetchPreventsStalePublish() {
  // Cumulative independent re-review (HIGH, "shared root authority
  // incomplete", "AssetCache.cpp:1826,3085,3402": "store has no
  // token"). AssetRequestCoordinator's own per-instance
  // issuedGeneration/tryApplyCacheKeyMutation() CAS (proved by
  // delayedStaleFetchSuccessNeverOverwritesNewerCrossLogicalKeyCacheEntry
  // above) only ever protects against races between operations of the
  // SAME coordinator instance. It has no way to observe a SIBLING
  // AssetRequestCoordinator/process that shares the identical physical
  // AssetCache root directory and independently, durably invalidates the
  // exact same resolved candidate/cache key while THIS coordinator's own
  // fetch for that key is still genuinely in flight -- exactly the
  // "delayed 200 vs 404/clear" race the review describes. This test
  // proves the production dispatch path -- not merely AssetCache's own
  // direct unit tests -- genuinely closes it, via a real, slow-dripped
  // in-flight HTTP fetch.
  MockHttpServer server;
  const QString path = QStringLiteral("/img/arkham/sets/valid01.png");
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8);
  // Slow-dripped so there is a deterministic window, after request()
  // mints its assetCacheGeneration token but before the fetch actually
  // completes, in which the sibling's invalidate() is guaranteed to have
  // already landed.
  response.slowDrip = true;
  response.chunkSize = 32;
  response.chunkDelayMs = 20;
  server.setResponse(path, response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);

  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;
  AssetCache cacheA(cacheConfig);
  // A second AssetCache instance over the SAME physical root -- stands
  // in for a sibling AssetRequestCoordinator/process sharing this cache
  // (see RootAuthority's comment in AssetCache.cpp: same-root instances
  // in this process genuinely share one authority object, exactly as
  // independent processes would via the on-disk root lock/manifest).
  AssetCache cacheB(cacheConfig);

  AssetRequestCoordinator coordinatorA(cacheA, fetcher);

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  std::optional<Result> result;
  coordinatorA.request(key, [&](Result r) { result = std::move(r); });

  // Genuinely in flight: the slow-dripped response has not finished
  // arriving yet, and the coordinator's own assetCacheGeneration token
  // was already minted synchronously inside request()/startCandidate()
  // before this point.
  QVERIFY(
      QTest::qWaitFor([&]() { return server.requestCount(path) > 0; }, 5000));
  QVERIFY(!result.has_value());

  // The sibling instance now durably invalidates this exact cache key --
  // standing in for a second, independent coordinator/process that has
  // already discovered and persisted a definitive, authoritative 404 for
  // the identical resolved candidate while A's fetch is still in flight.
  QCOMPARE(cacheB.invalidate(cacheKey),
           AssetCache::InvalidateResult::DurablyInvalidated);
  QVERIFY(!cacheA.lookupDisk(cacheKey).has_value());
  QVERIFY(!cacheA.lookupMemory(cacheKey).has_value());

  // Let A's in-flight, now-stale 200 finish arriving.
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  // A's own caller still genuinely receives its real network result --
  // this fix must never suppress DELIVERY to the consumer who asked for
  // it, only the stale CACHE PERSISTENCE that would otherwise resurrect
  // a sibling's newer, authoritative invalidate().
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).encodedBytes, response.body);

  // Fail-before/pass-after: prior to threading assetCacheGeneration
  // through dispatchCandidateFetchResult()'s store() call, A's stale
  // success would unconditionally overwrite B's authoritative
  // invalidate() the instant it completed. It must instead remain
  // durably absent from disk and memory, from BOTH siblings' point of
  // view, exactly as B left it.
  QVERIFY(!cacheA.lookupDisk(cacheKey).has_value());
  QVERIFY(!cacheB.lookupDisk(cacheKey).has_value());
  QVERIFY(!cacheA.lookupMemory(cacheKey).has_value());
  QVERIFY(!cacheB.lookupMemory(cacheKey).has_value());
}

void AssetRequestCoordinatorTests::
    crossInstanceSiblingDefinitiveInvalidateDuringInFlightRevalidationPreventsStaleTouch() {
  // Companion to the fetch-path test above, for the revalidation
  // (touchAfterNotModified) mutation instead of store(). A disk-hit-
  // with-validators triggers a real conditional GET; while THAT is in
  // flight, a sibling AssetCache instance both durably invalidates AND
  // republishes a genuinely newer entry for the identical key (modelling
  // a second coordinator/process that completed its own full fetch cycle
  // in the interim); the origin then confirms 304 (still the OLD
  // validator) for A's stale in-flight revalidation. Before threading
  // assetCacheGeneration through dispatchRevalidationResult()'s
  // touchAfterNotModified() call, this stale "confirmed unchanged" touch
  // would have clobbered the sibling's newer entry's metadata --
  // AssetCache-level rejection is the ONLY thing that can catch this,
  // since the coordinator's own per-instance issuedGeneration CAS cannot
  // observe a mutation made through a wholly separate AssetCache/
  // coordinator instance.
  MockHttpServer server;
  const QString path = QStringLiteral("/img/arkham/sets/valid01.png");
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodePng(8, 8); // must never be served: a 304 is answered
  response.etagForConditionalMatch = QStringLiteral("\"v1-etag\"").toUtf8();
  server.setResponse(path, response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);

  AssetCache::Config cacheConfig;
  cacheConfig.directory = m_tempDirPath;

  const AssetKey key =
      makeKey(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
  const auto candidates = AssetLocator::resolveCandidates(key);
  QVERIFY(bool(candidates));
  const QString cacheKey = AssetCache::cacheKeyFor(candidates->first().url);

  {
    // Scoped and destroyed before constructing cacheA below: RootAuthority
    // instances sharing one physical root also genuinely share ONE memory
    // cache (see AssetCache.cpp's RootAuthority comment) -- if this seed
    // instance stayed alive, cacheA would inherit its still-live v1 memory
    // entry and take the memory-hit path, which never revalidates at all.
    // Destroying it here (dropping RootAuthority's refcount to zero) is
    // exactly the established convention this codebase uses to force a
    // genuine "restart with empty memory, disk-hit-with-validators" path
    // -- see diskHitRevalidationCoalescesConcurrentIdenticalRequests()'s
    // identical pattern above.
    AssetCache seedCache(cacheConfig);
    AssetCache::CachedEntry v1;
    // Real, decodable PNG bytes: a confirmed-304 "stale-if-error" result
    // is decoded via ensureDecoded() exactly like any other served entry
    // (see registerCacheHitCompletion()'s comment) -- opaque placeholder
    // bytes would fail that decode and take the (correct, but unrelated)
    // quarantine-and-refetch path instead, masking the exact race this
    // test exists to prove.
    v1.encodedBytes = encodePng(4, 4);
    v1.contentType = QStringLiteral("image/png");
    v1.dimensions = QSize(4, 4);
    v1.etag = QStringLiteral("\"v1-etag\"");
    seedCache.store(cacheKey, v1);
  }

  // A fresh instance over the same root forces the disk-hit-with-
  // validators path (never the memory-hit path, which never
  // revalidates) -- same pattern as
  // diskHitRevalidationCoalescesConcurrentIdenticalRequests() above.
  AssetCache cacheA(cacheConfig);
  AssetRequestCoordinator coordinatorA(cacheA, fetcher);

  std::optional<Result> result;
  coordinatorA.request(key, [&](Result r) { result = std::move(r); });

  // The conditional GET's assetCacheGeneration token was minted
  // synchronously inside request()/startRevalidation() above, before
  // request() returned -- there has been no event-loop turn yet for the
  // real HTTP round trip (even to a fast, non-slow-dripped mock server)
  // to have completed, so this window is deterministic without needing
  // slowDrip (which this mock server's bodyless-304 path does not
  // support).
  QVERIFY(!result.has_value());

  // A second, independent AssetCache instance over the identical root --
  // standing in for a sibling coordinator/process -- durably invalidates
  // the stale v1 entry and republishes a genuinely newer v2, exactly as
  // if it had already completed its own full fetch cycle for this key in
  // the interim.
  AssetCache cacheB(cacheConfig);
  QCOMPARE(cacheB.invalidate(cacheKey),
           AssetCache::InvalidateResult::DurablyInvalidated);
  AssetCache::CachedEntry v2;
  v2.encodedBytes = encodePng(6, 6);
  v2.contentType = QStringLiteral("image/png");
  v2.dimensions = QSize(6, 6);
  v2.etag = QStringLiteral("\"v2-etag\"");
  cacheB.store(cacheKey, v2);

  // A's stale revalidation now completes with a 304 confirming the OLD
  // (v1) validator -- this must be rejected outright by the AssetCache-
  // level CAS, never overwriting v2's metadata with a "confirmed still
  // v1" touch.
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));
  QVERIFY2(bool(*result), qPrintable(result->error().message));

  const auto afterCompletion = cacheB.lookupDisk(cacheKey);
  QVERIFY(afterCompletion.has_value());
  QCOMPARE(afterCompletion->etag, QStringLiteral("\"v2-etag\""));
  QCOMPARE(afterCompletion->encodedBytes, v2.encodedBytes);
  // The origin was never asked for the full (unrelated) 8x8 fixture --
  // proof the confirmed-304 path, not some fallback full refetch, is
  // what actually ran.
  QCOMPARE(server.requestCount(path), 1);
}
