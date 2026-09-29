#include "MockHttpServer.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QQueue>
#include <QtTest>
#include <cstring>
#include <functional>
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

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 1000) {
  const QDeadlineTimer deadline(timeoutMs);
  while (!condition() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  }
  return condition();
}

AssetOutcome<AssetFetchResult> runFetch(AssetNetworkFetcher &fetcher,
                                        const QUrl &url, int timeoutMs = 2000) {
  std::optional<AssetOutcome<AssetFetchResult>> captured;
  fetcher.fetch(url, [&captured](AssetOutcome<AssetFetchResult> result) {
    captured.emplace(std::move(result));
  });
  waitUntil([&captured]() { return captured.has_value(); }, timeoutMs);
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

class SyntheticReply final : public QNetworkReply {
public:
  SyntheticReply(const QUrl &url, int status, QByteArray body,
                 QByteArray contentType, QUrl redirect, QObject *parent)
      : QNetworkReply(parent), m_body(std::move(body)) {
    setUrl(url);
    setOpenMode(QIODevice::ReadOnly);
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    if (!contentType.isEmpty()) {
      setRawHeader("Content-Type", contentType);
    }
    if (!redirect.isEmpty()) {
      setAttribute(QNetworkRequest::RedirectionTargetAttribute, redirect);
    }
    QMetaObject::invokeMethod(
        this, [this]() { emit finished(); }, Qt::QueuedConnection);
  }

  void abort() override {}

protected:
  qint64 readData(char *data, qint64 maxSize) override {
    const qint64 available = m_body.size() - m_offset;
    if (available <= 0) {
      return 0;
    }
    const qint64 count = qMin(available, maxSize);
    std::memcpy(data, m_body.constData() + m_offset,
                static_cast<size_t>(count));
    m_offset += count;
    return count;
  }

private:
  QByteArray m_body;
  qint64 m_offset{0};
};

class SyntheticNetworkAccessManager final : public QNetworkAccessManager {
public:
  struct Canned {
    int status{200};
    QByteArray body;
    QByteArray contentType{QByteArrayLiteral("image/png")};
    QUrl redirect;
  };

  void enqueue(Canned canned) { m_responses.enqueue(std::move(canned)); }

protected:
  QNetworkReply *createRequest(Operation, const QNetworkRequest &request,
                               QIODevice *) override {
    if (m_responses.isEmpty()) {
      qFatal("SyntheticNetworkAccessManager: no response enqueued");
    }
    Canned canned = m_responses.dequeue();
    return new SyntheticReply(
        request.url(), canned.status, std::move(canned.body),
        std::move(canned.contentType), std::move(canned.redirect), this);
  }

private:
  QQueue<Canned> m_responses;
};

} // namespace

class AssetLocatorFetcherTests final : public QObject {
  Q_OBJECT

private slots:
  void locatorBuildsDefaultCardFrontUrl();
  void locatorBuildsBackUrlAndStripsLeadingC();
  void locatorBuildsServerPrefixedHomebrewCardUrl();
  void locatorBuildsAlreadyStrippedHomebrewCardUrl();
  void locatorPreservesBasePathPrefix();
  void locatorAllowsAssetBaseWithApiPath();
  void locatorRejectsUnsafeOfficialCodes_data();
  void locatorRejectsUnsafeOfficialCodes();
  void locatorRejectsUnsafeHomebrewSegments_data();
  void locatorRejectsUnsafeHomebrewSegments();
  void locatorEnforcesHttpsExceptLoopbackHttp();

  void fetcherReturnsPngBytesWithoutCookies();
  void fetcherReports404NotFound();
  void fetcherReports500HttpError();
  void fetcherRejectsOversizeContentLengthBeforeTimeout();
  void fetcherRejectsOversizeStreamWithoutContentLength();
  void fetcherTimesOut();
  void fetcherUsesOneTotalTimeoutAcrossRedirects();
  void fetcherFollowsSameOriginRedirect();
  void fetcherRejectsRedirectToHttpNonLoopback();
  void fetcherRejectsHttpsRemoteRedirectToHttpLoopback();
  void fetcherAllowsCrossOriginHttpsRedirect();
  void fetcherCapsRedirectLoops();
  void fetcherReportsRedirectWithoutLocation();
  void fetcherReports304WithoutCache();
  void fetcherRejectsHtmlContentType();
  void destroyingFetcherCancelsInFlightWithoutCallback();
  void destroyingNetworkManagerDuringRequestDoesNotCallbackOrCrash();
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

void AssetLocatorFetcherTests::locatorBuildsServerPrefixedHomebrewCardUrl() {
  const auto url = AssetLocator::buildCardImageUrl(
      AssetLocator::defaultAssetBaseUrl(),
      {QStringLiteral("c:circus-ex-mortis:001"), AssetLocator::CardFace::Back});
  QVERIFY(url);
  QCOMPARE(
      url->path(),
      QStringLiteral("/img/arkham/homebrew/circus-ex-mortis/cards/001b.avif"));
}

void AssetLocatorFetcherTests::locatorBuildsAlreadyStrippedHomebrewCardUrl() {
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

void AssetLocatorFetcherTests::locatorAllowsAssetBaseWithApiPath() {
  const auto base = AssetLocator::assetBaseUrlFromString(
      QStringLiteral("https://example.com/api/assets"));
  QVERIFY(base);
  const auto url =
      AssetLocator::buildCardImageUrl(*base, {QStringLiteral("01002")});
  QVERIFY(url);
  QCOMPARE(url->toString(),
           QStringLiteral(
               "https://example.com/api/assets/img/arkham/cards/01002.avif"));
}

void AssetLocatorFetcherTests::locatorRejectsUnsafeOfficialCodes_data() {
  QTest::addColumn<QString>("cardCode");

  QTest::newRow("empty") << QString();
  QTest::newRow("dotdot") << QStringLiteral("..");
  QTest::newRow("path") << QStringLiteral("a/b");
  QTest::newRow("percent-dotdot") << QStringLiteral("%2e%2e");
  QTest::newRow("percent-slash") << QStringLiteral("010%2f001");
  QTest::newRow("percent-backslash") << QStringLiteral("010%5c001");
  QTest::newRow("fullwidth-slash") << QString::fromUtf8("010／001");
  QTest::newRow("fullwidth-dot") << QString::fromUtf8("010．001");
  QTest::newRow("division-slash") << QString::fromUtf8("010∕001");
  QTest::newRow("space") << QStringLiteral("010 01");
  QTest::newRow("colon") << QStringLiteral("01001:evil");
  QTest::newRow("non-ascii") << QString::fromUtf8("０1001");
}

void AssetLocatorFetcherTests::locatorRejectsUnsafeOfficialCodes() {
  QFETCH(QString, cardCode);
  const auto url = AssetLocator::buildCardImageUrl(
      AssetLocator::defaultAssetBaseUrl(), {cardCode});
  expectErrorCode(url, AssetErrorCode::InvalidAssetKey);
}

void AssetLocatorFetcherTests::locatorRejectsUnsafeHomebrewSegments_data() {
  QTest::addColumn<QString>("cardCode");

  QTest::newRow("empty-campaign") << QStringLiteral("c::001");
  QTest::newRow("path-campaign") << QStringLiteral("c:../a/b:001");
  QTest::newRow("empty-code") << QStringLiteral("c:circus-ex-mortis:");
  QTest::newRow("space-campaign") << QStringLiteral("c:circus ex:001");
  QTest::newRow("percent-campaign") << QStringLiteral("c:circus%2f:001");
  QTest::newRow("percent-code") << QStringLiteral("c:circus-ex-mortis:00%2f");
  QTest::newRow("colon-in-campaign")
      << QStringLiteral("c:circus:ex-mortis:001");
  QTest::newRow("fullwidth-code")
      << QString::fromUtf8("c:circus-ex-mortis:００1");
}

void AssetLocatorFetcherTests::locatorRejectsUnsafeHomebrewSegments() {
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
      QStringLiteral("http://localhost:8080")));
  QVERIFY(AssetLocator::assetBaseUrlFromString(
      QStringLiteral("http://[::1]:8080")));

  const auto insecureBase = AssetLocator::buildCardImageUrl(
      QUrl(QStringLiteral("http://example.com")), {QStringLiteral("01001")});
  expectErrorCode(insecureBase, AssetErrorCode::InsecureTransport);

  const auto loopbackBase = AssetLocator::buildCardImageUrl(
      QUrl(QStringLiteral("http://localhost")), {QStringLiteral("01001")});
  QVERIFY(loopbackBase);
}

void AssetLocatorFetcherTests::fetcherReturnsPngBytesWithoutCookies() {
  MockHttpServer server;
  QVERIFY(server.start());
  server.setResponse(QStringLiteral("/image.png"), response(200, tinyPng()));

  QNetworkAccessManager nam;
  auto *jar = new QNetworkCookieJar(&nam);
  QNetworkCookie cookie(QByteArrayLiteral("asset_session"),
                        QByteArrayLiteral("secret"));
  cookie.setDomain(QStringLiteral("127.0.0.1"));
  cookie.setPath(QStringLiteral("/"));
  jar->setCookiesFromUrl({cookie}, server.url(QStringLiteral("/image.png")));
  nam.setCookieJar(jar);

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

void AssetLocatorFetcherTests::
    fetcherRejectsOversizeContentLengthBeforeTimeout() {
  MockHttpServer server;
  QVERIFY(server.start());
  MockHttpServer::Response large = response(200);
  large.contentLengthOverride = 99;
  large.hangAfterHeaders = true;
  server.setResponse(QStringLiteral("/large.png"), large);

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

void AssetLocatorFetcherTests::fetcherUsesOneTotalTimeoutAcrossRedirects() {
  MockHttpServer server;
  QVERIFY(server.start());
  MockHttpServer::Response first = response(302);
  first.location = QByteArrayLiteral("/second.png");
  first.headerDelayMs = 45;
  MockHttpServer::Response second = response(302);
  second.location = QByteArrayLiteral("/final.png");
  second.headerDelayMs = 45;
  server.setResponse(QStringLiteral("/first.png"), first);
  server.setResponse(QStringLiteral("/second.png"), second);
  server.setResponse(QStringLiteral("/final.png"), response(200, tinyPng()));

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(70));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/first.png")), 1000);
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

void AssetLocatorFetcherTests::
    fetcherRejectsHttpsRemoteRedirectToHttpLoopback() {
  SyntheticNetworkAccessManager nam;
  nam.enqueue(
      {302, {}, {}, QUrl(QStringLiteral("http://127.0.0.1/image.png"))});

  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, QUrl(QStringLiteral("https://assets.example/start")));
  expectErrorCode(result, AssetErrorCode::RedirectRejected);
}

void AssetLocatorFetcherTests::fetcherAllowsCrossOriginHttpsRedirect() {
  SyntheticNetworkAccessManager nam;
  nam.enqueue({302, {}, {}, QUrl(QStringLiteral("https://cdn.example/final"))});
  nam.enqueue({200, tinyPng(), QByteArrayLiteral("image/png"), {}});

  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, QUrl(QStringLiteral("https://assets.example/start")));
  QVERIFY(result);
  QCOMPARE(result->finalUrl, QUrl(QStringLiteral("https://cdn.example/final")));
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

void AssetLocatorFetcherTests::fetcherReportsRedirectWithoutLocation() {
  MockHttpServer server;
  QVERIFY(server.start());
  server.setResponse(QStringLiteral("/redirect.png"), response(302));

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/redirect.png")));
  expectErrorCode(result, AssetErrorCode::MissingRedirectLocation);
}

void AssetLocatorFetcherTests::fetcherReports304WithoutCache() {
  MockHttpServer server;
  QVERIFY(server.start());
  server.setResponse(QStringLiteral("/not-modified.png"), response(304));

  QNetworkAccessManager nam;
  AssetNetworkFetcher fetcher(nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  const auto result =
      runFetch(fetcher, server.url(QStringLiteral("/not-modified.png")));
  expectErrorCode(result, AssetErrorCode::NotModifiedWithoutCache);
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
    QVERIFY(waitUntil([&server]() { return !server.lastRequest().isEmpty(); }));
  }
  QVERIFY(waitUntil([&server]() { return server.disconnectCount() > 0; }));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  QVERIFY(!callbackCalled);
}

void AssetLocatorFetcherTests::
    destroyingNetworkManagerDuringRequestDoesNotCallbackOrCrash() {
  MockHttpServer server;
  QVERIFY(server.start());
  MockHttpServer::Response hanging = response(200);
  hanging.hang = true;
  server.setResponse(QStringLiteral("/hang.png"), hanging);

  bool callbackCalled = false;
  auto *nam = new QNetworkAccessManager;
  AssetNetworkFetcher fetcher(*nam,
                              {.maxResponseBytes = 1024, .maxRedirects = 5},
                              std::chrono::milliseconds(500));
  fetcher.fetch(server.url(QStringLiteral("/hang.png")),
                [&callbackCalled](AssetOutcome<AssetFetchResult>) {
                  callbackCalled = true;
                });
  QVERIFY(waitUntil([&server]() { return !server.lastRequest().isEmpty(); }));
  delete nam;
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  QVERIFY(!callbackCalled);
}

QTEST_GUILESS_MAIN(AssetLocatorFetcherTests)

#include "AssetLocatorFetcherTests.moc"
