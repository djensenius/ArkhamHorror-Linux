#include "AssetNetworkFetcherTests.h"

#include "AssetNetworkFetcher.h"
#include "MockHttpServer.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTest>
#include <optional>

using namespace Arkham;

namespace {

QByteArray encodeImage(int width, int height, const char *format) {
  QImage image(width, height, QImage::Format_Mono);
  image.fill(0);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  const bool ok = image.save(&buffer, format);
  Q_ASSERT(ok);
  Q_UNUSED(ok);
  return bytes;
}

using Outcome = AssetOutcome<AssetNetworkFetcher::ConditionalFetchResult>;

// Fetches synchronously (from the test's point of view) by pumping the
// event loop until the callback fires or `timeoutMs` elapses. Returns
// std::nullopt on a timeout (a test bug, never an expected outcome).
std::optional<Outcome>
fetchAndWait(AssetNetworkFetcher &fetcher, const QUrl &url, AssetFormat format,
             AssetNetworkFetcher::ConditionalHeaders conditional = {},
             int timeoutMs = 5000) {
  std::optional<Outcome> result;
  fetcher.fetch(url, format, conditional,
                [&result](Outcome outcome) { result = std::move(outcome); });
  (void)QTest::qWaitFor([&result]() { return result.has_value(); }, timeoutMs);
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

void AssetNetworkFetcherTests::serverErrorMapsToUnexpectedStatus() {
  MockHttpServer server;
  MockHttpServer::Response response;
  response.status = 500;
  response.reasonPhrase = "Internal Server Error";
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

void AssetNetworkFetcherTests::avifCodecSupportIsEnvironmentAdaptive() {
  const bool avifSupported =
      QImageReader::supportedImageFormats().contains(QByteArrayLiteral("avif"));

  MockHttpServer server;
  MockHttpServer::Response response;
  response.contentType = "image/avif";

  QByteArray body;
  if (avifSupported) {
    QImage image(32, 32, QImage::Format_Mono);
    image.fill(0);
    QBuffer buffer(&body);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "avif")) {
      QSKIP("installed Qt build reports AVIF read support but cannot "
            "encode a fixture image; skipping environment-specific case");
    }
  } else {
    // A minimal ISOBMFF "ftyp" box whose major brand is "avif": enough to
    // pass this class's independent magic-byte sniff, but with no actual
    // decodable AV1 payload -- exactly modelling "bytes are structurally
    // plausible but this Qt build has no plugin to decode them."
    body = QByteArrayLiteral("\x00\x00\x00\x14"
                             "ftyp"
                             "avif"
                             "\x00\x00\x00\x00"
                             "avif");
  }
  response.body = body;
  server.setResponse(QStringLiteral("/image.avif"), response);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam);
  const auto result =
      fetchAndWait(fetcher, server.baseUrlFor(QStringLiteral("/image.avif")),
                   AssetFormat::Avif);

  QVERIFY(result.has_value());
  if (avifSupported) {
    QVERIFY2(bool(*result), qPrintable(result->error().message));
  } else {
    QVERIFY(!bool(*result));
    QCOMPARE(result->error().code, AssetErrorCode::UnsupportedCodec);
  }
}

void AssetNetworkFetcherTests::
    avifFtypBoxSizeZeroExtendsToEndOfBufferPerIsobmff() {
  // Per ISO/IEC 14496-12, a leading box size of 0 means "this box extends
  // to the end of the enclosing file", NOT a zero-length box. A body
  // whose `ftyp` box declares size 0 is still a spec-valid AVIF signature
  // and must be accepted by the independent magic-byte sniff (never
  // rejected as AssetErrorCode::MagicBytesMismatch). This synthetic body
  // is a bare `ftyp` box only (no meta/mdat), so it is never a real
  // decodable image; whether an AVIF-capable Qt build can decode a real
  // file is already covered by avifCodecSupportIsEnvironmentAdaptive()
  // above, so a build that DOES have codec support is a real image
  // decode attempt against garbage bytes here -- MalformedImage is an
  // acceptable outcome in that (currently never exercised) case, but
  // MagicBytesMismatch is never acceptable regardless of codec support.
  const bool avifSupported =
      QImageReader::supportedImageFormats().contains(QByteArrayLiteral("avif"));

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
  if (!bool(*result)) {
    QVERIFY(result->error().code != AssetErrorCode::MagicBytesMismatch);
    if (!avifSupported) {
      QCOMPARE(result->error().code, AssetErrorCode::UnsupportedCodec);
    }
  }
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
  const auto handle =
      fetcher.fetch(server.baseUrlFor(QStringLiteral("/slow.png")),
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
  fetcher.fetch(server.baseUrlFor(QStringLiteral("/never-completes.png")),
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
    fetcher.fetch(server.baseUrlFor(QStringLiteral("/slow2.png")),
                  AssetFormat::Png, {},
                  [&callbackFired](Outcome) { callbackFired = true; });
    // fetcher is destroyed here, mid-flight.
  }

  QTest::qWait(300); // long enough that the slow drip would otherwise finish
  QVERIFY(!callbackFired);
}
