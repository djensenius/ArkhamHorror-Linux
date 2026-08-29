#include "AssetNetworkFetcherTests.h"

#include "AssetNetworkFetcher.h"
#include "MockHttpServer.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>
#include <avif/avif.h>
#include <cstring>
#include <optional>
#include <type_traits>

using namespace Arkham;

// Review round-4 item 1, compile-time proof: AssetFetchUrl has no public
// constructor an arbitrary caller could invoke -- std::is_constructible_v
// correctly evaluates accessibility (a private constructor makes the
// expression ill-formed, which the trait reports as "not constructible"
// rather than a hard compile error), so this statically documents and
// enforces that no code anywhere -- test or production -- can construct
// an AssetFetchUrl directly from a QUrl without going through the
// validating validate() factory.
static_assert(!std::is_constructible_v<AssetFetchUrl, QUrl>,
              "AssetFetchUrl must not be constructible from an arbitrary "
              "QUrl outside AssetFetchUrl::validate()");
static_assert(!std::is_default_constructible_v<AssetFetchUrl>,
              "AssetFetchUrl must not be default-constructible");

namespace {

QByteArray encodeImage(int width, int height, const char *format) {
  QImage image(width, height, QImage::Format_Mono);
  image.fill(0);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  const bool ok = image.save(&buffer, format);
  // Copilot review: Q_ASSERT compiles out in release builds, which would
  // silently turn a fixture-encoding failure here into a confusing
  // downstream test failure (an empty/garbage `bytes` fed to the fetcher)
  // instead of a clear, immediate diagnosis. qFatal() is enforced in
  // every build configuration.
  if (!ok) {
    qFatal("encodeImage() failed to encode a %dx%d test fixture image as "
           "%s",
           width, height, format);
  }
  return bytes;
}

using Outcome = AssetOutcome<AssetNetworkFetcher::ConditionalFetchResult>;

// Encodes a tiny, genuinely valid, original (never-shipped) AVIF fixture
// at test-runtime via libavif's own encoder API -- exactly mirroring
// encodeImage()'s convention of generating JPEG/PNG fixtures on the fly
// via QImage::save() above, rather than committing any binary image
// blob to the repository. Used both to prove the real decode path
// (AssetAvifDecoder.cpp) succeeds on genuine input, and as the base for
// the "dimension bomb" test below (patchAvifIspeBoxDimensions()), which
// mutates a copy of this same real, tiny encoded payload's declared
// container dimensions without touching its actual (still-tiny) AV1
// pixel payload.
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
  // Fill with a flat, non-black colour: solid black is a degenerate input
  // some encoders special-case, and this fixture should exercise a
  // normal encode/decode round trip.
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

// Encodes a genuine, real (never-shipped) AVIF IMAGE SEQUENCE ("avis") of
// exactly `frameCount` (>= 2) identical tiny frames via libavif's own
// avifEncoderAddImage()/avifEncoderFinish() API -- used to prove (review
// item 6) that decodeAvifImage() rejects a sequence/animation outright
// via decoder->imageCount, never silently decoding only its first frame.
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
    const bool isLast = (frame == frameCount - 1);
    addResult = avifEncoderAddImage(encoder, image, /*durationInTimescales=*/1,
                                    isLast ? AVIF_ADD_IMAGE_FLAG_NONE
                                           : AVIF_ADD_IMAGE_FLAG_NONE);
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

// Returns a copy of `original` (a real, validly-encoded AVIF produced by
// encodeAvifFixture()) with its ISOBMFF `ispe` ("Image Spatial Extents")
// box's declared width/height fields overwritten to `width`/`height`,
// leaving the actual (still-tiny) AV1 pixel payload completely
// untouched. This lets a test assert that a declared-dimension check
// runs (and rejects the input) BEFORE any real pixel decode/allocation
// is attempted -- see AssetAvifDecoder.cpp's decodeAvifImage(), which
// checks decoder->image->width/height immediately after
// avifDecoderParse() (metadata only) and before ever calling
// avifDecoderNextImage() (full AV1 decode + pixel buffer allocation) --
// without needing to actually encode a real multi-billion-pixel image.
// The `ispe` box layout (ISO/IEC 14496-12 + ISO/IEC 23008-12 Annex B):
// size(4) + "ispe"(4) + version_and_flags(4) + width(4) + height(4), all
// big-endian.
QByteArray patchAvifIspeBoxDimensions(const QByteArray &original, quint32 width,
                                      quint32 height) {
  const char *needle = "ispe";
  const int needleIndex = original.indexOf(needle);
  if (needleIndex < 0) {
    qFatal("patchAvifIspeBoxDimensions() could not find an 'ispe' box in "
           "the supplied fixture bytes");
  }
  QByteArray patched = original;
  const int widthOffset = needleIndex + 4 /* "ispe" */ + 4 /* version+flags */;
  const int heightOffset = widthOffset + 4;
  if (heightOffset + 4 > patched.size()) {
    qFatal("patchAvifIspeBoxDimensions() found an 'ispe' box too close to "
           "the end of the buffer to hold width+height fields");
  }
  auto writeBigEndianU32 = [&patched](int offset, quint32 value) {
    patched[offset + 0] = static_cast<char>((value >> 24) & 0xFF);
    patched[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
    patched[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
    patched[offset + 3] = static_cast<char>(value & 0xFF);
  };
  writeBigEndianU32(widthOffset, width);
  writeBigEndianU32(heightOffset, height);
  return patched;
}

// Review round-4 item 1: every direct fetch() call site in this file must
// validate first, exactly like production code -- there is no overload
// that accepts a bare QUrl. Every URL this test suite ever passes here is
// a real loopback MockHttpServer URL, so a validation failure is always
// a test bug; qFatal() for the same reason fetchAndWait()'s timeout
// branch does.
AssetFetchUrl mustValidate(const QUrl &url) {
  AssetOutcome<AssetFetchUrl> validated = AssetFetchUrl::validate(url);
  if (!validated) {
    qFatal("mustValidate() was given a URL that failed "
           "AssetFetchUrl::validate(): %s",
           qPrintable(validated.error().message));
  }
  return *validated;
}

// Fetches synchronously (from the test's point of view) by pumping the
// event loop until the callback fires or `timeoutMs` elapses. A timeout
// here is always a test bug, never an expected outcome -- rather than
// returning std::nullopt and relying on every call site to check
// has_value() before dereferencing (a silent crash risk for any call
// site that ever forgot to), aborts deterministically via qFatal(),
// enforced in every build configuration, exactly like this test suite's
// other "must never silently continue" invariants (see
// MockHttpServer's constructor qFatal()ing on listen() failure).
std::optional<Outcome>
fetchAndWait(AssetNetworkFetcher &fetcher, const QUrl &url, AssetFormat format,
             AssetNetworkFetcher::ConditionalHeaders conditional = {},
             int timeoutMs = 5000) {
  // Review round-4 item 1: fetch() no longer accepts a bare QUrl (see
  // AssetFetchUrl's class comment) -- every test call site must validate
  // first too, exactly like production code. Every URL this test suite's
  // helper is ever called with is a real loopback MockHttpServer URL, so
  // a validation failure here is always a test bug, not an expected
  // outcome -- qFatal() for the same reason the timeout branch below
  // does.
  const AssetOutcome<AssetFetchUrl> validated = AssetFetchUrl::validate(url);
  if (!validated) {
    qFatal("fetchAndWait() was given a URL that failed "
           "AssetFetchUrl::validate(): %s",
           qPrintable(validated.error().message));
  }
  std::optional<Outcome> result;
  fetcher.fetch(*validated, format, conditional,
                [&result](Outcome outcome) { result = std::move(outcome); });
  if (!QTest::qWaitFor([&result]() { return result.has_value(); }, timeoutMs)) {
    qFatal("fetchAndWait() timed out after %dms waiting for the fetch "
           "callback to fire",
           timeoutMs);
  }
  return result;
}

} // namespace

void AssetNetworkFetcherTests::successfulFetchNeverSendsCookiesOrAuthHeader() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodeImage(64, 64, "png");
  server.setResponse(QStringLiteral("/ok.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/ok.png")), AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QVERIFY(!(**result).notModified);
  QCOMPARE((**result).asset->dimensions, QSize(64, 64));

  const auto headers = server.lastRequestHeaders(QStringLiteral("/ok.png"));
  QVERIFY(!headers.contains("cookie"));
  QVERIFY(!headers.contains("authorization"));
}

void AssetNetworkFetcherTests::
    nonHttpSchemeIsRejectedAsUnsupportedSchemeWithoutTouchingNetwork() {
  // Review round-4 item 1: AssetNetworkFetcher::fetch() no longer
  // accepts a bare QUrl at all (see AssetFetchUrl's class comment in
  // AssetNetworkFetcher.h) -- there is no overload, public or private,
  // that would let this file:// URL reach fetch() in the first place.
  // This test now proves the STRONGER claim directly at the validation
  // boundary: AssetFetchUrl::validate() itself rejects a non-http(s)
  // scheme with the typed UnsupportedScheme error, so a real, existing
  // local file containing known "secret" bytes is never read at all --
  // there is no fetch() call, no QNetworkAccessManager involvement, and
  // no possibility of the local file's content ever being surfaced as a
  // (mis-sniffed, but still leaked) successful result.
  QTemporaryFile localFile;
  QVERIFY(localFile.open());
  const QByteArray secretBytes = QByteArrayLiteral("not-a-real-image-secret");
  QCOMPARE(localFile.write(secretBytes), secretBytes.size());
  localFile.close();
  const QUrl fileUrl = QUrl::fromLocalFile(localFile.fileName());
  QCOMPARE(fileUrl.scheme(), QStringLiteral("file"));

  const AssetOutcome<AssetFetchUrl> validated =
      AssetFetchUrl::validate(fileUrl);
  QVERIFY(!validated);
  QCOMPARE(validated.error().code, AssetErrorCode::UnsupportedScheme);
}

void AssetNetworkFetcherTests::
    candidateUrlPolicyRejectsUserinfoQueryFragmentAndNonLoopbackHttp_data() {
  QTest::addColumn<QString>("urlString");
  QTest::addColumn<AssetErrorCode>("expectedCode");

  QTest::newRow("http-nonloopback-host")
      << QStringLiteral("http://example.com/a.png")
      << AssetErrorCode::InvalidCandidateUrl;
  QTest::newRow("http-lan-host") << QStringLiteral("http://192.168.1.5/a.png")
                                 << AssetErrorCode::InvalidCandidateUrl;
  QTest::newRow("userinfo-present")
      << QStringLiteral("https://user:pass@example.com/a.png")
      << AssetErrorCode::InvalidCandidateUrl;
  QTest::newRow("userinfo-present-on-loopback-http")
      << QStringLiteral("http://user:pass@127.0.0.1/a.png")
      << AssetErrorCode::InvalidCandidateUrl;
  QTest::newRow("query-present")
      << QStringLiteral("https://example.com/a.png?x=1")
      << AssetErrorCode::InvalidCandidateUrl;
  QTest::newRow("fragment-present")
      << QStringLiteral("https://example.com/a.png#frag")
      << AssetErrorCode::InvalidCandidateUrl;
  // Deliberately NOT tested here: "http://127.1/a.png" and other
  // ambiguous numeric-loopback spellings. AssetFetchUrl::validate()
  // necessarily operates on an already-QUrl-parsed candidate URL (every
  // real candidate comes from AssetLocator, which only ever builds a
  // path on top of an already-normalised base) -- and QUrl parsing
  // itself silently canonicalises "127.1" into "127.0.0.1" before this
  // function ever sees it (see AuthTransportSecurity.h's extensive
  // comment on isCleartextAuthAllowedForRawInput()). That raw-text
  // ambiguity defence already lives, and is already tested, at the
  // correct layer: UrlValidator::validateCustomUrl(), which
  // ValidatedAssetSource::fromRaw() calls against the ORIGINAL raw base
  // URL string before any QUrl round-trip -- see AssetLocatorTests.cpp's
  // asset-base validation tests.
}

void AssetNetworkFetcherTests::
    candidateUrlPolicyRejectsUserinfoQueryFragmentAndNonLoopbackHttp() {
  // Review round-4 item 1: proves AssetFetchUrl::validate() actually
  // reuses the shared transport policy (not a weaker asset-only
  // reimplementation) -- every one of these forgery attempts that a
  // "only check the scheme" fetch() implementation would have let
  // through is rejected here, before fetch() is ever reachable at all.
  QFETCH(QString, urlString);
  QFETCH(AssetErrorCode, expectedCode);

  const QUrl url(urlString, QUrl::StrictMode);
  QVERIFY(url.isValid());
  const AssetOutcome<AssetFetchUrl> validated = AssetFetchUrl::validate(url);
  QVERIFY(!validated);
  QCOMPARE(validated.error().code, expectedCode);
}

void AssetNetworkFetcherTests::
    candidateUrlPolicyAcceptsLoopbackHttpAndArbitraryHttpsHost() {
  // Companion positive case: a real resolved-candidate-shaped URL (https
  // to any host, or http to an exact canonical loopback spelling, no
  // userinfo/query/fragment) is accepted and round-trips unchanged.
  const QUrl httpsUrl(QStringLiteral("https://cdn.example.com/img/a.png"),
                      QUrl::StrictMode);
  const AssetOutcome<AssetFetchUrl> validatedHttps =
      AssetFetchUrl::validate(httpsUrl);
  QVERIFY2(bool(validatedHttps), qPrintable(validatedHttps.error().message));
  QCOMPARE(validatedHttps->url(), httpsUrl);

  const QUrl loopbackUrl(QStringLiteral("http://127.0.0.1:9999/img/a.png"),
                         QUrl::StrictMode);
  const AssetOutcome<AssetFetchUrl> validatedLoopback =
      AssetFetchUrl::validate(loopbackUrl);
  QVERIFY2(bool(validatedLoopback),
           qPrintable(validatedLoopback.error().message));
  QCOMPARE(validatedLoopback->url(), loopbackUrl);
}

void AssetNetworkFetcherTests::manualRedirectPolicyRejectsEvery3xx_data() {
  QTest::addColumn<int>("status");
  QTest::newRow("301") << 301;
  QTest::newRow("302") << 302;
  QTest::newRow("303") << 303;
  QTest::newRow("307") << 307;
  QTest::newRow("308") << 308;
}

void AssetNetworkFetcherTests::manualRedirectPolicyRejectsEvery3xx() {
  QFETCH(int, status);

  MockHttpServer server;
  MockHttpServer::Response response;
  response.status = status;
  response.reasonPhrase = "Redirect";
  response.extraHeaders.append(qMakePair(
      QByteArray("Location"), QByteArray("http://127.0.0.1/elsewhere")));
  server.setResponse(QStringLiteral("/redirect"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/redirect")),
                   AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::RedirectRejected);
}

void AssetNetworkFetcherTests::notFoundMapsToNotFoundError() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.status = 404;
  response.reasonPhrase = "Not Found";
  server.setResponse(QStringLiteral("/missing.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/missing.png")),
                   AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::NotFound);
}

void AssetNetworkFetcherTests::serverErrorMapsToUnexpectedStatus_data() {
  QTest::addColumn<int>("status");
  QTest::addColumn<bool>("withValidLookingImageBody");

  // Review item 4: body success is accepted ONLY for exactly HTTP 200
  // (304 is handled by the separate conditional-request tests below).
  // Every other status here -- including the 2xx variants a naive
  // "200 <= status < 300" range check would wrongly accept -- must be
  // rejected as UnexpectedStatus, even when (206 in particular) the body
  // looks like a perfectly valid, complete image: a partial-content
  // response must never be decoded/cached as if it were the full
  // representation.
  QTest::newRow("500-internal-server-error") << 500 << false;
  QTest::newRow("201-created") << 201 << false;
  QTest::newRow("202-accepted") << 202 << false;
  QTest::newRow("203-non-authoritative") << 203 << false;
  QTest::newRow("204-no-content") << 204 << false;
  QTest::newRow("206-partial-content-with-valid-looking-image-body")
      << 206 << true;
}

void AssetNetworkFetcherTests::serverErrorMapsToUnexpectedStatus() {
  QFETCH(int, status);
  QFETCH(bool, withValidLookingImageBody);

  MockHttpServer server;
  MockHttpServer::Response response;
  response.status = status;
  response.reasonPhrase = "Test Status";
  if (withValidLookingImageBody) {
    response.contentType = "image/png";
    response.body = encodeImage(4, 4, "PNG");
  }
  server.setResponse(QStringLiteral("/broken.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/broken.png")),
                   AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::UnexpectedStatus);
}

void AssetNetworkFetcherTests::incrementalByteCapAbortsBeforeFullBodyArrives() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  // The incremental byte cap is enforced purely on transferred byte count
  // (readyRead), before any Content-Type/magic-byte/image parsing, so the
  // body need not be a valid image at all here -- just larger than the
  // tiny 4 KiB test cap below and incompressible-content-agnostic (raw
  // pseudo-random bytes, so it can never accidentally compress away like
  // a uniform PNG fixture would), streamed slowly so the abort can be
  // observed to happen before the whole body was ever transmitted.
  QByteArray body(64 * 1024, Qt::Uninitialized);
  for (int i = 0; i < body.size(); ++i) {
    body[i] = static_cast<char>((i * 2654435761u) & 0xFF);
  }
  response.body = body;
  QVERIFY(response.body.size() > 4096);
  response.slowDrip = true;
  response.chunkSize = 256;
  response.chunkDelayMs = 10;
  server.setResponse(QStringLiteral("/big.png"), response);

  AssetNetworkFetcher::Limits limits;
  limits.maxEncodedBytes = 4096;
  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam, limits);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/big.png")), AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::ResponseTooLarge);

  // Give the server's timer-driven writer a moment to notice the
  // disconnect and stop, then confirm it never got to flush the whole
  // (much larger) body -- proving the cap was enforced incrementally,
  // not merely re-checked once the full body had already arrived.
  QTest::qWait(50);
  const qint64 flushed =
      server.lastBytesWrittenForSlowDrip(QStringLiteral("/big.png"));
  QVERIFY(flushed >= 0);
  QVERIFY2(flushed < response.body.size(),
           qPrintable(QStringLiteral("flushed=%1 total=%2")
                          .arg(flushed)
                          .arg(response.body.size())));
}

void AssetNetworkFetcherTests::
    finalDrainNeverExceedsByteCapForAFastNonIncrementalResponse() {
  // incrementalByteCapAbortsBeforeFullBodyArrives() above exercises the
  // readyRead-time abort path (a slow-drip body far larger than the cap,
  // caught mid-stream). This test targets the OTHER place the cap must
  // be enforced: handleFinished()'s drain of any bytes Qt already
  // buffered but never delivered via a readyRead signal before the
  // connection finished. A body sent all at once (no artificial delay),
  // only modestly larger than the cap, is the shape most likely to
  // arrive as a single burst with little or no intervening readyRead --
  // exactly the case the drain step alone must catch. The drain must
  // read with the same bounded "remaining budget plus one overflow byte"
  // technique as handleReadyRead(), never QNetworkReply::readAll(), so
  // the buffer can never transiently hold more than maxEncodedBytes + 1
  // bytes while still correctly surfacing ResponseTooLarge.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  QByteArray body(4096 + 64, Qt::Uninitialized);
  for (int i = 0; i < body.size(); ++i) {
    body[i] = static_cast<char>((i * 2654435761u) & 0xFF);
  }
  response.body = body;
  server.setResponse(QStringLiteral("/tail-overflow.png"), response);

  AssetNetworkFetcher::Limits limits;
  limits.maxEncodedBytes = 4096;
  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam, limits);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/tail-overflow.png")),
      AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::ResponseTooLarge);
}

void AssetNetworkFetcherTests::contentTypeMismatchIsRejected() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "text/plain"; // declared type does not match Jpeg
  response.body = encodeImage(32, 32, "png");
  server.setResponse(QStringLiteral("/wrong-type.jpg"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/wrong-type.jpg")),
      AssetFormat::Jpeg);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::ContentTypeMismatch);
}

void AssetNetworkFetcherTests::magicBytesMismatchIsRejected() {
  MockHttpServer server;
  MockHttpServer::Response response;
  // Content-Type correctly claims PNG, but the body is actually a JPEG:
  // this must be caught independently of the (possibly-lying) header.
  response.contentType = "image/png";
  response.body = encodeImage(32, 32, "jpg");
  server.setResponse(QStringLiteral("/lying.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/lying.png")),
                   AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MagicBytesMismatch);
}

void AssetNetworkFetcherTests::dimensionBombIsRejected() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  // A single dimension (8300) exceeds the default 8192 cap, while the
  // other dimension is tiny -- this is a genuine, fully valid PNG (highly
  // compressible, since it is a uniform mono image) so this exercises the
  // real QImageReader::size() pre-decode path, not a hand-crafted header.
  response.body = encodeImage(8300, 4, "png");
  server.setResponse(QStringLiteral("/wide.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/wide.png")),
                   AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::DimensionTooLarge);
}

void AssetNetworkFetcherTests::pixelBudgetBombIsRejected() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  // 6000 x 6000 = 36,000,000 pixels, over the 32,000,000 cap, while
  // neither dimension alone exceeds the 8192 per-side cap.
  response.body = encodeImage(6000, 6000, "png");
  server.setResponse(QStringLiteral("/huge.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/huge.png")),
                   AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::PixelBudgetExceeded);
}

void AssetNetworkFetcherTests::malformedImageBodyIsRejected() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  QByteArray body = encodeImage(64, 64, "png");
  body.chop(body.size() / 2); // truncate: passes magic-byte sniff, fails decode
  response.body = body;
  server.setResponse(QStringLiteral("/truncated.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/truncated.png")),
                   AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::jpegDecodesRegardlessOfQtPluginKeySpelling() {
  // Regression for a review finding: the fetcher's internal codec-support
  // check and QImageReader format hint must not hardcode a single spelling
  // of the JPEG plugin key. Qt's stock qjpeg plugin advertises both "jpeg"
  // and "jpg" (QImageReader::supportedImageFormats() includes both on this
  // build -- see the aliasing handled by isQtImageFormatSupported() in
  // AssetNetworkFetcher.cpp), but a fetcher that only recognised one
  // spelling would spuriously report UnsupportedCodec on any Qt build/
  // plugin set that only registers the other. A genuine, correctly
  // Content-Typed and magic-byte-valid JPEG must decode successfully
  // through the real production path end-to-end.
  const bool jpegSupported =
      QImageReader::supportedImageFormats().contains(
          QByteArrayLiteral("jpeg")) ||
      QImageReader::supportedImageFormats().contains(QByteArrayLiteral("jpg"));
  if (!jpegSupported) {
    QSKIP("this Qt build has no JPEG decode plugin under either key");
  }

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/jpeg";
  response.body = encodeImage(32, 32, "jpg");
  server.setResponse(QStringLiteral("/image.jpg"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/image.jpg")),
                   AssetFormat::Jpeg);

  QVERIFY(result.has_value());
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).asset->dimensions, QSize(32, 32));
}

void AssetNetworkFetcherTests::
    truncatedJpegMissingEoiIsRejectedDespiteQtDecodingIt() {
  // Review item 10 regression: Qt's bundled libjpeg-based decoder
  // tolerates a premature end-of-file within entropy-coded scan data by
  // *synthesising* a missing End-Of-Image marker -- it logs a "premature
  // end of data segment" warning but still returns a non-null image (this
  // was independently confirmed against this exact codebase's QImage/
  // QImageReader before writing this test). Chopping a substantial tail
  // off a real encoded JPEG removes both its genuine EOI marker AND a
  // chunk of entropy-coded scan data, which is exactly the shape a
  // network response truncated mid-transfer would have. This must be
  // rejected as MalformedImage, deliberately BEFORE ever reaching
  // QImageReader, regardless of whether this Qt build even has a JPEG
  // decode plugin installed -- so this test intentionally does not skip
  // on missing codec support.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/jpeg";
  QByteArray body = encodeImage(64, 64, "jpg");
  body.chop(100); // removes the trailing EOI marker and scan data
  response.body = body;
  server.setResponse(QStringLiteral("/truncated.jpg"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/truncated.jpg")),
                   AssetFormat::Jpeg);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::
    truncatedJpegMissingEoiAtVaryingCutPointsAllRejected_data() {
  QTest::addColumn<int>("chopPercent");
  // Cut depths expressed as a percentage of the encoded body's total
  // size (rather than a fixed byte count), so the codestream scanner is
  // reliably exercised truncating just past the trailing EOI, shallowly
  // into entropy-coded scan data, deep into scan data, and roughly at
  // the midpoint of the whole body -- regardless of exactly how large
  // the underlying JPEG fixture happens to encode to.
  QTest::newRow("chop-tiny-trailing-eoi-only") << 1;
  QTest::newRow("chop-shallow-into-scan-data") << 5;
  QTest::newRow("chop-deep-into-scan-data") << 25;
  QTest::newRow("chop-about-half-the-body") << 50;
}

void AssetNetworkFetcherTests::
    truncatedJpegMissingEoiAtVaryingCutPointsAllRejected() {
  QFETCH(int, chopPercent);

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/jpeg";
  QByteArray body = encodeImage(64, 64, "jpg");
  const int chopBytes =
      qMax(1, static_cast<int>(body.size()) * chopPercent / 100);
  QVERIFY(body.size() > chopBytes);
  body.chop(chopBytes);
  response.body = body;
  server.setResponse(QStringLiteral("/truncated.jpg"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/truncated.jpg")),
                   AssetFormat::Jpeg);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::
    jpegWithStuffedFFBytesInScanDataStillDecodesWhenComplete() {
  // The codestream-completeness scanner (review item 10) must not
  // misinterpret byte-stuffed 0xFF bytes (0xFF 0x00, mandated by the
  // JPEG spec for any literal 0xFF that occurs inside entropy-coded scan
  // data) as a marker that terminates the scan early -- a genuinely
  // COMPLETE JPEG containing such stuffing must still decode
  // successfully. A high-frequency/high-contrast image (checkerboard of
  // near-black/near-white pixels) reliably produces entropy-coded bytes
  // containing literal 0xFF values, forcing the encoder to emit stuffed
  // 0xFF 0x00 sequences.
  const bool jpegSupported =
      QImageReader::supportedImageFormats().contains(
          QByteArrayLiteral("jpeg")) ||
      QImageReader::supportedImageFormats().contains(QByteArrayLiteral("jpg"));
  if (!jpegSupported) {
    QSKIP("this Qt build has no JPEG decode plugin under either key");
  }

  QImage image(48, 48, QImage::Format_RGB32);
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const int v = ((x / 3) + (y / 3)) % 2 == 0 ? 255 : 0;
      image.setPixel(x, y, qRgb(v, v, v));
    }
  }
  QByteArray body;
  QBuffer buffer(&body);
  buffer.open(QIODevice::WriteOnly);
  const bool encoded = image.save(&buffer, "JPG", /*quality=*/100);
  if (!encoded) {
    qFatal("failed to encode the checkerboard JPEG fixture");
  }
  // Sanity-check the fixture actually contains at least one stuffed
  // 0xFF 0x00 pair -- otherwise this test would pass trivially without
  // ever exercising the stuffing-handling branch of the scanner.
  bool foundStuffedByte = false;
  for (qsizetype i = 0; i + 1 < body.size(); ++i) {
    if (static_cast<unsigned char>(body[i]) == 0xFF &&
        static_cast<unsigned char>(body[i + 1]) == 0x00) {
      foundStuffedByte = true;
      break;
    }
  }
  QVERIFY2(foundStuffedByte,
           "fixture image did not produce a byte-stuffed 0xFF 0x00 "
           "sequence; test needs a higher-contrast fixture to exercise "
           "the stuffing branch");

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/jpeg";
  response.body = body;
  server.setResponse(QStringLiteral("/checkerboard.jpg"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/checkerboard.jpg")),
      AssetFormat::Jpeg);

  QVERIFY(result.has_value());
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).asset->dimensions, QSize(48, 48));
}

void AssetNetworkFetcherTests::jpegTrailingDataAfterGenuineEoiIsRejected() {
  // Review item 6/10: the strict trailing-data policy requires a genuine
  // EOI marker to be the very LAST byte of the body -- anything after it
  // (padding, or a second concatenated JPEG stream) is rejected, even
  // though the body up to and including that EOI is itself perfectly
  // complete and would decode fine on its own.
  const bool jpegSupported =
      QImageReader::supportedImageFormats().contains(
          QByteArrayLiteral("jpeg")) ||
      QImageReader::supportedImageFormats().contains(QByteArrayLiteral("jpg"));
  if (!jpegSupported) {
    QSKIP("this Qt build has no JPEG decode plugin under either key");
  }

  const QByteArray validJpeg = encodeImage(16, 16, "JPG");
  QVERIFY(validJpeg.size() > 2);
  QVERIFY(static_cast<unsigned char>(validJpeg[validJpeg.size() - 2]) == 0xFF &&
          static_cast<unsigned char>(validJpeg[validJpeg.size() - 1]) == 0xD9);

  // Case 1: a second, fully valid, concatenated JPEG stream appended
  // after the first's genuine EOI.
  {
    QByteArray concatenated = validJpeg + validJpeg;
    MockHttpServer server;
    MockHttpServer::Response response;
    response.contentType = "image/jpeg";
    response.body = concatenated;
    server.setResponse(QStringLiteral("/concatenated.jpg"), response);

    QNetworkAccessManager nam;
    AssetNetworkFetcher fetcher(nam);
    const auto result = fetchAndWait(
        fetcher, server.baseUrlFor(QStringLiteral("/concatenated.jpg")),
        AssetFormat::Jpeg);
    QVERIFY(result.has_value());
    QVERIFY(!bool(*result));
    QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
  }

  // Case 2: arbitrary trailing padding bytes (not even a valid marker)
  // after the genuine EOI.
  {
    QByteArray padded = validJpeg + QByteArray("\x00\x00\x00\x00", 4);
    MockHttpServer server;
    MockHttpServer::Response response;
    response.contentType = "image/jpeg";
    response.body = padded;
    server.setResponse(QStringLiteral("/padded.jpg"), response);

    QNetworkAccessManager nam;
    AssetNetworkFetcher fetcher(nam);
    const auto result =
        fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/padded.jpg")),
                     AssetFormat::Jpeg);
    QVERIFY(result.has_value());
    QVERIFY(!bool(*result));
    QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
  }

  // Control: the unmodified, single, complete stream must still decode.
  {
    MockHttpServer server;
    MockHttpServer::Response response;
    response.contentType = "image/jpeg";
    response.body = validJpeg;
    server.setResponse(QStringLiteral("/valid.jpg"), response);

    QNetworkAccessManager nam;
    AssetNetworkFetcher fetcher(nam);
    const auto result =
        fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/valid.jpg")),
                     AssetFormat::Jpeg);
    QVERIFY(result.has_value());
    QVERIFY2(bool(*result), qPrintable(result->error().message));
  }
}

void AssetNetworkFetcherTests::avifRealFixtureAlwaysDecodesViaLibavif() {
  // Review item 4 (PR #18 cumulative review): AVIF decode is no longer
  // "environment adaptive" (dependent on whatever Qt image plugins a
  // given build happens to have registered) -- it is always routed
  // directly through libavif's own C API (AssetAvifDecoder.cpp), which
  // is now a hard, required build dependency (see CMakeLists.txt's
  // pkg_check_modules(LIBAVIF REQUIRED ...)). A genuine, validly-encoded
  // AVIF fixture must therefore ALWAYS decode successfully here,
  // regardless of what QImageReader::supportedImageFormats() reports.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/avif";
  response.body = encodeAvifFixture(32, 24);
  server.setResponse(QStringLiteral("/image.avif"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/image.avif")),
                   AssetFormat::Avif);

  QVERIFY(result.has_value());
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE((**result).asset->dimensions, QSize(32, 24));
  QVERIFY(!(**result).asset->decodedImage.isNull());
}

void AssetNetworkFetcherTests::avifImageSequenceIsRejectedAsMalformedImage() {
  // Review item 6 / round-4 item 10: a genuine, validly-encoded AVIF
  // image SEQUENCE (decoder->imageCount > 1, i.e. an "avis"-brand
  // animation) must be rejected outright rather than silently decoding
  // only its first frame -- this project only ever serves/consumes a
  // single still image per asset candidate. Classified as
  // MalformedImage, NOT UnsupportedCodec: this build's libavif backend
  // fully supports decoding this exact bytestream -- the multi-image
  // structure itself violates this project's single-still-image
  // contract, an integrity/content-policy failure rather than a genuine
  // codec-support gap. AssetRequestCoordinator's quarantine logic (review
  // item 9) treats the two completely differently: MalformedImage
  // quarantines a disk hit and retries the SAME candidate as a network
  // miss, whereas UnsupportedCodec never does, because reserving it for
  // multi-image AVIF would let a permanently-multi-image resource poison
  // a disk entry forever with no way to ever resolve it by quarantining.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/avif";
  response.body = encodeAvifSequenceFixture(3);
  server.setResponse(QStringLiteral("/sequence.avif"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/sequence.avif")),
                   AssetFormat::Avif);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::
    avifMalformedContainerIsReportedAsMalformedImage() {
  // A body whose magic bytes/major_brand genuinely identify it as AVIF
  // (so it passes the independent magic-byte sniff) but which has no
  // meta/mdat box at all -- i.e. no image content whatsoever -- must be
  // rejected by libavif's own parse step as a structurally invalid
  // container. This is a genuine integrity failure (MalformedImage), NOT
  // AssetErrorCode::UnsupportedCodec: the distinction matters because
  // review item 9's quarantine logic treats them completely differently
  // (MalformedImage quarantines and retries as a network miss;
  // UnsupportedCodec never does, because the bytes are still valid).
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/avif";
  response.body = QByteArrayLiteral("\x00\x00\x00\x14"
                                    "ftyp"
                                    "avif"
                                    "\x00\x00\x00\x00"
                                    "avif");
  server.setResponse(QStringLiteral("/no-content.avif"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/no-content.avif")),
      AssetFormat::Avif);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::
    avifDimensionBombIsRejectedBeforeAnyPixelDecodeOrAllocation() {
  // A REAL, validly-encoded (tiny) AVIF whose `ispe` box has been
  // byte-patched to declare an enormous width/height -- while its actual
  // AV1 pixel payload remains genuinely tiny -- must be rejected with
  // DimensionTooLarge purely from avifDecoderParse()'s (metadata-only)
  // output, without libavif ever being asked to allocate/decode a full
  // pixel buffer for the (fictitious) huge declared size. This directly
  // exercises AssetAvifDecoder.cpp's "check decoder->image->width/height
  // immediately after avifDecoderParse(), before ever calling
  // avifDecoderNextImage()" design -- if that ordering were ever
  // accidentally reversed, this test would hang/OOM instead of failing
  // fast with a clean error.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/avif";
  // 20000x100: both libavif's OWN built-in defaults (32768 per-dimension,
  // 16384*16384 total) and this image's actual (tiny) AV1 payload are
  // fine with this shape, so avifDecoderParse() itself succeeds -- it is
  // AssetAvifDecoder.cpp's OWN post-parse width check (configured
  // maxDimensionPixels, default 8192) that must reject this, not
  // libavif's internal defaults or a parse-level failure.
  response.body = patchAvifIspeBoxDimensions(encodeAvifFixture(8, 8),
                                             /*width=*/20000,
                                             /*height=*/100);
  server.setResponse(QStringLiteral("/dimension-bomb.avif"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/dimension-bomb.avif")),
      AssetFormat::Avif);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::DimensionTooLarge);
}

void AssetNetworkFetcherTests::
    avifFtypBoxSizeZeroExtendsToEndOfBufferPerIsobmff() {
  // Per ISO/IEC 14496-12, a leading box size of 0 means "this box extends
  // to the end of the enclosing file", NOT a zero-length box. A body
  // whose `ftyp` box declares size 0 is still a spec-valid AVIF signature
  // and must be accepted by the independent magic-byte sniff (never
  // rejected as AssetErrorCode::MagicBytesMismatch). This synthetic body
  // is a bare `ftyp` box only (no meta/mdat), so libavif's own parse
  // step correctly rejects it as a structurally invalid container
  // (MalformedImage) once past the sniff -- AVIF decode is unconditional
  // now (review item 4), so this is no longer environment-dependent.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/avif";
  // 4-byte box size (0 == "extends to end of buffer") + "ftyp" + "avif"
  // major brand: exactly 12 bytes, the minimum sniffMagicBytes() inspects.
  response.body = QByteArrayLiteral("\x00\x00\x00\x00"
                                    "ftyp"
                                    "avif");
  server.setResponse(QStringLiteral("/zero-size.avif"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/zero-size.avif")),
      AssetFormat::Avif);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QVERIFY(result->error().code != AssetErrorCode::MagicBytesMismatch);
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::
    avifFtypMinorVersionIsNeverMisreadAsCompatibleBrand() {
  // ISO/IEC 14496-12 `ftyp` box layout: [0..4) box size, [4..8) "ftyp",
  // [8..12) major_brand, [12..16) minor_version (a 4-byte VERSION NUMBER,
  // not a brand identifier), [16..end) compatible_brands. A body whose
  // major_brand is a real, non-AVIF brand (e.g. "mif1", used by HEIF)
  // but whose minor_version bytes happen to spell "avif" must be
  // rejected as MagicBytesMismatch, never accepted as a real AVIF
  // signature -- treating minor_version as a scannable brand slot would
  // let a crafted/adversarial file masquerade as AVIF. This is
  // deterministic (MagicBytesMismatch) regardless of whether the local
  // Qt build has AVIF codec support: without the fix, a build lacking
  // AVIF support would instead surface UnsupportedCodec (having wrongly
  // accepted the signature first), and a build with AVIF support would
  // instead attempt a real decode of this 16-byte non-image body and
  // surface MalformedImage -- neither of which is the correct outcome.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/avif";
  // 4-byte box size (0 == extends to end) + "ftyp" + major_brand="mif1"
  // (a real, non-AVIF ISOBMFF brand) + minor_version bytes == "avif".
  response.body = QByteArrayLiteral("\x00\x00\x00\x00"
                                    "ftyp"
                                    "mif1"
                                    "avif");
  server.setResponse(QStringLiteral("/minor-version-trap.avif"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/minor-version-trap.avif")),
      AssetFormat::Avif);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MagicBytesMismatch);
}

void AssetNetworkFetcherTests::
    avifFtypTruncatedBoxSizeNeverReadsPastDeclaredBoundary() {
  // Per ISO/IEC 14496-12, a `ftyp` box must declare at least 12 bytes
  // (4-byte size + "ftyp" + 4-byte major_brand) to legally contain a
  // major_brand field at all. This body's leading size field declares
  // only 8 bytes -- i.e. the box claims to end immediately after "ftyp",
  // BEFORE any major_brand -- yet the buffer keeps going with 4 more
  // bytes that spell "avif". Those trailing bytes are NOT part of the
  // declared box (they belong to whatever comes after it, or are
  // adversarially crafted padding); a correct sniff must never read them
  // as major_brand and must reject this as MagicBytesMismatch. Without
  // the fix, the code read bytes.mid(8, 4) unconditionally regardless of
  // the (too-small) declared boxEnd, and would have misclassified this
  // truncated/malformed box as a genuine AVIF signature.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/avif";
  // 4-byte box size (8: box claims to end right after "ftyp", with no
  // room for major_brand) + "ftyp" + 4 trailing bytes that spell "avif"
  // but lie outside the declared box boundary.
  response.body = QByteArrayLiteral("\x00\x00\x00\x08"
                                    "ftyp"
                                    "avif");
  server.setResponse(QStringLiteral("/truncated-box.avif"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/truncated-box.avif")),
      AssetFormat::Avif);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MagicBytesMismatch);
}

namespace {
// Builds a synthetic AVIF `ftyp` box of exactly `totalSize` bytes: 4-byte
// big-endian size + "ftyp" + major_brand="mif1" (a real, non-AVIF
// ISOBMFF brand, so major_brand alone never satisfies the sniff) +
// 4-byte minor_version + a compatible_brands region filled with
// `fillerBrand` repeated to pad out to totalSize, with `matchBrand`
// (if non-null) written into the LAST 4-byte compatible_brands slot.
// totalSize must be 16 plus a multiple of 4 so the compatible_brands
// region divides evenly into whole 4-byte slots.
QByteArray buildLargeFtypBox(qint64 totalSize, const char *fillerBrand,
                             const char *matchBrand) {
  Q_ASSERT(totalSize >= 16 && (totalSize - 16) % 4 == 0);
  QByteArray body(totalSize, Qt::Uninitialized);
  body[0] = static_cast<char>((totalSize >> 24) & 0xFF);
  body[1] = static_cast<char>((totalSize >> 16) & 0xFF);
  body[2] = static_cast<char>((totalSize >> 8) & 0xFF);
  body[3] = static_cast<char>(totalSize & 0xFF);
  memcpy(body.data() + 4, "ftyp", 4);
  memcpy(body.data() + 8, "mif1", 4); // major_brand: real, non-AVIF brand
  memcpy(body.data() + 12, "\x00\x00\x00\x00", 4); // minor_version
  for (qint64 offset = 16; offset + 4 <= totalSize; offset += 4) {
    memcpy(body.data() + offset, fillerBrand, 4);
  }
  if (matchBrand != nullptr) {
    memcpy(body.data() + totalSize - 4, matchBrand, 4);
  }
  return body;
}
} // namespace

void AssetNetworkFetcherTests::
    avifCompatibleBrandMatchAtVeryLastSlotOfLargeBoxIsFound() {
  // Regression test for a refactor (Copilot review round 25) that
  // rewrote the compatible_brands scan loop to compare directly against
  // bytes.constData() via memcmp() instead of allocating a QByteArray
  // per iteration via mid() -- purely a CPU/allocation-cost fix with no
  // intended behavioural change. A large box (1 MiB, ~262k compatible
  // brand slots) whose ONLY matching brand sits in the very last slot
  // proves the rewritten loop still scans the full declared range
  // (rather than stopping early or miscomputing the final offset), and
  // that the pointer-arithmetic-based comparison is exactly equivalent
  // to the byte-value comparison it replaced.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/avif";
  response.body =
      buildLargeFtypBox(1 * 1024 * 1024, "QQQQ", /*matchBrand=*/"avif");
  server.setResponse(QStringLiteral("/large-last-slot.avif"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/large-last-slot.avif")),
      AssetFormat::Avif);

  QVERIFY(result.has_value());
  // The signature matched (that is what this test is proving); this
  // body has no meta/mdat box (only ftyp compatible_brands padding), so
  // libavif's own parse step rejects it as a structurally invalid
  // container -- MalformedImage, never MagicBytesMismatch.
  QVERIFY(!bool(*result));
  QVERIFY(result->error().code != AssetErrorCode::MagicBytesMismatch);
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::
    avifLargeBoxWithNoMatchingBrandAnywhereIsRejected() {
  // Companion to the "last slot" test above: the same large box shape
  // with NO matching brand anywhere in the compatible_brands region
  // must still be rejected as MagicBytesMismatch. Together the two
  // tests bound the rewritten scan's correctness at both ends of its
  // range: it must neither stop before the true match (this test) nor
  // report a false match when none exists (this test).
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/avif";
  response.body =
      buildLargeFtypBox(1 * 1024 * 1024, "QQQQ", /*matchBrand=*/nullptr);
  server.setResponse(QStringLiteral("/large-no-match.avif"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/large-no-match.avif")),
      AssetFormat::Avif);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MagicBytesMismatch);
}

void AssetNetworkFetcherTests::conditionalRequestAcceptsMatchingNotModified() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodeImage(16, 16, "png");
  response.etagForConditionalMatch = "\"abc123\"";
  server.setResponse(QStringLiteral("/conditional.png"), response);

  AssetNetworkFetcher::ConditionalHeaders conditional;
  conditional.etag = QStringLiteral("\"abc123\"");

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/conditional.png")),
      AssetFormat::Png, conditional);

  QVERIFY(result.has_value());
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QVERIFY((**result).notModified);
  QVERIFY(!(**result).asset.has_value());
}

void AssetNetworkFetcherTests::
    conditionalRequestWithLastModifiedAcceptsMatchingNotModified() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodeImage(16, 16, "png");
  response.lastModifiedForConditionalMatch = "Wed, 21 Oct 2015 07:28:00 GMT";
  server.setResponse(QStringLiteral("/conditional-lm.png"), response);

  AssetNetworkFetcher::ConditionalHeaders conditional;
  conditional.lastModified = QStringLiteral("Wed, 21 Oct 2015 07:28:00 GMT");

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/conditional-lm.png")),
      AssetFormat::Png, conditional);

  QVERIFY(result.has_value());
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QVERIFY((**result).notModified);
  QVERIFY(!(**result).asset.has_value());

  // A DIFFERENT If-Modified-Since value must NOT be treated as a match
  // (this is the exact behavior MockHttpServer previously got wrong: it
  // only checked header *presence*, not an exact value match).
  AssetNetworkFetcher::ConditionalHeaders mismatched;
  mismatched.lastModified = QStringLiteral("Thu, 01 Jan 1970 00:00:00 GMT");
  const auto mismatchedResult = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/conditional-lm.png")),
      AssetFormat::Png, mismatched);
  QVERIFY(mismatchedResult.has_value());
  QVERIFY2(bool(*mismatchedResult),
           qPrintable(mismatchedResult->error().message));
  QVERIFY(!(**mismatchedResult).notModified);
  QVERIFY((**mismatchedResult).asset.has_value());
}

void AssetNetworkFetcherTests::unconditional304IsRejectedAsTypedError() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodeImage(16, 16, "png");
  // The server will 304 on ANY request once an etag/etc match is
  // configured with an empty request-side header, simulating a
  // buggy/hostile server that 304s even though this client never sent a
  // conditional header at all (etagForConditionalMatch empty means we
  // instead force it by matching an always-absent If-None-Match... so
  // instead directly configure the server to unconditionally 304).
  response.status = 304;
  response.reasonPhrase = "Not Modified";
  response.body.clear();
  server.setResponse(QStringLiteral("/unexpected-304.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/unexpected-304.png")),
      AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::ConditionalWithoutCachedBody);
}

void AssetNetworkFetcherTests::cancelInvokesCallbackExactlyOnceWithCancelled() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodeImage(2000, 2000, "png");
  response.slowDrip = true;
  response.chunkSize = 16;
  response.chunkDelayMs = 200; // slow enough that the test can cancel first
  server.setResponse(QStringLiteral("/slow.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);

  int callCount = 0;
  std::optional<Outcome> result;
  const auto handle = fetcher.fetch(
      mustValidate(server.baseUrlFor(QStringLiteral("/slow.png"))),
      AssetFormat::Png, {}, [&](Outcome outcome) {
        ++callCount;
        result = std::move(outcome);
      });

  fetcher.cancel(handle);
  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));

  QCOMPARE(callCount, 1);
  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::Cancelled);

  // Cancelling an already-completed handle (or a stale one) is a no-op:
  // the callback must never fire a second time.
  fetcher.cancel(handle);
  QTest::qWait(50);
  QCOMPARE(callCount, 1);
}

void AssetNetworkFetcherTests::timeoutFiresExactlyOnceAndCleansUpItsTimer() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodeImage(16, 16, "png");
  response.slowDrip = true;
  // Long enough that the fetcher's own short timeout below always fires
  // first, well before any body byte (or even a full chunk) arrives.
  response.chunkDelayMs = 5000;
  server.setResponse(QStringLiteral("/never-completes.png"), response);

  QNetworkAccessManager nam;
  // A deliberately short timeout so this test does not need to wait
  // anywhere near kDefaultTimeout (30s) to observe the timeout path.
  AssetNetworkFetcher fetcher(nam, AssetNetworkFetcher::Limits{},
                              std::chrono::milliseconds(50));

  int callCount = 0;
  std::optional<Outcome> result;
  fetcher.fetch(
      mustValidate(server.baseUrlFor(QStringLiteral("/never-completes.png"))),
      AssetFormat::Png, {}, [&](Outcome outcome) {
        ++callCount;
        result = std::move(outcome);
      });

  QVERIFY(QTest::qWaitFor([&]() { return result.has_value(); }, 5000));
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::Transport);

  // The single-shot timeout timer fires exactly once by construction;
  // this only re-confirms the callback itself is never invoked twice
  // (the timer's own cleanup, previously leaked, is exercised by simply
  // running this path at all -- a leak is not user-observable here
  // without a heap profiler, but this proves the timeout completion path
  // itself still functions correctly after that cleanup was added).
  QTest::qWait(100);
  QCOMPARE(callCount, 1);
}

void AssetNetworkFetcherTests::destructionNeverInvokesStaleCallback() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodeImage(2000, 2000, "png");
  response.slowDrip = true;
  response.chunkSize = 16;
  response.chunkDelayMs = 200;
  server.setResponse(QStringLiteral("/slow2.png"), response);

  bool callbackFired = false;
  QNetworkAccessManager nam;
  {
    AssetNetworkFetcher fetcher(nam);
    fetcher.fetch(mustValidate(server.baseUrlFor(QStringLiteral("/slow2.png"))),
                  AssetFormat::Png, {},
                  [&callbackFired](Outcome) { callbackFired = true; });
    // fetcher is destroyed here, mid-flight.
  }

  QTest::qWait(300); // long enough that the slow drip would otherwise finish
  QVERIFY(!callbackFired);
}

void AssetNetworkFetcherTests::
    applicationProxyWithCredentialsIsNeverUsedOrLeaked() {
  // Review item 5: AssetNetworkFetcher's dedicated QNetworkAccessManager
  // must explicitly force QNetworkProxy::NoProxy, so it can never
  // inherit a process-wide application proxy (even one an embedding
  // application configured with embedded credentials) -- every request
  // must go directly to the origin named by its URL, and no
  // Proxy-Authorization header may ever leave the process. Setting a
  // deliberately unreachable bogus proxy host+port with credentials
  // proves this two ways at once: if AssetNetworkFetcher ever tried to
  // actually route through it, the request would fail/time out (nothing
  // is listening there); direct success instead is only possible if the
  // NoProxy override is genuinely in effect.
  const QNetworkProxy previousApplicationProxy =
      QNetworkProxy::applicationProxy();
  QNetworkProxy bogusProxy(QNetworkProxy::HttpProxy,
                           QStringLiteral("203.0.113.1"), 1, // TEST-NET-3,
                           QStringLiteral("proxyuser"),      // never routable
                           QStringLiteral("proxypass"));
  QNetworkProxy::setApplicationProxy(bogusProxy);
  struct ProxyRestoreGuard {
    QNetworkProxy previous;
    ~ProxyRestoreGuard() { QNetworkProxy::setApplicationProxy(previous); }
  } restoreGuard{previousApplicationProxy};

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodeImage(4, 4, "PNG");
  server.setResponse(QStringLiteral("/direct.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/direct.png")),
                   AssetFormat::Png, {}, /*timeoutMs=*/5000);

  QVERIFY2(result.has_value(),
           "request never completed -- AssetNetworkFetcher appears to "
           "have actually attempted to route through the unreachable "
           "bogus application proxy instead of connecting directly");
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE(server.requestCount(QStringLiteral("/direct.png")), 1);
  QVERIFY(!server.anyRequestEverHadHeader(
      QByteArrayLiteral("proxy-authorization")));
}

void AssetNetworkFetcherTests::
    borrowedManagerProxyReconfiguredAfterConstructionIsStillOverridden() {
  // Review round-4 item 1: the constructor's one-time
  // `m_nam.setProxy(QNetworkProxy::NoProxy)` call is not sufficient on
  // its own for a borrowed (externally-owned) manager, because the
  // caller retains its own live reference to the exact same object and
  // can reconfigure its proxy at ANY later point -- including after
  // AssetNetworkFetcher's constructor already ran. This test proves the
  // fix: fetch() re-asserts NoProxy immediately before every single
  // request, so even a proxy set on the borrowed manager well after
  // construction (and immediately before this specific fetch() call)
  // can never actually be used.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodeImage(4, 4, "PNG");
  server.setResponse(QStringLiteral("/toctou.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);

  // Reconfigure the BORROWED manager's proxy well after construction --
  // this is exactly the gap a one-time constructor-only NoProxy call
  // would leave open.
  nam.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy,
                             QStringLiteral("203.0.113.2"), 1, // TEST-NET-3,
                             QStringLiteral("proxyuser"),      // unroutable
                             QStringLiteral("proxypass")));

  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/toctou.png")),
                   AssetFormat::Png, {}, /*timeoutMs=*/5000);

  QVERIFY2(result.has_value(),
           "request never completed -- AssetNetworkFetcher appears to "
           "have actually attempted to route through the unreachable "
           "post-construction proxy instead of connecting directly");
  QVERIFY2(bool(*result), qPrintable(result->error().message));
  QCOMPARE(server.requestCount(QStringLiteral("/toctou.png")), 1);
  QVERIFY(!server.anyRequestEverHadHeader(
      QByteArrayLiteral("proxy-authorization")));
}

void AssetNetworkFetcherTests::
    invalidLimitsOrTimeoutIsRejectedAsInvalidConfigurationWithoutThrowing_data() {
  QTest::addColumn<qint64>("maxEncodedBytes");
  QTest::addColumn<int>("maxDimensionPixels");
  QTest::addColumn<qint64>("maxTotalPixels");
  QTest::addColumn<qint64>("timeoutMs");

  const qint64 validEncodedBytes = 20LL * 1024 * 1024;
  const int validDimension = 8192;
  const qint64 validTotalPixels = 32'000'000;
  const qint64 validTimeoutMs = 30'000;

  QTest::newRow("zero-encoded-bytes")
      << qint64(0) << validDimension << validTotalPixels << validTimeoutMs;
  QTest::newRow("negative-encoded-bytes")
      << qint64(-1) << validDimension << validTotalPixels << validTimeoutMs;
  QTest::newRow("encoded-bytes-above-sane-cap")
      << (AssetNetworkFetcher::kMaxAllowedEncodedBytes + 1) << validDimension
      << validTotalPixels << validTimeoutMs;
  QTest::newRow("zero-dimension")
      << validEncodedBytes << 0 << validTotalPixels << validTimeoutMs;
  QTest::newRow("negative-dimension")
      << validEncodedBytes << -1 << validTotalPixels << validTimeoutMs;
  QTest::newRow("zero-total-pixels")
      << validEncodedBytes << validDimension << qint64(0) << validTimeoutMs;
  QTest::newRow("zero-timeout-does-not-disable-it")
      << validEncodedBytes << validDimension << validTotalPixels << qint64(0);
  QTest::newRow("negative-timeout")
      << validEncodedBytes << validDimension << validTotalPixels << qint64(-1);
  // std::chrono::milliseconds::rep is platform-defined -- typically
  // "long" (not Qt's "long long"-based qint64) under 64-bit Linux/glibc,
  // but "long long" under macOS/libc++. QTest::addColumn<qint64>() above
  // requires an EXACT type match on every pushed value, so an implicit
  // "long" here silently registers as a different, unrelated metatype
  // and QFETCH(qint64, ...) aborts with a fatal type-mismatch assert --
  // this only ever showed up on Linux CI, never on macOS. An explicit
  // qint64 cast keeps the column type identical on every platform.
  QTest::newRow("timeout-above-sane-cap")
      << validEncodedBytes << validDimension << validTotalPixels
      << static_cast<qint64>(AssetNetworkFetcher::kMaxAllowedTimeout.count() +
                             1);
}

void AssetNetworkFetcherTests::
    invalidLimitsOrTimeoutIsRejectedAsInvalidConfigurationWithoutThrowing() {
  QFETCH(qint64, maxEncodedBytes);
  QFETCH(int, maxDimensionPixels);
  QFETCH(qint64, maxTotalPixels);
  QFETCH(qint64, timeoutMs);

  AssetNetworkFetcher::Limits limits;
  limits.maxEncodedBytes = maxEncodedBytes;
  limits.maxDimensionPixels = maxDimensionPixels;
  limits.maxTotalPixels = maxTotalPixels;
  const std::chrono::milliseconds timeout(timeoutMs);

  // create() must report the typed error rather than throw.
  const auto factoryResult = AssetNetworkFetcher::create(limits, timeout);
  QVERIFY(!factoryResult.has_value());
  QCOMPARE(factoryResult.error().code, AssetErrorCode::InvalidConfiguration);

  // The raw constructor must likewise never throw -- it must construct
  // successfully into a permanently-invalid state instead.
  AssetNetworkFetcher fetcher(limits, timeout);
  QVERIFY(!fetcher.isValid());
  QCOMPARE(fetcher.configurationError().code,
           AssetErrorCode::InvalidConfiguration);

  // Every fetch() on such a fetcher must fail the exact same way,
  // asynchronously, without ever touching the network.
  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = encodeImage(4, 4, "PNG");
  server.setResponse(QStringLiteral("/should-never-be-requested.png"),
                     response);

  const auto fetchResult = fetchAndWait(
      fetcher,
      server.baseUrlFor(QStringLiteral("/should-never-be-requested.png")),
      AssetFormat::Png);
  QVERIFY(fetchResult.has_value());
  QVERIFY(!bool(*fetchResult));
  QCOMPARE(fetchResult->error().code, AssetErrorCode::InvalidConfiguration);
  QCOMPARE(
      server.requestCount(QStringLiteral("/should-never-be-requested.png")), 0);
}

void AssetNetworkFetcherTests::validConfigurationFactorySucceeds() {
  const auto result = AssetNetworkFetcher::create();
  QVERIFY2(result.has_value(), qPrintable(result.error().message));
  QVERIFY((*result)->isValid());
}
