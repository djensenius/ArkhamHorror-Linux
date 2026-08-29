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
#include "TokenEnvelope.h"

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

  // Returns the exact opaque raw string currently stored for (service,
  // key), or std::nullopt if nothing is stored. Test-only: lets a test
  // inspect the serialized envelope format (e.g. its "AHKV1:" prefix and
  // framed identity) directly, distinct from merely round-tripping it back
  // through readToken(). Never used by production code.
  [[nodiscard]] std::optional<QString>
  rawStoredValue(const QString &service, const QString &key) const {
    const auto it = m_store.constFind({service, key});
    if (it == m_store.constEnd()) {
      return std::nullopt;
    }
    return it.value();
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

// Arbitrary, distinct opaque endpoint-identity strings. QtKeychainTokenStore
// never interprets this string's structure itself (see
// ServerProfile::credentialEndpointIdentity(), which is the real production
// source of these values) -- it is only ever compared for exact equality --
// so a plain literal is sufficient here to exercise the store's own binding
// logic in isolation.
QString endpointIdentityA() { return QStringLiteral("https|example.com|443|"); }
QString endpointIdentityB() {
  return QStringLiteral("https|other-example.com|443|");
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
  void emptyEndpointIdentityRejectedOnSave();
  void emptyEndpointIdentityRejectedOnRead();
  void concurrentProfilesAreIsolated();
  void destructionSuppressesPendingCallback();
  void callbacksAreAsynchronous();
  void diagnosticsNeverContainToken();
  void productionConstructorLinksAndConstructs();

  // ─── Endpoint-bound envelope (durable credential binding) ───────────

  void saveWritesVersionedEndpointBoundEnvelope();
  void readMatchedEndpointBindingSucceeds();
  void readMismatchedEndpointBindingIsRejectedWithNoToken();
  void readLegacyUnboundRawTokenIsRejectedWithNoToken();
  void readMalformedEnvelopeIsRejectedWithNoToken_data();
  void readMalformedEnvelopeIsRejectedWithNoToken();
  void updatedEndpointBindingReplacesPreviousBinding();
  void diagnosticsNeverContainEndpointIdentityOrTokenForBindingOutcomes();

  // ─── TokenEnvelope free-function unit tests (bypassing async I/O) ───

  void envelopeRoundTripsIdentityAndToken();
  void envelopeRoundTripsContentContainingDelimiterCharacters();
  void envelopeParseRejectsLegacyRawText();
  void envelopeParseRejectsUnsupportedVersion();
  void envelopeParseRejectsTruncatedIdentityLength();
  void envelopeParseRejectsEmptyToken();
  void envelopeParseRejectsPrefixWithNoVersionDigits();
};

void TokenStoreTests::saveThenReadRoundTrip() {
  // See tests/AuthClientTests.cpp's authenticateSendsExpectedRequest() for
  // why this is bound to a named variable before use in the QVERIFY
  // expression below: Qt's QVERIFY/QCOMPARE macros always log the
  // literal, stringified "#statement" expression text on failure.
  const QString expectedToken = QStringLiteral("secret-token-abc");

  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileId = newProfileId();

  const auto saveResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, expectedToken, endpointIdentityA(),
                    std::move(cb));
  });
  QVERIFY(saveResult.has_value());
  QCOMPARE(saveResult->outcome, TokenStoreOutcome::Success);

  const auto readResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, endpointIdentityA(), std::move(cb));
  });
  QVERIFY(readResult.has_value());
  QCOMPARE(readResult->outcome, TokenStoreOutcome::Success);
  // QVERIFY (not QCOMPARE): a failure must never print the actual token
  // value into test/CI logs.
  QVERIFY(readResult->token == expectedToken);
}

void TokenStoreTests::updateOverwritesPreviousToken() {
  // See saveThenReadRoundTrip() above for why these are bound to named
  // variables before use in the QVERIFY expressions below.
  const QString firstToken = QStringLiteral("first-token");
  const QString secondToken = QStringLiteral("second-token");

  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileId = newProfileId();

  const auto firstSaveResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, firstToken, endpointIdentityA(), std::move(cb));
  });
  QVERIFY(firstSaveResult.has_value());
  QCOMPARE(firstSaveResult->outcome, TokenStoreOutcome::Success);

  const auto secondSaveResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, secondToken, endpointIdentityA(), std::move(cb));
  });
  QVERIFY(secondSaveResult.has_value());
  QCOMPARE(secondSaveResult->outcome, TokenStoreOutcome::Success);

  const auto readResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, endpointIdentityA(), std::move(cb));
  });
  QVERIFY(readResult.has_value());
  QCOMPARE(readResult->outcome, TokenStoreOutcome::Success);
  // QVERIFY (not QCOMPARE): a failure must never print the actual token
  // value into test/CI logs.
  QVERIFY(readResult->token == secondToken);
}

void TokenStoreTests::readMissingProfileIsNotFound() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(newProfileId(), endpointIdentityA(), std::move(cb));
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
    store.readToken(profileId, endpointIdentityA(), std::move(cb));
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

  const auto seedResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, QStringLiteral("some-token"),
                    endpointIdentityA(), std::move(cb));
  });
  QVERIFY(seedResult.has_value());
  QCOMPARE(seedResult->outcome, TokenStoreOutcome::Success);

  const auto deleteResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.deleteToken(profileId, std::move(cb));
  });
  QVERIFY(deleteResult.has_value());
  QCOMPARE(deleteResult->outcome, TokenStoreOutcome::Success);

  const auto readResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, endpointIdentityA(), std::move(cb));
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
    store.readToken(newProfileId(), endpointIdentityA(), std::move(cb));
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
    store.saveToken(newProfileId(), QStringLiteral("token"),
                    endpointIdentityA(), std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::AccessDenied);
}

void TokenStoreTests::otherBackendErrorIsTypedFailure() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  factory->injectNextError(QKeychain::OtherError);
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(newProfileId(), endpointIdentityA(), std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::BackendError);
}

void TokenStoreTests::invalidProfileIdRejected() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(QStringLiteral("not-a-uuid"), endpointIdentityA(),
                    std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::InvalidInput);
}

void TokenStoreTests::nilProfileIdRejected() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(QStringLiteral("00000000-0000-0000-0000-000000000000"),
                    endpointIdentityA(), std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::InvalidInput);
}

void TokenStoreTests::emptyTokenRejectedOnSave() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(newProfileId(), QString{}, endpointIdentityA(),
                    std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::InvalidInput);
}

void TokenStoreTests::whitespaceTokenRejectedOnSave() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(newProfileId(), QStringLiteral("   \t  "),
                    endpointIdentityA(), std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::InvalidInput);
}

void TokenStoreTests::emptyEndpointIdentityRejectedOnSave() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(newProfileId(), QStringLiteral("some-token"), QString{},
                    std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::InvalidInput);
}

void TokenStoreTests::emptyEndpointIdentityRejectedOnRead() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));

  const auto result = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(newProfileId(), QString{}, std::move(cb));
  });
  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, TokenStoreOutcome::InvalidInput);
}

void TokenStoreTests::concurrentProfilesAreIsolated() {
  // See saveThenReadRoundTrip() above for why these are bound to named
  // variables before use in the QVERIFY expressions below.
  const QString tokenA = QStringLiteral("token-a");
  const QString tokenB = QStringLiteral("token-b");

  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileA = newProfileId();
  const QString profileB = newProfileId();

  std::optional<TokenStoreResult> resultA;
  std::optional<TokenStoreResult> resultB;
  store.saveToken(profileA, tokenA, endpointIdentityA(),
                  [&resultA](TokenStoreResult r) { resultA = std::move(r); });
  store.saveToken(profileB, tokenB, endpointIdentityA(),
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
    store.readToken(profileA, endpointIdentityA(), std::move(cb));
  });
  const auto readB = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileB, endpointIdentityA(), std::move(cb));
  });
  QVERIFY(readA.has_value());
  QVERIFY(readB.has_value());
  // QVERIFY (not QCOMPARE): a failure must never print the actual token
  // value into test/CI logs.
  QVERIFY(readA->token == tokenA);
  QVERIFY(readB->token == tokenB);
}

void TokenStoreTests::destructionSuppressesPendingCallback() {
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  factory->useHangingReadJob(true);

  int callCount = 0;
  {
    QtKeychainTokenStore store(std::move(factory));
    store.readToken(newProfileId(), endpointIdentityA(),
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
  store.saveToken(newProfileId(), QStringLiteral("token"), endpointIdentityA(),
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
    store.saveToken(profileId, sentinelToken, endpointIdentityA(),
                    std::move(cb));
  });
  QVERIFY(saveResult.has_value());
  QVERIFY(!saveResult->diagnostic.contains(sentinelToken));

  const auto readResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, endpointIdentityA(), std::move(cb));
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

// ─── Endpoint-bound envelope (durable credential binding) ────────────────
//
// These exercise QtKeychainTokenStore's own envelope serialize/verify
// sequencing end-to-end (through the fake job factory's real in-memory
// map), distinct from the direct TokenEnvelope free-function unit tests
// below, which bypass the async keychain plumbing entirely.

void TokenStoreTests::saveWritesVersionedEndpointBoundEnvelope() {
  const QString sentinelToken = QStringLiteral("sentinel-secret-envelope-1");
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  auto *rawFactory = factory.get();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileId = newProfileId();

  const auto saveResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, sentinelToken, endpointIdentityA(),
                    std::move(cb));
  });
  QVERIFY(saveResult.has_value());
  QCOMPARE(saveResult->outcome, TokenStoreOutcome::Success);

  const auto raw = rawFactory->rawStoredValue(
      QtKeychainTokenStore::serviceName(), profileId);
  QVERIFY(raw.has_value());
  // The persisted payload must be the versioned envelope -- never the bare
  // token -- and must carry both the endpoint identity and the token
  // somewhere inside it (parseTokenEnvelope() below proves the EXACT
  // framing; this proves the raw persisted bytes are not simply the plain
  // token).
  QVERIFY(raw->startsWith(QStringLiteral("AHKV1:")));
  QVERIFY(raw->contains(endpointIdentityA()));
  QVERIFY(raw->contains(sentinelToken));
  QVERIFY(*raw != sentinelToken);

  const TokenEnvelopeParseResult parsed = parseTokenEnvelope(*raw);
  QCOMPARE(parsed.outcome, TokenEnvelopeParseOutcome::Parsed);
  QCOMPARE(parsed.endpointIdentity, endpointIdentityA());
  QVERIFY(parsed.token == sentinelToken);
}

void TokenStoreTests::readMatchedEndpointBindingSucceeds() {
  const QString expectedToken = QStringLiteral("matched-binding-token");
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileId = newProfileId();

  const auto saveResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, expectedToken, endpointIdentityA(),
                    std::move(cb));
  });
  QVERIFY(saveResult.has_value());
  QCOMPARE(saveResult->outcome, TokenStoreOutcome::Success);

  const auto readResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, endpointIdentityA(), std::move(cb));
  });
  QVERIFY(readResult.has_value());
  QCOMPARE(readResult->outcome, TokenStoreOutcome::Success);
  QVERIFY(readResult->token == expectedToken);
}

void TokenStoreTests::readMismatchedEndpointBindingIsRejectedWithNoToken() {
  const QString savedToken = QStringLiteral("bound-to-endpoint-a-token");
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileId = newProfileId();

  const auto saveResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, savedToken, endpointIdentityA(), std::move(cb));
  });
  QVERIFY(saveResult.has_value());
  QCOMPARE(saveResult->outcome, TokenStoreOutcome::Success);

  // Read back expecting a DIFFERENT endpoint identity than the one this
  // token was actually saved for -- simulating the same profileId() now
  // being associated with a different server (persisted URL changed, or
  // the UUID was reused by a different profile).
  const auto readResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, endpointIdentityB(), std::move(cb));
  });
  QVERIFY(readResult.has_value());
  QCOMPARE(readResult->outcome, TokenStoreOutcome::BindingMismatch);
  QVERIFY(readResult->token.isEmpty());
  // The diagnostic must never leak WHICH identity was expected/found nor
  // the token itself; see
  // diagnosticsNeverContainEndpointIdentityOrTokenForBindingOutcomes().
  QVERIFY(!readResult->diagnostic.contains(savedToken));
}

void TokenStoreTests::readLegacyUnboundRawTokenIsRejectedWithNoToken() {
  // Seeds a plain, pre-envelope raw token directly into the backing map --
  // exactly what a token saved by a release predating endpoint binding
  // would look like -- bypassing saveToken()'s own envelope wrapping
  // entirely.
  const QString legacyRawToken = QStringLiteral("legacy-pre-envelope-token");
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  auto *rawFactory = factory.get();
  const QString profileId = newProfileId();
  rawFactory->seedStoredToken(QtKeychainTokenStore::serviceName(), profileId,
                              legacyRawToken);
  QtKeychainTokenStore store(std::move(factory));

  const auto readResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, endpointIdentityA(), std::move(cb));
  });
  QVERIFY(readResult.has_value());
  QCOMPARE(readResult->outcome, TokenStoreOutcome::LegacyUnbound);
  QVERIFY(readResult->token.isEmpty());
  QVERIFY(!readResult->diagnostic.contains(legacyRawToken));
}

void TokenStoreTests::readMalformedEnvelopeIsRejectedWithNoToken_data() {
  QTest::addColumn<QString>("storedValue");
  QTest::newRow("unsupported-version")
      << QStringLiteral("AHKV2:5:hosta") + QStringLiteral("token-abc");
  QTest::newRow("truncated-identity-length")
      << QStringLiteral("AHKV1:999:tooshort");
  QTest::newRow("non-digit-length")
      << QStringLiteral("AHKV1:abc:identitytoken");
  QTest::newRow("no-version-terminator")
      << QStringLiteral("AHKV1identitytoken");
  QTest::newRow("empty-token-remainder") << QStringLiteral("AHKV1:5:hosta");
}

void TokenStoreTests::readMalformedEnvelopeIsRejectedWithNoToken() {
  QFETCH(QString, storedValue);

  auto factory = std::make_unique<FakeKeychainJobFactory>();
  auto *rawFactory = factory.get();
  const QString profileId = newProfileId();
  rawFactory->seedStoredToken(QtKeychainTokenStore::serviceName(), profileId,
                              storedValue);
  QtKeychainTokenStore store(std::move(factory));

  const auto readResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, endpointIdentityA(), std::move(cb));
  });
  QVERIFY(readResult.has_value());
  QCOMPARE(readResult->outcome, TokenStoreOutcome::Malformed);
  QVERIFY(readResult->token.isEmpty());
}

void TokenStoreTests::updatedEndpointBindingReplacesPreviousBinding() {
  // A profile whose endpoint changes and is re-saved (e.g. after the
  // coordinator's required delete-then-fresh-auth sequence) must bind the
  // NEW token to the NEW identity only -- reading with the OLD identity
  // must no longer succeed, and reading with the NEW identity must.
  const QString firstToken = QStringLiteral("token-for-endpoint-a");
  const QString secondToken = QStringLiteral("token-for-endpoint-b");
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileId = newProfileId();

  const auto firstSave = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, firstToken, endpointIdentityA(), std::move(cb));
  });
  QVERIFY(firstSave.has_value());
  QCOMPARE(firstSave->outcome, TokenStoreOutcome::Success);

  const auto secondSave = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, secondToken, endpointIdentityB(), std::move(cb));
  });
  QVERIFY(secondSave.has_value());
  QCOMPARE(secondSave->outcome, TokenStoreOutcome::Success);

  const auto readOld = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, endpointIdentityA(), std::move(cb));
  });
  QVERIFY(readOld.has_value());
  QCOMPARE(readOld->outcome, TokenStoreOutcome::BindingMismatch);
  QVERIFY(readOld->token.isEmpty());

  const auto readNew = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, endpointIdentityB(), std::move(cb));
  });
  QVERIFY(readNew.has_value());
  QCOMPARE(readNew->outcome, TokenStoreOutcome::Success);
  QVERIFY(readNew->token == secondToken);
}

void TokenStoreTests::
    diagnosticsNeverContainEndpointIdentityOrTokenForBindingOutcomes() {
  const QString sentinelToken = QStringLiteral("sentinel-binding-secret-42");
  auto factory = std::make_unique<FakeKeychainJobFactory>();
  QtKeychainTokenStore store(std::move(factory));
  const QString profileId = newProfileId();

  const auto saveResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.saveToken(profileId, sentinelToken, endpointIdentityA(),
                    std::move(cb));
  });
  QVERIFY(saveResult.has_value());

  const auto mismatchResult = runOp([&](ITokenStore::ResultCallback cb) {
    store.readToken(profileId, endpointIdentityB(), std::move(cb));
  });
  QVERIFY(mismatchResult.has_value());
  QVERIFY(!mismatchResult->diagnostic.contains(sentinelToken));
  QVERIFY(!mismatchResult->diagnostic.contains(endpointIdentityA()));
  QVERIFY(!mismatchResult->diagnostic.contains(endpointIdentityB()));
}

// ─── TokenEnvelope free-function unit tests ──────────────────────────────
//
// These call serializeTokenEnvelope()/parseTokenEnvelope() directly,
// bypassing the async keychain job plumbing entirely, for fast, isolated
// coverage of the wire format itself.

void TokenStoreTests::envelopeRoundTripsIdentityAndToken() {
  const QString identity = QStringLiteral("https|example.com|443|/api");
  const QString token = QStringLiteral("a-normal-looking-token-value");

  const QString serialized = serializeTokenEnvelope(identity, token);
  QVERIFY(serialized.startsWith(QStringLiteral("AHKV1:")));

  const TokenEnvelopeParseResult parsed = parseTokenEnvelope(serialized);
  QCOMPARE(parsed.outcome, TokenEnvelopeParseOutcome::Parsed);
  QCOMPARE(parsed.endpointIdentity, identity);
  QVERIFY(parsed.token == token);
}

void TokenStoreTests::envelopeRoundTripsContentContainingDelimiterCharacters() {
  // The identity string itself uses '|' internally, and a token is an
  // opaque backend-issued string that could in principle contain any
  // character including ':'. Both must still round-trip exactly, proving
  // the explicit-length framing never misreads either field's own content
  // as a boundary.
  const QString identity =
      QStringLiteral("https|host:extra|443|/weird:path|with|pipes");
  const QString token = QStringLiteral("token:with:colons|and|pipes");

  const QString serialized = serializeTokenEnvelope(identity, token);
  const TokenEnvelopeParseResult parsed = parseTokenEnvelope(serialized);
  QCOMPARE(parsed.outcome, TokenEnvelopeParseOutcome::Parsed);
  QCOMPARE(parsed.endpointIdentity, identity);
  QVERIFY(parsed.token == token);
}

void TokenStoreTests::envelopeParseRejectsLegacyRawText() {
  const TokenEnvelopeParseResult parsed =
      parseTokenEnvelope(QStringLiteral("just-a-plain-legacy-token"));
  QCOMPARE(parsed.outcome, TokenEnvelopeParseOutcome::LegacyUnbound);
  QVERIFY(parsed.token.isEmpty());
  QVERIFY(parsed.endpointIdentity.isEmpty());
}

void TokenStoreTests::envelopeParseRejectsUnsupportedVersion() {
  const QString serialized =
      serializeTokenEnvelope(QStringLiteral("host"), QStringLiteral("tok"));
  // serializeTokenEnvelope() always writes version 1; splice in an
  // unsupported version number instead.
  QString tampered = serialized;
  tampered.replace(QStringLiteral("AHKV1:"), QStringLiteral("AHKV2:"));
  const TokenEnvelopeParseResult parsed = parseTokenEnvelope(tampered);
  QCOMPARE(parsed.outcome, TokenEnvelopeParseOutcome::Malformed);
  QVERIFY(parsed.token.isEmpty());
}

void TokenStoreTests::envelopeParseRejectsTruncatedIdentityLength() {
  // Declares an identity length far longer than the actual remaining
  // payload.
  const TokenEnvelopeParseResult parsed =
      parseTokenEnvelope(QStringLiteral("AHKV1:9999:short"));
  QCOMPARE(parsed.outcome, TokenEnvelopeParseOutcome::Malformed);
  QVERIFY(parsed.token.isEmpty());
}

void TokenStoreTests::envelopeParseRejectsEmptyToken() {
  // A well-formed length prefix whose declared identity consumes the
  // ENTIRE remainder, leaving nothing for the token.
  const QString identity = QStringLiteral("host");
  const QString serialized =
      QStringLiteral("AHKV1:%1:%2").arg(identity.size()).arg(identity);
  const TokenEnvelopeParseResult parsed = parseTokenEnvelope(serialized);
  QCOMPARE(parsed.outcome, TokenEnvelopeParseOutcome::Malformed);
  QVERIFY(parsed.token.isEmpty());
}

void TokenStoreTests::envelopeParseRejectsPrefixWithNoVersionDigits() {
  const TokenEnvelopeParseResult parsed =
      parseTokenEnvelope(QStringLiteral("AHKV:5:hostatoken"));
  QCOMPARE(parsed.outcome, TokenEnvelopeParseOutcome::Malformed);
  QVERIFY(parsed.token.isEmpty());
}

QTEST_GUILESS_MAIN(TokenStoreTests)

#include "TokenStoreTests.moc"
