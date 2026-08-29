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
  void keyEventsForAnUnboundKeyAreNeverConsumed();
  void focusOutClearsHeldKeysWithoutDispatchingAndAllowsAFreshPress();
  void windowDeactivateClearsHeldKeysWithoutDispatchingAndAllowsAFreshPress();
  void
  applicationDeactivateClearsHeldKeysWithoutDispatchingAndAllowsAFreshPress();
  void uninstallForgetsHeldKeysSoAFreshInstallNeverInheritsStaleHolds();
  void installingANewTargetForgetsHeldKeysFromThePreviousTarget();
  void targetDestructionWhileHeldDoesNotLeakStaleHeldStateIntoALaterInstall();
  void keypadEnterIsReachableDespiteItsKeypadModifier();

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

QTEST_GUILESS_MAIN(InputRouterTests)

#include "InputRouterTests.moc"
