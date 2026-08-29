#include <QCoreApplication>
#include <QKeyEvent>
#include <QObject>
#include <QSignalSpy>
#include <QThread>
#include <QtTest>

#include "InputMapper.h"
#include "InputRouter.h"
#include "SemanticCommand.h"

using Arkham::CommandPhase;
using Arkham::InputMapper;
using Arkham::InputRouter;
using Arkham::PhysicalKey;
using Arkham::SemanticCommand;

namespace {

// Builds and delivers a real QKeyEvent synchronously through |target|'s
// installed event filters (InputRouter included), exactly like a real
// platform key event would be, rather than calling InputMapper directly.
// Returns whether the event was consumed (i.e. some filter, such as an
// installed InputRouter, returned true for it).
bool sendKey(QObject *target, const QEvent::Type type, const Qt::Key key,
             const bool autoRepeat = false,
             const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QKeyEvent event(type, key, modifiers, QString(), autoRepeat);
  return QCoreApplication::sendEvent(target, &event);
}

} // namespace

class InputRouterTests final : public QObject {
  Q_OBJECT

private slots:
  void dispatchesACommandForARealKeyPress();
  void installRejectsNullTarget();
  void installIsIdempotentForTheSameTarget();
  void installingANewTargetUninstallsThePrevious();
  void uninstallStopsDispatchAndIsSafeToCallRepeatedly();
  void routerNoticesWhenItsTargetIsDestroyedExternally();
  void destroyingTheRouterUninstallsAndNeverDispatchesAfterwards();
  void installAcrossThreadsIsRejected();
  void onlyKeyEventsOnTheInstalledTargetAreConsidered();
  void suppressedDedupTransitionsAreStillConsumedForABoundKey();
  void
  strayDuplicatePressWithDifferentModifiersForAnAlreadyHeldKeyIsStillConsumed();
  void remappingAnInitiallyUnboundKeyMidHoldDoesNotConsumeItsRepeatOrRelease();
  void keyEventsForAnUnboundKeyAreNeverConsumed();
  void focusOutClearsHeldKeysWithoutDispatchingAndAllowsAFreshPress();
  void windowDeactivateClearsHeldKeysWithoutDispatchingAndAllowsAFreshPress();
  void
  applicationDeactivateClearsHeldKeysWithoutDispatchingAndAllowsAFreshPress();
  void uninstallForgetsHeldKeysSoAFreshInstallNeverInheritsStaleHolds();
  void installingANewTargetForgetsHeldKeysFromThePreviousTarget();
  void targetDestructionWhileHeldDoesNotLeakStaleHeldStateIntoALaterInstall();
  void keypadEnterIsReachableDespiteItsKeypadModifier();
  void
  textEntrySuspensionDefaultsToDisabledButAutomaticDetectionDefaultsToEnabled();
  void
  explicitTextEntrySuspensionBlocksDispatchForOrdinaryAndReservedKeysAlike();
  void suspendingMidHoldClearsTheHoldSoItsReleaseIsNotDispatched();
  void disablingAutomaticDetectionLeavesExplicitSuspensionAsTheOnlyGate();

private:
  void assertLifecycleEventClearsHeldKeys(QEvent::Type eventType);
};

void InputRouterTests::dispatchesACommandForARealKeyPress() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);
  sendKey(&target, QEvent::KeyPress, Qt::Key_Up);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.constFirst().at(0).value<SemanticCommand>(),
           SemanticCommand::FocusUp);
  QCOMPARE(spy.constFirst().at(1).value<CommandPhase>(), CommandPhase::Pressed);
}

void InputRouterTests::installRejectsNullTarget() {
  InputMapper mapper;
  InputRouter router(mapper);
  QVERIFY(!router.install(nullptr));
  QVERIFY(!router.isInstalled());
}

void InputRouterTests::installIsIdempotentForTheSameTarget() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));
  QVERIFY(router.install(&target));
  QCOMPARE(router.installedTarget(), &target);
}

void InputRouterTests::installingANewTargetUninstallsThePrevious() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject first;
  QObject second;
  QVERIFY(router.install(&first));
  QVERIFY(router.install(&second));
  QCOMPARE(router.installedTarget(), &second);

  QSignalSpy spy(&router, &InputRouter::commandDispatched);
  // The first target no longer has the router installed: a key event
  // sent to it must not dispatch anything.
  sendKey(&first, QEvent::KeyPress, Qt::Key_Up);
  QCOMPARE(spy.count(), 0);

  sendKey(&second, QEvent::KeyPress, Qt::Key_Up);
  QCOMPARE(spy.count(), 1);
}

void InputRouterTests::uninstallStopsDispatchAndIsSafeToCallRepeatedly() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));

  router.uninstall();
  QVERIFY(!router.isInstalled());
  // Safe to call again, including when nothing is installed.
  router.uninstall();

  QSignalSpy spy(&router, &InputRouter::commandDispatched);
  sendKey(&target, QEvent::KeyPress, Qt::Key_Up);
  QCOMPARE(spy.count(), 0);
}

void InputRouterTests::routerNoticesWhenItsTargetIsDestroyedExternally() {
  InputMapper mapper;
  InputRouter router(mapper);
  auto *target = new QObject();
  QVERIFY(router.install(target));

  delete target;

  QVERIFY(!router.isInstalled());
  QVERIFY(router.installedTarget() == nullptr);
}

void InputRouterTests::
    destroyingTheRouterUninstallsAndNeverDispatchesAfterwards() {
  InputMapper mapper;
  QObject target;
  bool dispatched = false;
  {
    InputRouter router(mapper);
    QVERIFY(router.install(&target));
    QObject::connect(&router, &InputRouter::commandDispatched,
                     [&dispatched] { dispatched = true; });
    sendKey(&target, QEvent::KeyPress, Qt::Key_Up);
    QVERIFY(dispatched);
    dispatched = false;
  } // router destroyed here; its destructor must uninstall it first.

  // The target itself is still alive; sending it another key event must
  // not crash and must not reach any now-destroyed router.
  sendKey(&target, QEvent::KeyPress, Qt::Key_Down);
  QVERIFY(!dispatched);
}

void InputRouterTests::installAcrossThreadsIsRejected() {
  InputMapper mapper;
  InputRouter router(mapper); // lives on the test thread

  QThread workerThread;
  workerThread.start();
  auto *targetOnWorker = new QObject();
  targetOnWorker->moveToThread(&workerThread);

  QVERIFY(!router.install(targetOnWorker));
  QVERIFY(!router.isInstalled());

  workerThread.quit();
  workerThread.wait();

  // The worker thread has already stopped, so its event loop will never
  // process a deleteLater() posted for an object still affinitized to
  // it; move the object back to this thread first so it can be deleted
  // deterministically, without leaking and without deleting across
  // threads.
  targetOnWorker->moveToThread(QThread::currentThread());
  delete targetOnWorker;
}

void InputRouterTests::onlyKeyEventsOnTheInstalledTargetAreConsidered() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);
  QObject unrelated;
  sendKey(&unrelated, QEvent::KeyPress, Qt::Key_Up);
  QCOMPARE(spy.count(), 0);

  // Non-key events on the installed target must also be ignored.
  QEvent timerEvent(QEvent::Timer);
  QCoreApplication::sendEvent(&target, &timerEvent);
  QCOMPARE(spy.count(), 0);
}

void InputRouterTests::
    suppressedDedupTransitionsAreStillConsumedForABoundKey() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);

  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_Up));
  QCOMPARE(spy.count(), 1);

  // A stray duplicate press (no intervening release) is suppressed by
  // InputMapper's dedup rules, so no second command is dispatched --
  // but Qt::Key_Up is bound, so the event must still be consumed rather
  // than falling through to default Qt key handling.
  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_Up));
  QCOMPARE(spy.count(), 1);

  QVERIFY(sendKey(&target, QEvent::KeyRelease, Qt::Key_Up));
  QCOMPARE(spy.count(), 2);

  // A stray release (the key is no longer held) is likewise suppressed
  // but still consumed.
  QVERIFY(sendKey(&target, QEvent::KeyRelease, Qt::Key_Up));
  QCOMPARE(spy.count(), 2);
}

void InputRouterTests::keyEventsForAnUnboundKeyAreNeverConsumed() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);
  // Qt::Key_F13 has no default binding, so it must never be consumed --
  // ordinary default key handling for unrelated keys must keep working.
  QVERIFY(!sendKey(&target, QEvent::KeyPress, Qt::Key_F13));
  QCOMPARE(spy.count(), 0);
  QVERIFY(!sendKey(&target, QEvent::KeyRelease, Qt::Key_F13));
  QCOMPARE(spy.count(), 0);
}

void InputRouterTests::
    strayDuplicatePressWithDifferentModifiersForAnAlreadyHeldKeyIsStillConsumed() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);

  // Ctrl+Z is bound to Undo by default (see
  // InputMapper::resetToDefaults()). Pressing it arms the hold for
  // physical key Z. Held-key identity is modifier-insensitive (see
  // InputMapper.h's heldKeys_ comment): the key is now "owned" by this
  // router for the rest of its hold, regardless of what modifiers later
  // accompany any other event for it.
  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_Z, false,
                  Qt::ControlModifier));
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.constLast().at(0).value<SemanticCommand>(),
           SemanticCommand::Undo);

  // A stray duplicate press of the SAME physical key arrives with
  // different (here, no) live modifiers -- e.g. because a platform
  // delivered an extra press event without an intervening release, or
  // a modifier key's own release was processed first even though this
  // is still logically the same physical hold. commandFor({Z,
  // NoModifier}) has no binding at all, so a router that decided
  // consumption from commandFor() alone would incorrectly let this
  // fall through to Qt's default key handling for a key it already
  // owns. InputMapper's dedup still suppresses it (no second Pressed
  // is dispatched), but it must still be consumed.
  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_Z));
  QCOMPARE(spy.count(), 1);

  // The eventual release -- however its modifiers happen to read --
  // must also still be consumed and must dispatch the press-time
  // command's Released phase.
  QVERIFY(sendKey(&target, QEvent::KeyRelease, Qt::Key_Z));
  QCOMPARE(spy.count(), 2);
  QCOMPARE(spy.constLast().at(0).value<SemanticCommand>(),
           SemanticCommand::Undo);
  QCOMPARE(spy.constLast().at(1).value<CommandPhase>(), CommandPhase::Released);

  // A genuinely stray release afterwards -- the hold has already ended,
  // so this key is no longer "owned" by any measure (neither
  // commandFor() nor isArmedKeyHeld()) -- must NOT be consumed,
  // confirming the held-key-identity check only extends ownership for
  // the duration of an actual hold, not forever.
  QVERIFY(!sendKey(&target, QEvent::KeyRelease, Qt::Key_Z));
  QCOMPARE(spy.count(), 2);
}

void InputRouterTests::
    remappingAnInitiallyUnboundKeyMidHoldDoesNotConsumeItsRepeatOrRelease() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);

  // Qt::Key_F13 has no default binding, so pressing it starts an
  // *unarmed* hold: correctly not consumed and not dispatched, exactly
  // like any other unbound key's press.
  QVERIFY(!sendKey(&target, QEvent::KeyPress, Qt::Key_F13));
  QCOMPARE(spy.count(), 0);

  // Bind F13 to a command WHILE it is still physically held down --
  // e.g. a settings/remap UI applied mid-press. commandFor(F13) now
  // reports CameraZoomIn, but the F13 hold itself was frozen as
  // unarmed at press time and must keep behaving exactly as it did
  // when it started: neither its auto-repeat nor its eventual release
  // may suddenly become consumed, or downstream code would observe an
  // (unconsumed) press with no matching (also-unconsumed) release --
  // an asymmetric break of ordinary Qt key-event propagation for a key
  // this router never actually took ownership of.
  QVERIFY(!mapper.remap(PhysicalKey{Qt::Key_F13}, SemanticCommand::CameraZoomIn)
               .has_value());

  QVERIFY(!sendKey(&target, QEvent::KeyPress, Qt::Key_F13, true));
  QCOMPARE(spy.count(), 0);
  QVERIFY(!sendKey(&target, QEvent::KeyRelease, Qt::Key_F13));
  QCOMPARE(spy.count(), 0);

  // A brand-new press afterwards -- no longer mid-hold, so this is a
  // completely fresh key-down -- must now correctly dispatch the newly
  // bound command and be consumed, proving the mid-hold remap did take
  // effect for the *next* independent hold.
  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_F13));
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.constLast().at(0).value<SemanticCommand>(),
           SemanticCommand::CameraZoomIn);
}

void InputRouterTests::assertLifecycleEventClearsHeldKeys(
    const QEvent::Type eventType) {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);
  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_Up));
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.constLast().at(1).value<CommandPhase>(), CommandPhase::Pressed);

  // The platform is free to never deliver Up's real KeyRelease once
  // focus/activation is lost (e.g. Alt-Tabbing away while a key is held
  // down), so this lifecycle event must forget the held state itself,
  // without dispatching anything of its own.
  QEvent lifecycleEvent(eventType);
  QCoreApplication::sendEvent(&target, &lifecycleEvent);
  QCOMPARE(spy.count(), 1);

  // Because held state was forgotten, a fresh press of the same key with
  // no intervening release is correctly a brand-new hold (a new
  // Pressed), never a suppressed stray duplicate.
  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_Up));
  QCOMPARE(spy.count(), 2);
  QCOMPARE(spy.constLast().at(1).value<CommandPhase>(), CommandPhase::Pressed);
}

void InputRouterTests::
    focusOutClearsHeldKeysWithoutDispatchingAndAllowsAFreshPress() {
  assertLifecycleEventClearsHeldKeys(QEvent::FocusOut);
}

void InputRouterTests::
    windowDeactivateClearsHeldKeysWithoutDispatchingAndAllowsAFreshPress() {
  assertLifecycleEventClearsHeldKeys(QEvent::WindowDeactivate);
}

void InputRouterTests::
    applicationDeactivateClearsHeldKeysWithoutDispatchingAndAllowsAFreshPress() {
  assertLifecycleEventClearsHeldKeys(QEvent::ApplicationDeactivate);
}

void InputRouterTests::
    uninstallForgetsHeldKeysSoAFreshInstallNeverInheritsStaleHolds() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject first;
  QVERIFY(router.install(&first));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);
  QVERIFY(sendKey(&first, QEvent::KeyPress, Qt::Key_Up));
  QCOMPARE(spy.count(), 1);

  // Up was never released on |first|; uninstall() must forget it rather
  // than let it leak into whatever target is installed next.
  router.uninstall();

  QObject second;
  QVERIFY(router.install(&second));
  QVERIFY(sendKey(&second, QEvent::KeyPress, Qt::Key_Up));
  QCOMPARE(spy.count(), 2);
  QCOMPARE(spy.constLast().at(1).value<CommandPhase>(), CommandPhase::Pressed);
}

void InputRouterTests::
    installingANewTargetForgetsHeldKeysFromThePreviousTarget() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject first;
  QObject second;
  QVERIFY(router.install(&first));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);
  QVERIFY(sendKey(&first, QEvent::KeyPress, Qt::Key_Up));
  QCOMPARE(spy.count(), 1);

  // Installing a *different* target implicitly uninstalls the previous
  // one; the still-held Up key from |first| must not leak into |second|.
  QVERIFY(router.install(&second));
  QVERIFY(sendKey(&second, QEvent::KeyPress, Qt::Key_Up));
  QCOMPARE(spy.count(), 2);
  QCOMPARE(spy.constLast().at(1).value<CommandPhase>(), CommandPhase::Pressed);
}

void InputRouterTests::
    targetDestructionWhileHeldDoesNotLeakStaleHeldStateIntoALaterInstall() {
  InputMapper mapper;
  InputRouter router(mapper);
  auto *target = new QObject();
  QVERIFY(router.install(target));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);
  QVERIFY(sendKey(target, QEvent::KeyPress, Qt::Key_Up));
  QCOMPARE(spy.count(), 1);

  delete target; // Never released; QPointer nulls installedTarget_.
  QVERIFY(!router.isInstalled());

  QObject second;
  QVERIFY(router.install(&second));
  QVERIFY(sendKey(&second, QEvent::KeyPress, Qt::Key_Up));
  QCOMPARE(spy.count(), 2);
  QCOMPARE(spy.constLast().at(1).value<CommandPhase>(), CommandPhase::Pressed);
}

void InputRouterTests::keypadEnterIsReachableDespiteItsKeypadModifier() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);
  // A real keypad Enter QKeyEvent carries Qt::KeypadModifier alongside
  // Qt::NoModifier; without normalizing it away, this would never match
  // the plain PhysicalKey{Key_Enter, NoModifier} default binding and
  // keypad Enter would be permanently unreachable.
  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_Enter, false,
                  Qt::KeypadModifier));
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.constFirst().at(0).value<SemanticCommand>(),
           SemanticCommand::PrimaryAction);
  QCOMPARE(spy.constFirst().at(1).value<CommandPhase>(), CommandPhase::Pressed);

  QVERIFY(sendKey(&target, QEvent::KeyRelease, Qt::Key_Enter, false,
                  Qt::KeypadModifier));
  QCOMPARE(spy.count(), 2);
  QCOMPARE(spy.constLast().at(1).value<CommandPhase>(), CommandPhase::Released);
}

void InputRouterTests::
    textEntrySuspensionDefaultsToDisabledButAutomaticDetectionDefaultsToEnabled() {
  InputMapper mapper;
  InputRouter router(mapper);
  QVERIFY(!router.isTextEntrySuspended());
  QVERIFY(!router.isSemanticInputExplicitlySuspended());
  QVERIFY(router.isAutomaticTextEntryDetectionEnabled());
}

void InputRouterTests::
    explicitTextEntrySuspensionBlocksDispatchForOrdinaryAndReservedKeysAlike() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);

  // Sanity: with nothing suspended, W (a default FocusUp alias) and
  // Escape (reserved) both dispatch normally.
  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_W));
  QCOMPARE(spy.count(), 1);
  QVERIFY(sendKey(&target, QEvent::KeyRelease, Qt::Key_W));
  QCOMPARE(spy.count(), 2);

  router.setSemanticInputSuspended(true);
  QVERIFY(router.isTextEntrySuspended());
  QVERIFY(router.isSemanticInputExplicitlySuspended());

  // While suspended, an ordinary bound letter (W/FocusUp) must be
  // treated exactly like an unbound key: never consumed, never
  // dispatched -- this is the production text-entry gate.
  QVERIFY(!sendKey(&target, QEvent::KeyPress, Qt::Key_W));
  QCOMPARE(spy.count(), 2);
  QVERIFY(!sendKey(&target, QEvent::KeyRelease, Qt::Key_W));
  QCOMPARE(spy.count(), 2);

  // Reserved keys are NOT exempted while suspended: this router must not
  // decide what Escape/Back/Menu mean while a text control owns focus.
  QVERIFY(!sendKey(&target, QEvent::KeyPress, Qt::Key_Escape));
  QCOMPARE(spy.count(), 2);
  QVERIFY(!sendKey(&target, QEvent::KeyRelease, Qt::Key_Escape));
  QCOMPARE(spy.count(), 2);

  router.setSemanticInputSuspended(false);
  QVERIFY(!router.isTextEntrySuspended());

  // Resuming restores completely normal dispatch for a brand-new press.
  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_W));
  QCOMPARE(spy.count(), 3);
  QCOMPARE(spy.constLast().at(0).value<SemanticCommand>(),
           SemanticCommand::FocusUp);
  QVERIFY(sendKey(&target, QEvent::KeyRelease, Qt::Key_W));
  QCOMPARE(spy.count(), 4);
}

void InputRouterTests::
    suspendingMidHoldClearsTheHoldSoItsReleaseIsNotDispatched() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));

  QSignalSpy spy(&router, &InputRouter::commandDispatched);

  // Press and hold W: this arms a FocusUp hold exactly like any other
  // bound key.
  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_W));
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.constLast().at(1).value<CommandPhase>(), CommandPhase::Pressed);

  // Suspending mid-hold (e.g. the user clicked into a text field while
  // still physically holding W) must forget that hold immediately --
  // not just start ignoring *new* presses -- so the eventual release
  // event (which arrives while still suspended) is not misinterpreted
  // as belonging to a command this router no longer owns.
  router.setSemanticInputSuspended(true);
  QVERIFY(!sendKey(&target, QEvent::KeyRelease, Qt::Key_W));
  QCOMPARE(spy.count(), 1);

  router.setSemanticInputSuspended(false);

  // A fresh press afterwards must start a brand-new hold and dispatch
  // normally -- proving no stale armed state survived the suspend/
  // resume round trip to swallow this press instead.
  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_W));
  QCOMPARE(spy.count(), 2);
  QCOMPARE(spy.constLast().at(1).value<CommandPhase>(), CommandPhase::Pressed);
  QVERIFY(sendKey(&target, QEvent::KeyRelease, Qt::Key_W));
  QCOMPARE(spy.count(), 3);
  QCOMPARE(spy.constLast().at(1).value<CommandPhase>(), CommandPhase::Released);
}

void InputRouterTests::
    disablingAutomaticDetectionLeavesExplicitSuspensionAsTheOnlyGate() {
  InputMapper mapper;
  InputRouter router(mapper);
  QObject target;
  QVERIFY(router.install(&target));

  // With no QGuiApplication instance (this test binary is
  // QTEST_GUILESS_MAIN), automatic detection can never actually observe
  // a focused text control regardless of whether it is enabled --
  // toggling it here must not affect isTextEntrySuspended() at all while
  // the explicit override is untouched.
  QVERIFY(!router.isTextEntrySuspended());
  router.setAutomaticTextEntryDetectionEnabled(false);
  QVERIFY(!router.isAutomaticTextEntryDetectionEnabled());
  QVERIFY(!router.isTextEntrySuspended());

  QSignalSpy spy(&router, &InputRouter::commandDispatched);
  QVERIFY(sendKey(&target, QEvent::KeyPress, Qt::Key_W));
  QCOMPARE(spy.count(), 1);
  QVERIFY(sendKey(&target, QEvent::KeyRelease, Qt::Key_W));
  QCOMPARE(spy.count(), 2);

  router.setAutomaticTextEntryDetectionEnabled(true);
  QVERIFY(router.isAutomaticTextEntryDetectionEnabled());
  QVERIFY(!router.isTextEntrySuspended());
}

QTEST_GUILESS_MAIN(InputRouterTests)

#include "InputRouterTests.moc"
