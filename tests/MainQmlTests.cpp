// Loads the real qml/Main.qml against a stand-in sessionCoordinator and
// checks that a long, many-line maintenance notice stays inside the window:
// it is height-bounded and scrollable, keeps the whole text (including the
// trailing "Try again ..." sentence), and never pushes the input hint below
// the window.

#include <QAccessible>
#include <QAccessibleInterface>
#include <QGuiApplication>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QTextOption>
#include <QUrl>
#include <QtTest>
#include <algorithm>
#include <memory>

namespace {

// Exposes only the SessionCoordinator properties Main.qml reads.
class FakeCoordinator final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString stateDescription READ stateDescription CONSTANT)
  Q_PROPERTY(QString selectedProfileDisplayName READ selectedProfileDisplayName
                 CONSTANT)
  Q_PROPERTY(
      QString selectedProfileBaseUrl READ selectedProfileBaseUrl CONSTANT)
  Q_PROPERTY(bool serverMaintenance READ serverMaintenance NOTIFY
                 serverMaintenanceChanged)
  Q_PROPERTY(QString diagnostic READ diagnostic CONSTANT)

public:
  explicit FakeCoordinator(
      QString diagnostic, QString displayName = QStringLiteral("Arkham Horror"))
      : m_diagnostic(std::move(diagnostic)),
        m_displayName(std::move(displayName)) {}

  [[nodiscard]] QString stateDescription() const {
    return QStringLiteral("Server maintenance");
  }
  [[nodiscard]] QString selectedProfileDisplayName() const {
    return m_displayName;
  }
  [[nodiscard]] QString selectedProfileBaseUrl() const {
    return QStringLiteral("https://arkhamhorror.app");
  }
  [[nodiscard]] bool serverMaintenance() const { return m_serverMaintenance; }
  void setServerMaintenance(bool serverMaintenance) {
    if (m_serverMaintenance != serverMaintenance) {
      m_serverMaintenance = serverMaintenance;
      emit serverMaintenanceChanged();
    }
  }
  [[nodiscard]] QString diagnostic() const { return m_diagnostic; }

signals:
  void serverMaintenanceChanged();

private:
  QString m_diagnostic;
  QString m_displayName;
  bool m_serverMaintenance = true;
};

} // namespace

class MainQmlTests final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void longMaintenanceNoticeIsBoundedAndScrollable_data();
  void longMaintenanceNoticeIsBoundedAndScrollable();
  void shortMaintenanceNoticeIsShownWhole();
  void unbrokenMaintenanceNoticeWrapsWithinWidth();
  void maintenanceNoticeRetryTimeIsReachableWithKeysAlone();
  void longProfileNameStaysInsideTheStatusLabel();
  void maintenanceNoticeReturnsFocusToTheCardItWasEnteredFrom();
  void endingMaintenanceReturnsFocusFromTheNoticeToACard();
  void yourGamesCardOpensTheBoardScreenAndEscapeReturns();
  void destinationsFitAtTheMinimumWidth_data();
  void destinationsFitAtTheMinimumWidth();

private:
  QQuickItem *focusNoticeFromThirdCard(QQuickWindow *window,
                                       QQuickItem *notice);
  QQuickWindow *
  load(const QString &diagnostic, int height,
       const QString &displayName = QStringLiteral("Arkham Horror"));

  std::unique_ptr<FakeCoordinator> m_coordinator;
  std::unique_ptr<QQmlApplicationEngine> m_engine;
};

QQuickWindow *MainQmlTests::load(const QString &diagnostic, int height,
                                 const QString &displayName) {
  m_engine.reset();
  m_coordinator = std::make_unique<FakeCoordinator>(diagnostic, displayName);
  m_engine = std::make_unique<QQmlApplicationEngine>();
  m_engine->rootContext()->setContextProperty(
      QStringLiteral("sessionCoordinator"), m_coordinator.get());
  m_engine->setInitialProperties({{QStringLiteral("configuredServer"),
                                   QStringLiteral("https://arkhamhorror.app")},
                                  {QStringLiteral("height"), height}});
  m_engine->load(
      QUrl::fromLocalFile(QStringLiteral(ARKHAM_TEST_QML_DIR "/Main.qml")));
  if (m_engine->rootObjects().isEmpty()) {
    return nullptr;
  }
  return qobject_cast<QQuickWindow *>(m_engine->rootObjects().constFirst());
}

void MainQmlTests::initTestCase() {
  // A fixed, customizable style keeps the layout the same on every
  // platform (the macOS native style rejects Main.qml's custom controls).
  qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
}

void MainQmlTests::longMaintenanceNoticeIsBoundedAndScrollable_data() {
  QTest::addColumn<int>("height");
  QTest::newRow("default height") << 800;
  QTest::newRow("minimum height") << 600;
}

void MainQmlTests::longMaintenanceNoticeIsBoundedAndScrollable() {
  QFETCH(int, height);
  // A message the parser accepts (well under the 1000-unit cap) that is
  // still 166 lines long.
  const QString diagnostic = QStringLiteral("Down.\n").repeated(166) +
                             QStringLiteral(" Try again in about 2 minutes.");
  QQuickWindow *window = load(diagnostic, height);
  QVERIFY(window != nullptr);

  auto *notice =
      window->findChild<QQuickItem *>(QStringLiteral("maintenanceNotice"));
  auto *text =
      window->findChild<QQuickItem *>(QStringLiteral("maintenanceNoticeText"));
  auto *hint = window->findChild<QQuickItem *>(QStringLiteral("inputHint"));
  QVERIFY(notice != nullptr);
  QVERIFY(text != nullptr);
  QVERIFY(hint != nullptr);

  QTRY_VERIFY(notice->height() > 0);
  QCOMPARE(window->height(), height);
  QVERIFY(notice->isVisible());
  QVERIFY(notice->clip());
  // The whole diagnostic is kept, plain text, and taller than the bound.
  QCOMPARE(text->property("text").toString(), diagnostic);
  QCOMPARE(text->property("textFormat").toInt(), int(Qt::PlainText));
  QVERIFY(text->height() > notice->height());
  QVERIFY(notice->height() <= window->height() * 0.2 + 0.5);
  QCOMPARE(notice->property("contentHeight").toReal(), text->height());
  // Nothing below the notice leaves the window's layout area.
  QQuickItem *column = hint->parentItem();
  QVERIFY(column != nullptr);
  const qreal hintBottom = hint->y() + hint->height();
  QVERIFY2(hintBottom <= column->height() + 0.5,
           qPrintable(QStringLiteral("hint bottom %1 > layout height %2")
                          .arg(hintBottom)
                          .arg(column->height())));
  const QPointF noticeBottom =
      notice->mapToItem(column, QPointF(0, notice->height()));
  const QPointF hintTop = hint->mapToItem(column, QPointF(0, 0));
  QVERIFY(noticeBottom.y() <= hintTop.y());
}

void MainQmlTests::shortMaintenanceNoticeIsShownWhole() {
  const QString diagnostic =
      QStringLiteral("Back soon. Try again in about 2 minutes.");
  QQuickWindow *window = load(diagnostic, 800);
  QVERIFY(window != nullptr);
  auto *notice =
      window->findChild<QQuickItem *>(QStringLiteral("maintenanceNotice"));
  auto *text =
      window->findChild<QQuickItem *>(QStringLiteral("maintenanceNoticeText"));
  QVERIFY(notice != nullptr);
  QVERIFY(text != nullptr);
  QTRY_VERIFY(notice->height() > 0);
  QCOMPARE(text->property("text").toString(), diagnostic);
  // No clipping for a notice that fits.
  QCOMPARE(notice->height(), text->height());
}

void MainQmlTests::unbrokenMaintenanceNoticeWrapsWithinWidth() {
  // A message the parser accepts at its 1000-unit cap with no word
  // boundary (e.g. a long URL) must still wrap inside the notice, which
  // never scrolls horizontally.
  const QString diagnostic = QStringLiteral("x").repeated(1000) +
                             QStringLiteral(" Try again in about 2 minutes.");
  QQuickWindow *window = load(diagnostic, 800);
  QVERIFY(window != nullptr);
  auto *notice =
      window->findChild<QQuickItem *>(QStringLiteral("maintenanceNotice"));
  auto *text =
      window->findChild<QQuickItem *>(QStringLiteral("maintenanceNoticeText"));
  QVERIFY(notice != nullptr);
  QVERIFY(text != nullptr);
  QTRY_VERIFY(notice->height() > 0);
  QCOMPARE(text->property("text").toString(), diagnostic);
  // Text.Wrap == WrapAtWordBoundaryOrAnywhere.
  QCOMPARE(text->property("wrapMode").toInt(),
           int(QTextOption::WrapAtWordBoundaryOrAnywhere));
  QVERIFY(text->width() > 0);
  QCOMPARE(text->width(), notice->property("availableWidth").toReal());
  const qreal contentWidth = text->property("contentWidth").toReal();
  QVERIFY2(contentWidth <= text->width() + 0.5,
           qPrintable(QStringLiteral("content width %1 > label width %2")
                          .arg(contentWidth)
                          .arg(text->width())));
  QVERIFY(text->property("lineCount").toInt() > 2);
}

void MainQmlTests::maintenanceNoticeRetryTimeIsReachableWithKeysAlone() {
  // A server message at the parser's 1000-character cap, followed by the
  // retry sentence the way ServerMaintenance::notice() composes it
  // ("%1 Try again in %2."), overflows the notice; without a pointer the
  // retry time must still be reachable from the destination cards.
  const QString reason =
      QStringLiteral("The server is down for planned maintenance. ")
          .repeated(30)
          .left(1000)
          .trimmed();
  QVERIFY(reason.size() > 990);
  const QString diagnostic =
      reason + QStringLiteral(" Try again in about 2 minutes.");
  QQuickWindow *window = load(diagnostic, 800);
  QVERIFY(window != nullptr);
  window->requestActivate();
  QVERIFY(QTest::qWaitForWindowActive(window));

  auto *notice =
      window->findChild<QQuickItem *>(QStringLiteral("maintenanceNotice"));
  auto *text =
      window->findChild<QQuickItem *>(QStringLiteral("maintenanceNoticeText"));
  QVERIFY(notice != nullptr);
  QVERIFY(text != nullptr);
  QTRY_VERIFY(notice->height() > 0);
  QCOMPARE(text->property("text").toString(), diagnostic);
  auto *flickable = notice->property("contentItem").value<QQuickItem *>();
  QVERIFY(flickable != nullptr);
  // The retry sentence starts below the fold.
  QVERIFY(text->height() > flickable->height());
  QCOMPARE(flickable->property("contentY").toReal(), 0.0);

  // The first destination card starts focused (a Button, not the notice).
  QTRY_VERIFY(window->activeFocusItem() != nullptr);
  QQuickItem *firstCard = window->activeFocusItem();
  QVERIFY(firstCard->inherits("QQuickButton"));
  QCOMPARE(firstCard->property("text").toString(), QStringLiteral("Connect"));

  // Down from a card focuses the notice.
  QTest::keyClick(window, Qt::Key_Down);
  QTRY_VERIFY(notice->hasActiveFocus());
  QCOMPARE(notice->property("activeFocusOnTab").toBool(), true);

  // Down then scrolls it until the end of the text (the retry sentence) is
  // inside the viewport.
  const auto atEnd = [&] {
    return flickable->property("contentY").toReal() + flickable->height() >=
           text->height() - 0.5;
  };
  // Each press moves by the notice's own explicit step (not a style's
  // built-in arrow handling).
  const qreal step = notice->property("scrollStep").toReal();
  QVERIFY(step > 0);
  QTest::keyClick(window, Qt::Key_Down);
  QCOMPARE(flickable->property("contentY").toReal(),
           std::min(step, text->height() - flickable->height()));
  for (int presses = 0; presses < 50 && !atEnd(); ++presses) {
    QTest::keyClick(window, Qt::Key_Down);
  }
  QVERIFY2(atEnd(),
           qPrintable(QStringLiteral("contentY %1 + height %2 < text %3")
                          .arg(flickable->property("contentY").toReal())
                          .arg(flickable->height())
                          .arg(text->height())));
  QVERIFY(notice->hasActiveFocus());
  QVERIFY(flickable->property("contentY").toReal() > 0);
  // A screen reader gets the whole text, retry sentence included.
  QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(notice);
  QVERIFY(iface != nullptr);
  QCOMPARE(iface->text(QAccessible::Name), diagnostic);

  // Up scrolls back to the top and then returns focus to the card.
  for (int presses = 0; presses < 50 && notice->hasActiveFocus(); ++presses) {
    QTest::keyClick(window, Qt::Key_Up);
  }
  QCOMPARE(flickable->property("contentY").toReal(), 0.0);
  QTRY_VERIFY(firstCard->hasActiveFocus());
}

void MainQmlTests::longProfileNameStaysInsideTheStatusLabel() {
  // A custom profile's display name has no length limit; the status label
  // ("<state> — <profile>") must stay on one line inside the window and
  // keep its full text for assistive technology.
  const QString displayName =
      QStringLiteral("My very long custom server profile ")
          .repeated(9)
          .left(300);
  QCOMPARE(displayName.size(), 300);
  QQuickWindow *window =
      load(QStringLiteral("Back soon. Try again in about 2 minutes."), 800,
           displayName);
  QVERIFY(window != nullptr);

  auto *status = window->findChild<QQuickItem *>(QStringLiteral("statusLabel"));
  QVERIFY(status != nullptr);
  QTRY_VERIFY(status->width() > 0);
  const QString expected =
      QStringLiteral("Server maintenance — %1").arg(displayName);
  QCOMPARE(status->property("text").toString(), expected);
  QVERIFY(status->isVisible());
  QCOMPARE(status->property("elide").toInt(), int(Qt::ElideRight));
  QVERIFY(status->property("truncated").toBool());
  QCOMPARE(status->property("lineCount").toInt(), 1);

  // The label (and its drawn text) fits the layout column, which itself
  // sits inside the window.
  QQuickItem *column = status->parentItem();
  QVERIFY(column != nullptr);
  QVERIFY(column->width() > 0);
  QVERIFY(column->width() <= window->width());
  QVERIFY2(status->x() + status->width() <= column->width() + 0.5,
           qPrintable(QStringLiteral("label right %1 > column width %2")
                          .arg(status->x() + status->width())
                          .arg(column->width())));
  QVERIFY(status->property("contentWidth").toReal() <= status->width() + 0.5);

  // Label already exposes its text, so this only shows the full text stays
  // available; the elide/truncation checks above guard the elision.
  QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(status);
  QVERIFY(iface != nullptr);
  QCOMPARE(iface->text(QAccessible::Name), expected);
}

// Moves from the first card to the third ("Input guide") and then Down into
// the notice with real key events; returns the third card (nullptr on
// failure, with the failure already recorded).
QQuickItem *MainQmlTests::focusNoticeFromThirdCard(QQuickWindow *window,
                                                   QQuickItem *notice) {
  window->requestActivate();
  if (!QTest::qWaitForWindowActive(window)) {
    QTest::qFail("window never became active", __FILE__, __LINE__);
    return nullptr;
  }
  if (!QTest::qWaitFor([&] { return window->activeFocusItem() != nullptr; })) {
    QTest::qFail("no initial focus", __FILE__, __LINE__);
    return nullptr;
  }
  QTest::keyClick(window, Qt::Key_Right);
  QTest::keyClick(window, Qt::Key_Right);
  QQuickItem *thirdCard = window->activeFocusItem();
  if (thirdCard == nullptr || !thirdCard->inherits("QQuickButton") ||
      thirdCard->property("text").toString() != QStringLiteral("Input guide")) {
    QTest::qFail("Right, Right did not reach the Input guide card", __FILE__,
                 __LINE__);
    return nullptr;
  }
  QTest::keyClick(window, Qt::Key_Down);
  if (!QTest::qWaitFor([&] { return notice->hasActiveFocus(); })) {
    QTest::qFail("Down from the Input guide card did not focus the notice",
                 __FILE__, __LINE__);
    return nullptr;
  }
  return thirdCard;
}

void MainQmlTests::maintenanceNoticeReturnsFocusToTheCardItWasEnteredFrom() {
  QQuickWindow *window =
      load(QStringLiteral("Back soon. Try again in about 2 minutes."), 800);
  QVERIFY(window != nullptr);
  auto *notice =
      window->findChild<QQuickItem *>(QStringLiteral("maintenanceNotice"));
  QVERIFY(notice != nullptr);
  QTRY_VERIFY(notice->isVisible());

  QQuickItem *thirdCard = focusNoticeFromThirdCard(window, notice);
  QVERIFY(thirdCard != nullptr);
  // A short notice is already at the top: one Up returns to that same card,
  // not the first one.
  QTest::keyClick(window, Qt::Key_Up);
  QTRY_VERIFY(thirdCard->hasActiveFocus());
  QVERIFY(!notice->hasActiveFocus());
}

void MainQmlTests::endingMaintenanceReturnsFocusFromTheNoticeToACard() {
  QQuickWindow *window =
      load(QStringLiteral("Back soon. Try again in about 2 minutes."), 800);
  QVERIFY(window != nullptr);
  auto *notice =
      window->findChild<QQuickItem *>(QStringLiteral("maintenanceNotice"));
  QVERIFY(notice != nullptr);
  QTRY_VERIFY(notice->isVisible());

  QQuickItem *thirdCard = focusNoticeFromThirdCard(window, notice);
  QVERIFY(thirdCard != nullptr);

  // Maintenance ends while the notice holds focus: the notice hides and
  // focus goes back to the card it was entered from, so directional input
  // keeps working.
  m_coordinator->setServerMaintenance(false);
  QTRY_VERIFY(!notice->isVisible());
  QTRY_VERIFY(thirdCard->hasActiveFocus());
  QVERIFY(!notice->hasActiveFocus());
  QTest::keyClick(window, Qt::Key_Left);
  QTRY_VERIFY(window->activeFocusItem() != nullptr);
  QCOMPARE(window->activeFocusItem()->property("text").toString(),
           QStringLiteral("Browse fixtures"));
}

void MainQmlTests::yourGamesCardOpensTheBoardScreenAndEscapeReturns() {
  // No gameBoard context property here (as in --smoke-test): the screen
  // still opens, asks the user to sign in, and Escape comes back.
  QQuickWindow *window =
      load(QStringLiteral("Back soon. Try again in about 2 minutes."), 800);
  QVERIFY(window != nullptr);
  window->requestActivate();
  QVERIFY(QTest::qWaitForWindowActive(window));
  QTRY_VERIFY(window->activeFocusItem() != nullptr);
  for (int i = 0; i < 3; ++i) {
    QTest::keyClick(window, Qt::Key_Right);
  }
  QQuickItem *gamesCard = window->activeFocusItem();
  QVERIFY(gamesCard != nullptr);
  QCOMPARE(gamesCard->property("text").toString(),
           QStringLiteral("Your games"));

  auto *loader = window->findChild<QQuickItem *>(QStringLiteral("boardLoader"));
  auto *landing = window->findChild<QQuickItem *>(QStringLiteral("landing"));
  QVERIFY(loader != nullptr);
  QVERIFY(landing != nullptr);
  QVERIFY(!loader->property("active").toBool());

  QTest::keyClick(window, Qt::Key_Return);
  QTRY_VERIFY(loader->property("active").toBool());
  QVERIFY(!landing->isVisible());
  auto *screen = loader->property("item").value<QQuickItem *>();
  QVERIFY(screen != nullptr);
  auto *signedOut =
      screen->findChild<QQuickItem *>(QStringLiteral("signedOutLabel"));
  QVERIFY(signedOut != nullptr);
  QVERIFY(signedOut->isVisible());
  QTRY_VERIFY(screen->hasActiveFocus());

  QTest::keyClick(window, Qt::Key_Escape);
  QTRY_VERIFY(!loader->property("active").toBool());
  QVERIFY(landing->isVisible());
  QTRY_VERIFY(gamesCard->hasActiveFocus());
}

void MainQmlTests::destinationsFitAtTheMinimumWidth_data() {
  QTest::addColumn<int>("width"); // 0: the window's minimum width
  QTest::addColumn<int>("height");
  QTest::newRow("minimum width") << 0 << 600;
  QTest::newRow("Steam Deck") << 1280 << 800;
  QTest::newRow("wide") << 1700 << 600;
}

void MainQmlTests::destinationsFitAtTheMinimumWidth() {
  // Every landing card, its whole hit area and its title, stays inside the
  // window from the supported minimum width up, and keys still reach the
  // last card.
  QFETCH(int, width);
  QFETCH(int, height);
  QQuickWindow *window = load(QString(), height);
  QVERIFY(window != nullptr);
  QCOMPARE(window->minimumWidth(), 960);
  const bool minimum = width == 0;
  if (minimum)
    width = window->minimumWidth();
  window->setWidth(width);
  window->requestActivate();
  QVERIFY(QTest::qWaitForWindowActive(window));
  QTRY_COMPARE(window->contentItem()->width(), qreal(width));

  const QStringList titles{
      QStringLiteral("Connect"), QStringLiteral("Browse fixtures"),
      QStringLiteral("Input guide"), QStringLiteral("Your games")};
  auto *landing = window->findChild<QQuickItem *>(QStringLiteral("landing"));
  QVERIFY(landing != nullptr);
  // Repeater delegates are visual (not QObject) children of the row.
  QList<QQuickItem *> cards;
  QList<QQuickItem *> pending{landing};
  while (!pending.isEmpty()) {
    QQuickItem *item = pending.takeFirst();
    if (item->inherits("QQuickButton") &&
        titles.contains(item->property("text").toString()))
      cards.append(item);
    pending.append(item->childItems());
  }
  QCOMPARE(cards.size(), titles.size());
  // The design width and spacing, as Main.qml sets them: the cards'
  // Layout.maximumWidth (Layout's attached object is a child of the card)
  // and their row's spacing.
  const auto designWidth = [](const QQuickItem *card) {
    for (const QObject *attached : card->children()) {
      if (attached->inherits("QQuickLayoutAttached"))
        return attached->property("maximumWidth").toReal();
    }
    return -1.0;
  };
  const qreal cardWidth = designWidth(cards.constFirst());
  const qreal spacing =
      cards.constFirst()->parentItem()->property("spacing").toReal();
  QVERIFY(cardWidth > 0 && spacing > 0);

  const QRectF bounds(QPointF(0, 0), window->contentItem()->size());
  const auto inside = [](const QRectF &outer, const QRectF &inner) {
    return inner.left() >= outer.left() - 0.5 &&
           inner.top() >= outer.top() - 0.5 &&
           inner.right() <= outer.right() + 0.5 &&
           inner.bottom() <= outer.bottom() + 0.5;
  };
  for (QQuickItem *card : cards) {
    const QString name = card->property("text").toString();
    const QRectF cardRect =
        card->mapRectToScene(QRectF(0, 0, card->width(), card->height()));
    QVERIFY2(card->width() > 0 && inside(bounds, cardRect),
             qPrintable(QStringLiteral("%1 card %2,%3 %4x%5 outside %6")
                            .arg(name)
                            .arg(cardRect.x())
                            .arg(cardRect.y())
                            .arg(cardRect.width())
                            .arg(cardRect.height())
                            .arg(bounds.width())));
    // With room (the default size and up) the cards keep their full
    // design width.
    if (!minimum)
      QCOMPARE(card->width(), cardWidth);
    auto *content = card->property("contentItem").value<QQuickItem *>();
    QVERIFY(content != nullptr && !content->childItems().isEmpty());
    QQuickItem *title = content->childItems().constFirst();
    QCOMPARE(title->property("text").toString(), name);
    const qreal drawn =
        qMin(title->width(), title->property("contentWidth").toReal());
    const QRectF titleRect = title->mapRectToScene(
        QRectF(0, 0, drawn, title->property("contentHeight").toReal()));
    QVERIFY2(inside(cardRect, titleRect) && inside(bounds, titleRect),
             qPrintable(QStringLiteral("%1 title %2..%3 outside card %4..%5")
                            .arg(name)
                            .arg(titleRect.left())
                            .arg(titleRect.right())
                            .arg(cardRect.left())
                            .arg(cardRect.right())));
  }

  // The cards stay packed left to right with the row's spacing.
  std::sort(cards.begin(), cards.end(),
            [](QQuickItem *a, QQuickItem *b) { return a->x() < b->x(); });
  for (qsizetype i = 1; i < cards.size(); ++i) {
    const QQuickItem *previous = cards.at(i - 1);
    QCOMPARE(cards.at(i)->x() - (previous->x() + previous->width()), spacing);
  }

  QTRY_VERIFY(window->activeFocusItem() != nullptr);
  for (int i = 0; i < 3; ++i)
    QTest::keyClick(window, Qt::Key_Right);
  QCOMPARE(window->activeFocusItem()->property("text").toString(),
           QStringLiteral("Your games"));
}

QTEST_MAIN(MainQmlTests)

#include "MainQmlTests.moc"
