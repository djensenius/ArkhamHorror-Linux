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
void sendKey(QObject *target, const QEvent::Type type, const Qt::Key key,
             const bool autoRepeat = false,
             const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QKeyEvent event(type, key, modifiers, QString(), autoRepeat);
  QCoreApplication::sendEvent(target, &event);
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
  targetOnWorker->deleteLater();
  // Process the deleteLater() posted above before the test object graph
  // tears down, now that targetOnWorker is back under no thread's active
  // event loop ownership concerns for this short-lived test.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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

QTEST_GUILESS_MAIN(InputRouterTests)

#include "InputRouterTests.moc"
