// Tests for the production-composition hermetic seam used by main.cpp's
// --smoke-test handling (see AppBootstrap.h). This is a pure, synchronous
// function with no Qt event loop, network, QSettings, or keychain access of
// its own, so it is exercised directly here with a spy in place of the real
// composer -- proving the gating behaviour structurally rather than relying
// on a comment in main.cpp being kept accurate by hand.

#include <QtTest>

#include "AppBootstrap.h"

using namespace Arkham;

class AppBootstrapTests final : public QObject {
  Q_OBJECT

private slots:
  void smokeTestNeverInvokesComposer();
  void normalModeInvokesComposerExactlyOnce();
  void missingComposerIsSafeNoOp();
};

void AppBootstrapTests::smokeTestNeverInvokesComposer() {
  int callCount = 0;
  bootstrapSession(ProcessMode::SmokeTest, [&callCount] { ++callCount; });
  QCOMPARE(callCount, 0);
}

void AppBootstrapTests::normalModeInvokesComposerExactlyOnce() {
  int callCount = 0;
  bootstrapSession(ProcessMode::Normal, [&callCount] { ++callCount; });
  QCOMPARE(callCount, 1);
}

void AppBootstrapTests::missingComposerIsSafeNoOp() {
  // A default-constructed std::function is falsy; bootstrapSession must not
  // attempt to invoke it in either mode.
  bootstrapSession(ProcessMode::Normal, {});
  bootstrapSession(ProcessMode::SmokeTest, {});
  QVERIFY(true); // reaching here without crashing is the assertion
}

QTEST_APPLESS_MAIN(AppBootstrapTests)

#include "AppBootstrapTests.moc"
