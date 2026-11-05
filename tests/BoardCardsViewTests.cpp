// Loads the real qml/BoardCardsView.qml (and its CardImage delegate) over a
// BoardCardsModel decoded from the vendored get-game fixture, with the
// image://arkham-card provider pointed at a mock asset server, and checks
// that the board's cards are requested at their exact asset paths,
// including mutated investigator art, a form override and a side-B act,
// and that every card's accessible name is its catalog card name.

#include "MockHttpServer.h"

#include <QAccessible>
#include <QAccessibleInterface>
#include <QBuffer>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <memory>

#include "AssetImageProvider.h"
#include "AssetImageRequestCoordinator.h"
#include "Board.h"
#include "BoardCardsModel.h"

using namespace Arkham;
using namespace Qt::StringLiterals;

namespace {

QByteArray tinyPng() {
  QImage image(QSize(1, 1), QImage::Format_RGBA8888);
  image.fill(Qt::red);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return bytes;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 3000) {
  const QDeadlineTimer deadline(timeoutMs);
  while (!condition() && !deadline.hasExpired())
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  return condition();
}

// get-game.json's PublicGame with each `edits` pair's first occurrence
// replaced.
PublicGame
editedGame(std::initializer_list<std::pair<QByteArray, QByteArray>> edits) {
  QFile f(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) +
          u"/fixtures/get-game.json");
  if (!f.open(QIODevice::ReadOnly))
    qFatal("missing fixture: get-game.json");
  QByteArray bytes = f.readAll();
  for (const auto &[from, to] : edits) {
    const qsizetype at = bytes.indexOf(from);
    if (at < 0)
      qFatal("fixture edit target not found: %s", from.constData());
    bytes.replace(at, from.size(), to);
  }
  auto envelope = Json::Value::parse(bytes, u"get-game.json");
  if (!envelope)
    qFatal("fixture must parse: %s", qPrintable(envelope.error()));
  auto game = PublicGame::fromRawJson(envelope->value("game"_L1), u"game");
  if (!game)
    qFatal("fixture must decode: %s", qPrintable(game.error()));
  return *game;
}

// get-game.json's PublicGame with Roland's art mutated and the Study
// unrevealed.
PublicGame mutatedGame() {
  return editedGame({{R"("mutated": null)", R"("mutated": "Mutated21")"},
                     {R"("revealed": true)", R"("revealed": false)"}});
}

std::unique_ptr<QObject> createBoardWindow(QQmlEngine &engine,
                                           BoardCardsModel &model) {
  engine.rootContext()->setContextProperty(u"boardCards"_s, &model);
  QQmlComponent component(&engine);
  component.setData("import QtQuick\n"
                    "Window {\n"
                    "  width: 800; height: 600; visible: true\n"
                    "  BoardCardsView { anchors.fill: parent;"
                    " model: boardCards }\n"
                    "}\n",
                    QUrl::fromLocalFile(QStringLiteral(ARKHAM_TEST_QML_DIR
                                                       "/BoardCardsTest.qml")));
  std::unique_ptr<QObject> window(component.create());
  if (!window)
    qWarning("%s", qPrintable(component.errorString()));
  return window;
}

QString accessibleName(QObject *item) {
  QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(item);
  return iface ? iface->text(QAccessible::Name) : u"<no interface>"_s;
}

// The view's delegates, found through the visual item tree (delegates are
// parented to the GridView's contentItem, not to the window QObject).
QList<QQuickItem *> boardCards(QQuickItem *item) {
  QList<QQuickItem *> found;
  if (item->objectName() == "boardCard"_L1)
    found.append(item);
  for (QQuickItem *child : item->childItems())
    found.append(boardCards(child));
  return found;
}

QList<QQuickItem *> boardCards(QObject *window) {
  auto *quickWindow = qobject_cast<QQuickWindow *>(window);
  return quickWindow ? boardCards(quickWindow->contentItem())
                     : QList<QQuickItem *>{};
}

QQuickItem *cardOfKind(QObject *window, const QString &kind) {
  for (auto *item : boardCards(window)) {
    if (item->property("kind").toString() == kind)
      return item;
  }
  return nullptr;
}

} // namespace

class BoardCardsViewTests : public QObject {
  Q_OBJECT

private slots:
  void rendersBoardCardsFromGameState() {
    MockHttpServer server;
    QVERIFY(server.start());
    const QString mutatedPath = u"/img/arkham/cards/01001_Mutated21.avif"_s;
    const QString studyBackPath = u"/img/arkham/cards/01111b.avif"_s;
    const QString agendaPath = u"/img/arkham/cards/01105.avif"_s;
    const QString actPath = u"/img/arkham/cards/01108.avif"_s;
    MockHttpServer::Response png;
    png.status = 200;
    png.contentType = "image/png"_ba;
    png.body = tinyPng();
    server.setResponse(mutatedPath, png);
    server.setResponse(studyBackPath, png);
    server.setResponse(agendaPath, png);
    MockHttpServer::Response missing;
    missing.status = 404;
    server.setResponse(actPath, missing);

    QTemporaryDir cacheRoot;
    QVERIFY(cacheRoot.isValid());
    AssetImageRequestCoordinator::Config config;
    config.assetBaseUrl = server.url(QString());
    config.cacheConfig.rootDirectory = cacheRoot.path();

    BoardCardsModel model;
    model.setGame(mutatedGame());

    QQmlEngine engine;
    engine.addImageProvider(u"arkham-card"_s,
                            new AssetCardImageProvider(config));
    engine.rootContext()->setContextProperty(u"boardCards"_s, &model);

    QQmlComponent component(&engine);
    component.setData("import QtQuick\n"
                      "Window {\n"
                      "  width: 800; height: 600; visible: true\n"
                      "  BoardCardsView { anchors.fill: parent;"
                      " model: boardCards }\n"
                      "}\n",
                      QUrl::fromLocalFile(QStringLiteral(
                          ARKHAM_TEST_QML_DIR "/BoardCardsTest.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window != nullptr, qPrintable(component.errorString()));

    QVERIFY(
        waitUntil([&window] { return boardCards(window.get()).size() == 4; }));

    QQuickItem *investigator = cardOfKind(window.get(), u"investigator"_s);
    QQuickItem *location = cardOfKind(window.get(), u"location"_s);
    QQuickItem *agenda = cardOfKind(window.get(), u"agenda"_s);
    QQuickItem *act = cardOfKind(window.get(), u"act"_s);
    QVERIFY(investigator && location && agenda && act);
    QCOMPARE(investigator->property("imageSource").toString(),
             u"image://arkham-card/c01001?mutation=Mutated21"_s);

    // Image.Ready == 1, Image.Error == 3.
    QVERIFY(waitUntil([&] {
      return investigator->property("status").toInt() == 1 &&
             location->property("status").toInt() == 1 &&
             agenda->property("status").toInt() == 1 &&
             act->property("status").toInt() == 3;
    }));
    QCOMPARE(server.requestCountForPath(mutatedPath), 1);
    QCOMPARE(server.requestCountForPath(studyBackPath), 1);
    QCOMPARE(server.requestCountForPath(agendaPath), 1);
    QCOMPARE(server.requestCountForPath(actPath), 1);
    QCOMPARE(server.requestCountForPath(u"/img/arkham/cards/01001.avif"_s), 0);

    // A loaded card hides its placeholder; a failed one shows its error
    // state with the card's label.
    auto *investigatorPlaceholder =
        investigator->findChild<QQuickItem *>(u"cardPlaceholder"_s);
    auto *actPlaceholder = act->findChild<QQuickItem *>(u"cardPlaceholder"_s);
    QVERIFY(investigatorPlaceholder && actPlaceholder);
    QVERIFY(!investigatorPlaceholder->isVisible());
    QVERIFY(actPlaceholder->isVisible());
  }

  // Model to provider: a Shattered investigator requests the form's art
  // and a side-B act its back, at the web's paths.
  void formOverrideAndSideBActReachTheProvider() {
    MockHttpServer server;
    QVERIFY(server.start());
    const QString shatteredPath = u"/img/arkham/cards/10661.avif"_s;
    const QString actBackPath = u"/img/arkham/cards/01108b.avif"_s;
    MockHttpServer::Response png;
    png.status = 200;
    png.contentType = "image/png"_ba;
    png.body = tinyPng();
    server.setResponse(shatteredPath, png);
    server.setResponse(actBackPath, png);

    QTemporaryDir cacheRoot;
    QVERIFY(cacheRoot.isValid());
    AssetImageRequestCoordinator::Config config;
    config.assetBaseUrl = server.url(QString());
    config.cacheConfig.rootDirectory = cacheRoot.path();

    BoardCardsModel model;
    model.setGame(editedGame({
        {R"("tag": "RegularForm")", R"("tag": "ShatteredForm")"},
        {"\"sequence\": [\n          1,\n          \"A\"",
         "\"sequence\": [\n          1,\n          \"B\""},
    }));

    QQmlEngine engine;
    engine.addImageProvider(u"arkham-card"_s,
                            new AssetCardImageProvider(config));
    std::unique_ptr<QObject> window = createBoardWindow(engine, model);
    QVERIFY(window != nullptr);
    QVERIFY(
        waitUntil([&window] { return boardCards(window.get()).size() == 4; }));

    QQuickItem *investigator = cardOfKind(window.get(), u"investigator"_s);
    QQuickItem *act = cardOfKind(window.get(), u"act"_s);
    QVERIFY(investigator && act);
    QCOMPARE(investigator->property("imageSource").toString(),
             u"image://arkham-card/c10661"_s);
    QCOMPARE(act->property("imageSource").toString(),
             u"image://arkham-card/c01108b"_s);
    // Image.Ready == 1.
    QVERIFY(waitUntil([&] {
      return investigator->property("status").toInt() == 1 &&
             act->property("status").toInt() == 1;
    }));
    QCOMPARE(server.requestCountForPath(shatteredPath), 1);
    QCOMPARE(server.requestCountForPath(actBackPath), 1);
    QCOMPARE(server.requestCountForPath(u"/img/arkham/cards/01001.avif"_s), 0);
    QCOMPARE(server.requestCountForPath(u"/img/arkham/cards/01108.avif"_s), 0);
  }

  // Each card's accessible name, and its placeholder text, is its catalog
  // card name, or its card code when the catalog has none; names arriving
  // later reach the delegates too.
  void accessibleNamesAreCatalogCardNames() {
    BoardCardsModel model;
    model.setCardNames({
        {u"c01105"_s, {CardName{u"What's Going On?!"_s, std::nullopt}, {}}},
        {u"c01111"_s,
         {CardName{u"Study (unrevealed)"_s, std::nullopt},
          CardName{u"Study"_s, std::nullopt}}},
    });
    model.setGame(editedGame({}));

    // No image provider: every card stays on its placeholder.
    QQmlEngine engine;
    std::unique_ptr<QObject> window = createBoardWindow(engine, model);
    QVERIFY(window != nullptr);
    QVERIFY(
        waitUntil([&window] { return boardCards(window.get()).size() == 4; }));

    const QList<std::pair<QString, QString>> expected{
        {u"agenda"_s, u"What's Going On?!"_s},
        {u"act"_s, u"c01108"_s},
        {u"location"_s, u"Study"_s},
        {u"investigator"_s, u"Roland Banks"_s},
    };
    const auto check = [&window](const QString &kind, const QString &name) {
      QQuickItem *card = cardOfKind(window.get(), kind);
      if (!card)
        return u"no %1 card"_s.arg(kind);
      const QString cardName = accessibleName(card);
      auto *image = card->childItems().value(0);
      const QString imageName = image ? accessibleName(image) : QString();
      auto *text = card->findChild<QQuickItem *>(u"cardLabel"_s);
      const QString shown =
          text ? text->property("text").toString() : QString();
      if (cardName != name || imageName != name || !shown.startsWith(name))
        return u"%1: card %2, image %3, text %4"_s.arg(kind, cardName,
                                                       imageName, shown);
      return QString();
    };
    for (const auto &[kind, name] : expected)
      QVERIFY2(check(kind, name).isEmpty(), qPrintable(check(kind, name)));

    model.setCardNames({{u"c01108"_s, {CardName{u"Trapped"_s, {}}, {}}}});
    QTRY_VERIFY2(check(u"act"_s, u"Trapped"_s).isEmpty(),
                 qPrintable(check(u"act"_s, u"Trapped"_s)));
    QVERIFY2(check(u"agenda"_s, u"c01105"_s).isEmpty(),
             qPrintable(check(u"agenda"_s, u"c01105"_s)));
  }

  // Refilling the model while the grid has focus leaves a current card, so
  // the focus mark (currentCardIndicator) still shows where the arrows are.
  void focusedGridKeepsACurrentCardAcrossAModelReset() {
    BoardCardsModel model;
    model.setGame(mutatedGame());
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(u"boardCards"_s, &model);
    QQmlComponent component(&engine);
    component.setData("import QtQuick\n"
                      "Window {\n"
                      "  width: 800; height: 600; visible: true\n"
                      "  BoardCardsView { objectName: \"grid\";"
                      " anchors.fill: parent; model: boardCards;"
                      " focus: true }\n"
                      "}\n",
                      QUrl::fromLocalFile(QStringLiteral(
                          ARKHAM_TEST_QML_DIR "/BoardCardsTest.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window != nullptr, qPrintable(component.errorString()));
    auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
    QVERIFY(quickWindow != nullptr);
    quickWindow->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(quickWindow));
    auto *grid = window->findChild<QQuickItem *>(u"grid"_s);
    QVERIFY(grid != nullptr);
    QTRY_VERIFY(grid->hasActiveFocus());
    QTRY_COMPARE(grid->property("currentIndex").toInt(), 0);
    grid->setProperty("currentIndex", 2);

    // As on the board screen: the model empties (closeGame()/reset()) and
    // the next board resets it while the grid keeps focus.
    model.clear();
    QTRY_COMPARE(grid->property("count").toInt(), 0);
    model.setGame(mutatedGame());
    QTRY_COMPARE(grid->property("count").toInt(), 4);
    QTRY_VERIFY(grid->property("currentIndex").toInt() >= 0);
    const auto marked = [&window] {
      int shown = 0;
      for (QQuickItem *card : boardCards(window.get())) {
        for (QQuickItem *child : card->childItems()) {
          if (child->objectName() == "currentCardIndicator"_L1 &&
              child->isVisible())
            ++shown;
        }
      }
      return shown;
    };
    QTRY_COMPARE(marked(), 1);
  }

  // Labels are server data: markup in one is shown literally and never
  // fetches anything (e.g. an <img> outside the card-art provider).
  void labelMarkupIsPlainText() {
    MockHttpServer server;
    QVERIFY(server.start());
    const QString label =
        u"<img src=\""_s + server.url(u"/x.png"_s).toString() + u"\">Study"_s;

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData("import QtQuick\n"
                      "Window {\n"
                      "  width: 200; height: 260; visible: true\n"
                      "  CardImage { objectName: \"card\"; anchors.fill: "
                      "parent }\n"
                      "}\n",
                      QUrl::fromLocalFile(QStringLiteral(
                          ARKHAM_TEST_QML_DIR "/CardImageTest.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window != nullptr, qPrintable(component.errorString()));
    auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
    QVERIFY(quickWindow);
    auto *card = quickWindow->contentItem()->childItems().value(0);
    QVERIFY(card && card->objectName() == "card"_L1);
    QVERIFY(card->setProperty("label", label));

    auto *text = card->findChild<QQuickItem *>(u"cardLabel"_s);
    QVERIFY(text);
    QVERIFY(text->isVisible());
    // Text.PlainText == 0 (Text.AutoText, the default, is 2).
    QCOMPARE(text->property("textFormat").toInt(), 0);
    QCOMPARE(text->property("text").toString(), label);

    // Spin the event loop for the full interval so a delayed rich-text
    // image fetch would reach the mock server before the check.
    QTest::qWait(250);
    QCOMPARE(server.requestCountForPath(u"/x.png"_s), 0);
  }
};

QTEST_MAIN(BoardCardsViewTests)

#include "BoardCardsViewTests.moc"
