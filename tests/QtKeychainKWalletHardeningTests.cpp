// Regression coverage for two downstream hardening patches applied to the
// pinned QtKeychain 0.17.0 source (see
// third_party/qtkeychain/patches/0001-disable-insecure-kwallet-fallback-and-error-mapping.patch):
//
//   1. ReadPasswordJobPrivate::kwalletOpenFinished() must never read or
//      migrate a legacy plaintext (QSettings-backed) entry when the caller
//      has insecureFallback(false) (QtKeychainJobFactory's production
//      setting -- see src/QtKeychainJobFactory.cpp). Upstream read/migrated
//      it unconditionally.
//   2. JobPrivate::kwalletFinished() (the shared completion handler for
//      KWallet read/write/delete D-Bus replies) must map a failed final
//      D-Bus reply to an explicit error instead of reporting success.
//
// Running these tests against the real production QtKeychainJobFactory also
// caught a third, more severe bug in src/QtKeychainJobFactory.cpp itself
// (fixed alongside this test file, not in the upstream/patched QtKeychain
// source): QKeychain::Job defaults to autoDelete(true) and calls
// this->deleteLater() from emitFinished(), but RealReadJob/RealWriteJob/
// RealDeleteJob each hold their real QKeychain job as a plain member
// subobject, not a heap allocation. Left at the default, the very first
// real job completion posted a deferred-delete event against a non-heap
// address, corrupting the heap ("double free or corruption") on every real
// save/read/delete -- a production bug that no prior test caught because
// existing token-store tests use a fake IKeychainJobFactory and never
// construct a real QKeychain job. RealReadJob/RealWriteJob/RealDeleteJob
// now call setAutoDelete(false) on their member job (see
// src/QtKeychainJobFactory.cpp); this test file failing with exactly that
// crash before the fix, and passing after it, is this bug's regression
// coverage.
//
// These tests exercise the real, production-used Arkham::QtKeychainJobFactory
// -- which constructs real QKeychain::ReadPasswordJob / WritePasswordJob /
// DeletePasswordJob instances with insecureFallback(false), exactly as
// production QtKeychainTokenStore does -- against a minimal fake
// implementation of the org.kde.KWallet D-Bus interface registered on an
// isolated session bus. QTKEYCHAIN_BACKEND=kwallet5 forces QtKeychain's
// internal backend detection to the kwallet5 code path in keychain_unix.cpp
// regardless of desktop environment, so this genuinely runs the patched
// production source, not a reimplementation of its logic.
//
// This test can only meaningfully run on Linux: the kwallet5 backend only
// exists in keychain_unix.cpp (compiled only for Unix-like, non-Apple
// platforms -- see CMakeLists.txt's Linux-only guard around this test
// target), and it requires a session D-Bus bus, which is why CI wraps this
// binary with `dbus-run-session`. It cannot be validated on macOS (this
// project's usual dev platform uses keychain_apple.mm instead, and
// dbus-run-session does not behave as a plain standalone session bus on
// macOS due to launchd integration expectations).

#include "IKeychainJobFactory.h"
#include "QtKeychainJobFactory.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QObject>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

using namespace Arkham;

namespace {

constexpr const char *kKWalletServiceName = "org.kde.kwalletd5";
constexpr const char *kKWalletObjectPath = "/modules/kwalletd5";

// Must be in sync with KWallet::EntryType (kwallet.h), mirrored from
// QtKeychain's own keychain_unix.cpp (enum KWalletEntryType).
enum KWalletEntryType {
  EntryUnknown = 0,
  EntryPassword = 1,
  EntryStream = 2,
  EntryMap = 3,
};

// A minimal fake implementation of the org.kde.KWallet D-Bus interface
// (see third_party's cached org.kde.KWallet.xml), registered on an isolated
// session bus for the duration of each test. Only the methods QtKeychain's
// kwallet5 backend actually calls are implemented; each can be configured
// to simulate a failed final D-Bus reply via sendErrorReply(), which is
// exactly the condition the second hardening patch guards against.
class FakeKWalletService : public QObject, protected QDBusContext {
  Q_OBJECT
  // Required for QDBusConnection::registerObject(..., ExportAllSlots) to
  // actually expose this object under the org.kde.KWallet interface name:
  // without it, Qt's D-Bus dispatch has no interface name to match against
  // QtKeychain's generated kwallet_interface.cpp client proxy (which calls
  // methods scoped to "org.kde.KWallet" specifically), and every call fails
  // with "No such interface 'org.kde.KWallet'".
  Q_CLASSINFO("D-Bus Interface", "org.kde.KWallet")

public:
  explicit FakeKWalletService(QObject *parent = nullptr) : QObject(parent) {}

  bool openShouldFail = false;
  int openHandle = 1;
  int entryTypeResult = EntryUnknown;
  bool writeShouldFail = false;
  bool removeShouldFail = false;
  bool readShouldFail = false;
  QString readPasswordValue;

public Q_SLOTS:
  QString networkWallet() { return QStringLiteral("kdewallet"); }

  int open(const QString &wallet, qlonglong wId, const QString &appid) {
    Q_UNUSED(wallet);
    Q_UNUSED(wId);
    Q_UNUSED(appid);
    if (openShouldFail) {
      sendErrorReply(QDBusError::AccessDenied,
                     QStringLiteral("simulated open failure"));
      return -1;
    }
    return openHandle;
  }

  int entryType(int handle, const QString &folder, const QString &key,
                const QString &appid) {
    Q_UNUSED(handle);
    Q_UNUSED(folder);
    Q_UNUSED(key);
    Q_UNUSED(appid);
    return entryTypeResult;
  }

  QString readPassword(int handle, const QString &folder, const QString &key,
                       const QString &appid) {
    Q_UNUSED(handle);
    Q_UNUSED(folder);
    Q_UNUSED(key);
    Q_UNUSED(appid);
    if (readShouldFail) {
      sendErrorReply(QDBusError::Failed,
                     QStringLiteral("simulated read failure"));
      return QString();
    }
    return readPasswordValue;
  }

  int writePassword(int handle, const QString &folder, const QString &key,
                    const QString &value, const QString &appid) {
    Q_UNUSED(handle);
    Q_UNUSED(folder);
    Q_UNUSED(key);
    Q_UNUSED(value);
    Q_UNUSED(appid);
    if (writeShouldFail) {
      sendErrorReply(QDBusError::Failed,
                     QStringLiteral("simulated write failure"));
      return -1;
    }
    return 0;
  }

  int removeEntry(int handle, const QString &folder, const QString &key,
                  const QString &appid) {
    Q_UNUSED(handle);
    Q_UNUSED(folder);
    Q_UNUSED(key);
    Q_UNUSED(appid);
    if (removeShouldFail) {
      sendErrorReply(QDBusError::Failed,
                     QStringLiteral("simulated remove failure"));
      return -1;
    }
    return 0;
  }
};

// Blocks (pumping the event loop) until job's finished() signal fires or a
// safety deadline expires, mirroring the deadline-bounded event pumping
// pattern used by this repo's other async tests (see AuthClientTests.cpp's
// runAndWait). A real D-Bus round trip cannot complete synchronously inside
// start(), so this always needs to pump the event loop.
template <typename JobInterface> bool waitForFinished(JobInterface &job) {
  bool finished = false;
  QObject::connect(&job, &JobInterface::finished, &job,
                   [&finished]() { finished = true; });
  job.start();
  const QDeadlineTimer deadline(5000);
  while (!finished && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return finished;
}

} // namespace

class QtKeychainKWalletHardeningTests final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void init();
  void cleanup();

  void plaintextNeverReturnedWhenInsecureFallbackDisabled();
  void plaintextMigratedOnlyWhenInsecureFallbackExplicitlyEnabled();
  void writeFailureReportedAsError();
  void deleteFailureReportedAsError();
  void readFailureReportedAsError();

private:
  std::unique_ptr<QTemporaryDir> m_configDir;
  std::unique_ptr<FakeKWalletService> m_fakeService;
  QtKeychainJobFactory m_factory;
};

void QtKeychainKWalletHardeningTests::initTestCase() {
  // Force QtKeychain's backend detection straight to the kwallet5 code
  // path, bypassing desktop-environment auto-detection (CI runners have no
  // desktop session at all).
  qputenv("QTKEYCHAIN_BACKEND", "kwallet5");

  // Redirect QtKeychain's legacy-plaintext QSettings store (used only via
  // PlainTextStore, backed by `new QSettings(service)` with no explicit
  // format) into an isolated temporary directory instead of this
  // machine/CI-runner's real $HOME, so seeding/reading a legacy plaintext
  // entry never touches real user configuration. On Unix, QSettings'
  // NativeFormat and IniFormat are the same on-disk mechanism (differing
  // only in file extension), so overriding NativeFormat's UserScope path
  // here is sufficient.
  m_configDir = std::make_unique<QTemporaryDir>();
  QVERIFY(m_configDir->isValid());
  qputenv("HOME", m_configDir->path().toUtf8());
  qputenv("XDG_CONFIG_HOME", m_configDir->path().toUtf8());
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                     m_configDir->path());

  QVERIFY2(QDBusConnection::sessionBus().isConnected(),
           "No session D-Bus bus available; this test requires being run "
           "under dbus-run-session (see CMakeLists.txt).");
}

void QtKeychainKWalletHardeningTests::init() {
  m_fakeService = std::make_unique<FakeKWalletService>();
  QVERIFY(QDBusConnection::sessionBus().registerObject(
      QString::fromLatin1(kKWalletObjectPath), m_fakeService.get(),
      QDBusConnection::ExportAllSlots));
  QVERIFY(QDBusConnection::sessionBus().registerService(
      QString::fromLatin1(kKWalletServiceName)));
}

void QtKeychainKWalletHardeningTests::cleanup() {
  QDBusConnection::sessionBus().unregisterService(
      QString::fromLatin1(kKWalletServiceName));
  QDBusConnection::sessionBus().unregisterObject(
      QString::fromLatin1(kKWalletObjectPath));
  m_fakeService.reset();
}

void QtKeychainKWalletHardeningTests::
    plaintextNeverReturnedWhenInsecureFallbackDisabled() {
  const QString service = QStringLiteral("arkham-kwallet-hardening-test");
  const QString key = QStringLiteral("plaintext-guard-key");
  const QString secret = QStringLiteral("super-secret-legacy-token");

  // Seed a legacy plaintext entry directly via QSettings, exactly as
  // PlainTextStore itself would have written it (see plaintextstore.cpp:
  // "<key>/data" + "<key>/type").
  {
    QSettings settings(service);
    settings.setValue(key + QLatin1String("/data"), secret.toUtf8());
    settings.setValue(key + QLatin1String("/type"), QStringLiteral("Text"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
  }

  // The fake KWallet reports a successfully opened wallet (a real handle),
  // so the read job reaches ReadPasswordJobPrivate::kwalletOpenFinished()'s
  // patched plaintext-guard branch. entryType() then reports Unknown, so
  // -- once the plaintext short-circuit is correctly skipped -- the job
  // must terminate with EntryNotFound rather than any KWallet D-Bus data.
  m_fakeService->openHandle = 1;
  m_fakeService->entryTypeResult = EntryUnknown;

  auto job = m_factory.createReadJob(service, key);
  QVERIFY(waitForFinished(*job));

  QCOMPARE(job->error(), QKeychain::EntryNotFound);
  QVERIFY(job->textData() != secret);
  QVERIFY(job->textData().isEmpty());

  // The seeded plaintext entry must remain completely untouched: no read,
  // no migration, no removal.
  QSettings settings(service);
  QVERIFY(settings.contains(key + QLatin1String("/data")));
  QCOMPARE(settings.value(key + QLatin1String("/data")).toByteArray(),
           secret.toUtf8());
}

void QtKeychainKWalletHardeningTests::
    plaintextMigratedOnlyWhenInsecureFallbackExplicitlyEnabled() {
  // Sanity/harness-validation counterpart to the test above: proves the
  // seeded plaintext entry genuinely is readable through this exact fake
  // KWallet + QSettings harness when insecureFallback(true) is explicitly
  // requested (QtKeychain's own opt-in migration behaviour, unaffected by
  // either hardening patch). This is not the production configuration --
  // QtKeychainJobFactory always sets insecureFallback(false) -- but proves
  // the guard in the previous test is specifically gated on the flag,
  // rather than on some other incidental harness detail.
  const QString service = QStringLiteral("arkham-kwallet-hardening-test-optin");
  const QString key = QStringLiteral("plaintext-optin-key");
  const QString secret = QStringLiteral("legacy-optin-secret");

  {
    QSettings settings(service);
    settings.setValue(key + QLatin1String("/data"), secret.toUtf8());
    settings.setValue(key + QLatin1String("/type"), QStringLiteral("Text"));
    settings.sync();
  }

  m_fakeService->openHandle = 1;
  m_fakeService->entryTypeResult = EntryUnknown;
  m_fakeService->writeShouldFail = false; // migration's nested write job

  QKeychain::ReadPasswordJob job(service);
  job.setKey(key);
  job.setInsecureFallback(true);
  // job is a local stack variable, not heap-allocated, but QKeychain::Job
  // defaults to autoDelete(true) and calls this->deleteLater() from
  // emitFinished() -- without disabling it here, the deferred-delete event
  // fires against this non-heap address once the job completes, corrupting
  // the heap (see RealReadJob's own setAutoDelete(false) in
  // src/QtKeychainJobFactory.cpp for the equivalent production-code fix).
  job.setAutoDelete(false);
  QVERIFY(waitForFinished(job));

  QCOMPARE(job.error(), QKeychain::NoError);
  QCOMPARE(job.textData(), secret);

  // Migration removes the plaintext entry synchronously (before
  // finished()), even though the follow-up KWallet write happens
  // asynchronously afterwards.
  QSettings settings(service);
  QVERIFY(!settings.contains(key + QLatin1String("/data")));

  // Let the fire-and-forget migration WritePasswordJob (started internally
  // by QtKeychain after emitFinished()) complete while the fake service is
  // still alive, rather than leaving it to race object teardown.
  QTest::qWait(100);
}

void QtKeychainKWalletHardeningTests::writeFailureReportedAsError() {
  const QString service = QStringLiteral("arkham-kwallet-hardening-test");
  const QString key = QStringLiteral("write-failure-key");

  m_fakeService->openHandle = 1;
  m_fakeService->writeShouldFail = true;

  auto job = m_factory.createWriteJob(service, key);
  job->setTextData(QStringLiteral("value"));
  QSignalSpy finishedSpy(job.get(), &IKeychainWriteJob::finished);

  QVERIFY(waitForFinished(*job));

  // Before the hardening patch, kwalletFinished() ignored the D-Bus
  // watcher's error and always emitted success -- this must now be a
  // typed OtherError, and finished() must still fire exactly once.
  QCOMPARE(job->error(), QKeychain::OtherError);
  QVERIFY(!job->errorString().isEmpty());
  QCOMPARE(finishedSpy.count(), 1);
}

void QtKeychainKWalletHardeningTests::deleteFailureReportedAsError() {
  const QString service = QStringLiteral("arkham-kwallet-hardening-test");
  const QString key = QStringLiteral("delete-failure-key");

  m_fakeService->openHandle = 1;
  m_fakeService->removeShouldFail = true;

  auto job = m_factory.createDeleteJob(service, key);
  QSignalSpy finishedSpy(job.get(), &IKeychainDeleteJob::finished);

  QVERIFY(waitForFinished(*job));

  QCOMPARE(job->error(), QKeychain::OtherError);
  QVERIFY(!job->errorString().isEmpty());
  QCOMPARE(finishedSpy.count(), 1);
}

void QtKeychainKWalletHardeningTests::readFailureReportedAsError() {
  const QString service = QStringLiteral("arkham-kwallet-hardening-test");
  const QString key = QStringLiteral("read-failure-key");

  m_fakeService->openHandle = 1;
  // Report a real (non-Unknown) entry type so the job proceeds past
  // entryType() to the readPassword() D-Bus call that is made to fail.
  m_fakeService->entryTypeResult = EntryPassword;
  m_fakeService->readShouldFail = true;

  auto job = m_factory.createReadJob(service, key);
  QSignalSpy finishedSpy(job.get(), &IKeychainReadJob::finished);

  QVERIFY(waitForFinished(*job));

  QCOMPARE(job->error(), QKeychain::OtherError);
  QVERIFY(!job->errorString().isEmpty());
  QVERIFY(job->textData().isEmpty());
  QCOMPARE(finishedSpy.count(), 1);
}

QTEST_GUILESS_MAIN(QtKeychainKWalletHardeningTests)

#include "QtKeychainKWalletHardeningTests.moc"
