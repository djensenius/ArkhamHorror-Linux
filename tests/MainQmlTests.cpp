// Loads the real qml/Main.qml against a stand-in sessionCoordinator and
// checks that a long, many-line maintenance notice stays inside the window:
// it is height-bounded and scrollable, keeps the whole text (including the
// trailing "Try again ..." sentence), and never pushes the input hint below
// the window.

#include <QGuiApplication>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QUrl>
#include <QtTest>
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
  Q_PROPERTY(bool serverMaintenance READ serverMaintenance CONSTANT)
  Q_PROPERTY(QString diagnostic READ diagnostic CONSTANT)

public:
  explicit FakeCoordinator(QString diagnostic)
      : m_diagnostic(std::move(diagnostic)) {}

  [[nodiscard]] QString stateDescription() const {
    return QStringLiteral("Server maintenance");
  }
  [[nodiscard]] QString selectedProfileDisplayName() const {
    return QStringLiteral("Arkham Horror");
  }
  [[nodiscard]] QString selectedProfileBaseUrl() const {
    return QStringLiteral("https://arkhamhorror.app");
  }
  [[nodiscard]] bool serverMaintenance() const { return true; }
  [[nodiscard]] QString diagnostic() const { return m_diagnostic; }

private:
  QString m_diagnostic;
};

} // namespace

class MainQmlTests final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void longMaintenanceNoticeIsBoundedAndScrollable_data();
  void longMaintenanceNoticeIsBoundedAndScrollable();
  void shortMaintenanceNoticeIsShownWhole();

private:
  QQuickWindow *load(const QString &diagnostic, int height);

  std::unique_ptr<FakeCoordinator> m_coordinator;
  std::unique_ptr<QQmlApplicationEngine> m_engine;
};

QQuickWindow *MainQmlTests::load(const QString &diagnostic, int height) {
  m_engine.reset();
  m_coordinator = std::make_unique<FakeCoordinator>(diagnostic);
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

QTEST_MAIN(MainQmlTests)

#include "MainQmlTests.moc"
