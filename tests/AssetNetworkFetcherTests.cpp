#include "AssetNetworkFetcherTests.h"

#include "AssetFetchUrlTestSupport.h"
#include "AssetJpegDecoder.h"
#include "AssetNetworkFetcher.h"
#include "AssetPngValidator.h"
#include "MockHttpServer.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>
#include <array>
#include <atomic>
#include <avif/avif.h>
#include <cstring>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>
#include <zlib.h>

using namespace Arkham;

// Review round-4 item 1, compile-time proof: AssetFetchUrl has no public
// constructor an arbitrary caller could invoke -- std::is_constructible_v
// correctly evaluates accessibility (a private constructor makes the
// expression ill-formed, which the trait reports as "not constructible"
// rather than a hard compile error), so this statically documents and
// enforces that no code anywhere -- test or production -- can construct
// an AssetFetchUrl directly from a QUrl without going through the
// validating validate() factory.
//
// Review round-5 item 1 (PR #18 cumulative review at 6bdc68cf): validate()
// itself is now private too (reachable only from AssetRequestCoordinator
// and, via the dedicated AssetFetchUrlTestSupport seam included above,
// this test suite) -- see AssetNetworkFetcher.h's class comment on
// AssetFetchUrl. That access restriction is enforced structurally by the
// compiler at every call site (this translation unit only compiles
// because it goes through AssetFetchUrlTestSupport::validate(), never
// AssetFetchUrl::validate() directly) rather than by a trait here:
// std::is_constructible's access-checking guarantee is specific to
// constructors, and there is no equivalent SFINAE-friendly trait for an
// arbitrary private static member function's accessibility (confirmed
// experimentally: a requires-expression naming an inaccessible private
// member is a hard compile error in this project's compilers, not a
// substitution failure) -- so unlike the constructibility checks below,
// this specific guarantee cannot be expressed as a negative static_assert
// without producing a permanently-failing build.
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
  AssetOutcome<AssetFetchUrl> validated =
      AssetFetchUrlTestSupport::validate(url);
  if (!validated) {
    qFatal("mustValidate() was given a URL that failed "
           "AssetFetchUrlTestSupport::validate(): %s",
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
  const AssetOutcome<AssetFetchUrl> validated =
      AssetFetchUrlTestSupport::validate(url);
  if (!validated) {
    qFatal("fetchAndWait() was given a URL that failed "
           "AssetFetchUrlTestSupport::validate(): %s",
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

// Cumulative-review finding (PR #18, exact head 4a47ea34): shared PNG
// chunk-construction helpers for the new zTXt/iCCP/iTXt-metadata-bomb and
// IDAT-exact-zlib-consumption regression tests below. Deliberately a
// second, independent implementation of the same CRC-32 algorithm as
// pngCrc32() in src/AssetPngValidator.cpp (mirroring this test file's own
// pre-existing local per-test lambdas of the identical algorithm, e.g.
// pngWithApngAnimationChunksIsRejected()/pngWithNonConsecutiveIdatChunksIsRejected()
// above) -- a bug in the production CRC-32 table would then be equally
// likely to appear in both the code under test and this fixture-building
// helper, which would be worthless; keeping this test-side implementation
// independent (typed out separately, not calling into
// src/AssetPngValidator.cpp at all) is what makes a fixture actually
// prove the production validator's OWN CRC checking is correct.
quint32 pngCrc32ForTests(const QByteArray &bytes) {
  static const auto table = [] {
    std::array<quint32, 256> t{};
    for (quint32 n = 0; n < 256; ++n) {
      quint32 c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[n] = c;
    }
    return t;
  }();
  quint32 crc = 0xFFFFFFFFu;
  for (unsigned char byte : bytes) {
    crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

void appendPngChunkForTests(QByteArray &out, const QByteArray &type,
                            const QByteArray &data) {
  const quint32 length = static_cast<quint32>(data.size());
  out.append(static_cast<char>((length >> 24) & 0xFF));
  out.append(static_cast<char>((length >> 16) & 0xFF));
  out.append(static_cast<char>((length >> 8) & 0xFF));
  out.append(static_cast<char>(length & 0xFF));
  out.append(type);
  out.append(data);
  const quint32 crc = pngCrc32ForTests(type + data);
  out.append(static_cast<char>((crc >> 24) & 0xFF));
  out.append(static_cast<char>((crc >> 16) & 0xFF));
  out.append(static_cast<char>((crc >> 8) & 0xFF));
  out.append(static_cast<char>(crc & 0xFF));
}

// Splits a valid, freshly-encoded PNG fixture (as produced by
// encodeImage(w, h, "png")) into its IHDR-and-before prefix, the exact
// concatenated data bytes of every IDAT chunk (in file order), and its
// IEND-and-after suffix -- letting a test rebuild the same image with a
// deliberately modified IDAT run while keeping IHDR/IEND byte-for-byte
// identical to the known-good original (so any rejection observed is
// attributable only to the IDAT modification).
struct SplitPngForTests {
  QByteArray beforeIdat; // signature + IHDR chunk, verbatim
  QByteArray idatData;   // every IDAT chunk's data, concatenated
  QByteArray fromIend;   // IEND chunk (and, for a valid fixture, nothing
                         // else), verbatim
};

SplitPngForTests splitValidPngForTests(const QByteArray &png) {
  SplitPngForTests split;
  const qsizetype firstIdatType = png.indexOf("IDAT");
  if (firstIdatType < 0) {
    qFatal("splitValidPngForTests() was given a PNG fixture with no IDAT "
           "chunk at all");
  }
  split.beforeIdat = png.left(firstIdatType - 4); // exclude length field too

  qsizetype pos = firstIdatType - 4;
  while (true) {
    const quint32 length =
        (static_cast<unsigned char>(png[static_cast<int>(pos)]) << 24) |
        (static_cast<unsigned char>(png[static_cast<int>(pos + 1)]) << 16) |
        (static_cast<unsigned char>(png[static_cast<int>(pos + 2)]) << 8) |
        static_cast<unsigned char>(png[static_cast<int>(pos + 3)]);
    const QByteArray type = png.mid(pos + 4, 4);
    const qsizetype dataStart = pos + 8;
    if (type == "IDAT") {
      split.idatData += png.mid(dataStart, static_cast<qsizetype>(length));
    } else if (type == "IEND") {
      split.fromIend = png.mid(pos);
      break;
    }
    pos = dataStart + static_cast<qsizetype>(length) + 4; // + CRC
    if (pos >= png.size()) {
      qFatal("splitValidPngForTests() walked off the end of the fixture "
             "without finding IEND");
    }
  }
  return split;
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
      AssetFetchUrlTestSupport::validate(fileUrl);
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
  // Round-9+ review (MEDIUM): an explicit but empty "?" or "#" delimiter
  // must be rejected exactly like a non-empty one -- see
  // AssetFetchUrl::validate()'s hasQuery()/hasFragment() comment.
  QTest::newRow("query-present-empty")
      << QStringLiteral("https://example.com/a.png?")
      << AssetErrorCode::InvalidCandidateUrl;
  QTest::newRow("fragment-present-empty")
      << QStringLiteral("https://example.com/a.png#")
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
  const AssetOutcome<AssetFetchUrl> validated =
      AssetFetchUrlTestSupport::validate(url);
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
      AssetFetchUrlTestSupport::validate(httpsUrl);
  QVERIFY2(bool(validatedHttps), qPrintable(validatedHttps.error().message));
  QCOMPARE(validatedHttps->url(), httpsUrl);

  const QUrl loopbackUrl(QStringLiteral("http://127.0.0.1:9999/img/a.png"),
                         QUrl::StrictMode);
  const AssetOutcome<AssetFetchUrl> validatedLoopback =
      AssetFetchUrlTestSupport::validate(loopbackUrl);
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

void AssetNetworkFetcherTests::
    jpegDecodesDirectlyViaLibjpegIndependentOfQtImagePlugins() {
  // Regression for review item 2 (PR #18 cumulative review at 14cf8de6):
  // JPEG is decoded directly against libjpeg's own C API (see
  // AssetJpegDecoder.h/.cpp), never through QImageReader/Qt's plugin
  // registry -- exactly mirroring how AVIF is decoded (AssetAvifDecoder.h/
  // .cpp). This is a required build dependency (see CMakeLists.txt), so a
  // genuine, correctly Content-Typed and magic-byte-valid JPEG must decode
  // successfully through the real production path unconditionally,
  // regardless of whatever Qt image plugins this build happens to have
  // registered (this test therefore, deliberately, does NOT guard/skip on
  // QImageReader::supportedImageFormats() the way it did before this
  // architecture change).
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

void AssetNetworkFetcherTests::
    jpegTruncatedEntropyDataWithForgedGenuineEoiIsRejected() {
  // Round-4 review item 8: jpegCodestreamHasGenuineEoi() (review item 10)
  // proves the marker STRUCTURE ends in a genuine EOI, but it cannot
  // detect a truncated entropy-coded SCAN with a forged EOI appended
  // directly after the cut -- from the marker scanner's point of view,
  // "truncate mid-scan, then append a real 0xFF 0xD9" looks identical to
  // "the scan legitimately ended here": the very next 0xFF byte after the
  // cut point is treated as terminating the scan, and it happens to
  // actually be 0xD9. This is precisely the "truncate entropy then append
  // EOI" attack this review item requires rejecting -- proven here by
  // confirming the OLD scanner-only check alone would have accepted this
  // fixture, then confirming the full decode path (which additionally
  // requires the per-call libjpeg error/warning manager in
  // AssetJpegDecoder.cpp to observe no corrupt-data warning) rejects it.
  const QByteArray validJpeg = encodeImage(64, 64, "JPG");
  QVERIFY(validJpeg.size() > 200);
  // Cut well into the entropy-coded scan data (past SOI/APPn/DQT/SOF/DHT/
  // SOS headers for a 64x64 fixture), then append a genuine top-level EOI
  // marker directly -- no attempt is made to keep the truncated entropy
  // bytes self-consistent; libjpeg's own entropy decoder recovering from
  // this (by synthesising the rest of the image and warning, rather than
  // erroring) is exactly the behaviour under test.
  QByteArray forged = validJpeg.left(validJpeg.size() * 6 / 10);
  forged.append(static_cast<char>(0xFF));
  forged.append(static_cast<char>(0xD9));

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/jpeg";
  response.body = forged;
  server.setResponse(QStringLiteral("/forged-eoi.jpg"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/forged-eoi.jpg")),
      AssetFormat::Jpeg);

  QVERIFY(result.has_value());
  QVERIFY2(!bool(*result),
           "a truncated entropy stream with a forged trailing EOI must "
           "never be accepted as a successfully-decoded image");
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::
    concurrentJpegDecodesOnDifferentThreadsNeverCrossContaminate() {
  // Review item 2 (PR #18 cumulative review at 14cf8de6): the previous
  // ScopedJpegDecodeWarningDetector kept its entire warning-detection
  // state (previous handler, active flag, "saw warning" flag) in
  // process-global `static inline` variables shared across EVERY decode.
  // Two decodes overlapping on different threads would corrupt each
  // other's state -- most dangerously, a second constructor call would
  // capture the first instance's own forwardingHandler as "previous",
  // making the eventual restore reinstall a self-referential handler
  // (infinite recursion/stack overflow on the next logged message).
  // AssetJpegDecoder.h/.cpp's decodeJpegImage() fixes this at the root:
  // every piece of per-decode state (the jpeg_error_mgr, its setjmp
  // buffer, its warning counter) lives in a plain local on THIS call's
  // own stack, threaded through libjpeg only via this call's own cinfo --
  // nothing is ever static or shared, so concurrent calls on different
  // threads are independent by construction, with no lock required.
  //
  // This test proves that directly: many threads repeatedly decode a
  // MIX of a genuinely valid JPEG and a genuinely corrupt/truncated one
  // (forcing libjpeg's real error/warning path on some calls while other
  // threads are mid-decode of the valid one), then asserts every single
  // call -- across every thread, every iteration -- got exactly the
  // outcome its own input warrants. Any cross-contamination (a valid
  // decode spuriously failing, or a corrupt decode spuriously
  // "succeeding" because some other thread's state leaked in) would fail
  // this deterministically, not just probabilistically -- the old
  // handler-corruption bug's failure mode was a crash/hang, and this
  // test's structure (fully independent stack-local state, verified
  // outcome-by-outcome) is what makes a clean pass actually meaningful
  // rather than merely "didn't happen to crash this run."
  const QByteArray validJpeg = encodeImage(48, 48, "JPG");
  QVERIFY(validJpeg.size() > 200);

  QByteArray corruptJpeg = validJpeg.left(validJpeg.size() * 4 / 10);
  // No forged EOI appended here -- decodeJpegImage() is exercised
  // directly (not through the marker-structure EOI prescan in
  // AssetNetworkFetcher.cpp), so libjpeg's own entropy decoder is what
  // must observe and recover from (and thus warn on) this truncation.

  constexpr int kThreadCount = 8;
  constexpr int kIterationsPerThread = 25;
  std::atomic<int> unexpectedOutcomes{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int t = 0; t < kThreadCount; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kIterationsPerThread; ++i) {
        // Alternate which input this thread/iteration decodes so valid
        // and corrupt decodes are genuinely interleaved/overlapping in
        // time across threads, not merely running the same input on
        // every thread.
        const bool decodeValid = ((t + i) % 2) == 0;
        const AssetOutcome<QImage> outcome =
            decodeValid
                ? Arkham::decodeJpegImage(validJpeg, /*maxDimensionPixels=*/
                                          8192,      /*maxTotalPixels=*/
                                          32 * 1024 * 1024)
                : Arkham::decodeJpegImage(corruptJpeg, 8192, 32 * 1024 * 1024);
        if (decodeValid) {
          if (!bool(outcome) || (*outcome).size() != QSize(48, 48)) {
            unexpectedOutcomes.fetch_add(1);
          }
        } else {
          if (bool(outcome) ||
              outcome.error().code != AssetErrorCode::MalformedImage) {
            unexpectedOutcomes.fetch_add(1);
          }
        }
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  QCOMPARE(unexpectedOutcomes.load(), 0);
}

void AssetNetworkFetcherTests::pngWithCorruptChunkCrcIsRejected() {
  // Round-4 review item 8: a PNG whose IDAT payload was tampered with
  // (leaving its declared length and the surrounding chunk structure
  // otherwise perfectly well-formed, but invalidating the stored CRC-32)
  // must be rejected before ever reaching QImageReader, rather than
  // relying on libpng's own leniency (or strictness) around CRC errors.
  QByteArray body = encodeImage(16, 16, "png");
  // Locate an IDAT chunk and flip one bit inside its data, well clear of
  // the length/type/CRC fields.
  const qsizetype idatTypeOffset = body.indexOf("IDAT");
  QVERIFY(idatTypeOffset > 4);
  const qsizetype flipOffset = idatTypeOffset + 4 + 1; // inside IDAT data
  QVERIFY(flipOffset < body.size());
  body[static_cast<int>(flipOffset)] = static_cast<char>(
      static_cast<unsigned char>(body[static_cast<int>(flipOffset)]) ^ 0xFF);

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = body;
  server.setResponse(QStringLiteral("/corrupt-crc.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/corrupt-crc.png")),
      AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::pngWithApngAnimationChunksIsRejected_data() {
  QTest::addColumn<QByteArray>("chunkType");
  QTest::newRow("acTL") << QByteArray("acTL");
  QTest::newRow("fcTL") << QByteArray("fcTL");
  QTest::newRow("fdAT") << QByteArray("fdAT");
}

void AssetNetworkFetcherTests::pngWithApngAnimationChunksIsRejected() {
  // Round-4 review item 8: an APNG (animated PNG) is a "multiple image"
  // in the same sense the AVIF imageCount!=1 case is -- QImageReader can
  // plausibly decode just the base IDAT frame while silently ignoring the
  // animation frames declared by acTL/fcTL/fdAT, which is exactly the
  // kind of accept-a-subset-of-the-payload behaviour this review item
  // requires rejecting outright rather than tolerating.
  QFETCH(QByteArray, chunkType);
  QVERIFY(chunkType.size() == 4);

  QByteArray body = encodeImage(16, 16, "png");
  const qsizetype ihdrTypeOffset = body.indexOf("IHDR");
  QVERIFY(ihdrTypeOffset > 4);
  // Insert the animation chunk immediately after IHDR (a structurally
  // valid, well-formed, correctly-CRC'd chunk with an empty payload is
  // sufficient to exercise the rejection -- its contents are never
  // otherwise interpreted).
  const qsizetype insertPos = ihdrTypeOffset + 4 + 13 + 4; // past IHDR+CRC
  QVERIFY(insertPos <= body.size());
  QByteArray animationChunk;
  animationChunk.append(char(0), 4); // length = 0, big-endian
  animationChunk.append(chunkType);
  const quint32 crc = [&]() {
    // Mirror pngCrc32()'s zlib/ISO-3309 polynomial by computing the CRC
    // with the same table-driven algorithm, so the test fixture's
    // injected chunk has a valid CRC and the test exercises the
    // chunk-TYPE rejection specifically, not an incidental CRC failure.
    static const auto table = [] {
      std::array<quint32, 256> t{};
      for (quint32 n = 0; n < 256; ++n) {
        quint32 c = n;
        for (int k = 0; k < 8; ++k) {
          c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        t[n] = c;
      }
      return t;
    }();
    quint32 crcAcc = 0xFFFFFFFFu;
    for (unsigned char byte : chunkType) {
      crcAcc = table[(crcAcc ^ byte) & 0xFFu] ^ (crcAcc >> 8);
    }
    return crcAcc ^ 0xFFFFFFFFu;
  }();
  animationChunk.append(static_cast<char>((crc >> 24) & 0xFF));
  animationChunk.append(static_cast<char>((crc >> 16) & 0xFF));
  animationChunk.append(static_cast<char>((crc >> 8) & 0xFF));
  animationChunk.append(static_cast<char>(crc & 0xFF));
  body.insert(insertPos, animationChunk);

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = body;
  server.setResponse(QStringLiteral("/apng.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/apng.png")),
                   AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::pngWithTrailingBytesAfterIendIsRejected() {
  // Round-4 review item 8: any byte after IEND (padding, or a second
  // concatenated PNG stream) must be rejected -- mirroring the JPEG
  // trailing-data policy (jpegTrailingDataAfterGenuineEoiIsRejected()).
  const QByteArray validPng = encodeImage(16, 16, "png");
  QByteArray withTrailer = validPng + validPng;

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = withTrailer;
  server.setResponse(QStringLiteral("/trailing.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/trailing.png")),
                   AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::pngWithMultipleIhdrChunksIsRejected() {
  // Round-4 review item 8: more than one IHDR chunk is never valid in a
  // single bare (non-animated) PNG stream.
  QByteArray body = encodeImage(16, 16, "png");
  const qsizetype ihdrTypeOffset = body.indexOf("IHDR");
  QVERIFY(ihdrTypeOffset > 4);
  const qsizetype ihdrChunkStart = ihdrTypeOffset - 4; // include length field
  const qsizetype ihdrChunkEnd = ihdrTypeOffset + 4 + 13 + 4; // + data + CRC
  QVERIFY(ihdrChunkEnd <= body.size());
  const QByteArray ihdrChunk =
      body.mid(ihdrChunkStart, ihdrChunkEnd - ihdrChunkStart);
  // Duplicate the well-formed IHDR chunk immediately after itself.
  body.insert(ihdrChunkEnd, ihdrChunk);

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = body;
  server.setResponse(QStringLiteral("/dup-ihdr.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/dup-ihdr.png")),
                   AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::pngWithNonConsecutiveIdatChunksIsRejected() {
  // Round-5 review item 3: a PNG whose single logical IDAT run has been
  // split into two separate IDAT chunks with an intervening chunk of a
  // different type (IHDR,IDAT,tEXt,IDAT,IEND) is CRC-valid per-chunk and
  // a decoder that merely concatenates every IDAT payload it encounters
  // (ignoring the PNG spec's "IDAT chunks must be consecutive"
  // requirement) would accept and cache it. This must be rejected.
  auto crc32Of = [](const QByteArray &bytes) {
    static const auto table = [] {
      std::array<quint32, 256> t{};
      for (quint32 n = 0; n < 256; ++n) {
        quint32 c = n;
        for (int k = 0; k < 8; ++k) {
          c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        t[n] = c;
      }
      return t;
    }();
    quint32 crcAcc = 0xFFFFFFFFu;
    for (unsigned char byte : bytes) {
      crcAcc = table[(crcAcc ^ byte) & 0xFFu] ^ (crcAcc >> 8);
    }
    return crcAcc ^ 0xFFFFFFFFu;
  };
  auto appendChunk = [&](QByteArray &out, const QByteArray &type,
                         const QByteArray &data) {
    QVERIFY(type.size() == 4);
    const quint32 length = static_cast<quint32>(data.size());
    out.append(static_cast<char>((length >> 24) & 0xFF));
    out.append(static_cast<char>((length >> 16) & 0xFF));
    out.append(static_cast<char>((length >> 8) & 0xFF));
    out.append(static_cast<char>(length & 0xFF));
    out.append(type);
    out.append(data);
    const quint32 crc = crc32Of(type + data);
    out.append(static_cast<char>((crc >> 24) & 0xFF));
    out.append(static_cast<char>((crc >> 16) & 0xFF));
    out.append(static_cast<char>((crc >> 8) & 0xFF));
    out.append(static_cast<char>(crc & 0xFF));
  };

  const QByteArray originalPng = encodeImage(16, 16, "png");
  const qsizetype idatTypeOffset = originalPng.indexOf("IDAT");
  QVERIFY(idatTypeOffset > 4);
  const qsizetype idatLengthOffset = idatTypeOffset - 4;
  const quint32 idatLength =
      (static_cast<unsigned char>(
           originalPng[static_cast<int>(idatLengthOffset)])
       << 24) |
      (static_cast<unsigned char>(
           originalPng[static_cast<int>(idatLengthOffset + 1)])
       << 16) |
      (static_cast<unsigned char>(
           originalPng[static_cast<int>(idatLengthOffset + 2)])
       << 8) |
      static_cast<unsigned char>(
          originalPng[static_cast<int>(idatLengthOffset + 3)]);
  const qsizetype idatDataOffset = idatTypeOffset + 4;
  const QByteArray idatData =
      originalPng.mid(idatDataOffset, static_cast<qsizetype>(idatLength));
  QVERIFY(idatData.size() > 4); // needs to be splittable into two nonempty
                                // halves for this test to be meaningful

  const qsizetype splitPoint = idatData.size() / 2;
  const QByteArray firstHalf = idatData.left(splitPoint);
  const QByteArray secondHalf = idatData.mid(splitPoint);

  // Rebuild: signature + IHDR (unchanged) + IDAT(firstHalf) + tEXt +
  // IDAT(secondHalf) + IEND (unchanged). Concatenating firstHalf+secondHalf
  // reproduces the exact original (valid) zlib stream, so any rejection
  // observed below is attributable ONLY to the non-consecutive IDAT
  // structure, not to corrupted image data.
  const qsizetype idatChunkEnd = idatDataOffset + idatLength + 4; // + CRC
  QByteArray body = originalPng.left(idatLengthOffset);
  appendChunk(body, "IDAT", firstHalf);
  appendChunk(body, "tEXt", QByteArrayLiteral("split"));
  appendChunk(body, "IDAT", secondHalf);
  body += originalPng.mid(idatChunkEnd);

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = body;
  server.setResponse(QStringLiteral("/split-idat.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/split-idat.png")),
      AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);

  // The above end-to-end assertion alone is not sufficient proof that
  // THIS project's own pngChunksAreStrictlyValid() is what rejected the
  // payload: a sufficiently strict host libpng build may independently
  // refuse to decode a non-consecutive IDAT stream regardless of our own
  // pre-check (verified locally: this exact crafted body is ALSO
  // rejected by QImageReader's underlying libpng even with the
  // idatRunClosed tracking above disabled). Calling the validator
  // directly proves this project's OWN structural policy -- not an
  // incidental agreement with one particular host decoder -- is what
  // makes this rejection deterministic across every target platform.
  QVERIFY(!Arkham::pngChunksAreStrictlyValid(body));
  // Control: the unmodified original body (same image data, still
  // structurally a single consecutive IDAT run) must still be accepted,
  // proving the rejection above is specific to the non-consecutive
  // structure and not some incidental difference introduced by rebuilding
  // the byte sequence chunk-by-chunk.
  QVERIFY(Arkham::pngChunksAreStrictlyValid(originalPng));
}

void AssetNetworkFetcherTests::
    pngWithCompressedAncillaryChunkIsRejected_data() {
  QTest::addColumn<QByteArray>("chunkType");
  QTest::newRow("zTXt") << QByteArray("zTXt");
  QTest::newRow("iCCP") << QByteArray("iCCP");
  QTest::newRow("iTXt-compressed") << QByteArray("iTXt");
}

void AssetNetworkFetcherTests::pngWithCompressedAncillaryChunkIsRejected() {
  // Cumulative-review finding (PR #18, prior head 4a47ea34): a PNG can
  // carry a zTXt/iCCP/compressed-iTXt chunk whose OWN independently
  // zlib-compressed payload QImageReader's underlying libpng would
  // decompress during header parsing (png_read_info()) -- entirely
  // independent of the image's declared dimensions/pixel budget, and
  // strictly before this project's own dimension/pixel-budget checks
  // would otherwise run. This proves such a chunk is rejected purely
  // STRUCTURALLY (by chunk TYPE alone, never by inspecting its payload)
  // -- including when the payload is a genuine "metadata bomb" shape (a
  // tiny compressed blob that would decompress to many megabytes), built
  // here via a real zlib deflate of a large, highly-repetitive buffer so
  // the fixture itself stays small while still being a faithful bomb
  // shape. Rejecting by TYPE alone, before ever touching the payload
  // bytes, is what keeps this bounded: this project never allocates or
  // runs an inflate() proportional to the bomb's declared/decompressed
  // size at all.
  QFETCH(QByteArray, chunkType);

  constexpr int kBombDecompressedSize = 8 * 1024 * 1024;
  const QByteArray bombSource(kBombDecompressedSize, '\0');
  uLongf compressedBound = compressBound(static_cast<uLong>(bombSource.size()));
  QByteArray compressed(static_cast<qsizetype>(compressedBound),
                        Qt::Uninitialized);
  uLongf actualCompressedSize = compressedBound;
  const int compressResult = compress2(
      reinterpret_cast<Bytef *>(compressed.data()), &actualCompressedSize,
      reinterpret_cast<const Bytef *>(bombSource.constData()),
      static_cast<uLong>(bombSource.size()), Z_BEST_COMPRESSION);
  QCOMPARE(compressResult, Z_OK);
  compressed.resize(static_cast<qsizetype>(actualCompressedSize));
  // Stays tiny in the fixture despite representing an 8 MiB payload --
  // proving this is a faithful "small on the wire, huge if decompressed"
  // bomb shape (an all-zero 8 MiB buffer compresses to roughly 8 KiB
  // under zlib's own maximum window/block-size limits, verified via
  // zlib's compress2()/Z_BEST_COMPRESSION directly).
  QVERIFY(compressed.size() < 16384);

  QByteArray chunkData;
  if (chunkType == "zTXt") {
    // zTXt: Keyword\0 Compression-method(1 byte, must be 0) then the
    // compressed text.
    chunkData = QByteArrayLiteral("Bomb") + QByteArray(1, '\0') +
                QByteArray(1, '\0') + compressed;
  } else if (chunkType == "iCCP") {
    // iCCP: Profile-name\0 Compression-method(1 byte, must be 0) then
    // the compressed ICC profile.
    chunkData = QByteArrayLiteral("Bomb") + QByteArray(1, '\0') +
                QByteArray(1, '\0') + compressed;
  } else {
    // iTXt: Keyword\0 Compression-flag(1, nonzero=compressed)
    // Compression-method(1, must be 0 when flag set) Language-tag\0
    // Translated-keyword\0 then the (compressed) text.
    chunkData = QByteArrayLiteral("Bomb") + QByteArray(1, '\0') +
                QByteArray(1, '\x01') + QByteArray(1, '\0') +
                QByteArray(1, '\0') + QByteArray(1, '\0') + compressed;
  }

  QByteArray body = encodeImage(16, 16, "png");
  const qsizetype ihdrTypeOffset = body.indexOf("IHDR");
  QVERIFY(ihdrTypeOffset > 4);
  const qsizetype insertPos = ihdrTypeOffset + 4 + 13 + 4; // past IHDR + CRC
  QVERIFY(insertPos <= body.size());
  QByteArray chunk;
  appendPngChunkForTests(chunk, chunkType, chunkData);
  body.insert(insertPos, chunk);

  // Prove the rejection is structural/immediate, calling the validator
  // directly (never through QImageReader/network at all) -- the
  // "before QImageReader" and "bounded memory" proof: a real 8 MiB bomb
  // payload is rejected without this project's own code ever attempting
  // to decompress it.
  QVERIFY(!Arkham::pngChunksAreStrictlyValid(body));

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = body;
  server.setResponse(QStringLiteral("/metadata-bomb.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/metadata-bomb.png")),
      AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
}

void AssetNetworkFetcherTests::pngWithUncompressedTextChunksIsStillAccepted() {
  // Control for pngWithCompressedAncillaryChunkIsRejected(): a plain
  // uncompressed tEXt chunk, and an iTXt chunk with its compression flag
  // explicitly clear, pose no decompression risk at all and must still
  // be accepted -- proving the rejection above is specific to actual
  // compression being in play, not "any text/metadata chunk of any
  // kind."
  QByteArray body = encodeImage(16, 16, "png");
  const qsizetype ihdrTypeOffset = body.indexOf("IHDR");
  QVERIFY(ihdrTypeOffset > 4);
  const qsizetype insertPos = ihdrTypeOffset + 4 + 13 + 4;
  QVERIFY(insertPos <= body.size());

  QByteArray chunks;
  appendPngChunkForTests(chunks, QByteArrayLiteral("tEXt"),
                         QByteArrayLiteral("Comment") + QByteArray(1, '\0') +
                             QByteArrayLiteral("hello"));
  appendPngChunkForTests(
      chunks, QByteArrayLiteral("iTXt"),
      QByteArrayLiteral("Comment") + QByteArray(1, '\0') +
          QByteArray(1, '\0') /* compression flag = 0 */ +
          QByteArray(1, '\0') /* compression method */ +
          QByteArray(1, '\0') /* empty language tag */ +
          QByteArray(1, '\0') /* empty translated keyword */ +
          QByteArrayLiteral("hello"));
  body.insert(insertPos, chunks);

  QVERIFY(Arkham::pngChunksAreStrictlyValid(body));

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = body;
  server.setResponse(QStringLiteral("/uncompressed-text.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/uncompressed-text.png")),
      AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY2(bool(*result), qPrintable(result->error().message));
}

void AssetNetworkFetcherTests::
    pngWithTrailingBytesAfterValidIdatZlibStreamIsRejected() {
  // Cumulative-review finding (PR #18, prior head 4a47ea34): a CRC-valid
  // IDAT run whose zlib stream is a complete, valid image encoding
  // followed by extra bytes (or an entirely separate second zlib
  // stream) is exactly what libpng/QImageReader tolerates (with at most
  // a swallowed warning) -- this project must reject it instead.
  const QByteArray originalPng = encodeImage(16, 16, "png");
  const SplitPngForTests split = splitValidPngForTests(originalPng);

  // Case 1: arbitrary trailing garbage bytes after the legitimate stream.
  const QByteArray withGarbage =
      split.idatData + QByteArrayLiteral("EXTRA-DATA-AFTER-VALID-STREAM");

  // Case 2: an entirely separate second, independently-valid zlib stream
  // appended after the first.
  QByteArray secondStream;
  {
    const unsigned char raw[4] = {1, 2, 3, 4};
    uLongf bound = compressBound(sizeof(raw));
    QByteArray compressed(static_cast<qsizetype>(bound), Qt::Uninitialized);
    uLongf actualSize = bound;
    const int rc = compress2(reinterpret_cast<Bytef *>(compressed.data()),
                             &actualSize, raw, sizeof(raw), Z_BEST_COMPRESSION);
    QCOMPARE(rc, Z_OK);
    compressed.resize(static_cast<qsizetype>(actualSize));
    secondStream = compressed;
  }
  const QByteArray withSecondStream = split.idatData + secondStream;

  for (const QByteArray &tamperedIdat : {withGarbage, withSecondStream}) {
    QByteArray body = split.beforeIdat;
    appendPngChunkForTests(body, QByteArrayLiteral("IDAT"), tamperedIdat);
    body += split.fromIend;

    // Structural chunk validity (CRC, ordering, etc.) is unaffected --
    // the IDAT chunk itself is perfectly well-formed; only its zlib
    // stream's CONTENT is tampered with.
    PngStructuralInfo info;
    QVERIFY(Arkham::pngChunksAreStrictlyValid(body, &info));
    QVERIFY(!Arkham::pngIdatDecompressesToExactExpectedSize(
        info.idatPayload, info.width, info.height, info.bitDepth,
        info.colorType));

    MockHttpServer server;
    MockHttpServer::Response response;
    response.contentType = "image/png";
    response.body = body;
    server.setResponse(QStringLiteral("/idat-trailing.png"), response);

    QNetworkAccessManager nam;
    AssetNetworkFetcher fetcher(nam);
    const auto result = fetchAndWait(
        fetcher, server.baseUrlFor(QStringLiteral("/idat-trailing.png")),
        AssetFormat::Png);

    QVERIFY(result.has_value());
    QVERIFY(!bool(*result));
    QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
  }

  // Control: the untouched original stream must still pass both checks.
  PngStructuralInfo originalInfo;
  QVERIFY(Arkham::pngChunksAreStrictlyValid(originalPng, &originalInfo));
  QVERIFY(Arkham::pngIdatDecompressesToExactExpectedSize(
      originalInfo.idatPayload, originalInfo.width, originalInfo.height,
      originalInfo.bitDepth, originalInfo.colorType));
}

void AssetNetworkFetcherTests::
    pngWithTruncatedIdatZlibStreamAtVaryingCutPointsAllRejected_data() {
  QTest::addColumn<int>("truncatedLength");
  const QByteArray originalPng = encodeImage(16, 16, "png");
  const SplitPngForTests split = splitValidPngForTests(originalPng);
  for (int len = 0; len < static_cast<int>(split.idatData.size()); ++len) {
    QTest::addRow("truncated-to-%d-of-%d-bytes", len,
                  static_cast<int>(split.idatData.size()))
        << len;
  }
}

void AssetNetworkFetcherTests::
    pngWithTruncatedIdatZlibStreamAtVaryingCutPointsAllRejected() {
  // Cumulative-review finding (PR #18, prior head 4a47ea34): "Tests ...
  // every truncation" -- every possible truncation point of a
  // legitimate IDAT zlib stream, short of the full/complete stream, must
  // be rejected: a partial zlib stream can never validly reach
  // Z_STREAM_END, so pngIdatDecompressesToExactExpectedSize() must
  // reject every one of them. This is a pure structural/unit-level test
  // (no network round trip) so exhaustively covering every cut point of
  // this fixture's (small) IDAT payload stays fast.
  QFETCH(int, truncatedLength);

  const QByteArray originalPng = encodeImage(16, 16, "png");
  const SplitPngForTests split = splitValidPngForTests(originalPng);
  QVERIFY(truncatedLength < static_cast<int>(split.idatData.size()));
  const QByteArray truncatedIdat = split.idatData.left(truncatedLength);

  QByteArray body = split.beforeIdat;
  appendPngChunkForTests(body, QByteArrayLiteral("IDAT"), truncatedIdat);
  body += split.fromIend;

  PngStructuralInfo info;
  QVERIFY(Arkham::pngChunksAreStrictlyValid(body, &info));
  QVERIFY(!Arkham::pngIdatDecompressesToExactExpectedSize(
      info.idatPayload, info.width, info.height, info.bitDepth,
      info.colorType));
}

void AssetNetworkFetcherTests::pngWithInterlacedIhdrIsRejected() {
  // Cumulative-review finding (PR #18, prior head 4a47ea34): Adam7
  // interlacing (IHDR interlace method 1) is rejected outright -- see
  // AssetPngValidator.h's doc comment for why.
  QByteArray body = encodeImage(16, 16, "png");
  const qsizetype ihdrTypeOffset = body.indexOf("IHDR");
  QVERIFY(ihdrTypeOffset > 4);
  const qsizetype ihdrDataStart = ihdrTypeOffset + 4;
  const qsizetype interlaceMethodOffset = ihdrDataStart + 12; // last IHDR byte
  QVERIFY(interlaceMethodOffset < body.size());
  body[static_cast<int>(interlaceMethodOffset)] = 1;
  // Recompute IHDR's CRC over its (now-modified) type+data.
  const qsizetype crcOffset = ihdrDataStart + 13;
  const QByteArray typeAndData = body.mid(ihdrTypeOffset, 4 + 13);
  const quint32 crc = pngCrc32ForTests(typeAndData);
  body[static_cast<int>(crcOffset)] = static_cast<char>((crc >> 24) & 0xFF);
  body[static_cast<int>(crcOffset + 1)] = static_cast<char>((crc >> 16) & 0xFF);
  body[static_cast<int>(crcOffset + 2)] = static_cast<char>((crc >> 8) & 0xFF);
  body[static_cast<int>(crcOffset + 3)] = static_cast<char>(crc & 0xFF);

  QVERIFY(!Arkham::pngChunksAreStrictlyValid(body));

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/png";
  response.body = body;
  server.setResponse(QStringLiteral("/interlaced.png"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result = fetchAndWait(
      fetcher, server.baseUrlFor(QStringLiteral("/interlaced.png")),
      AssetFormat::Png);

  QVERIFY(result.has_value());
  QVERIFY(!bool(*result));
  QCOMPARE(result->error().code, AssetErrorCode::MalformedImage);
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
  // Regression for a use-after-free: AssetAvifDecoder.cpp used to call
  // avifDecoderDestroy(decoder) and THEN read decoder->imageCount to
  // format this message, reading already-freed memory. A plain (non-ASan)
  // build can appear to "work" by accident depending on allocator reuse
  // timing, so this asserts the exact expected count is present in the
  // message (proving the read observed real, not clobbered/reused,
  // memory) rather than merely checking the error code. Built and run
  // once locally under `-fsanitize=address` against a deliberately
  // reintroduced pre-fix version of this function to confirm ASan
  // reports a heap-use-after-free at this exact call site, and reports
  // none after the fix.
  QVERIFY2(result->error().message.contains(QStringLiteral("imageCount=3")),
           qPrintable(result->error().message));
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
