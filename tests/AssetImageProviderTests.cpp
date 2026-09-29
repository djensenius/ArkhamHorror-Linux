#include "MockHttpServer.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEvent>
#include <QEventLoop>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "AssetImageProvider.h"
#include "AssetImageRequestCoordinator.h"

using namespace Arkham;

namespace {

QByteArray pngBytes(const QSize &size, const QColor &color = Qt::red) {
  QImage image(size, QImage::Format_RGBA8888);
  image.fill(color);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return bytes;
}

QByteArray tinyPng() { return pngBytes(QSize(1, 1)); }

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

AssetImageRequestCoordinator::Config
configFor(const MockHttpServer &server, const QTemporaryDir &cacheRoot,
          std::chrono::milliseconds negativeCacheTtl = std::chrono::seconds(5),
          qsizetype negativeCacheMaxEntries = 256) {
  AssetImageRequestCoordinator::Config config;
  config.assetBaseUrl = server.url(QString());
  config.cacheConfig.rootDirectory = cacheRoot.path();
  config.negativeCacheTtl = negativeCacheTtl;
  config.negativeCacheMaxEntries = negativeCacheMaxEntries;
  return config;
}

bool waitForPendingResponseDrain(const AssetCardImageResponse &response,
                                 int timeoutMs = 2000) {
  const QDeadlineTimer deadline(timeoutMs);
  const auto state = response.state();
  while (!deadline.hasExpired()) {
    {
      std::lock_guard lock(state->mutex);
      if (state->drainPosted || state->result.has_value()) {
        return true;
      }
    }
    QThread::msleep(1);
  }
  return false;
}

} // namespace

class AssetImageProviderTests final : public QObject {
  Q_OBJECT

private slots:
  void coordinatorCoalescesInFlightRequestsAndUsesCache();
  void coordinatorCancelOneCoalescedWaiterKeepsOtherWaiter();
  void coordinatorCachesNotFoundBriefly();
  void coordinatorDoesNotCacheTransientFailures();
  void coordinatorNegativeCacheExpiresAfterTtl();
  void coordinatorCapsNegativeCacheEntries();
  void coordinatorCancelDetachesWaiter();
  void coordinatorRerequestAfterCancelCompletesNewFlight();
  void providerLoadsMutatedCardImage();
  void providerRejectsInvalidIds();
  void providerCancelAndTeardownWithInFlightRequestsDoesNotCrash();
  void providerCancelWithImmediateCompletionsDoesNotEmitFinished();
  void qmlImageLoadsFromProvider();
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

void AssetImageProviderTests::
    coordinatorCancelOneCoalescedWaiterKeepsOtherWaiter() {
  MockHttpServer server;
  QVERIFY(server.start());
  const QString path = QStringLiteral("/img/arkham/cards/01005.avif");
  auto delayed = response(200, tinyPng());
  delayed.headerDelayMs = 75;
  server.setResponse(path, delayed);

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  AssetImageRequestCoordinator coordinator(configFor(server, cacheRoot));

  bool firstCalled = false;
  std::optional<AssetOutcome<QImage>> second;
  const quint64 firstToken = coordinator.requestCardImage(
      {QStringLiteral("01005")},
      [&firstCalled](AssetOutcome<QImage>) { firstCalled = true; });
  const quint64 secondToken = coordinator.requestCardImage(
      {QStringLiteral("01005")}, [&second](AssetOutcome<QImage> result) {
        second.emplace(std::move(result));
      });
  Q_UNUSED(secondToken);
  coordinator.cancel(firstToken);

  QVERIFY(waitUntil([&]() { return second.has_value(); }));
  QVERIFY(*second);
  QVERIFY(!firstCalled);
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

void AssetImageProviderTests::coordinatorDoesNotCacheTransientFailures() {
  MockHttpServer server;
  QVERIFY(server.start());
  const QString errorPath = QStringLiteral("/img/arkham/cards/01006.avif");
  server.setResponse(errorPath, response(500, QByteArrayLiteral("error")));
  const QString timeoutPath = QStringLiteral("/img/arkham/cards/01007.avif");
  MockHttpServer::Response hanging = response(200);
  hanging.hang = true;
  server.setResponse(timeoutPath, hanging);

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  auto config = configFor(server, cacheRoot);
  config.fetchTimeout = std::chrono::milliseconds(40);
  AssetImageRequestCoordinator coordinator(config);

  auto run = [&coordinator](const QString &code) {
    std::optional<AssetOutcome<QImage>> captured;
    const quint64 token = coordinator.requestCardImage(
        {code}, [&captured](AssetOutcome<QImage> result) {
          captured.emplace(std::move(result));
        });
    Q_UNUSED(token);
    if (!waitUntil([&]() { return captured.has_value(); }, 1000)) {
      return AssetOutcome<QImage>(
          AssetError{AssetErrorCode::Timeout, QStringLiteral("test timeout")});
    }
    return std::move(*captured);
  };

  QVERIFY(!run(QStringLiteral("01006")));
  QVERIFY(!run(QStringLiteral("01006")));
  QCOMPARE(server.requestCountForPath(errorPath), 2);

  QVERIFY(!run(QStringLiteral("01007")));
  QVERIFY(!run(QStringLiteral("01007")));
  QCOMPARE(server.requestCountForPath(timeoutPath), 2);
}

void AssetImageProviderTests::coordinatorNegativeCacheExpiresAfterTtl() {
  MockHttpServer server;
  QVERIFY(server.start());
  const QString path = QStringLiteral("/img/arkham/cards/01008.avif");
  server.setResponse(path, response(404, QByteArrayLiteral("missing")));

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  AssetImageRequestCoordinator coordinator(
      configFor(server, cacheRoot, std::chrono::milliseconds(200), 256));

  auto run = [&coordinator](const QString &code) {
    std::optional<AssetOutcome<QImage>> captured;
    const quint64 token = coordinator.requestCardImage(
        {code}, [&captured](AssetOutcome<QImage> result) {
          captured.emplace(std::move(result));
        });
    Q_UNUSED(token);
    if (!waitUntil([&]() { return captured.has_value(); })) {
      return AssetOutcome<QImage>(
          AssetError{AssetErrorCode::Timeout, QStringLiteral("test timeout")});
    }
    return std::move(*captured);
  };

  QVERIFY(!run(QStringLiteral("01008")));
  QCOMPARE(server.requestCountForPath(path), 1);
  QVERIFY(!run(QStringLiteral("01008")));
  QCOMPARE(server.requestCountForPath(path), 1);

  QTest::qWait(300);
  QVERIFY(!run(QStringLiteral("01008")));
  QCOMPARE(server.requestCountForPath(path), 2);
}

void AssetImageProviderTests::coordinatorCapsNegativeCacheEntries() {
  MockHttpServer server;
  QVERIFY(server.start());
  const QString firstPath = QStringLiteral("/img/arkham/cards/01008.avif");
  const QString secondPath = QStringLiteral("/img/arkham/cards/01009.avif");
  server.setResponse(firstPath, response(404, QByteArrayLiteral("missing")));
  server.setResponse(secondPath, response(404, QByteArrayLiteral("missing")));

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  AssetImageRequestCoordinator coordinator(
      configFor(server, cacheRoot, std::chrono::seconds(5), 1));

  auto run = [&coordinator](const QString &code) {
    std::optional<AssetOutcome<QImage>> captured;
    const quint64 token = coordinator.requestCardImage(
        {code}, [&captured](AssetOutcome<QImage> result) {
          captured.emplace(std::move(result));
        });
    Q_UNUSED(token);
    if (!waitUntil([&]() { return captured.has_value(); })) {
      return AssetOutcome<QImage>(
          AssetError{AssetErrorCode::Timeout, QStringLiteral("test timeout")});
    }
    return std::move(*captured);
  };

  QVERIFY(!run(QStringLiteral("01008")));
  QVERIFY(!run(QStringLiteral("01008")));
  QCOMPARE(server.requestCountForPath(firstPath), 1);

  QVERIFY(!run(QStringLiteral("01009")));
  QVERIFY(!run(QStringLiteral("01009")));
  QCOMPARE(server.requestCountForPath(secondPath), 1);

  QVERIFY(!run(QStringLiteral("01008")));
  QCOMPARE(server.requestCountForPath(firstPath), 2);
}

void AssetImageProviderTests::coordinatorCancelDetachesWaiter() {
  MockHttpServer server;
  QVERIFY(server.start());
  const QString path = QStringLiteral("/img/arkham/cards/01003.avif");
  MockHttpServer::Response hanging = response(200);
  hanging.hang = true;
  server.setResponse(path, hanging);

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  AssetImageRequestCoordinator coordinator(configFor(server, cacheRoot));

  bool called = false;
  const quint64 token = coordinator.requestCardImage(
      {QStringLiteral("01003")},
      [&called](AssetOutcome<QImage>) { called = true; });
  QVERIFY(waitUntil([&]() { return server.requestCountForPath(path) == 1; }));
  coordinator.cancel(token);

  QVERIFY(waitUntil([&server]() { return server.disconnectCount() > 0; }));
  QTest::qWait(100);
  QVERIFY(!called);
  QCOMPARE(server.requestCountForPath(path), 1);
}

void AssetImageProviderTests::
    coordinatorRerequestAfterCancelCompletesNewFlight() {
  MockHttpServer server;
  QVERIFY(server.start());
  const QString path = QStringLiteral("/img/arkham/cards/01012.avif");
  server.setResponse(path, response(200, pngBytes(QSize(1, 1), Qt::red)));

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  AssetImageRequestCoordinator coordinator(configFor(server, cacheRoot));

  bool staleCalled = false;
  const quint64 firstToken = coordinator.requestCardImage(
      {QStringLiteral("01012")},
      [&staleCalled](AssetOutcome<QImage>) { staleCalled = true; });
  QVERIFY(waitUntil([&]() { return server.requestCountForPath(path) == 1; }));
  QThread::msleep(25);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
  server.setResponse(path, response(200, pngBytes(QSize(2, 2), Qt::blue)));
  coordinator.cancel(firstToken);

  std::optional<AssetOutcome<QImage>> second;
  const quint64 secondToken = coordinator.requestCardImage(
      {QStringLiteral("01012")}, [&second](AssetOutcome<QImage> result) {
        second.emplace(std::move(result));
      });
  Q_UNUSED(secondToken);

  QVERIFY(waitUntil([&]() { return second.has_value(); }));
  QVERIFY(*second);
  QCOMPARE((*second)->size(), QSize(2, 2));
  QVERIFY(!staleCalled);
  QCOMPARE(server.requestCountForPath(path), 2);
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
                                    QSize(4, 0)));
  QSignalSpy finishedSpy(imageResponse.get(), &QQuickImageResponse::finished);
  QVERIFY(finishedSpy.wait(2000) || finishedSpy.count() == 1);
  QCOMPARE(imageResponse->errorString(), QString());
  std::unique_ptr<QQuickTextureFactory> factory(
      imageResponse->textureFactory());
  QVERIFY(factory != nullptr);
  QCOMPARE(factory->textureSize(), QSize(4, 4));

  std::unique_ptr<QQuickImageResponse> heightOnlyResponse(
      provider.requestImageResponse(QStringLiteral("01004?mutation=Mutated1"),
                                    QSize(0, 5)));
  QSignalSpy heightOnlyFinished(heightOnlyResponse.get(),
                                &QQuickImageResponse::finished);
  QVERIFY(heightOnlyFinished.wait(2000) || heightOnlyFinished.count() == 1);
  QCOMPARE(heightOnlyResponse->errorString(), QString());
  std::unique_ptr<QQuickTextureFactory> heightOnlyFactory(
      heightOnlyResponse->textureFactory());
  QVERIFY(heightOnlyFactory != nullptr);
  QCOMPARE(heightOnlyFactory->textureSize(), QSize(5, 5));
  QCOMPARE(server.requestCountForPath(path), 1);
}

void AssetImageProviderTests::providerRejectsInvalidIds() {
  AssetCardImageProvider provider;
  const QStringList ids{
      QStringLiteral("sideways/01001"),
      QStringLiteral("01001_MutatedX"),
      QStringLiteral("01001_Mutated1?mutation=Mutated2"),
      QStringLiteral("01001?mutation="),
      QStringLiteral("front/01001/extra"),
  };

  for (const QString &id : ids) {
    std::unique_ptr<QQuickImageResponse> response(
        provider.requestImageResponse(id, QSize()));
    QVERIFY2(!response->errorString().isEmpty(), qPrintable(id));
  }
}

void AssetImageProviderTests::
    providerCancelAndTeardownWithInFlightRequestsDoesNotCrash() {
  MockHttpServer server;
  QVERIFY(server.start());
  MockHttpServer::Response hanging = response(200);
  hanging.hang = true;
  server.setResponse(QStringLiteral("/img/arkham/cards/01010.avif"), hanging);

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  std::vector<std::unique_ptr<QQuickImageResponse>> responses;
  {
    AssetCardImageProvider provider(configFor(server, cacheRoot));
    for (int index = 0; index < 50; ++index) {
      std::unique_ptr<QQuickImageResponse> response(
          provider.requestImageResponse(QStringLiteral("01010"), QSize()));
      if ((index % 2) == 0) {
        response->cancel();
      }
      responses.push_back(std::move(response));
    }
    QVERIFY(waitUntil([&server]() {
      return server.requestCountForPath(
                 QStringLiteral("/img/arkham/cards/01010.avif")) >= 1;
    }));
  }

  for (auto &response : responses) {
    response->cancel();
    response.reset();
  }
  QVERIFY(true);
}

void AssetImageProviderTests::
    providerCancelWithImmediateCompletionsDoesNotEmitFinished() {
  MockHttpServer server;
  QVERIFY(server.start());
  const QString path = QStringLiteral("/img/arkham/cards/01013.avif");
  server.setResponse(path, response(200, tinyPng()));

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  AssetCardImageProvider provider(configFor(server, cacheRoot));

  {
    std::unique_ptr<QQuickImageResponse> warmResponse(
        provider.requestImageResponse(QStringLiteral("01013"), QSize()));
    QSignalSpy warmFinished(warmResponse.get(), &QQuickImageResponse::finished);
    QVERIFY(warmFinished.wait(2000) || warmFinished.count() == 1);
    QCOMPARE(warmResponse->errorString(), QString());
  }
  QCOMPARE(server.requestCountForPath(path), 1);

  std::vector<std::unique_ptr<QQuickImageResponse>> keptResponses;
  std::vector<std::unique_ptr<QSignalSpy>> keptSpies;
  for (int index = 0; index < 50; ++index) {
    std::unique_ptr<QQuickImageResponse> response(
        provider.requestImageResponse(QStringLiteral("01013"), QSize()));
    auto *typedResponse = static_cast<AssetCardImageResponse *>(response.get());
    auto spy = std::make_unique<QSignalSpy>(response.get(),
                                            &QQuickImageResponse::finished);
    QVERIFY(waitForPendingResponseDrain(*typedResponse));
    response->cancel();
    if ((index % 2) == 0) {
      keptSpies.push_back(std::move(spy));
      keptResponses.push_back(std::move(response));
    } else {
      response.reset();
    }
  }

  QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
  for (const auto &spy : keptSpies) {
    QCOMPARE(spy->count(), 0);
  }
  QCOMPARE(server.requestCountForPath(path), 1);
}

void AssetImageProviderTests::qmlImageLoadsFromProvider() {
  MockHttpServer server;
  QVERIFY(server.start());
  const QString path = QStringLiteral("/img/arkham/cards/01011.avif");
  server.setResponse(path, response(200, tinyPng()));

  QTemporaryDir cacheRoot;
  QVERIFY(cacheRoot.isValid());
  QQmlEngine engine;
  engine.addImageProvider(
      QStringLiteral("arkham-card"),
      new AssetCardImageProvider(configFor(server, cacheRoot)));

  QQmlComponent component(&engine);
  component.setData(
      "import QtQuick\nImage { source: 'image://arkham-card/01011' }", QUrl());
  std::unique_ptr<QObject> object(component.create());
  QVERIFY2(object != nullptr, qPrintable(component.errorString()));

  QVERIFY(waitUntil(
      [&object]() { return object->property("status").toInt() == 1; }, 3000));
  QCOMPARE(server.requestCountForPath(path), 1);
}

QTEST_MAIN(AssetImageProviderTests)

#include "AssetImageProviderTests.moc"
