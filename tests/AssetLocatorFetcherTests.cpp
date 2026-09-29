#include "MockHttpServer.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QtTest>
#include <optional>

#include "AssetLocator.h"
#include "AssetNetworkFetcher.h"

using namespace Arkham;

namespace {

QByteArray tinyPng() {
  return QByteArray::fromBase64(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAFgwJ"
      "l6p7xNwAAAABJRU5ErkJggg==");
}

AssetOutcome<AssetFetchResult> runFetch(AssetNetworkFetcher &fetcher,
                                        const QUrl &url, int timeoutMs = 2000) {
  std::optional<AssetOutcome<AssetFetchResult>> captured;
  fetcher.fetch(url, [&captured](AssetOutcome<AssetFetchResult> result) {
    captured.emplace(std::move(result));
  });
  const QDeadlineTimer deadline(timeoutMs);
  while (!captured.has_value() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  }
  if (!captured.has_value()) {
    return AssetError{AssetErrorCode::Timeout,
                      QStringLiteral("test timed out waiting for fetch")};
  }
  return std::move(*captured);
}

void expectErrorCode(const AssetOutcome<QUrl> &result, AssetErrorCode code) {
  QVERIFY(!result);
  QCOMPARE(static_cast<int>(result.error().code), static_cast<int>(code));
}

void expectErrorCode(const AssetOutcome<AssetFetchResult> &result,
                     AssetErrorCode code) {
  QVERIFY(!result);
  QCOMPARE(static_cast<int>(result.error().code), static_cast<int>(code));
}

MockHttpServer::Response
response(int status, QByteArray body = {},
         QByteArray contentType = QByteArrayLiteral("image/png")) {
  MockHttpServer::Response result;
  result.status = status;
  result.body = std::move(body);
  result.contentType = std::move(contentType);
  return result;
}

} // namespace

class AssetLocatorFetcherTests final : public QObject {
  Q_OBJECT

private slots:
  void locatorBuildsDefaultCardFrontUrl();
  void locatorBuildsBackUrlAndStripsLeadingC();
  void locatorBuildsHomebrewCardUrl();
  void locatorPreservesBasePathPrefix();
  void locatorRejectsUnsafeCardSegments_data();
  void locatorRejectsUnsafeCardSegments();
  void locatorEnforcesHttpsExceptLoopbackHttp();

  void fetcherReturnsPngBytes();
  void fetcherReports404NotFound();
  void fetcherReports500HttpError();
  void fetcherRejectsOversizeContentLength();
  void fetcherRejectsOversizeStreamWithoutContentLength();
  void fetcherTimesOut();
  void fetcherFollowsSameOriginRedirect();
  void fetcherRejectsRedirectToHttpNonLoopback();
  void fetcherCapsRedirectLoops();
  void fetcherRejectsHtmlContentType();
  void destroyingFetcherCancelsInFlightWithoutCallback();
};

void AssetLocatorFetcherTests::locatorBuildsDefaultCardFrontUrl() {
  const auto url = AssetLocator::buildCardImageUrl(
      AssetLocator::defaultAssetBaseUrl(), {QStringLiteral("01001")});
  QVERIFY(url);
  QCOMPARE(url->toString(),
           QStringLiteral(
               "https://assets.arkhamhorror.app/img/arkham/cards/01001.avif"));
}

void AssetLocatorFetcherTests::locatorBuildsBackUrlAndStripsLeadingC() {
  const auto url = AssetLocator::buildCardImageUrl(
      AssetLocator::defaultAssetBaseUrl(),
      {QStringLiteral("c01001"), AssetLocator::CardFace::Back});
  QVERIFY(url);
  QCOMPARE(url->path(), QStringLiteral("/img/arkham/cards/01001b.avif"));
}

void AssetLocatorFetcherTests::locatorBuildsHomebrewCardUrl() {
  const auto url = AssetLocator::buildCardImageUrl(
      AssetLocator::defaultAssetBaseUrl(),
      {QStringLiteral(":circus-ex-mortis:001"), AssetLocator::CardFace::Back});
  QVERIFY(url);
  QCOMPARE(
      url->path(),
      QStringLiteral("/img/arkham/homebrew/circus-ex-mortis/cards/001b.avif"));
}

void AssetLocatorFetcherTests::locatorPreservesBasePathPrefix() {
  const auto base = AssetLocator::assetBaseUrlFromString(
      QStringLiteral("https://cdn.example/assets/"));
  QVERIFY(base);
  const auto url =
      AssetLocator::buildCardImageUrl(*base, {QStringLiteral("01002")});
  QVERIFY(url);
  QCOMPARE(
      url->toString(),
      QStringLiteral("https://cdn.example/assets/img/arkham/cards/01002.avif"));
}

void AssetLocatorFetcherTests::locatorRejectsUnsafeCardSegments_data() {
  QTest::addColumn<QString>("cardCode");

  QTest::newRow("empty") << QString();
  QTest::newRow("slash") << QStringLiteral("01/001");
  QTest::newRow("backslash") << QStringLiteral("01\\001");
  QTest::newRow("dot") << QStringLiteral(".");
  QTest::newRow("dotdot") << QStringLiteral("..");
  QTest::newRow("control") << QStringLiteral("01001\n");
}

void AssetLocatorFetcherTests::locatorRejectsUnsafeCardSegments() {
  QFETCH(QString, cardCode);
  const auto url = AssetLocator::buildCardImageUrl(
      AssetLocator::defaultAssetBaseUrl(), {cardCode});
  expectErrorCode(url, AssetErrorCode::InvalidAssetKey);
}

void AssetLocatorFetcherTests::locatorEnforcesHttpsExceptLoopbackHttp() {
  QVERIFY(AssetLocator::assetBaseUrlFromString(
      QStringLiteral("https://example.com")));
  QVERIFY(AssetLocator::assetBaseUrlFromString(
      QStringLiteral("http://127.0.0.1:8080")));
  QVERIFY(AssetLocator::assetBaseUrlFromString(
      QStringLiteral("http://[::1]:8080")));
  const auto rejected = AssetLocator::assetBaseUrlFromString(
      QStringLiteral("http://example.com"));
  expectErrorCode(rejected, AssetErrorCode::InvalidAssetBaseUrl);
}

void AssetLocatorFetcherTests::fetcherReturnsPngBytes() {
  MockHttpServer server;
  QVERIFY(server.start());
  server.setResponse(QStringLiteral("/image.png"), response(200, tinyPng()));

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/image.png")));

  QVERIFY(result);
  QCOMPARE(result->bytes, tinyPng());
  QCOMPARE(result->finalUrl, server.url(QStringLiteral("/image.png")));
  QCOMPARE(result->contentType, QStringLiteral("image/png"));
  QVERIFY(!server.lastRequest().contains("Cookie:"));
  QVERIFY(!server.lastRequest().contains("Authorization:"));
}

void AssetLocatorFetcherTests::fetcherReports404NotFound() {
  MockHttpServer server;
  QVERIFY(server.start());
  server.setResponse(QStringLiteral("/missing.png"), response(404));

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/missing.png")));
  expectErrorCode(result, AssetErrorCode::NotFound);
  QCOMPARE(result.error().httpStatus, 404);
}

void AssetLocatorFetcherTests::fetcherReports500HttpError() {
  MockHttpServer server;
  QVERIFY(server.start());
  server.setResponse(QStringLiteral("/error.png"), response(500));

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/error.png")));
  expectErrorCode(result, AssetErrorCode::HttpError);
  QCOMPARE(result.error().httpStatus, 500);
}

void AssetLocatorFetcherTests::fetcherRejectsOversizeContentLength() {
  MockHttpServer server;
  QVERIFY(server.start());
  server.setResponse(QStringLiteral("/large.png"),
                     response(200, QByteArray(8, 'x')));

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam, {.maxResponseBytes = 4, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/large.png")));
  expectErrorCode(result, AssetErrorCode::ResponseTooLarge);
}

void AssetLocatorFetcherTests::
    fetcherRejectsOversizeStreamWithoutContentLength() {
  MockHttpServer server;
  QVERIFY(server.start());
  MockHttpServer::Response streamed = response(200);
  streamed.includeContentLength = false;
  streamed.chunks = {QByteArrayLiteral("ab"), QByteArrayLiteral("cde")};
  streamed.chunkDelayMs = 10;
  server.setResponse(QStringLiteral("/stream.png"), streamed);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam, {.maxResponseBytes = 4, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/stream.png")));
  expectErrorCode(result, AssetErrorCode::ResponseTooLarge);
}

void AssetLocatorFetcherTests::fetcherTimesOut() {
  MockHttpServer server;
  QVERIFY(server.start());
  MockHttpServer::Response hanging = response(200);
  hanging.hang = true;
  server.setResponse(QStringLiteral("/hang.png"), hanging);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(50));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/hang.png")), 1000);
  expectErrorCode(result, AssetErrorCode::Timeout);
}

void AssetLocatorFetcherTests::fetcherFollowsSameOriginRedirect() {
  MockHttpServer server;
  QVERIFY(server.start());
  MockHttpServer::Response redirect = response(302);
  redirect.location = QByteArrayLiteral("/final.png");
  server.setResponse(QStringLiteral("/redirect.png"), redirect);
  server.setResponse(QStringLiteral("/final.png"), response(200, tinyPng()));

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/redirect.png")));
  QVERIFY(result);
  QCOMPARE(result->finalUrl, server.url(QStringLiteral("/final.png")));
}

void AssetLocatorFetcherTests::fetcherRejectsRedirectToHttpNonLoopback() {
  MockHttpServer server;
  QVERIFY(server.start());
  MockHttpServer::Response redirect = response(302);
  redirect.location = QByteArrayLiteral("http://example.com/final.png");
  server.setResponse(QStringLiteral("/redirect.png"), redirect);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/redirect.png")));
  expectErrorCode(result, AssetErrorCode::RedirectRejected);
}

void AssetLocatorFetcherTests::fetcherCapsRedirectLoops() {
  MockHttpServer server;
  QVERIFY(server.start());
  MockHttpServer::Response redirect = response(302);
  redirect.location = QByteArrayLiteral("/loop.png");
  server.setResponse(QStringLiteral("/loop.png"), redirect);

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 2},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/loop.png")));
  expectErrorCode(result, AssetErrorCode::TooManyRedirects);
}

void AssetLocatorFetcherTests::fetcherRejectsHtmlContentType() {
  MockHttpServer server;
  QVERIFY(server.start());
  server.setResponse(QStringLiteral("/not-image"),
                     response(200, QByteArrayLiteral("<html></html>"),
                              QByteArrayLiteral("text/html")));

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/not-image")));
  expectErrorCode(result, AssetErrorCode::UnsupportedContentType);
}

void AssetLocatorFetcherTests::
    destroyingFetcherCancelsInFlightWithoutCallback() {
  MockHttpServer server;
  QVERIFY(server.start());
  MockHttpServer::Response hanging = response(200);
  hanging.hang = true;
  server.setResponse(QStringLiteral("/hang.png"), hanging);

  QNetworkAccessManager nam;
  bool callbackCalled = false;
  {
    AssetNetworkFetcher fetcher(nam,
                                {.maxResponseBytes = 1024, .maxRedirects = 5},
                                std::chrono::milliseconds(500));
    fetcher.fetch(server.url(QStringLiteral("/hang.png")),
                  [&callbackCalled](AssetOutcome<AssetFetchResult>) {
                    callbackCalled = true;
                  });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  }
  const QDeadlineTimer deadline(200);
  while (!deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  }
  QVERIFY(!callbackCalled);
}

QTEST_GUILESS_MAIN(AssetLocatorFetcherTests)

#include "AssetLocatorFetcherTests.moc"
