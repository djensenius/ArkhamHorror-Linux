#include "MockHttpServer.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <memory>
#include <optional>

#include "AssetImageProvider.h"
#include "AssetImageRequestCoordinator.h"

using namespace Arkham;

namespace {

QByteArray tinyPng() {
  QImage image(1, 1, QImage::Format_RGBA8888);
  image.fill(Qt::red);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return bytes;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 2000) {
  const QDeadlineTimer deadline(timeoutMs);
  while (!condition() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  }
  return condition();
}

MockHttpServer::Response response(int status, QByteArray body = {}) {
  MockHttpServer::Response result;
  result.status = status;
  result.contentType = QByteArrayLiteral("image/png");
  result.body = std::move(body);
  return result;
}

AssetImageRequestCoordinator::Config configFor(const MockHttpServer &server,
                                               const QTemporaryDir &cacheRoot) {
  AssetImageRequestCoordinator::Config config;
  config.assetBaseUrl = server.url(QString());
  config.cacheConfig.rootDirectory = cacheRoot.path();
  config.negativeCacheTtl = std::chrono::seconds(5);
  return config;
}

} // namespace

class AssetImageProviderTests final : public QObject {
  Q_OBJECT

private slots:
  void coordinatorCoalescesInFlightRequestsAndUsesCache();
  void coordinatorCachesNotFoundBriefly();
  void coordinatorCancelDetachesWaiter();
  void providerLoadsMutatedCardImage();
};

void AssetImageProviderTests::
    coordinatorCoalescesInFlightRequestsAndUsesCache() {
  MockHttpServer server;
  QVERIFY(server.start());
  const QString path = QStringLiteral("/img/arkham/cards/01001.avif");
  auto delayed = response(200, tinyPng());
  delayed.headerDelayMs = 50;
  server.setResponse(path, delayed);

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  AssetImageRequestCoordinator coordinator(configFor(server, cacheRoot));

  std::optional<AssetOutcome<QImage>> first;
  std::optional<AssetOutcome<QImage>> second;
  const quint64 firstToken = coordinator.requestCardImage(
      {QStringLiteral("c01001")}, [&first](AssetOutcome<QImage> result) {
        first.emplace(std::move(result));
      });
  const quint64 secondToken = coordinator.requestCardImage(
      {QStringLiteral("c01001")}, [&second](AssetOutcome<QImage> result) {
        second.emplace(std::move(result));
      });
  Q_UNUSED(firstToken);
  Q_UNUSED(secondToken);

  QVERIFY(waitUntil([&]() { return first.has_value() && second.has_value(); }));
  QVERIFY(*first);
  QVERIFY(*second);
  QCOMPARE(server.requestCountForPath(path), 1);

  std::optional<AssetOutcome<QImage>> cached;
  const quint64 cachedToken = coordinator.requestCardImage(
      {QStringLiteral("c01001")}, [&cached](AssetOutcome<QImage> result) {
        cached.emplace(std::move(result));
      });
  Q_UNUSED(cachedToken);
  QVERIFY(waitUntil([&]() { return cached.has_value(); }));
  QVERIFY(*cached);
  QCOMPARE(server.requestCountForPath(path), 1);
}

void AssetImageProviderTests::coordinatorCachesNotFoundBriefly() {
  MockHttpServer server;
  QVERIFY(server.start());
  const QString path = QStringLiteral("/img/arkham/cards/01002.avif");
  server.setResponse(path, response(404, QByteArrayLiteral("missing")));

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  AssetImageRequestCoordinator coordinator(configFor(server, cacheRoot));

  std::optional<AssetOutcome<QImage>> first;
  const quint64 firstToken = coordinator.requestCardImage(
      {QStringLiteral("01002")}, [&first](AssetOutcome<QImage> result) {
        first.emplace(std::move(result));
      });
  Q_UNUSED(firstToken);
  QVERIFY(waitUntil([&]() { return first.has_value(); }));
  QVERIFY(!*first);
  QCOMPARE(static_cast<int>(first->error().code),
           static_cast<int>(AssetErrorCode::NotFound));
  QCOMPARE(server.requestCountForPath(path), 1);

  std::optional<AssetOutcome<QImage>> second;
  const quint64 secondToken = coordinator.requestCardImage(
      {QStringLiteral("01002")}, [&second](AssetOutcome<QImage> result) {
        second.emplace(std::move(result));
      });
  Q_UNUSED(secondToken);
  QVERIFY(waitUntil([&]() { return second.has_value(); }));
  QVERIFY(!*second);
  QCOMPARE(static_cast<int>(second->error().code),
           static_cast<int>(AssetErrorCode::NotFound));
  QCOMPARE(server.requestCountForPath(path), 1);
}

void AssetImageProviderTests::coordinatorCancelDetachesWaiter() {
  MockHttpServer server;
  QVERIFY(server.start());
  const QString path = QStringLiteral("/img/arkham/cards/01003.avif");
  auto delayed = response(200, tinyPng());
  delayed.headerDelayMs = 75;
  server.setResponse(path, delayed);

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  AssetImageRequestCoordinator coordinator(configFor(server, cacheRoot));

  bool called = false;
  const quint64 token = coordinator.requestCardImage(
      {QStringLiteral("01003")},
      [&called](AssetOutcome<QImage>) { called = true; });
  coordinator.cancel(token);

  QTest::qWait(200);
  QVERIFY(!called);
  QVERIFY(server.requestCountForPath(path) <= 1);
}

void AssetImageProviderTests::providerLoadsMutatedCardImage() {
  MockHttpServer server;
  QVERIFY(server.start());
  const QString path = QStringLiteral("/img/arkham/cards/01004_Mutated1.avif");
  server.setResponse(path, response(200, tinyPng()));

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  AssetCardImageProvider provider(configFor(server, cacheRoot));

  std::unique_ptr<QQuickImageResponse> imageResponse(
      provider.requestImageResponse(QStringLiteral("01004?mutation=Mutated1"),
                                    QSize()));
  QSignalSpy finishedSpy(imageResponse.get(), &QQuickImageResponse::finished);
  QVERIFY(finishedSpy.wait(2000) || finishedSpy.count() == 1);
  QCOMPARE(imageResponse->errorString(), QString());
  std::unique_ptr<QQuickTextureFactory> factory(
      imageResponse->textureFactory());
  QVERIFY(factory != nullptr);
  QCOMPARE(server.requestCountForPath(path), 1);
}

QTEST_MAIN(AssetImageProviderTests)

#include "AssetImageProviderTests.moc"
