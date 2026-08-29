#include <QAccessible>
#include <QAccessibleInterface>
#include <QGuiApplication>
#include <QObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QtTest>

#include <memory>
#include <tuple>

#include "FocusController.h"
#include "InputMapper.h"
#include "InputRouter.h"
#include "SemanticCommand.h"

using Arkham::CommandPhase;
using Arkham::FocusController;
using Arkham::FocusDirection;
using Arkham::FocusNodeSpec;
using Arkham::InputMapper;
using Arkham::InputRouter;
using Arkham::SemanticCommand;
using Arkham::WrapPolicy;

namespace {

// Registers exactly the same node ids/zones/adjacency the QML fixture
// visually represents (see qml/SemanticInputFixture.qml's board/hand/log
// delegates), so the C++ focus graph and the QML focus rectangles stay
// in lockstep for this test's assertions.
void registerFixtureNodes(FocusController &focus) {
  focus.registerNode({.id = QStringLiteral("board.nw"),
                      .zoneId = QStringLiteral("board"),
                      .neighbors = {
                          {FocusDirection::Right, QStringLiteral("board.ne")},
                          {FocusDirection::Down, QStringLiteral("board.sw")},
                      }});
  focus.registerNode({.id = QStringLiteral("board.ne"),
                      .zoneId = QStringLiteral("board"),
                      .neighbors = {
                          {FocusDirection::Left, QStringLiteral("board.nw")},
                          {FocusDirection::Down, QStringLiteral("board.se")},
                      }});
  focus.registerNode({.id = QStringLiteral("board.sw"),
                      .zoneId = QStringLiteral("board"),
                      .neighbors = {
                          {FocusDirection::Right, QStringLiteral("board.se")},
                          {FocusDirection::Up, QStringLiteral("board.nw")},
                      }});
  focus.registerNode({.id = QStringLiteral("board.se"),
                      .zoneId = QStringLiteral("board"),
                      .neighbors = {
                          {FocusDirection::Left, QStringLiteral("board.sw")},
                          {FocusDirection::Up, QStringLiteral("board.ne")},
                      }});

  focus.registerNode({.id = QStringLiteral("hand.card1"),
                      .zoneId = QStringLiteral("hand"),
                      .neighbors = {
                          {FocusDirection::Right, QStringLiteral("hand.card2")},
                      }});
  focus.registerNode({.id = QStringLiteral("hand.card2"),
                      .zoneId = QStringLiteral("hand"),
                      .neighbors = {
                          {FocusDirection::Left, QStringLiteral("hand.card1")},
                          {FocusDirection::Right, QStringLiteral("hand.card3")},
                      }});
  focus.registerNode({.id = QStringLiteral("hand.card3"),
                      .zoneId = QStringLiteral("hand"),
                      .neighbors = {
                          {FocusDirection::Left, QStringLiteral("hand.card2")},
                      }});

  focus.registerNode({.id = QStringLiteral("log.entry"),
                      .zoneId = QStringLiteral("log"),
                      .neighbors = {}});
}

// Wires InputRouter::commandDispatched to FocusController, exactly as
// described in SemanticInputFixture.qml's header comment: all command-
// to-focus-graph routing happens here in C++, never in QML.
void wireCommandsToFocus(InputRouter &router, FocusController &focus) {
  QObject::connect(
      &router, &InputRouter::commandDispatched, &focus,
      [&focus](const SemanticCommand command, const CommandPhase phase) {
        if (phase != CommandPhase::Pressed) {
          return;
        }
        switch (command) {
        case SemanticCommand::FocusUp:
          focus.moveFocus(FocusDirection::Up);
          break;
        case SemanticCommand::FocusDown:
          focus.moveFocus(FocusDirection::Down);
          break;
        case SemanticCommand::FocusLeft:
          focus.moveFocus(FocusDirection::Left);
          break;
        case SemanticCommand::FocusRight:
          focus.moveFocus(FocusDirection::Right);
          break;
        case SemanticCommand::CycleNextZone:
          focus.cycleZone(true);
          break;
        case SemanticCommand::CyclePreviousZone:
          focus.cycleZone(false);
          break;
        default:
          break;
        }
      });
}

} // namespace

class SemanticInputFixtureTests final : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void loadsWithAccessibleNamesAndRolesForEveryFixtureNode();
  void semanticFocusMovesTheQmlActiveFocusRectangle();
  void shoulderZoneSwitchingMovesFocusBetweenZones();
  void wrapPolicyKeepsTheQmlFocusRectangleInStepOnWrap();

private:
  std::unique_ptr<QQmlApplicationEngine> engine_;
  std::unique_ptr<FocusController> focus_;
  std::unique_ptr<InputMapper> mapper_;
  std::unique_ptr<InputRouter> router_;
  QQuickWindow *window_ = nullptr;

  // Loads the real QML fixture with a fresh FocusController/InputMapper/
  // InputRouter wired together exactly as a real host application would,
  // and returns the root QQuickWindow.
  QQuickWindow *loadFixture(WrapPolicy wrapPolicy = WrapPolicy::NoWrap);
};

void SemanticInputFixtureTests::init() {
  engine_.reset();
  focus_.reset();
  mapper_.reset();
  router_.reset();
  window_ = nullptr;
}

void SemanticInputFixtureTests::cleanup() {
  engine_.reset();
  router_.reset();
  mapper_.reset();
  focus_.reset();
  window_ = nullptr;
}

QQuickWindow *
SemanticInputFixtureTests::loadFixture(const WrapPolicy wrapPolicy) {
  focus_ = std::make_unique<FocusController>(wrapPolicy);
  registerFixtureNodes(*focus_);
  focus_->setInitialFocus(QStringLiteral("board.nw"));

  mapper_ = std::make_unique<InputMapper>();
  router_ = std::make_unique<InputRouter>(*mapper_);
  wireCommandsToFocus(*router_, *focus_);

  engine_ = std::make_unique<QQmlApplicationEngine>();
  engine_->setInitialProperties(
      {{QStringLiteral("focusController"), QVariant::fromValue(focus_.get())}});
  engine_->load(QUrl::fromLocalFile(
      QStringLiteral(ARKHAM_TEST_QML_DIR "/SemanticInputFixture.qml")));

  if (engine_->rootObjects().isEmpty()) {
    return nullptr;
  }
  auto *window =
      qobject_cast<QQuickWindow *>(engine_->rootObjects().constFirst());
  if (window != nullptr) {
    router_->install(window);
  }
  return window;
}

void SemanticInputFixtureTests::
    loadsWithAccessibleNamesAndRolesForEveryFixtureNode() {
  window_ = loadFixture();
  QVERIFY(window_ != nullptr);

  const QList<std::tuple<QString, QString, QAccessible::Role>> expected{
      {QStringLiteral("board.nw"), QStringLiteral("Northwest Zone"),
       QAccessible::Button},
      {QStringLiteral("board.ne"), QStringLiteral("Northeast Zone"),
       QAccessible::Button},
      {QStringLiteral("board.sw"), QStringLiteral("Southwest Zone"),
       QAccessible::Button},
      {QStringLiteral("board.se"), QStringLiteral("Southeast Zone"),
       QAccessible::Button},
      {QStringLiteral("hand.card1"), QStringLiteral("Card 1"),
       QAccessible::Button},
      {QStringLiteral("hand.card2"), QStringLiteral("Card 2"),
       QAccessible::Button},
      {QStringLiteral("hand.card3"), QStringLiteral("Card 3"),
       QAccessible::Button},
      {QStringLiteral("log.entry"), QStringLiteral("Log Entry"),
       QAccessible::StaticText},
  };

  QTRY_VERIFY(window_->property("itemsById")
                  .toMap()
                  .contains(QStringLiteral("board.nw")));
  const QVariantMap itemsById = window_->property("itemsById").toMap();

  for (const auto &[nodeId, expectedName, expectedRole] : expected) {
    QVERIFY2(itemsById.contains(nodeId),
             qPrintable(QStringLiteral("missing fixture item for ") + nodeId));
    auto *item = itemsById.value(nodeId).value<QQuickItem *>();
    QVERIFY(item != nullptr);

    QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(item);
    QVERIFY2(
        iface != nullptr,
        qPrintable(QStringLiteral("no accessible interface for ") + nodeId));
    QCOMPARE(iface->text(QAccessible::Name), expectedName);
    QCOMPARE(iface->role(), expectedRole);
  }
}

void SemanticInputFixtureTests::semanticFocusMovesTheQmlActiveFocusRectangle() {
  window_ = loadFixture();
  QVERIFY(window_ != nullptr);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));

  const QVariantMap itemsById = window_->property("itemsById").toMap();
  auto *nw = itemsById.value(QStringLiteral("board.nw")).value<QQuickItem *>();
  auto *ne = itemsById.value(QStringLiteral("board.ne")).value<QQuickItem *>();
  QVERIFY(nw != nullptr);
  QVERIFY(ne != nullptr);
  // Real QML activeFocus is applied asynchronously via the
  // currentFocusChanged -> Connections -> forceActiveFocus() signal
  // chain (see qml/SemanticInputFixture.qml), so poll rather than assert
  // immediately to stay robust against event-loop timing.
  QTRY_VERIFY(nw->hasActiveFocus());
  QTRY_VERIFY(!ne->hasActiveFocus());

  QVERIFY(focus_->moveFocus(FocusDirection::Right));
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.ne"));
  QTRY_VERIFY(!nw->hasActiveFocus());
  QTRY_VERIFY(ne->hasActiveFocus());
}

void SemanticInputFixtureTests::shoulderZoneSwitchingMovesFocusBetweenZones() {
  window_ = loadFixture();
  QVERIFY(window_ != nullptr);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));

  QVERIFY(focus_->cycleZone(true));
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("hand.card1"));

  QVERIFY(focus_->cycleZone(true));
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("log.entry"));

  QVERIFY(focus_->cycleZone(false));
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("hand.card1"));

  const QVariantMap itemsById = window_->property("itemsById").toMap();
  auto *handCard1 =
      itemsById.value(QStringLiteral("hand.card1")).value<QQuickItem *>();
  QVERIFY(handCard1 != nullptr);
  QTRY_VERIFY(handCard1->hasActiveFocus());
}

void SemanticInputFixtureTests::
    wrapPolicyKeepsTheQmlFocusRectangleInStepOnWrap() {
  window_ = loadFixture(WrapPolicy::WrapWithinZone);
  QVERIFY(window_ != nullptr);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));

  // board.nw has no explicit Left neighbor; with wrap enabled this must
  // wrap to the last-registered node in the same zone (board.se), and the
  // QML fixture's real active focus must follow.
  QVERIFY(focus_->moveFocus(FocusDirection::Left));
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.se"));

  const QVariantMap itemsById = window_->property("itemsById").toMap();
  auto *se = itemsById.value(QStringLiteral("board.se")).value<QQuickItem *>();
  QVERIFY(se != nullptr);
  QTRY_VERIFY(se->hasActiveFocus());
}

QTEST_MAIN(SemanticInputFixtureTests)

#include "SemanticInputFixtureTests.moc"
