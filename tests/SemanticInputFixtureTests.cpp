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
  void
  realKeyEventsThroughTheInstalledRouterDriveFocusAndTheQmlFixtureEndToEnd();
  void unboundKeyEventsThroughTheInstalledRouterNeverMoveFocus();
  void tabAndBacktabNeverDivergeSemanticFocusFromRealQmlActiveFocus();
  void textEntryFieldSuspendsSemanticDispatchAndHandlesKeysNatively();
  void leavingTextEntryResumesSemanticMappingWithoutDuplicateOrStuckDispatch();
  void switchingToTextEntryMidHoldClearsTheHoldSoItsReleaseIsNotDispatched();
  void explicitOverrideSuspendsSemanticDispatchEvenWithoutTextEntryFocus();
  void destroyingTheWindowWhileTextEntrySuspendedNeverCrashesOrDispatches();

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
  // Reset router_ before mapper_: InputRouter borrows InputMapper by
  // reference, so if a prior test's teardown were ever skipped (e.g. an
  // early failure return), destroying mapper_ first while router_ still
  // held a reference to it would be a use-after-free in InputRouter's own
  // destructor/uninstall(). Mirrors the ordering already used below in
  // cleanup().
  router_.reset();
  mapper_.reset();
  focus_.reset();
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

void SemanticInputFixtureTests::
    realKeyEventsThroughTheInstalledRouterDriveFocusAndTheQmlFixtureEndToEnd() {
  // Unlike semanticFocusMovesTheQmlActiveFocusRectangle() and
  // shoulderZoneSwitchingMovesFocusBetweenZones() above (which call
  // focus_->moveFocus()/cycleZone() directly), this test drives real
  // QKeyEvents through the *installed* InputRouter -- exercising the
  // full router -> commandDispatched -> wireCommandsToFocus ->
  // FocusController -> QML activeFocus chain end-to-end, exactly as a
  // real keyboard or a Steam Deck's generic-controller/Steam Input
  // desktop template would drive it. It fails if wireCommandsToFocus()
  // is ever removed or has its command->direction switch cases inverted,
  // since focus_->currentFocusId() would then never change in response
  // to these real key events at all.
  window_ = loadFixture();
  QVERIFY(window_ != nullptr);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));

  const QVariantMap itemsById = window_->property("itemsById").toMap();
  auto *nw = itemsById.value(QStringLiteral("board.nw")).value<QQuickItem *>();
  auto *ne = itemsById.value(QStringLiteral("board.ne")).value<QQuickItem *>();
  auto *handCard1 =
      itemsById.value(QStringLiteral("hand.card1")).value<QQuickItem *>();
  QVERIFY(nw != nullptr);
  QVERIFY(ne != nullptr);
  QVERIFY(handCard1 != nullptr);
  QTRY_VERIFY(nw->hasActiveFocus());

  // Arrow-key directional focus: the same keys a generic controller's
  // D-pad/left-stick maps to under Steam Input's desktop template.
  QTest::keyClick(window_, Qt::Key_Right);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.ne"));
  QTRY_VERIFY(!nw->hasActiveFocus());
  QTRY_VERIFY(ne->hasActiveFocus());

  QTest::keyClick(window_, Qt::Key_Left);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));
  QTRY_VERIFY(nw->hasActiveFocus());
  QTRY_VERIFY(!ne->hasActiveFocus());

  // Shoulder-zone switching: Page Down/Page Up, matching the L1/R1
  // shoulder-button convention a generic controller/Steam Input desktop
  // template (and the Steam Deck's own shoulder buttons) map to.
  QTest::keyClick(window_, Qt::Key_PageDown);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("hand.card1"));
  QTRY_VERIFY(handCard1->hasActiveFocus());

  QTest::keyClick(window_, Qt::Key_PageUp);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));
  QTRY_VERIFY(nw->hasActiveFocus());
}

void SemanticInputFixtureTests::
    unboundKeyEventsThroughTheInstalledRouterNeverMoveFocus() {
  window_ = loadFixture();
  QVERIFY(window_ != nullptr);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));

  const QVariantMap itemsById = window_->property("itemsById").toMap();
  auto *nw = itemsById.value(QStringLiteral("board.nw")).value<QQuickItem *>();
  QVERIFY(nw != nullptr);
  QTRY_VERIFY(nw->hasActiveFocus());

  // Qt::Key_F13 has no default binding in InputMapper::resetToDefaults();
  // the router must let it propagate as an ordinary unconsumed key event
  // rather than the fixture (or anything else) reacting to it, and focus
  // must not move.
  QTest::keyClick(window_, Qt::Key_F13);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));
  QTRY_VERIFY(nw->hasActiveFocus());
}

void SemanticInputFixtureTests::
    tabAndBacktabNeverDivergeSemanticFocusFromRealQmlActiveFocus() {
  // Regression test: Tab/Backtab are not bound to any SemanticCommand,
  // so the router never consumes them and they reach Qt's own native
  // tab-focus-chain handling. Every semantic delegate in
  // qml/SemanticInputFixture.qml sets activeFocusOnTab: false precisely
  // so that native chain can never move real Qt activeFocus out from
  // under FocusController, which must remain the sole source of truth.
  // If activeFocusOnTab were ever reverted to true, Tab/Backtab would
  // move real active focus to a neighbouring delegate in the QML
  // Repeater's declaration order while focus_->currentFocusId() stayed
  // unchanged, so semantic focus and real active focus would visibly
  // diverge -- exactly what this test asserts never happens.
  window_ = loadFixture();
  QVERIFY(window_ != nullptr);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));

  const QVariantMap itemsById = window_->property("itemsById").toMap();
  auto *nw = itemsById.value(QStringLiteral("board.nw")).value<QQuickItem *>();
  QVERIFY(nw != nullptr);
  QTRY_VERIFY(nw->hasActiveFocus());

  QTest::keyClick(window_, Qt::Key_Tab);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));
  QTRY_VERIFY(nw->hasActiveFocus());

  QTest::keyClick(window_, Qt::Key_Backtab);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));
  QTRY_VERIFY(nw->hasActiveFocus());

  // A real semantic-command key still works normally afterwards, proving
  // Tab/Backtab left no lingering native-focus-chain state that would
  // interfere with ordinary semantic navigation.
  QTest::keyClick(window_, Qt::Key_Right);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.ne"));
  auto *ne = itemsById.value(QStringLiteral("board.ne")).value<QQuickItem *>();
  QVERIFY(ne != nullptr);
  QTRY_VERIFY(ne->hasActiveFocus());
}

void SemanticInputFixtureTests::
    textEntryFieldSuspendsSemanticDispatchAndHandlesKeysNatively() {
  // Regression test for the production text-entry gate: qml/
  // SemanticInputFixture.qml's textEntryField is a real QtQuick.Controls
  // TextField, wholly outside the semantic focus graph. While it holds
  // real Qt active focus, InputRouter's automatic Qt::ImEnabled
  // detection must suspend semantic dispatch entirely -- including the
  // reserved Escape key -- so every one of these default-bound keys
  // (see InputMapper::resetToDefaults()) reaches the field's own native
  // text/cursor/editing handling instead.
  window_ = loadFixture();
  QVERIFY(window_ != nullptr);
  auto *textField =
      window_->findChild<QQuickItem *>(QStringLiteral("textEntryField"));
  QVERIFY(textField != nullptr);

  QAccessibleInterface *iface =
      QAccessible::queryAccessibleInterface(textField);
  QVERIFY(iface != nullptr);
  QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("Notes"));

  QVERIFY(!router_->isTextEntrySuspended());
  textField->forceActiveFocus();
  QTRY_VERIFY(textField->hasActiveFocus());
  QVERIFY(router_->isTextEntrySuspended());

  QSignalSpy spy(router_.get(), &InputRouter::commandDispatched);

  // W/A/S/D are FocusUp/FocusLeft/FocusDown/FocusRight by default; here
  // they must simply type letters into the field.
  QTest::keyClick(window_, Qt::Key_W);
  QTest::keyClick(window_, Qt::Key_A);
  QTest::keyClick(window_, Qt::Key_S);
  QTest::keyClick(window_, Qt::Key_D);
  QCOMPARE(textField->property("text").toString(), QStringLiteral("wasd"));

  // Space is PrimaryAction by default; here it must type a space.
  QTest::keyClick(window_, Qt::Key_Space);
  QCOMPARE(textField->property("text").toString(), QStringLiteral("wasd "));

  // Backspace has no semantic binding, but is exactly the kind of
  // ordinary editing key the gate exists to protect: it must reach the
  // field's native handling.
  QTest::keyClick(window_, Qt::Key_Backspace);
  QCOMPARE(textField->property("text").toString(), QStringLiteral("wasd"));

  // Arrow keys are FocusRight/FocusLeft by default; here they must move
  // the text cursor, not semantic focus, and must not alter the text.
  QTest::keyClick(window_, Qt::Key_Left);
  QTest::keyClick(window_, Qt::Key_Left);
  QTest::keyClick(window_, Qt::Key_Right);
  QCOMPARE(textField->property("text").toString(), QStringLiteral("wasd"));

  // Return is PrimaryAction by default; here it must be left to the
  // field (TextField has no default Enter side effect beyond its own
  // accepted() signal), never a semantic dispatch.
  QTest::keyClick(window_, Qt::Key_Return);
  QCOMPARE(textField->property("text").toString(), QStringLiteral("wasd"));

  // Escape/Back is reserved and normally always consumed -- but per the
  // documented text-entry policy, reserved keys are NOT exempted from
  // suspension: while a text control owns focus, Escape must pass
  // through unconsumed rather than being interpreted as
  // SecondaryOwnAction, and must not delete the field's text or dismiss
  // anything on this router's behalf.
  QTest::keyClick(window_, Qt::Key_Escape);
  QCOMPARE(textField->property("text").toString(), QStringLiteral("wasd"));

  QCOMPARE(spy.count(), 0);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));
}

void SemanticInputFixtureTests::
    leavingTextEntryResumesSemanticMappingWithoutDuplicateOrStuckDispatch() {
  window_ = loadFixture();
  QVERIFY(window_ != nullptr);
  auto *textField =
      window_->findChild<QQuickItem *>(QStringLiteral("textEntryField"));
  QVERIFY(textField != nullptr);
  const QVariantMap itemsById = window_->property("itemsById").toMap();
  auto *nw = itemsById.value(QStringLiteral("board.nw")).value<QQuickItem *>();
  auto *ne = itemsById.value(QStringLiteral("board.ne")).value<QQuickItem *>();
  QVERIFY(nw != nullptr);
  QVERIFY(ne != nullptr);
  QTRY_VERIFY(nw->hasActiveFocus());

  textField->forceActiveFocus();
  QTRY_VERIFY(textField->hasActiveFocus());
  QVERIFY(router_->isTextEntrySuspended());

  QSignalSpy spy(router_.get(), &InputRouter::commandDispatched);
  QTest::keyClick(window_, Qt::Key_Right);
  QCOMPARE(spy.count(), 0);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));

  // Simulates a host returning real Qt active focus to a semantic
  // delegate (e.g. the user clicked a board tile): automatic detection
  // must notice this with no explicit setSemanticInputSuspended() call
  // at all.
  nw->forceActiveFocus();
  QTRY_VERIFY(nw->hasActiveFocus());
  QVERIFY(!router_->isTextEntrySuspended());

  QTest::keyClick(window_, Qt::Key_Right);
  // A full keyClick dispatches twice -- one Pressed, one Released, per
  // CommandPhase's documented press/release contract (see
  // InputMapper.h) -- not once; this is the same convention every
  // existing InputRouterTests keyClick assertion already follows.
  QCOMPARE(spy.count(), 2);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.ne"));
  QTRY_VERIFY(ne->hasActiveFocus());

  // A further key click dispatches exactly twice more -- proving nothing
  // about the earlier suppressed Right press was queued or replayed
  // once suspension lifted.
  QTest::keyClick(window_, Qt::Key_Left);
  QCOMPARE(spy.count(), 4);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));
}

void SemanticInputFixtureTests::
    switchingToTextEntryMidHoldClearsTheHoldSoItsReleaseIsNotDispatched() {
  window_ = loadFixture();
  QVERIFY(window_ != nullptr);
  auto *textField =
      window_->findChild<QQuickItem *>(QStringLiteral("textEntryField"));
  QVERIFY(textField != nullptr);
  const QVariantMap itemsById = window_->property("itemsById").toMap();
  auto *nw = itemsById.value(QStringLiteral("board.nw")).value<QQuickItem *>();
  auto *ne = itemsById.value(QStringLiteral("board.ne")).value<QQuickItem *>();
  QVERIFY(nw != nullptr);
  QVERIFY(ne != nullptr);
  QTRY_VERIFY(nw->hasActiveFocus());

  QSignalSpy spy(router_.get(), &InputRouter::commandDispatched);

  QTest::keyPress(window_, Qt::Key_Right);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.constLast().at(1).value<CommandPhase>(), CommandPhase::Pressed);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.ne"));

  // Switch real Qt active focus to the text field while Right is still
  // physically held (e.g. a pointer click into the field mid-hold): the
  // hold must be forgotten immediately, not merely ignored for future
  // presses.
  textField->forceActiveFocus();
  QTRY_VERIFY(textField->hasActiveFocus());
  QVERIFY(router_->isTextEntrySuspended());

  QTest::keyRelease(window_, Qt::Key_Right);
  QCOMPARE(spy.count(), 1); // No Released dispatched: the hold was cleared.

  // Returning to a semantic delegate and pressing Right fresh must work
  // exactly like any ordinary press -- proving no stale armed/held state
  // survived the suspend transition to swallow it.
  ne->forceActiveFocus();
  QTRY_VERIFY(ne->hasActiveFocus());
  QVERIFY(!router_->isTextEntrySuspended());

  QTest::keyClick(window_, Qt::Key_Right);
  // A full keyClick after the hold was cleared is an ordinary fresh
  // press+release, dispatching twice (Pressed, then Released) -- see
  // the comment on the earlier keyClick in
  // leavingTextEntryResumesSemanticMappingWithoutDuplicateOrStuckDispatch.
  QCOMPARE(spy.count(), 3);
}

void SemanticInputFixtureTests::
    explicitOverrideSuspendsSemanticDispatchEvenWithoutTextEntryFocus() {
  window_ = loadFixture();
  QVERIFY(window_ != nullptr);
  const QVariantMap itemsById = window_->property("itemsById").toMap();
  auto *nw = itemsById.value(QStringLiteral("board.nw")).value<QQuickItem *>();
  QVERIFY(nw != nullptr);
  QTRY_VERIFY(nw->hasActiveFocus());
  QVERIFY(!router_->isTextEntrySuspended());

  QSignalSpy spy(router_.get(), &InputRouter::commandDispatched);

  // No text control has real Qt focus at all here -- automatic detection
  // alone would report "not suspended" -- yet an explicit host override
  // must still suspend everything, exactly like real text entry would.
  router_->setSemanticInputSuspended(true);
  QVERIFY(router_->isTextEntrySuspended());
  QTest::keyClick(window_, Qt::Key_Right);
  QCOMPARE(spy.count(), 0);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.nw"));

  router_->setSemanticInputSuspended(false);
  QVERIFY(!router_->isTextEntrySuspended());
  QTest::keyClick(window_, Qt::Key_Right);
  // Ordinary keyClick dispatches twice (Pressed, then Released) -- see
  // the comment on the earlier keyClick in
  // leavingTextEntryResumesSemanticMappingWithoutDuplicateOrStuckDispatch.
  QCOMPARE(spy.count(), 2);
  QCOMPARE(focus_->currentFocusId(), QStringLiteral("board.ne"));
}

void SemanticInputFixtureTests::
    destroyingTheWindowWhileTextEntrySuspendedNeverCrashesOrDispatches() {
  window_ = loadFixture();
  QVERIFY(window_ != nullptr);
  auto *textField =
      window_->findChild<QQuickItem *>(QStringLiteral("textEntryField"));
  QVERIFY(textField != nullptr);

  textField->forceActiveFocus();
  QTRY_VERIFY(textField->hasActiveFocus());
  QVERIFY(router_->isTextEntrySuspended());

  QSignalSpy spy(router_.get(), &InputRouter::commandDispatched);
  QVERIFY(router_->isInstalled());

  // Destroys the QML root window/textEntryField entirely while text
  // entry is suspended -- exercising exactly the same
  // targetDestroyed-while-installed seam as
  // routerNoticesWhenItsTargetIsDestroyedExternally() in
  // InputRouterTests, but with the suspension gate active. Not crashing
  // is itself part of what this test asserts.
  engine_.reset();
  QVERIFY(!router_->isInstalled());
  QCOMPARE(spy.count(), 0);
  window_ = nullptr;
}

QTEST_MAIN(SemanticInputFixtureTests)

#include "SemanticInputFixtureTests.moc"
