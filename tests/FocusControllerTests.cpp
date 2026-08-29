#include <QSignalSpy>
#include <QtTest>

#include "FocusController.h"

using Arkham::FocusController;
using Arkham::FocusDirection;
using Arkham::FocusNodeSpec;
using Arkham::FocusSnapshot;
using Arkham::WrapPolicy;

namespace {

// A small 2x2 "board" zone with full four-directional adjacency between
// adjacent cells (diagonals are never adjacent), used by most tests
// below.
void registerBoardZone(FocusController &controller) {
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("board.nw"),
                    QStringLiteral("board"),
                    {{FocusDirection::Right, QStringLiteral("board.ne")},
                     {FocusDirection::Down, QStringLiteral("board.sw")}}});
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("board.ne"),
                    QStringLiteral("board"),
                    {{FocusDirection::Left, QStringLiteral("board.nw")},
                     {FocusDirection::Down, QStringLiteral("board.se")}}});
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("board.sw"),
                    QStringLiteral("board"),
                    {{FocusDirection::Up, QStringLiteral("board.nw")},
                     {FocusDirection::Right, QStringLiteral("board.se")}}});
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("board.se"),
                    QStringLiteral("board"),
                    {{FocusDirection::Up, QStringLiteral("board.ne")},
                     {FocusDirection::Left, QStringLiteral("board.sw")}}});
}

} // namespace

class FocusControllerTests final : public QObject {
  Q_OBJECT

private slots:
  void movesFocusAlongExplicitAdjacency();
  void isNoOpWhenNoExplicitNeighborAndNoWrap();
  void wrapsWithinZoneWhenConfigured();
  void reregisteringANodeIsATieResolvedByLastWriteWins();
  void cyclesZonesInRegistrationOrderAndRemembersLastFocused();
  void cycleZoneIsNoOpWithFewerThanTwoPopulatedZones();
  void modalPushAndPopReturnsToExactPriorFocus();
  void nestedModalsUnwindInReverseOrder();
  void removingCurrentlyFocusedNodeFallsBackToExplicitFallback();
  void removingCurrentlyFocusedNodeFallsBackToNeighborThenZone();
  void removingCurrentlyFocusedNodeWithNoFallbackClearsFocus();
  void removalAlsoFixesUpModalReturnTargets();
  void snapshotRestorationIsDeterministicAndRepeatable();
  void geometryFallbackIsOptInAndNeverUsedByDefault();
  void currentFocusChangedFiresOnlyOnActualChange();
};

void FocusControllerTests::movesFocusAlongExplicitAdjacency() {
  FocusController controller;
  registerBoardZone(controller);
  QVERIFY(controller.setInitialFocus(QStringLiteral("board.nw")));

  QVERIFY(controller.moveFocus(FocusDirection::Right));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.ne"));

  QVERIFY(controller.moveFocus(FocusDirection::Down));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.se"));

  QVERIFY(controller.moveFocus(FocusDirection::Left));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.sw"));

  QVERIFY(controller.moveFocus(FocusDirection::Up));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));
}

void FocusControllerTests::isNoOpWhenNoExplicitNeighborAndNoWrap() {
  FocusController controller(WrapPolicy::NoWrap);
  registerBoardZone(controller);
  controller.setInitialFocus(QStringLiteral("board.nw"));

  // board.nw has no explicit Up or Left neighbor.
  QVERIFY(!controller.moveFocus(FocusDirection::Up));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));
  QVERIFY(!controller.moveFocus(FocusDirection::Left));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));
}

void FocusControllerTests::wrapsWithinZoneWhenConfigured() {
  FocusController controller(WrapPolicy::WrapWithinZone);

  // A simple 3-card row zone with only Right adjacency explicitly wired
  // forward (card1->card2->card3); Left is deliberately left unset on
  // card1 and Right unset on card3 so wrap is the only way past either
  // edge.
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card1"),
                    QStringLiteral("hand"),
                    {{FocusDirection::Right, QStringLiteral("hand.card2")}}});
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card2"),
                    QStringLiteral("hand"),
                    {{FocusDirection::Left, QStringLiteral("hand.card1")},
                     {FocusDirection::Right, QStringLiteral("hand.card3")}}});
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card3"),
                    QStringLiteral("hand"),
                    {{FocusDirection::Left, QStringLiteral("hand.card2")}}});

  controller.setInitialFocus(QStringLiteral("hand.card3"));
  QVERIFY(controller.moveFocus(FocusDirection::Right));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));

  QVERIFY(controller.moveFocus(FocusDirection::Left));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card3"));
}

void FocusControllerTests::reregisteringANodeIsATieResolvedByLastWriteWins() {
  FocusController controller;
  registerBoardZone(controller);
  controller.setInitialFocus(QStringLiteral("board.nw"));

  // Re-register board.nw with a conflicting/different Right neighbor:
  // the LATEST registration must win outright, never merge with the
  // original adjacency.
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("board.nw"),
                    QStringLiteral("board"),
                    {{FocusDirection::Right, QStringLiteral("board.se")}}});

  QVERIFY(controller.moveFocus(FocusDirection::Right));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.se"));
  // The old Down neighbor from the original registration must be gone.
  QVERIFY(!controller.moveFocus(FocusDirection::Down));
}

void FocusControllerTests::
    cyclesZonesInRegistrationOrderAndRemembersLastFocused() {
  FocusController controller;
  registerBoardZone(controller); // zone "board", registered first
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card1"), QStringLiteral("hand"), {}});
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("log.entry"), QStringLiteral("log"), {}});

  controller.setInitialFocus(QStringLiteral("board.se"));
  QVERIFY(controller.cycleZone(true));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));
  QVERIFY(controller.cycleZone(true));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("log.entry"));
  QVERIFY(controller.cycleZone(true));
  // Wraps back around to "board", remembering the last-focused node
  // there (board.se), not just the zone's first node (board.nw).
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.se"));

  QVERIFY(controller.cycleZone(false));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("log.entry"));
}

void FocusControllerTests::cycleZoneIsNoOpWithFewerThanTwoPopulatedZones() {
  FocusController controller;
  registerBoardZone(controller);
  controller.setInitialFocus(QStringLiteral("board.nw"));

  QVERIFY(!controller.cycleZone(true));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));
}

void FocusControllerTests::modalPushAndPopReturnsToExactPriorFocus() {
  FocusController controller;
  registerBoardZone(controller);
  controller.registerNode(FocusNodeSpec{
      QStringLiteral("prompt.confirm"), QStringLiteral("prompt"), {}});
  controller.setInitialFocus(QStringLiteral("board.se"));

  QVERIFY(controller.pushModal(QStringLiteral("prompt.confirm")));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("prompt.confirm"));
  QVERIFY(controller.isModalActive());

  QVERIFY(controller.popModal());
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.se"));
  QVERIFY(!controller.isModalActive());
}

void FocusControllerTests::nestedModalsUnwindInReverseOrder() {
  FocusController controller;
  registerBoardZone(controller);
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("modal.a"), QStringLiteral("modal"), {}});
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("modal.b"), QStringLiteral("modal"), {}});
  controller.setInitialFocus(QStringLiteral("board.nw"));

  controller.pushModal(QStringLiteral("modal.a"));
  controller.pushModal(QStringLiteral("modal.b"));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("modal.b"));

  QVERIFY(controller.popModal());
  QCOMPARE(controller.currentFocusId(), QStringLiteral("modal.a"));
  QVERIFY(controller.isModalActive());

  QVERIFY(controller.popModal());
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));
  QVERIFY(!controller.isModalActive());

  QVERIFY(!controller.popModal());
}

void FocusControllerTests::
    removingCurrentlyFocusedNodeFallsBackToExplicitFallback() {
  FocusController controller;
  registerBoardZone(controller);
  controller.setInitialFocus(QStringLiteral("board.se"));

  controller.removeNode(QStringLiteral("board.se"), QStringLiteral("board.nw"));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));
  QVERIFY(!controller.hasNode(QStringLiteral("board.se")));
}

void FocusControllerTests::
    removingCurrentlyFocusedNodeFallsBackToNeighborThenZone() {
  {
    FocusController controller;
    registerBoardZone(controller);
    controller.setInitialFocus(QStringLiteral("board.se"));
    // No explicit fallback given: falls back to an explicit neighbor,
    // trying Up/Down/Left/Right in that fixed priority order. board.se's
    // only neighbors are Up=board.ne and Left=board.sw, so Up wins.
    controller.removeNode(QStringLiteral("board.se"));
    QCOMPARE(controller.currentFocusId(), QStringLiteral("board.ne"));
  }
  {
    FocusController controller;
    controller.registerNode(FocusNodeSpec{
        QStringLiteral("isolated.a"), QStringLiteral("isolated"), {}});
    controller.registerNode(FocusNodeSpec{
        QStringLiteral("isolated.b"), QStringLiteral("isolated"), {}});
    controller.setInitialFocus(QStringLiteral("isolated.a"));
    // No explicit fallback and no explicit neighbors at all: falls back
    // to the first remaining node in the same zone by registration
    // order.
    controller.removeNode(QStringLiteral("isolated.a"));
    QCOMPARE(controller.currentFocusId(), QStringLiteral("isolated.b"));
  }
}

void FocusControllerTests::
    removingCurrentlyFocusedNodeWithNoFallbackClearsFocus() {
  FocusController controller;
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("only.node"), QStringLiteral("only"), {}});
  controller.setInitialFocus(QStringLiteral("only.node"));

  controller.removeNode(QStringLiteral("only.node"));
  QVERIFY(controller.currentFocusId().isEmpty());
}

void FocusControllerTests::removalAlsoFixesUpModalReturnTargets() {
  FocusController controller;
  registerBoardZone(controller);
  controller.registerNode(FocusNodeSpec{
      QStringLiteral("prompt.confirm"), QStringLiteral("prompt"), {}});
  controller.setInitialFocus(QStringLiteral("board.se"));
  controller.pushModal(QStringLiteral("prompt.confirm"));

  // The node the modal would have returned to (board.se) is removed
  // while the modal is still active.
  controller.removeNode(QStringLiteral("board.se"));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("prompt.confirm"));

  QVERIFY(controller.popModal());
  // Falls back the same deterministic way removeNode's own current-focus
  // fallback would have: board.se's Up neighbor, board.ne.
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.ne"));
}

void FocusControllerTests::snapshotRestorationIsDeterministicAndRepeatable() {
  FocusController controller;
  registerBoardZone(controller);
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card1"), QStringLiteral("hand"), {}});
  controller.setInitialFocus(QStringLiteral("board.se"));
  controller.cycleZone(true); // now focused on hand.card1; board.se remembered

  const FocusSnapshot snap = controller.snapshot();
  QCOMPARE(snap.focusedId, QStringLiteral("hand.card1"));

  // Mutate the graph, restore, and confirm the restored state is exactly
  // what was captured.
  controller.setInitialFocus(QStringLiteral("board.nw"));
  controller.restoreSnapshot(snap);
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));
  QVERIFY(controller.cycleZone(false));
  // zoneLastFocused for "board" must also have been restored to board.se.
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.se"));

  // Restoring the SAME snapshot a second time, after further mutation,
  // must produce the exact same result again -- no drift.
  controller.moveFocus(FocusDirection::Up);
  controller.restoreSnapshot(snap);
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));

  // If the previously-focused node has since been removed, restoration
  // falls back deterministically rather than restoring a dangling id.
  controller.removeNode(QStringLiteral("hand.card1"));
  controller.restoreSnapshot(snap);
  QVERIFY(controller.currentFocusId() != QStringLiteral("hand.card1"));
  QVERIFY(controller.hasNode(controller.currentFocusId()));
}

void FocusControllerTests::geometryFallbackIsOptInAndNeverUsedByDefault() {
  FocusController controller(WrapPolicy::NoWrap);
  registerBoardZone(controller);
  controller.setInitialFocus(QStringLiteral("board.nw"));

  bool fallbackCalled = false;
  controller.setGeometryFallback(
      [&fallbackCalled](const QString &, FocusDirection,
                        const QVector<QString> &) -> std::optional<QString> {
        fallbackCalled = true;
        return std::nullopt;
      });

  // board.nw HAS an explicit Right neighbor, so the fallback must never
  // even be consulted.
  QVERIFY(controller.moveFocus(FocusDirection::Right));
  QVERIFY(!fallbackCalled);

  // board.ne (now current) has no explicit Right neighbor: only now
  // should the deliberately-installed fallback be consulted.
  QVERIFY(!controller.moveFocus(FocusDirection::Right));
  QVERIFY(fallbackCalled);
}

void FocusControllerTests::currentFocusChangedFiresOnlyOnActualChange() {
  FocusController controller;
  registerBoardZone(controller);
  controller.setInitialFocus(QStringLiteral("board.nw"));

  QSignalSpy spy(&controller, &FocusController::currentFocusChanged);
  // No explicit Up neighbor and no wrap configured: this must be a
  // complete no-op, including no signal emission.
  QVERIFY(!controller.moveFocus(FocusDirection::Up));
  QCOMPARE(spy.count(), 0);

  QVERIFY(controller.moveFocus(FocusDirection::Right));
  QCOMPARE(spy.count(), 1);
}

QTEST_APPLESS_MAIN(FocusControllerTests)

#include "FocusControllerTests.moc"
