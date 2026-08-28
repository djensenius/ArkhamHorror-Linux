// Tests for QtKeychainTokenStore, driven entirely through a fake
// IKeychainJobFactory (FakeKeychainJobFactory below). No real OS keyring,
// D-Bus session, or Secret Service/KWallet backend is touched: every job the
// factory creates completes asynchronously (via a queued event) against an
// in-memory map, so these tests exercise QtKeychainTokenStore's actual
// sequencing and error-mapping logic deterministically.

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEvent>
#include <QEventLoop>
#include <QHash>
#include <QMetaObject>
#include <QPair>
#include <QPointer>
#include <QUuid>
#include <QtTest>
#include <functional>
#include <optional>
#include <utility>

#include "IKeychainJobFactory.h"
#include "ITokenStore.h"
#include "QtKeychainJobFactory.h"
#include "QtKeychainTokenStore.h"

using namespace Arkham;

namespace {

// ─── Fake QtKeychain jobs ──────────────────────────────────────────────────
//
// Each job schedules its finished() signal on the next event-loop iteration
// (Qt::QueuedConnection), mirroring the asynchronous nature of real
// QtKeychain jobs, so tests exercise the same async delivery path as
// production code.

class FakeReadJob final : public IKeychainReadJob {
public:
  FakeReadJob(QKeychain::Error err, QString text)
      : m_error(err), m_text(std::move(text)) {}

  void start() override {
    QMetaObject::invokeMethod(
        this, [this]() { emit finished(); }, Qt::QueuedConnection);
  }
  [[nodiscard]] QKeychain::Error error() const override { return m_error; }
  [[nodiscard]] QString errorString() const override {
    return QStringLiteral("simulated");
  }
  [[nodiscard]] QString textData() const override { return m_text; }

private:
  QKeychain::Error m_error;
  QString m_text;
};

class FakeWriteJob final : public IKeychainWriteJob {
public:
  FakeWriteJob(QKeychain::Error err,
               std::function<void(const QString &)> onSuccess)
      : m_error(err), m_onSuccess(std::move(onSuccess)) {}

  void setTextData(const QString &data) override { m_data = data; }
  void start() override {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          if (m_error == QKeychain::NoError && m_onSuccess) {
            m_onSuccess(m_data);
          }
          emit finished();
        },
        Qt::QueuedConnection);
  }
  [[nodiscard]] QKeychain::Error error() const override { return m_error; }
  [[nodiscard]] QString errorString() const override {
    return QStringLiteral("simulated");
  }

private:
  QKeychain::Error m_error;
  QString m_data;
  std::function<void(const QString &)> m_onSuccess;
};

class FakeDeleteJob final : public IKeychainDeleteJob {
public:
  FakeDeleteJob(QKeychain::Error err, std::function<void()> onSuccess)
      : m_error(err), m_onSuccess(std::move(onSuccess)) {}

  void start() override {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          if (m_error == QKeychain::NoError && m_onSuccess) {
            m_onSuccess();
          }
          emit finished();
        },
        Qt::QueuedConnection);
  }
  [[nodiscard]] QKeychain::Error error() const override { return m_error; }
  [[nodiscard]] QString errorString() const override {
    return QStringLiteral("simulated");
  }

private:
  QKeychain::Error m_error;
  std::function<void()> m_onSuccess;
};

// A job that never emits finished(), used to test destruction while a
// request is still outstanding.
class HangingReadJob final : public IKeychainReadJob {
public:
  void start() override {}
  [[nodiscard]] QKeychain::Error error() const override {
    return QKeychain::NoError;
  }
  [[nodiscard]] QString errorString() const override { return {}; }
  [[nodiscard]] QString textData() const override { return {}; }
};

// ─── Fake IKeychainJobFactory ──────────────────────────────────────────────
//
// Backs reads/writes/deletes with an in-memory map keyed by (service, key),
// so save/read/delete/update sequences exercise real persistence semantics
// (EntryNotFound for an absent key, etc.) without any real backend.

class FakeKeychainJobFactory final : public IKeychainJobFactory {
public:
  void setUnavailable(bool unavailable) { m_unavailable = unavailable; }
  void setAccessDenied(bool denied) { m_accessDenied = denied; }
  // Forces the next single operation (of any kind) to fail with |err|,
  // then reverts to normal in-memory-map behaviour.
  void injectNextError(QKeychain::Error err) { m_injectedError = err; }
  void useHangingReadJob(bool hang) { m_hangNextRead = hang; }
  // Directly seeds the in-memory store with |value| for (service, key),
  // bypassing QtKeychainTokenStore::saveToken()'s own empty/whitespace
  // validation entirely. This simulates a corrupt or externally-tampered
  // keyring entry (production saveToken() never persists a blank token
  // itself), so tests can prove readToken() defends against a backend that
  // nonetheless returns one.
  void seedStoredToken(const QString &service, const QString &key,
                       const QString &value) {
    m_store.insert({service, key}, value);
  }

  [[nodiscard]] std::unique_ptr<IKeychainReadJob>
  createReadJob(const QString &service, const QString &key) override {
    lastService = service;
    lastReadKey = key;
    if (m_hangNextRead) {
      m_hangNextRead = false;
      return std::make_unique<HangingReadJob>();
    }
    if (auto err = takeError()) {
      return std::make_unique<FakeReadJob>(*err, QString{});
    }
    const auto it = m_store.constFind({service, key});
    if (it == m_store.constEnd()) {
      return std::make_unique<FakeReadJob>(QKeychain::EntryNotFound, QString{});
    }
    return std::make_unique<FakeReadJob>(QKeychain::NoError, it.value());
  }

  [[nodiscard]] std::unique_ptr<IKeychainWriteJob>
  createWriteJob(const QString &service, const QString &key) override {
    lastService = service;
    lastWriteKey = key;
    if (auto err = takeError()) {
      return std::make_unique<FakeWriteJob>(*err, nullptr);
    }
    return std::make_unique<FakeWriteJob>(
        QKeychain::NoError, [this, service, key](const QString &data) {
          m_store.insert({service, key}, data);
        });
  }

  [[nodiscard]] std::unique_ptr<IKeychainDeleteJob>
  createDeleteJob(const QString &service, const QString &key) override {
    lastService = service;
    lastDeleteKey = key;
    if (auto err = takeError()) {
      return std::make_unique<FakeDeleteJob>(*err, nullptr);
    }
    const auto pairKey = qMakePair(service, key);
    if (!m_store.contains(pairKey)) {
      return std::make_unique<FakeDeleteJob>(QKeychain::EntryNotFound, nullptr);
    }
    return std::make_unique<FakeDeleteJob>(
        QKeychain::NoError, [this, pairKey]() { m_store.remove(pairKey); });
  }

  QString lastService;
  QString lastReadKey;
  QString lastWriteKey;
  QString lastDeleteKey;

private:
  std::optional<QKeychain::Error> takeError() {
    if (m_unavailable) {
      return QKeychain::NoBackendAvailable;
    }
    if (m_accessDenied) {
      return QKeychain::AccessDenied;
    }
    if (m_injectedError) {
      const QKeychain::Error err = *m_injectedError;
      m_injectedError.reset();
      return err;
    }
    return std::nullopt;
  }

  QHash<QPair<QString, QString>, QString> m_store;
  bool m_unavailable{false};
  bool m_accessDenied{false};
  bool m_hangNextRead{false};
  std::optional<QKeychain::Error> m_injectedError;
};

// ─── Helpers ────────────────────────────────────────────────────────────

QString newProfileId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// Runs an async token-store operation and returns its result, or fails the
// calling test (via the returned optional being empty) if no callback fires
// within the deadline.
std::optional<TokenStoreResult>
runOp(const std::function<void(ITokenStore::ResultCallback)> &invoke) {
  std::optional<TokenStoreResult> captured;
  invoke(
      [&captured](TokenStoreResult result) { captured = std::move(result); });
  const QDeadlineTimer deadline(2000);
  while (!captured.has_value() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return captured;
}

} // namespace

class TokenStoreTests final : public QObject {
  Q_OBJECT

private slots:
  void saveThenReadRoundTrip();
  void updateOverwritesPreviousToken();
  void readMissingProfileIsNotFound();
  void readBlankStoredTokenIsBackendError_data();
  void readBlankStoredTokenIsBackendError();
  void deleteRemovesToken();
  void deleteMissingProfileIsIdempotentSuccess();
  void backendUnavailableIsTypedFailure();
  void accessDeniedIsTypedFailure();
  void otherBackendErrorIsTypedFailure();
  void invalidProfileIdRejected();
  void nilProfileIdRejected();
  void emptyTokenRejectedOnSave();
  void whitespaceTokenRejectedOnSave();
  void concurrentProfilesAreIsolated();
  void destructionSuppressesPendingCallback();
  void callbacksAreAsynchronous();
  void diagnosticsNeverContainToken();
  void productionConstructorLinksAndConstructs();
};

void TokenStoreTests::saveThenReadRoundTrip() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileId = newProfileId();

  const auto saveResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, QStringLiteral("secret-token-abc"),
                    std::move(cb));
  });
  QVERIFY(saveResult.has_value());
  QCOMPARE(saveResult->outcome, TokenStoreOutcome::Success);

  const auto readResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, std::move(cb));
  });
  QVERIFY(readResult.has_value());
  QCOMPARE(readResult->outcome, TokenStoreOutcome::Success);
  QCOMPARE(readResult->token, QStringLiteral("secret-token-abc"));
}

void TokenStoreTests::updateOverwritesPreviousToken() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileId = newProfileId();

  runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, QStringLiteral("first-token"), std::move(cb));
  });
  runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, QStringLiteral("second-token"), std::move(cb));
  });

  const auto readResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, std::move(cb));
  });
  QVERIFY(readResult.has_value());
  QCOMPARE(readResult->outcome, TokenStoreOutcome::Success);
  QCOMPARE(readResult->token, QStringLiteral("second-token"));
}

void TokenStoreTests::readMissingProfileIsNotFound() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(newProfileId(), std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::NotFound);
  QVERIFY(result->token.isEmpty());
}

void TokenStoreTests::readBlankStoredTokenIsBackendError() {
  // saveToken() rejects empty/whitespace-only tokens outright (see
  // emptyTokenRejectedOnSave/whitespaceTokenRejectedOnSave below), so a
  // successfully-read blank token can only happen via a corrupt or
  // externally-tampered keyring entry. readToken() must not surface that
  // as a usable Success -- production code that treats Success as "signed
  // in" must never receive an empty token string.
  QFETCH(QString, storedValue);

  auto factory = std::make_unique<FakeKeychainJobFactory>();
  auto *rawFactory = factory.get();
  const QString profileId = newProfileId();
  rawFactory->seedStoredToken(QtKeychainTokenStore::serviceName(), profileId,
                              storedValue);
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::BackendError);
  QVERIFY(result->token.isEmpty());
  QCOMPARE(result->diagnostic,
           QStringLiteral(
               "secure storage returned an empty or whitespace-only token"));
}

void TokenStoreTests::readBlankStoredTokenIsBackendError_data() {
  QTest::addColumn<QString>("storedValue");
  QTest::newRow("empty") << QString();
  QTest::newRow("whitespace-only") << QStringLiteral("   \t  ");
}

void TokenStoreTests::deleteRemovesToken() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileId = newProfileId();

  runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, QStringLiteral("some-token"), std::move(cb));
  });

  const auto deleteResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.deleteToken(profileId, std::move(cb));
  });
  QVERIFY(deleteResult.has_value());
  QCOMPARE(deleteResult->outcome, TokenStoreOutcome::Success);

  const auto readResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, std::move(cb));
  });
  QVERIFY(readResult.has_value());
  QCOMPARE(readResult->outcome, TokenStoreOutcome::NotFound);
}

void TokenStoreTests::deleteMissingProfileIsIdempotentSuccess() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.deleteToken(newProfileId(), std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::Success);
}

void TokenStoreTests::backendUnavailableIsTypedFailure() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  factory->setUnavailable(true);
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(newProfileId(), std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::Unavailable);
  QVERIFY(!result->diagnostic.isEmpty());
}

void TokenStoreTests::accessDeniedIsTypedFailure() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  factory->setAccessDenied(true);
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(newProfileId(), QStringLiteral("token"), std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::AccessDenied);
}

void TokenStoreTests::otherBackendErrorIsTypedFailure() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  factory->injectNextError(QKeychain::OtherError);
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(newProfileId(), std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::BackendError);
}

void TokenStoreTests::invalidProfileIdRejected() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(QStringLiteral("not-a-uuid"), std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::InvalidInput);
}

void TokenStoreTests::nilProfileIdRejected() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(QStringLiteral("00000000-0000-0000-0000-000000000000"),
                    std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::InvalidInput);
}

void TokenStoreTests::emptyTokenRejectedOnSave() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(newProfileId(), QString{}, std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::InvalidInput);
}

void TokenStoreTests::whitespaceTokenRejectedOnSave() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(newProfileId(), QStringLiteral("   \t  "), std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::InvalidInput);
}

void TokenStoreTests::concurrentProfilesAreIsolated() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileA = newProfileId();
  const QString profileB = newProfileId();

  std::optional<TokenStoreResult> resultA;
  std::optional<TokenStoreResult> resultB;
  store.saveToken(profileA, QStringLiteral("token-a"),
                  [&resultA](TokenStoreResult r) { resultA = std::move(r); });
  store.saveToken(profileB, QStringLiteral("token-b"),
                  [&resultB](TokenStoreResult r) { resultB = std::move(r); });

  const QDeadlineTimer deadline(2000);
  while ((!resultA.has_value() || !resultB.has_value()) &&
         !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  QVERIFY(resultA.has_value());
  QVERIFY(resultB.has_value());
  QCOMPARE(resultA->outcome, TokenStoreOutcome::Success);
  QCOMPARE(resultB->outcome, TokenStoreOutcome::Success);

  const auto readA = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileA, std::move(cb));
  });
  const auto readB = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileB, std::move(cb));
  });
  QCOMPARE(readA->token, QStringLiteral("token-a"));
  QCOMPARE(readB->token, QStringLiteral("token-b"));
}

void TokenStoreTests::destructionSuppressesPendingCallback() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  factory->useHangingReadJob(true);

  int callCount = 0;
  {
    QtKeychainTokenStore store(std::move(factory));
    store.readToken(newProfileId(),
                    [&callCount](TokenStoreResult) { ++callCount; });
    // The store is destroyed here while the (hanging) job is still pending.
  }

  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCOMPARE(callCount, 0);
}

void TokenStoreTests::callbacksAreAsynchronous() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  bool calledBack = false;
  store.saveToken(newProfileId(), QStringLiteral("token"),
                  [&calledBack](TokenStoreResult) { calledBack = true; });
  // saveToken() must return without having already invoked the callback.
  QVERIFY(!calledBack);

  const QDeadlineTimer deadline(2000);
  while (!calledBack && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  QVERIFY(calledBack);
}

void TokenStoreTests::diagnosticsNeverContainToken() {
  const QString sentinelToken = QStringLiteral("sentinel-secret-xyz-987");
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileId = newProfileId();

  const auto saveResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, sentinelToken, std::move(cb));
  });
  QVERIFY(saveResult.has_value());
  QVERIFY(!saveResult->diagnostic.contains(sentinelToken));

  const auto readResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, std::move(cb));
  });
  QVERIFY(readResult.has_value());
  // The token itself is expected in TokenStoreResult::token, but never in
  // the human-readable diagnostic field.
  QVERIFY(!readResult->diagnostic.contains(sentinelToken));
}

void TokenStoreTests::productionConstructorLinksAndConstructs() {
  // Constructs the real production adapter (using QtKeychainJobFactory, not
  // a fake) to prove the production seam links and constructs correctly.
  // The constructor itself performs no secure-storage I/O -- only
  // readToken()/saveToken()/deleteToken() do -- so this remains
  // deterministic and touches no real keyring.
  auto store = std::make_unique<QtKeychainTokenStore>();
  QVERIFY(store != nullptr);
  QCOMPARE(QtKeychainTokenStore::serviceName(),
           QStringLiteral("app.arkhamhorror.auth.token"));
}

QTEST_GUILESS_MAIN(TokenStoreTests)

#include "TokenStoreTests.moc"
