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
    // The current card is marked while the board has focus, and the mark
    // follows the arrow keys.
    const auto indicator = [board](int index) -> QQuickItem * {
      const QList<QQuickItem *> cards = itemsNamed(board, u"boardCard"_s);
      for (QQuickItem *card : cards) {
        if (card->property("index").toInt() == index)
          return itemsNamed(card, u"currentCardIndicator"_s).value(0);
      }
      return nullptr;
    };
    QTRY_VERIFY(indicator(0) && indicator(1));
    QTRY_VERIFY(indicator(0)->isVisible());
    QVERIFY(!indicator(1)->isVisible());
    QTest::keyClick(m_view.get(), Qt::Key_Right);
    QTRY_VERIFY(indicator(1)->isVisible());
    QVERIFY(!indicator(0)->isVisible());
    QTest::keyClick(m_view.get(), Qt::Key_Left);
    QTRY_VERIFY(indicator(0)->isVisible());
    // No mark once focus leaves the board.
    child(u"backButton"_s)->forceActiveFocus();
    QTRY_VERIFY(!indicator(0)->isVisible());
    board->forceActiveFocus();
    QTRY_VERIFY(indicator(0)->isVisible());
    // The provider's output is requested at the cell's size (it still
    // decodes and caches full-resolution art; see TASK-3.3.5).
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

  void boardErrorOffersRetryAndCloseGameRefocusesTheList() {
    m_server->setResponse(path(u"arkham/games"_s),
                          jsonResponse(200, fixture(u"game-list.json"_s)));
    m_server->setResponse(path(u"arkham/games/"_s + kGameId),
                          jsonResponse(404, "{}"_ba));
    QVERIFY(show(m_controller.get()));
    QQuickItem *list = child(u"gamesList"_s);
    QTRY_VERIFY(list->hasActiveFocus());
    QTest::keyClick(m_view.get(), Qt::Key_Return);
    QQuickItem *error = child(u"errorLabel"_s);
    QTRY_VERIFY(error->isVisible());
    QCOMPARE(error->property("text").toString(), u"Game not found."_s);
    QTRY_VERIFY(child(u"retryButton"_s)->hasActiveFocus());
    QVERIFY(!child(u"board"_s)->isVisible());
    QCOMPARE(child(u"backButton"_s)->property("text").toString(),
             u"Back to games"_s);

    m_controller->closeGame();
    QTRY_VERIFY(list->isVisible());
    QTRY_VERIFY(list->hasActiveFocus());
    QVERIFY(!error->isVisible());
  }

  void emptyListSaysSo() {
    m_server->setResponse(path(u"arkham/games"_s), jsonResponse(200, "[]"_ba));
    QVERIFY(show(m_controller.get()));
    QQuickItem *empty = child(u"emptyLabel"_s);
    QTRY_VERIFY(empty->isVisible());
    QVERIFY(!child(u"gamesList"_s)->isVisible());
    QTRY_VERIFY(child(u"backButton"_s)->hasActiveFocus());
  }

  void signOutWhileOpenOffersReloadAndSignInReloads() {
    m_server->setResponse(path(u"arkham/games"_s),
                          jsonResponse(200, fixture(u"game-list.json"_s)));
    m_controller->setSignedIn(true);
    QVERIFY(show(m_controller.get()));
    QQuickItem *list = child(u"gamesList"_s);
    QTRY_VERIFY(list->isVisible());

    // The session leaves SignedIn: a sign-in prompt and Reload, not a
    // blank screen.
    m_controller->setSignedIn(false);
    QQuickItem *signedOut = child(u"signedOutLabel"_s);
    QQuickItem *reload = child(u"reloadButton"_s);
    QTRY_VERIFY(signedOut->isVisible());
    QVERIFY(reload->isVisible());
    QVERIFY(!list->isVisible());
    QTRY_VERIFY(reload->hasActiveFocus());
    QTest::keyClick(m_view.get(), Qt::Key_Right);
    QTRY_VERIFY(child(u"backButton"_s)->hasActiveFocus());
    QTest::keyClick(m_view.get(), Qt::Key_Left);
    QTRY_VERIFY(reload->hasActiveFocus());

    // Back to SignedIn: the open screen reloads by itself.
    const int before = m_server->requestCountForPath(path(u"arkham/games"_s));
    m_controller->setSignedIn(true);
    QTRY_VERIFY(list->isVisible());
    QTRY_VERIFY(list->hasActiveFocus());
    QVERIFY(!signedOut->isVisible());
    QVERIFY(!reload->isVisible());
    QCOMPARE(m_server->requestCountForPath(path(u"arkham/games"_s)),
             before + 1);

    // Reload by hand does the same.
    m_controller->reset();
    QTRY_VERIFY(reload->hasActiveFocus());
    QTest::keyClick(m_view.get(), Qt::Key_Return);
    QTRY_VERIFY(list->isVisible());
    QCOMPARE(m_server->requestCountForPath(path(u"arkham/games"_s)),
             before + 2);
  }

  void longMaintenanceNoticeIsBoundedAndKeepsTheButtons() {
    // A message the maintenance parser accepts that is still 160 lines.
    const QString message = u"Down.\n"_s.repeated(160);
    QByteArray body = R"({"maintenance":true,"message":)"_ba;
    body += '"' + message.toUtf8().replace("\n", "\\n") + '"';
    body += R"(,"since":"2026-11-03T22:15:00Z"})"_ba;
    m_server->setResponse(path(u"arkham/games"_s), jsonResponse(503, body));
    QVERIFY(show(m_controller.get()));

    QQuickItem *notice = child(u"errorNotice"_s);
    QQuickItem *text = child(u"errorLabel"_s);
    QQuickItem *retry = child(u"retryButton"_s);
    QQuickItem *back = child(u"backButton"_s);
    QVERIFY(notice && text && retry && back);
    QTRY_VERIFY(notice->isVisible());
    QTRY_VERIFY(text->height() > notice->height());
    QVERIFY(text->property("text").toString().startsWith(u"Down.\nDown."_s));
    QVERIFY(notice->clip());
    QVERIFY(notice->height() <= m_view->height() * 0.3 + 0.5);
    // Retry and Back stay inside the window.
    for (QQuickItem *button : {retry, back}) {
      QVERIFY(button->isVisible());
      const QPointF bottom = button->mapToScene(QPointF(0, button->height()));
      QVERIFY2(bottom.y() <= m_view->height() + 0.5,
               qPrintable(u"%1 bottom %2 > %3"_s.arg(button->objectName())
                              .arg(bottom.y())
                              .arg(m_view->height())));
    }

    // Keys alone: Up from Retry focuses the notice, Down scrolls it, and
    // Down at the end returns to Retry.
    QTRY_VERIFY(retry->hasActiveFocus());
    QTest::keyClick(m_view.get(), Qt::Key_Up);
    QTRY_VERIFY(notice->hasActiveFocus());
    auto *flick = notice->property("contentItem").value<QQuickItem *>();
    QVERIFY(flick != nullptr);
    QCOMPARE(flick->property("contentY").toReal(), 0.0);
    QTest::keyClick(m_view.get(), Qt::Key_Down);
    QTRY_VERIFY(flick->property("contentY").toReal() > 0.0);
    for (int i = 0; i < 200 && notice->hasActiveFocus(); ++i)
      QTest::keyClick(m_view.get(), Qt::Key_Down);
    QTRY_VERIFY(retry->hasActiveFocus());
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
    QVERIFY(!child(u"reloadButton"_s)->isVisible());
    QTRY_VERIFY(child(u"backButton"_s)->hasActiveFocus());
    QVERIFY(!child(u"gamesList"_s)->isVisible());
  }
};

QTEST_MAIN(BoardScreenQmlTests)

#include "BoardScreenQmlTests.moc"
