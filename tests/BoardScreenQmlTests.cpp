// Loads the real qml/BoardScreen.qml over a real GameBoardController (a
// GameClient against a loopback MockHttpServer) and drives it with keys
// alone: list, Enter opens the board, Escape goes back, errors offer
// Retry, and no controller shows a sign-in prompt.

#include <QAbstractItemModel>
#include <QFile>
#include <QGuiApplication>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QtTest>

#include <memory>

#include "GameBoardController.h"
#include "GameClient.h"
#include "MockHttpServer.h"
#include "ServerProfile.h"

using namespace Arkham;
using namespace Qt::StringLiterals;

namespace {

const QString kGameId = u"00000000-0000-0000-0000-000000000003"_s;

QByteArray fixture(const QString &name) {
  QFile f(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + u"/fixtures/"_s + name);
  if (!f.open(QIODevice::ReadOnly))
    qFatal("missing fixture: %s", qPrintable(name));
  return f.readAll();
}

MockHttpServer::Response jsonResponse(int status, QByteArray body) {
  MockHttpServer::Response response;
  response.status = status;
  response.contentType = "application/json"_ba;
  response.body = std::move(body);
  return response;
}

QList<QQuickItem *> itemsNamed(QQuickItem *item, const QString &name) {
  QList<QQuickItem *> found;
  if (item->objectName() == name)
    found.append(item);
  for (QQuickItem *child : item->childItems())
    found.append(itemsNamed(child, name));
  return found;
}

} // namespace

class BoardScreenQmlTests final : public QObject {
  Q_OBJECT

private:
  std::unique_ptr<MockHttpServer> m_server;
  std::unique_ptr<QNetworkAccessManager> m_nam;
  std::unique_ptr<GameClient> m_client;
  std::unique_ptr<GameBoardController> m_controller;
  std::unique_ptr<QQuickView> m_view;
  std::optional<ServerProfile> m_profile;

  QString path(const QString &apiPath) const {
    return m_profile->apiUrl(apiPath).path();
  }

  QQuickItem *child(const QString &name) const {
    return m_view->rootObject()->findChild<QQuickItem *>(name);
  }

  // Shows BoardScreen.qml with |controller| (may be null).
  bool show(QObject *controller) {
    m_view = std::make_unique<QQuickView>();
    m_view->setResizeMode(QQuickView::SizeRootObjectToView);
    m_view->resize(1280, 800);
    m_view->setInitialProperties(
        {{u"controller"_s, QVariant::fromValue(controller)}});
    m_view->setSource(QUrl::fromLocalFile(QStringLiteral(ARKHAM_TEST_QML_DIR) +
                                          u"/BoardScreen.qml"_s));
    if (m_view->status() != QQuickView::Ready)
      return false;
    m_view->show();
    m_view->requestActivate();
    return QTest::qWaitForWindowActive(m_view.get());
  }

private slots:
  void initTestCase() { qputenv("QT_QUICK_CONTROLS_STYLE", "Basic"); }

  void init() {
    m_server = std::make_unique<MockHttpServer>();
    QVERIFY(m_server->start());
    auto profile = ServerProfile::custom(
        u"Mock"_s, u"http://127.0.0.1:%1"_s.arg(m_server->serverPort()));
    QVERIFY(profile.has_value());
    m_profile = *profile;
    m_nam = std::make_unique<QNetworkAccessManager>();
    m_client = std::make_unique<GameClient>(*m_nam);
    const ServerProfile tokenProfile = *m_profile;
    m_controller = std::make_unique<GameBoardController>(
        *m_client,
        [this, tokenProfile](SessionCoordinator::AccessTokenCallback callback) {
          QMetaObject::invokeMethod(
              this,
              [callback = std::move(callback), tokenProfile] {
                callback(
                    SessionCoordinator::AccessToken{tokenProfile, u"tok-1"_s},
                    QString());
              },
              Qt::QueuedConnection);
        });
  }

  void cleanup() {
    m_view.reset();
    m_controller.reset();
    m_client.reset();
    m_nam.reset();
    m_server.reset();
    m_profile.reset();
  }

  void keysOpenTheBoardAndGoBack() {
    m_server->setResponse(path(u"arkham/games"_s),
                          jsonResponse(200, fixture(u"game-list.json"_s)));
    m_server->setResponse(path(u"arkham/games/"_s + kGameId),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    QVERIFY(show(m_controller.get()));
    QSignalSpy closed(m_view->rootObject(), SIGNAL(closed()));

    QQuickItem *list = child(u"gamesList"_s);
    QVERIFY(list != nullptr);
    QTRY_VERIFY(list->isVisible());
    QTRY_VERIFY(list->hasActiveFocus());
    QCOMPARE(list->property("count").toInt(), 4);

    // Enter on the first game opens its board.
    QTest::keyClick(m_view.get(), Qt::Key_Return);
    QQuickItem *board = child(u"board"_s);
    QVERIFY(board != nullptr);
    QTRY_VERIFY(board->isVisible());
    QTRY_VERIFY(board->hasActiveFocus());
    QVERIFY(!list->isVisible());
    QCOMPARE(child(u"boardTitle"_s)->property("text").toString(),
             u"Contract fixture game"_s);
    QTRY_VERIFY(!itemsNamed(board, u"boardCard"_s).isEmpty());
    // Card art is decoded at the cell's size, not full size.
    const QList<QQuickItem *> cards = itemsNamed(board, u"boardCard"_s);
    auto *cardImage = cards.first()->childItems().first();
    QCOMPARE(cardImage->property("sourceSize").toSize(), QSize(150, 210));

    // Escape: board -> list -> closed().
    QTest::keyClick(m_view.get(), Qt::Key_Escape);
    QTRY_VERIFY(list->isVisible());
    QTRY_VERIFY(list->hasActiveFocus());
    QVERIFY(!board->isVisible());
    QCOMPARE(closed.count(), 0);
    QTest::keyClick(m_view.get(), Qt::Key_Escape);
    QCOMPARE(closed.count(), 1);
  }

  void errorOffersRetry() {
    m_server->setResponse(path(u"arkham/games"_s), jsonResponse(500, "{}"_ba));
    QVERIFY(show(m_controller.get()));
    QQuickItem *error = child(u"errorLabel"_s);
    QQuickItem *retry = child(u"retryButton"_s);
    QVERIFY(error != nullptr);
    QVERIFY(retry != nullptr);
    QTRY_VERIFY(error->isVisible());
    QCOMPARE(error->property("text").toString(),
             u"The server returned HTTP 500."_s);
    QTRY_VERIFY(retry->hasActiveFocus());

    m_server->setResponse(path(u"arkham/games"_s),
                          jsonResponse(200, fixture(u"game-list.json"_s)));
    QTest::keyClick(m_view.get(), Qt::Key_Return);
    QQuickItem *list = child(u"gamesList"_s);
    QTRY_VERIFY(list->isVisible());
    QTRY_VERIFY(list->hasActiveFocus());
    QVERIFY(!error->isVisible());
    QVERIFY(!retry->isVisible());
  }

  void loadingStateIsShown() {
    MockHttpServer::Response slow =
        jsonResponse(200, fixture(u"game-list.json"_s));
    slow.headerDelayMs = 300;
    m_server->setResponse(path(u"arkham/games"_s), slow);
    QVERIFY(show(m_controller.get()));
    QQuickItem *loading = child(u"loadingLabel"_s);
    QVERIFY(loading != nullptr);
    QVERIFY(loading->isVisible());
    QTRY_VERIFY(child(u"gamesList"_s)->isVisible());
    QVERIFY(!loading->isVisible());
  }

  void withoutControllerAsksToSignIn() {
    QVERIFY(show(nullptr));
    QQuickItem *signedOut = child(u"signedOutLabel"_s);
    QVERIFY(signedOut != nullptr);
    QVERIFY(signedOut->isVisible());
    QTRY_VERIFY(child(u"backButton"_s)->hasActiveFocus());
    QVERIFY(!child(u"gamesList"_s)->isVisible());
  }
};

QTEST_MAIN(BoardScreenQmlTests)

#include "BoardScreenQmlTests.moc"
