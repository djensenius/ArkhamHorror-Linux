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
  void cycleZoneWithNoCurrentFocusLandsOnARealFirstOrLastZone();
  void modalPushAndPopReturnsToExactPriorFocus();
  void nestedModalsUnwindInReverseOrder();
  void popModalPreservesAnIntentionallyEmptyReturnTarget();
  void restoreSnapshotPreservesAnIntentionallyEmptyModalReturnTarget();
  void removingCurrentlyFocusedNodeFallsBackToExplicitFallback();
  void removingCurrentlyFocusedNodeFallsBackToNeighborThenZone();
  void removingCurrentlyFocusedNodeWithNoFallbackClearsFocus();
  void removalAlsoFixesUpModalReturnTargets();
  void removingAModalsOwnEntryNodeStillUnwindsCorrectlyOnPop();
  void snapshotRestorationIsDeterministicAndRepeatable();
  void geometryFallbackIsOptInAndNeverUsedByDefault();
  void currentFocusChangedFiresOnlyOnActualChange();
  void reentrantSignalEmissionSeesEachEmissionsOwnValueNotTheFinalMutatedOne();
  void
  restoreSnapshotAlwaysWritesZoneMemoryForTheTargetEvenWhenAlreadyFocused();
  void
  removedEmptyZoneIsPrunedFromCycleOrderAndReappearsAtTheEndIfReintroduced();
  void reregisteringANodeIntoADifferentZonePrunesTheOldZoneOnceItIsEmpty();
  void pruningAnEmptyZoneAlsoForgetsItsLastFocusedMemory();
  void reregisteringANodeIntoADifferentNonEmptyZoneForgetsItsStaleZoneMemory();
  void
  reregisteringTheCurrentlyFocusedNodeIntoADifferentZoneUpdatesThatZonesMemory();
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

void FocusControllerTests::
    cycleZoneWithNoCurrentFocusLandsOnARealFirstOrLastZone() {
  // Regression test: with no current focus at all, cycleZone(true) must
  // land on the very first registered zone's node (never skip over it
  // to the second zone), and cycleZone(false) must land on the very
  // last registered zone's node.
  FocusController forwardController;
  registerBoardZone(forwardController); // zone "board", registered first
  forwardController.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card1"), QStringLiteral("hand"), {}});
  QVERIFY(forwardController.currentFocusId().isEmpty());

  QVERIFY(forwardController.cycleZone(true));
  QCOMPARE(forwardController.currentFocusId(), QStringLiteral("board.nw"));

  FocusController backwardController;
  registerBoardZone(backwardController);
  backwardController.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card1"), QStringLiteral("hand"), {}});
  QVERIFY(backwardController.currentFocusId().isEmpty());

  QVERIFY(backwardController.cycleZone(false));
  QCOMPARE(backwardController.currentFocusId(), QStringLiteral("hand.card1"));
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

void FocusControllerTests::popModalPreservesAnIntentionallyEmptyReturnTarget() {
  FocusController controller;
  registerBoardZone(controller);
  controller.registerNode(FocusNodeSpec{
      QStringLiteral("prompt.confirm"), QStringLiteral("prompt"), {}});
  // No setInitialFocus(): currentFocusId() starts empty, so pushModal()
  // below records an intentionally-empty return target -- there was
  // never any "prior focus" to return to.
  QCOMPARE(controller.currentFocusId(), QString());

  QVERIFY(controller.pushModal(QStringLiteral("prompt.confirm")));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("prompt.confirm"));

  // popModal() must restore the exact prior (empty) state, not silently
  // pick an arbitrary node just because an empty return target also
  // fails a "does this node still exist" check.
  QVERIFY(controller.popModal());
  QCOMPARE(controller.currentFocusId(), QString());
  QVERIFY(!controller.isModalActive());
}

void FocusControllerTests::
    restoreSnapshotPreservesAnIntentionallyEmptyModalReturnTarget() {
  FocusController controller;
  registerBoardZone(controller);
  controller.registerNode(FocusNodeSpec{
      QStringLiteral("prompt.confirm"), QStringLiteral("prompt"), {}});
  QVERIFY(controller.pushModal(QStringLiteral("prompt.confirm")));

  FocusSnapshot snap = controller.snapshot();
  QCOMPARE(snap.modalStack.size(), 1);
  QCOMPARE(snap.modalStack.first().second, QString());

  // Restoring into a controller that currently has real focus set must
  // still reproduce the snapshot's own intentionally-empty return
  // target for the modal level -- restoreSnapshot() must not invent a
  // fallback node for an empty (not merely invalid) return target.
  FocusController other;
  registerBoardZone(other);
  other.registerNode(FocusNodeSpec{
      QStringLiteral("prompt.confirm"), QStringLiteral("prompt"), {}});
  other.setInitialFocus(QStringLiteral("board.se"));

  other.restoreSnapshot(snap);
  QCOMPARE(other.currentFocusId(), QStringLiteral("prompt.confirm"));
  QVERIFY(other.popModal());
  QCOMPARE(other.currentFocusId(), QString());
}

void FocusControllerTests::
    reregisteringANodeIntoADifferentNonEmptyZoneForgetsItsStaleZoneMemory() {
  FocusController controller;
  registerBoardZone(controller);
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card1"), QStringLiteral("hand"), {}});
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card2"), QStringLiteral("hand"), {}});
  controller.setInitialFocus(QStringLiteral("board.nw"));

  // Visiting "hand" remembers hand.card1 (registered first) as its
  // last-focused node.
  QVERIFY(controller.cycleZone(true));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));
  QVERIFY(controller.cycleZone(false));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));

  // Re-register hand.card1 into a different zone. "hand" still has
  // hand.card2, so it is NOT pruned from zoneOrder_ -- this must not be
  // confused with the reregisteringANodeIntoADifferentZonePrunesTheOld
  // ZoneOnceItIsEmpty test above, which covers the fully-emptied case.
  // The snapshot must no longer claim hand.card1 (which has moved away)
  // as "hand"'s last-focused node.
  controller.registerNode(FocusNodeSpec{
      QStringLiteral("hand.card1"), QStringLiteral("archive"), {}});
  const FocusSnapshot snap = controller.snapshot();
  QVERIFY(snap.zoneLastFocused.contains(QStringLiteral("hand")) == false ||
          snap.zoneLastFocused.value(QStringLiteral("hand")) !=
              QStringLiteral("hand.card1"));

  // Cycling back into "hand" must land on the real remaining node
  // (hand.card2), never on the now-departed hand.card1.
  QVERIFY(controller.cycleZone(true));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card2"));
}

void FocusControllerTests::
    reregisteringTheCurrentlyFocusedNodeIntoADifferentZoneUpdatesThatZonesMemory() {
  // Regression test: re-registering the node that is *currently
  // focused* into a different zone must populate zoneLastFocused_ for
  // its new zone -- setCurrentFocus()'s own zoneLastFocused_ bookkeeping
  // never runs here, since currentFocusId_ itself does not change (only
  // the node's zoneId does), so registerNode() must do this update
  // itself or the new zone's memory would stay stale/absent even though
  // the live current focus genuinely belongs to it now.
  FocusController controller;
  registerBoardZone(controller);
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card1"), QStringLiteral("hand"), {}});
  controller.setInitialFocus(QStringLiteral("board.nw"));

  // Focus a node, then re-register that same, still-focused node into a
  // brand-new zone it was never a member of before.
  QVERIFY(controller.cycleZone(true));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));
  controller.registerNode(FocusNodeSpec{
      QStringLiteral("hand.card1"), QStringLiteral("archive"), {}});
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));

  // The new zone's memory must already reflect the currently-focused
  // node, deterministically, with no further focus change required.
  const FocusSnapshot snap = controller.snapshot();
  QVERIFY(snap.zoneLastFocused.contains(QStringLiteral("archive")));
  QCOMPARE(snap.zoneLastFocused.value(QStringLiteral("archive")),
           QStringLiteral("hand.card1"));

  // Cycling away and back into "archive" must land on hand.card1 using
  // that freshly-recorded memory, not some stale/absent entry.
  QVERIFY(controller.cycleZone(false));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));
  QVERIFY(controller.cycleZone(false));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));
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

void FocusControllerTests::
    removingAModalsOwnEntryNodeStillUnwindsCorrectlyOnPop() {
  // Removing the node a modal level is itself currently focused on (not
  // just its return target, covered above) must not corrupt later
  // popModal() unwinding: the return target captured at push time is
  // untouched and still used, one level at a time, regardless of
  // whether that level's own entry node still exists.
  FocusController controller;
  registerBoardZone(controller);
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("modal.a"), QStringLiteral("modal-a"), {}});
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("modal.b"), QStringLiteral("modal-b"), {}});
  controller.setInitialFocus(QStringLiteral("board.nw"));

  controller.pushModal(QStringLiteral("modal.a"));
  controller.pushModal(QStringLiteral("modal.b"));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("modal.b"));

  // Remove the innermost modal's own currently-focused entry node.
  controller.removeNode(QStringLiteral("modal.b"));
  QVERIFY(controller.isModalActive());
  // modal.b has no explicit fallback, no neighbors, and no other node
  // in its own zone, so focus falls all the way to unfocused; this
  // deliberately does *not* auto-pop the modal.
  QVERIFY(controller.currentFocusId().isEmpty());

  // A single popModal() must still unwind exactly one level, returning
  // to modal.a (modal.b's captured return target) -- not skipping past
  // it to board.nw -- even though modal.b's own node is gone.
  QVERIFY(controller.popModal());
  QCOMPARE(controller.currentFocusId(), QStringLiteral("modal.a"));
  QVERIFY(controller.isModalActive());

  QVERIFY(controller.popModal());
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));
  QVERIFY(!controller.isModalActive());
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

void FocusControllerTests::
    reentrantSignalEmissionSeesEachEmissionsOwnValueNotTheFinalMutatedOne() {
  // A direct-connected slot that reenters (calls moveFocus() again
  // before the outer emission's later-connected slots have run) must
  // never make those later slots observe the already-mutated final
  // value: currentFocusChanged() must carry each emission's own value,
  // never a live reference to the mutable member.
  FocusController controller;
  registerBoardZone(controller);
  controller.setInitialFocus(QStringLiteral("board.nw"));

  bool reentered = false;
  QVector<QString> secondObserverSeen;
  // Connected first: reenters exactly once, on the outer emission for
  // "board.ne", by moving focus again (triggering a fully-nested emit
  // for "board.se") before returning.
  QObject::connect(&controller, &FocusController::currentFocusChanged,
                   &controller, [&](const QString &id) {
                     if (!reentered && id == QStringLiteral("board.ne")) {
                       reentered = true;
                       QVERIFY(controller.moveFocus(FocusDirection::Down));
                     }
                   });
  // Connected second: for the OUTER emission, this must still run *after*
  // the nested emission has fully completed (Qt direct connections invoke
  // slots in connection order within a single activate() call), and must
  // see the value that was current when the *outer* emission started
  // ("board.ne"), not the value the nested reentrant call already moved
  // on to ("board.se").
  QObject::connect(
      &controller, &FocusController::currentFocusChanged, &controller,
      [&](const QString &id) { secondObserverSeen.push_back(id); });

  QVERIFY(controller.moveFocus(FocusDirection::Right));

  QCOMPARE(secondObserverSeen, (QVector<QString>{QStringLiteral("board.se"),
                                                 QStringLiteral("board.ne")}));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.se"));
}

void FocusControllerTests::
    restoreSnapshotAlwaysWritesZoneMemoryForTheTargetEvenWhenAlreadyFocused() {
  FocusController controller;
  // Registration order matters here: hand.card1 is registered first (and
  // is later removed), so firstRegisteredNodeId()'s fallback lands on
  // board.nw once hand.card1 no longer exists.
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card1"), QStringLiteral("hand"), {}});
  registerBoardZone(controller);
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("log.entry"), QStringLiteral("log"), {}});

  controller.setInitialFocus(QStringLiteral("board.se"));
  // board -> hand, remembering board.se as board's last-focused node and
  // hand.card1 as hand's.
  QVERIFY(controller.cycleZone(false));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));

  const FocusSnapshot snap = controller.snapshot();
  QCOMPARE(snap.focusedId, QStringLiteral("hand.card1"));
  QCOMPARE(snap.zoneLastFocused.value(QStringLiteral("board")),
           QStringLiteral("board.se"));

  // Remove the snapshot's focused node entirely (hand becomes empty and
  // is pruned from zoneOrder_ -- see the zone-pruning test -- which is
  // irrelevant to this bug but incidental to this exact setup).
  controller.removeNode(QStringLiteral("hand.card1"));

  // Set up the exact coincidence the bug depends on: manually put focus
  // on precisely the id restoreSnapshot's fallback logic is about to
  // land on (board.nw, since hand.card1 -- registration order 0 -- no
  // longer exists, the next-lowest surviving registration order is
  // board.nw), *before* calling restoreSnapshot.
  QVERIFY(controller.setInitialFocus(QStringLiteral("board.nw")));

  controller.restoreSnapshot(snap);
  // The fallback target is exactly what was already live, so
  // setCurrentFocus()'s own "no actual change" guard fires internally --
  // but restoreSnapshot() must still have deterministically written
  // board's zone memory to reflect the *actual new* focus (board.nw),
  // not left it as whatever the snapshot's rebuild loop restored from the
  // old recorded memory (board.se).
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));

  // Cycle away from "board" and back: this must land on board.nw (the
  // correctly-updated zone memory), never on the stale board.se from the
  // snapshot's own zoneLastFocused (which would prove the update was
  // skipped).
  QVERIFY(controller.cycleZone(true)); // board -> log
  QCOMPARE(controller.currentFocusId(), QStringLiteral("log.entry"));
  QVERIFY(controller.cycleZone(false)); // log -> board
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));

  // Repeated restoration of the exact same snapshot, from this new live
  // state (which now again coincidentally matches the fallback target),
  // must still produce the exact same deterministic result -- no
  // history-dependent drift across repeated restorations.
  controller.restoreSnapshot(snap);
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));
  QVERIFY(controller.cycleZone(true));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("log.entry"));
  QVERIFY(controller.cycleZone(false));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));
}

void FocusControllerTests::
    removedEmptyZoneIsPrunedFromCycleOrderAndReappearsAtTheEndIfReintroduced() {
  FocusController controller;
  registerBoardZone(controller);
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card1"), QStringLiteral("hand"), {}});
  controller.setInitialFocus(QStringLiteral("board.nw"));

  // zoneOrder_ so far: [board, hand].
  QVERIFY(controller.cycleZone(true));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));
  QVERIFY(controller.cycleZone(true));
  // Only two zones exist, so cycling forward again wraps back to board.
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));

  // Removing hand's only node empties the zone entirely: it must be
  // pruned from the cycle order (not merely skipped), so with only one
  // populated zone left, cycling is a pure no-op.
  controller.removeNode(QStringLiteral("hand.card1"));
  QVERIFY(!controller.cycleZone(true));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));

  // Introduce a third zone, then re-register a node under the *same*
  // "hand" zone id: it must be treated as a brand-new zone appended at
  // the current end of the cycle order (after "log"), not reinserted at
  // its original position (which was right after "board").
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("log.entry"), QStringLiteral("log"), {}});
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card2"), QStringLiteral("hand"), {}});

  QVERIFY(controller.cycleZone(true)); // board -> log
  QCOMPARE(controller.currentFocusId(), QStringLiteral("log.entry"));
  QVERIFY(controller.cycleZone(true)); // log -> hand
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card2"));
  QVERIFY(controller.cycleZone(true)); // hand -> board (wraps)
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));

  // A repeated remove/re-add cycle behaves exactly the same way each
  // time: still pruned once empty, still appended at the (new) end when
  // reintroduced again.
  controller.removeNode(QStringLiteral("hand.card2"));
  QVERIFY(controller.cycleZone(true)); // board -> log (hand pruned again)
  QCOMPARE(controller.currentFocusId(), QStringLiteral("log.entry"));
  QVERIFY(controller.cycleZone(true)); // log -> board (only two zones left)
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));

  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card3"), QStringLiteral("hand"), {}});
  QVERIFY(controller.cycleZone(true)); // board -> log
  QCOMPARE(controller.currentFocusId(), QStringLiteral("log.entry"));
  QVERIFY(controller.cycleZone(true)); // log -> hand (re-appended at the end)
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card3"));
}

void FocusControllerTests::
    reregisteringANodeIntoADifferentZonePrunesTheOldZoneOnceItIsEmpty() {
  FocusController controller;
  registerBoardZone(controller);
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card1"), QStringLiteral("hand"), {}});
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("log.entry"), QStringLiteral("log"), {}});
  controller.setInitialFocus(QStringLiteral("board.nw"));

  // zoneOrder_ so far: [board, hand, log].
  QVERIFY(controller.cycleZone(true)); // board -> hand
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));
  QVERIFY(controller.cycleZone(true)); // hand -> log
  QCOMPARE(controller.currentFocusId(), QStringLiteral("log.entry"));
  QVERIFY(controller.cycleZone(true)); // log -> board (wraps)
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));

  // Re-register hand's only node under a brand-new "archive" zone
  // instead of removing it outright: this must empty "hand" exactly
  // like removeNode() would, and prune it from the cycle order the same
  // way -- not merely leave a stale, permanently-empty "hand" entry
  // sitting in zoneOrder_ forever.
  controller.registerNode(FocusNodeSpec{
      QStringLiteral("hand.card1"), QStringLiteral("archive"), {}});

  // zoneOrder_ must now be [board, log, archive] -- "hand" pruned,
  // "archive" appended at the end -- not [board, hand, log, archive]
  // with a dead "hand" entry still occupying its old slot.
  QVERIFY(controller.cycleZone(true)); // board -> log
  QCOMPARE(controller.currentFocusId(), QStringLiteral("log.entry"));
  QVERIFY(controller.cycleZone(true)); // log -> archive
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));
  QVERIFY(controller.cycleZone(true)); // archive -> board (wraps)
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));

  // Registering a brand-new node under the now-pruned "hand" zone id
  // must be treated as a fresh zone, appended at the *current* end of
  // the order (after "archive"), not reinserted at "hand"'s original
  // position right after "board" -- proving the old "hand" entry was
  // truly pruned, not merely bypassed.
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card2"), QStringLiteral("hand"), {}});
  QVERIFY(controller.cycleZone(true)); // board -> log
  QCOMPARE(controller.currentFocusId(), QStringLiteral("log.entry"));
  QVERIFY(controller.cycleZone(true)); // log -> archive
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));
  QVERIFY(controller.cycleZone(true)); // archive -> hand (re-appended at end)
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card2"));
  QVERIFY(controller.cycleZone(true)); // hand -> board (wraps)
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));
}

void FocusControllerTests::pruningAnEmptyZoneAlsoForgetsItsLastFocusedMemory() {
  FocusController controller;
  registerBoardZone(controller);
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("hand.card1"), QStringLiteral("hand"), {}});
  controller.setInitialFocus(QStringLiteral("board.nw"));

  // Visiting "hand" records hand.card1 as its last-focused node in
  // zoneLastFocused_.
  QVERIFY(controller.cycleZone(true));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));
  QVERIFY(
      controller.snapshot().zoneLastFocused.contains(QStringLiteral("hand")));

  QVERIFY(controller.cycleZone(false));
  QCOMPARE(controller.currentFocusId(), QStringLiteral("board.nw"));

  // Re-registering hand's only node into a different zone empties
  // "hand" and prunes it from zoneOrder_ (see the test above); the
  // snapshot must also no longer contain a "hand" entry in
  // zoneLastFocused -- otherwise that memory would linger forever
  // (never cleared, since no removeNode() call for hand.card1 itself
  // ever occurs here) even though "hand" can no longer be cycled to.
  controller.registerNode(FocusNodeSpec{
      QStringLiteral("hand.card1"), QStringLiteral("archive"), {}});
  QVERIFY(
      !controller.snapshot().zoneLastFocused.contains(QStringLiteral("hand")));

  // The same must hold for a zone emptied via outright node removal,
  // not just re-registration into another zone.
  controller.registerNode(
      FocusNodeSpec{QStringLiteral("log.entry"), QStringLiteral("log"), {}});
  QVERIFY(controller.cycleZone(true)); // board -> archive
  QCOMPARE(controller.currentFocusId(), QStringLiteral("hand.card1"));
  QVERIFY(controller.snapshot().zoneLastFocused.contains(
      QStringLiteral("archive")));

  controller.removeNode(QStringLiteral("hand.card1"));
  QVERIFY(!controller.snapshot().zoneLastFocused.contains(
      QStringLiteral("archive")));
}

QTEST_APPLESS_MAIN(FocusControllerTests)

#include "FocusControllerTests.moc"
