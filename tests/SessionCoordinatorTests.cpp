// Deterministic tests for SessionCoordinator, driven entirely through fake
// IProfileStore/ICapabilityProbe/ITokenStore/IAuthenticationClient
// implementations. No live service, keyring, or network connection is used.
//
// FakeTokenStore doubles as an invariant checker: it fails fatally if two
// operations are ever in flight for the same profile ID at once, which is
// exactly the per-profile FIFO guarantee SessionCoordinator must uphold
// (see SessionCoordinator::enqueueTokenOp).

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSignalSpy>
#include <QUuid>
#include <QtTest>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "AuthModels.h"
#include "IAuthenticationClient.h"
#include "ICapabilityProbe.h"
#include "IProfileStore.h"
#include "ITokenStore.h"
#include "ProbeResult.h"
#include "ServerProfile.h"
#include "SessionCoordinator.h"
#include "ValueOrError.h"

using namespace Arkham;

namespace {

// ─── Fixture / secret-free assertion helper ─────────────────────────────
//
// Every token/password/username/email fixture value below is bound to a
// named local variable and compared with QVERIFY(actual == expected) --
// never QCOMPARE, and never with the literal embedded in the assertion
// expression itself -- so Qt's on-failure "#statement" logging can never
// print a fixture secret, per this repository's established test
// convention (see AuthClientTests.cpp).

void pumpEventsUntil(const std::function<bool()> &predicate,
                     int timeoutMs = 2000) {
  const QDeadlineTimer deadline(timeoutMs);
  while (!predicate() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
}

// ─── Fake IProfileStore ──────────────────────────────────────────────────

class FakeProfileStore final : public IProfileStore {
public:
  QList<ServerProfile> profiles;
  QString selectedId;

  std::optional<QString> failLoadProfiles;
  std::optional<QString> failLoadSelectedId;
  std::optional<QString> failSaveProfiles;
  std::optional<QString> failSaveSelectedId;

  int saveProfilesCalls{0};
  int saveSelectedIdCalls{0};

  [[nodiscard]] ValueOrError<QList<ServerProfile>>
  loadProfiles() const override {
    if (failLoadProfiles) {
      return failure(*failLoadProfiles);
    }
    return profiles;
  }

  [[nodiscard]] ValueOrError<QString> loadSelectedProfileId() const override {
    if (failLoadSelectedId) {
      return failure(*failLoadSelectedId);
    }
    return selectedId;
  }

  [[nodiscard]] ValueOrError<bool>
  saveProfiles(const QList<ServerProfile> &newProfiles) override {
    ++saveProfilesCalls;
    if (failSaveProfiles) {
      return failure(*failSaveProfiles);
    }
    profiles = newProfiles;
    return true;
  }

  [[nodiscard]] ValueOrError<bool>
  saveSelectedProfileId(const QString &id) override {
    ++saveSelectedIdCalls;
    if (failSaveSelectedId) {
      return failure(*failSaveSelectedId);
    }
    selectedId = id;
    return true;
  }
};

// ─── Fake ICapabilityProbe + factory ─────────────────────────────────────

class FakeCapabilityProbe final : public ICapabilityProbe {
  Q_OBJECT
public:
  using ICapabilityProbe::ICapabilityProbe;

  [[nodiscard]] int probeCallCount() const { return m_probeCallCount; }
  [[nodiscard]] const ServerProfile &lastProfile() const {
    return m_lastProfile;
  }

  void probe(const ServerProfile &profile) override {
    ++m_probeCallCount;
    m_lastProfile = profile;
  }

  // Test control: fires finished() synchronously. Real probes complete
  // asynchronously; here the test itself controls exactly when, which is
  // sufficient since the coordinator's probe() call has already returned
  // by the time a test invokes this.
  void complete(ProbeResult result) { emit finished(std::move(result)); }

private:
  int m_probeCallCount{0};
  ServerProfile m_lastProfile;
};

class FakeProbeFactory {
public:
  [[nodiscard]] SessionCoordinator::ProbeFactory asFactory() {
    return [this] {
      ++m_totalCreated;
      auto probe = std::make_unique<FakeCapabilityProbe>();
      m_instances.append(probe.get());
      return probe;
    };
  }

  // Returns the most recently created instance still alive, or nullptr.
  // Destroying a probe (see SessionCoordinator's destroy-and-recreate
  // cancellation strategy) leaves this correctly reflecting the new
  // current instance, since QPointer clears itself automatically.
  [[nodiscard]] FakeCapabilityProbe *current() {
    while (!m_instances.isEmpty() && m_instances.last().isNull()) {
      m_instances.removeLast();
    }
    return m_instances.isEmpty() ? nullptr : m_instances.last().data();
  }

  [[nodiscard]] int totalCreated() const { return m_totalCreated; }

private:
  QList<QPointer<FakeCapabilityProbe>> m_instances;
  int m_totalCreated{0};
};

// ─── Fake ITokenStore ─────────────────────────────────────────────────────

class FakeTokenStore final : public QObject, public ITokenStore {
  Q_OBJECT
public:
  struct Call {
    QString profileId;
    QString kind; // "read" | "save" | "delete"
    QString token;
  };
  QList<Call> calls;

  void readToken(const QString &profileId, ResultCallback callback) override {
    calls.append({profileId, QStringLiteral("read"), QString()});
    registerPending(profileId, std::move(callback));
  }

  void saveToken(const QString &profileId, const QString &token,
                 ResultCallback callback) override {
    calls.append({profileId, QStringLiteral("save"), token});
    registerPending(profileId, std::move(callback));
  }

  void deleteToken(const QString &profileId, ResultCallback callback) override {
    calls.append({profileId, QStringLiteral("delete"), QString()});
    registerPending(profileId, std::move(callback));
  }

  [[nodiscard]] bool hasPending(const QString &profileId) const {
    return m_pending.contains(profileId);
  }

  // Releases the currently pending operation for |profileId| asynchronously
  // (Qt::QueuedConnection), mirroring the real ITokenStore contract that no
  // callback is ever invoked reentrantly from within the call that
  // requested the operation.
  void complete(const QString &profileId, TokenStoreResult result) {
    auto it = m_pending.find(profileId);
    if (it == m_pending.end()) {
      qFatal("FakeTokenStore: no pending operation for this profile");
    }
    ResultCallback callback = it.value();
    m_pending.erase(it);
    QMetaObject::invokeMethod(
        this, [callback, result]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }

private:
  void registerPending(const QString &profileId, ResultCallback callback) {
    if (m_pending.contains(profileId)) {
      // SessionCoordinator's per-profile FIFO invariant requires exactly
      // one in-flight ITokenStore operation per profile ID at a time; a
      // second concurrent call for the same profile means that invariant
      // was violated.
      qFatal("FakeTokenStore: overlapping token-store operation for the "
             "same profile ID");
    }
    m_pending.insert(profileId, std::move(callback));
  }

  QHash<QString, ResultCallback> m_pending;
};

// ─── Fake IAuthenticationClient ──────────────────────────────────────────

class FakeAuthClient final : public QObject, public IAuthenticationClient {
  Q_OBJECT
public:
  enum class CallKind { Authenticate, Register, WhoAmI };
  struct Call {
    CallKind kind;
    ServerProfile profile;
    QString token; // whoAmI only
  };
  QList<Call> calls;

  AuthRequestHandle authenticate(const ServerProfile &profile,
                                 const AuthenticateRequest & /*request*/,
                                 AuthTokenCallback callback) override {
    calls.append({CallKind::Authenticate, profile, QString()});
    const AuthRequestHandle handle = nextHandle();
    m_pendingToken.insert(handle.id, std::move(callback));
    return handle;
  }

  AuthRequestHandle registerAccount(const ServerProfile &profile,
                                    const RegisterRequest & /*request*/,
                                    AuthTokenCallback callback) override {
    calls.append({CallKind::Register, profile, QString()});
    const AuthRequestHandle handle = nextHandle();
    m_pendingToken.insert(handle.id, std::move(callback));
    return handle;
  }

  AuthRequestHandle whoAmI(const ServerProfile &profile, const QString &token,
                           CurrentUserCallback callback) override {
    calls.append({CallKind::WhoAmI, profile, token});
    const AuthRequestHandle handle = nextHandle();
    m_pendingUser.insert(handle.id, std::move(callback));
    return handle;
  }

  void cancel(AuthRequestHandle handle) override {
    if (auto it = m_pendingToken.find(handle.id); it != m_pendingToken.end()) {
      AuthTokenCallback callback = it.value();
      m_pendingToken.erase(it);
      AuthResult<AuthToken> cancelled;
      cancelled.outcome = AuthOutcome::Cancelled;
      cancelled.diagnostic = QStringLiteral("cancelled");
      QMetaObject::invokeMethod(
          this, [callback, cancelled]() mutable { callback(cancelled); },
          Qt::QueuedConnection);
      return;
    }
    if (auto it = m_pendingUser.find(handle.id); it != m_pendingUser.end()) {
      CurrentUserCallback callback = it.value();
      m_pendingUser.erase(it);
      AuthResult<CurrentUser> cancelled;
      cancelled.outcome = AuthOutcome::Cancelled;
      cancelled.diagnostic = QStringLiteral("cancelled");
      QMetaObject::invokeMethod(
          this, [callback, cancelled]() mutable { callback(cancelled); },
          Qt::QueuedConnection);
    }
  }

  [[nodiscard]] bool hasPendingToken(quint64 handleId) const {
    return m_pendingToken.contains(handleId);
  }
  [[nodiscard]] bool hasPendingUser(quint64 handleId) const {
    return m_pendingUser.contains(handleId);
  }

  void completeToken(quint64 handleId, AuthResult<AuthToken> result) {
    auto it = m_pendingToken.find(handleId);
    if (it == m_pendingToken.end()) {
      qFatal("FakeAuthClient: no pending token request for handle");
    }
    AuthTokenCallback callback = it.value();
    m_pendingToken.erase(it);
    QMetaObject::invokeMethod(
        this, [callback, result]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }

  void completeUser(quint64 handleId, AuthResult<CurrentUser> result) {
    auto it = m_pendingUser.find(handleId);
    if (it == m_pendingUser.end()) {
      qFatal("FakeAuthClient: no pending whoami request for handle");
    }
    CurrentUserCallback callback = it.value();
    m_pendingUser.erase(it);
    QMetaObject::invokeMethod(
        this, [callback, result]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }

private:
  AuthRequestHandle nextHandle() { return AuthRequestHandle{m_nextId++}; }

  quint64 m_nextId{1};
  QHash<quint64, AuthTokenCallback> m_pendingToken;
  QHash<quint64, CurrentUserCallback> m_pendingUser;
};

// ─── Result-fixture helpers ──────────────────────────────────────────────

ProbeResult compatibleProbeResult() {
  ProbeResult result;
  result.outcome = ProbeOutcome::Compatible;
  result.httpStatus = 200;
  return result;
}

ProbeResult legacyFallbackProbeResult() {
  ProbeResult result;
  result.outcome = ProbeOutcome::LegacyFallback;
  result.httpStatus = 404;
  return result;
}

ProbeResult incompatibleProbeResult() {
  ProbeResult result;
  result.outcome = ProbeOutcome::Incompatible;
  result.diagnostic = QStringLiteral("client too old");
  result.httpStatus = 200;
  return result;
}

ProbeResult networkErrorProbeResult() {
  ProbeResult result;
  result.outcome = ProbeOutcome::NetworkError;
  result.diagnostic = QStringLiteral("connection refused");
  return result;
}

TokenStoreResult notFoundResult() {
  return TokenStoreResult{TokenStoreOutcome::NotFound,
                          QStringLiteral("no token stored"), QString()};
}

TokenStoreResult successReadResult(const QString &token) {
  return TokenStoreResult{TokenStoreOutcome::Success, QStringLiteral("ok"),
                          token};
}

TokenStoreResult successWriteResult() {
  return TokenStoreResult{TokenStoreOutcome::Success, QStringLiteral("ok"),
                          QString()};
}

TokenStoreResult accessDeniedResult() {
  return TokenStoreResult{TokenStoreOutcome::AccessDenied,
                          QStringLiteral("keyring locked"), QString()};
}

TokenStoreResult backendErrorResult() {
  return TokenStoreResult{TokenStoreOutcome::BackendError,
                          QStringLiteral("backend failure"), QString()};
}

AuthResult<AuthToken> tokenSuccess(const QString &token) {
  AuthResult<AuthToken> result;
  result.outcome = AuthOutcome::Success;
  result.httpStatus = 200;
  result.value = AuthToken{token};
  return result;
}

AuthResult<AuthToken> tokenUnauthorized() {
  AuthResult<AuthToken> result;
  result.outcome = AuthOutcome::Unauthorized;
  result.httpStatus = 401;
  result.diagnostic = QStringLiteral("invalid credentials");
  return result;
}

AuthResult<AuthToken> tokenTransport() {
  AuthResult<AuthToken> result;
  result.outcome = AuthOutcome::Transport;
  result.diagnostic = QStringLiteral("connection reset");
  return result;
}

AuthResult<CurrentUser> userSuccess(const QString &username,
                                    const QString &email) {
  AuthResult<CurrentUser> result;
  result.outcome = AuthOutcome::Success;
  result.httpStatus = 200;
  result.value = CurrentUser{username, email, false, false};
  return result;
}

AuthResult<CurrentUser> userUnauthorized() {
  AuthResult<CurrentUser> result;
  result.outcome = AuthOutcome::Unauthorized;
  result.httpStatus = 401;
  result.diagnostic = QStringLiteral("token rejected");
  return result;
}

AuthResult<CurrentUser> userTransport() {
  AuthResult<CurrentUser> result;
  result.outcome = AuthOutcome::Transport;
  result.diagnostic = QStringLiteral("connection reset");
  return result;
}

// ─── Test harness ─────────────────────────────────────────────────────────
//
// Field order matters exactly as it does in production
// (AppSessionComposition.h): coordinator is declared LAST, so C++'s
// reverse-declaration-order destruction destroys it FIRST -- it never
// outlives the fakes it only borrows references to.
struct Harness {
  FakeProfileStore profileStore;
  FakeProbeFactory probeFactory;
  FakeTokenStore tokenStore;
  FakeAuthClient authClient;
  std::unique_ptr<SessionCoordinator> coordinator;

  Harness() {
    coordinator = std::make_unique<SessionCoordinator>(
        profileStore, probeFactory.asFactory(), tokenStore, authClient);
  }

  // Advances the harness through boot + a Compatible probe + a NotFound
  // token read, landing on SignedOut with a usable profile. Used as a
  // common starting point by tests that only care about later stages.
  void bootToSignedOut() {
    coordinator->start();
    pumpEventsUntil([this] { return probeFactory.current() != nullptr; });
    probeFactory.current()->complete(compatibleProbeResult());
    const QString profileId = coordinator->selectedProfileId();
    pumpEventsUntil(
        [this, profileId] { return tokenStore.hasPending(profileId); });
    tokenStore.complete(profileId, notFoundResult());
    pumpEventsUntil([this] {
      return coordinator->state() == SessionCoordinator::State::SignedOut;
    });
  }
};

} // namespace

class SessionCoordinatorTests final : public QObject {
  Q_OBJECT

private slots:
  // Boot / profile loading
  void firstRunSeedsHostedDefaultAndPersistsSelection();
  void loadProfilesFailureIsProfileStorageFailure();
  void loadSelectedIdFailureIsProfileStorageFailure();
  void saveProfilesFailureDuringSeedIsProfileStorageFailure();
  void persistedSelectionMissingFromListIsProfileStorageFailure();
  void emptySelectionWithExistingProfilesSeedsFirstProfile();

  // Capability probing
  void compatibleProbeProceedsToCredentialRestore();
  void legacyFallbackProbeProceedsToCredentialRestore();
  void incompatibleProbeSetsIncompatibleStateAndIsRetryable();
  void probeNetworkErrorSetsRecoverableFailureAndIsRetryable();

  // Credential restore
  void missingTokenBecomesSignedOut();
  void validTokenWhoAmISuccessBecomesSignedIn();
  void unauthorizedRestoredTokenDeletesThenSignedOut();
  void unauthorizedRestoredTokenDeletionFailureIsSecureStorageUnavailable();
  void whoAmITransportFailureDuringRestoreIsRecoverableAndDoesNotDeleteToken();
  void tokenStoreAccessDeniedDuringRestoreIsSecureStorageUnavailable();

  // Sign in / register
  void signInSuccessValidatesThenSavesThenSignedIn();
  void signInAuthFailureReturnsSignedOut();
  void signInWhoAmIUnauthorizedDoesNotSaveAndReturnsSignedOut();
  void signInSaveFailureIsSecureStorageUnavailable();
  void signInNoOpWhenProfileNotYetUsable();
  void signInAndRegisterNoOpDuringCredentialRestore();
  void registerAccountSuccessFlow();

  // Sign out
  void signOutDeletesTokenAndBecomesSignedOut();
  void signOutDeletionFailurePreservesIdentityAndIsRetryable();
  void signOutNoOpWhenNotSignedIn();

  // Profile switching / races
  void switchProfilePersistsSelectionAndRestartsProbe();
  void switchProfilePersistenceFailureLeavesCurrentProfileUntouched();
  void switchProfileCancelsInFlightAuthRequest();
  void rapidSwitchAwayAndBackIgnoresStaleProbeCompletion();
  void rapidSwitchDuringWhoAmIIgnoresStaleCompletion();
  void perProfileFifoOrdersSaveBeforeLaterDelete();
  void concurrentProfilesHaveIndependentTokenQueues();

  // Destruction
  void destructionSuppressesProbeCompletion();
  void destructionSuppressesTokenStoreCompletion();
  void destructionCancelsPendingAuthRequest();

  // Secret-free diagnostics
  void diagnosticsAndStateNeverContainSecrets();
};

// ─── Boot / profile loading ──────────────────────────────────────────────

void SessionCoordinatorTests::firstRunSeedsHostedDefaultAndPersistsSelection() {
  Harness h;
  QCOMPARE(h.profileStore.profiles.size(), 0);
  h.coordinator->start();

  QCOMPARE(h.profileStore.saveProfilesCalls, 1);
  QCOMPARE(h.profileStore.saveSelectedIdCalls, 1);
  QCOMPARE(h.profileStore.profiles.size(), 1);
  const QString expectedProfileId = ServerProfile::hostedDefault().profileId();
  const QString actualSelectedId = h.profileStore.selectedId;
  QVERIFY(actualSelectedId == expectedProfileId);
  const QString actualCoordinatorSelection = h.coordinator->selectedProfileId();
  QVERIFY(actualCoordinatorSelection == expectedProfileId);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);
}

void SessionCoordinatorTests::loadProfilesFailureIsProfileStorageFailure() {
  Harness h;
  h.profileStore.failLoadProfiles = QStringLiteral("corrupt data");
  h.coordinator->start();
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProfileStorageFailure);
  QVERIFY(!h.coordinator->diagnostic().isEmpty());
}

void SessionCoordinatorTests::loadSelectedIdFailureIsProfileStorageFailure() {
  Harness h;
  h.profileStore.profiles = {ServerProfile::hostedDefault()};
  h.profileStore.failLoadSelectedId = QStringLiteral("access denied");
  h.coordinator->start();
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProfileStorageFailure);
}

void SessionCoordinatorTests::
    saveProfilesFailureDuringSeedIsProfileStorageFailure() {
  Harness h;
  h.profileStore.failSaveProfiles = QStringLiteral("disk full");
  h.coordinator->start();
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProfileStorageFailure);
  // Never pretend first-run success on a persistence failure.
  QCOMPARE(h.profileStore.profiles.size(), 0);
}

void SessionCoordinatorTests::
    persistedSelectionMissingFromListIsProfileStorageFailure() {
  Harness h;
  h.profileStore.profiles = {ServerProfile::hostedDefault()};
  h.profileStore.selectedId = QUuid::createUuid().toString();
  h.coordinator->start();
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProfileStorageFailure);
}

void SessionCoordinatorTests::
    emptySelectionWithExistingProfilesSeedsFirstProfile() {
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  h.profileStore.profiles = {hosted};
  h.profileStore.selectedId.clear();
  h.coordinator->start();
  QCOMPARE(h.profileStore.saveSelectedIdCalls, 1);
  const QString expectedId = hosted.profileId();
  const QString actualId = h.coordinator->selectedProfileId();
  QVERIFY(actualId == expectedId);
}

// ─── Capability probing ──────────────────────────────────────────────────

void SessionCoordinatorTests::compatibleProbeProceedsToCredentialRestore() {
  Harness h;
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  QCOMPARE(h.probeFactory.current()->probeCallCount(), 1);
  h.probeFactory.current()->complete(compatibleProbeResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() ==
               SessionCoordinator::State::RestoringCredential ||
           !h.tokenStore.calls.isEmpty();
  });
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::RestoringCredential);
  QCOMPARE(h.tokenStore.calls.size(), 1);
  QCOMPARE(h.tokenStore.calls.first().kind, QStringLiteral("read"));
}

void SessionCoordinatorTests::legacyFallbackProbeProceedsToCredentialRestore() {
  Harness h;
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(legacyFallbackProbeResult());
  pumpEventsUntil([&h] { return !h.tokenStore.calls.isEmpty(); });
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::RestoringCredential);
}

void SessionCoordinatorTests::
    incompatibleProbeSetsIncompatibleStateAndIsRetryable() {
  Harness h;
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(incompatibleProbeResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::Incompatible;
  });
  QVERIFY(!h.coordinator->diagnostic().isEmpty());

  // retry() re-probes: a new probe instance is created.
  const int createdBefore = h.probeFactory.totalCreated();
  h.coordinator->retry();
  pumpEventsUntil([&h, createdBefore] {
    return h.probeFactory.totalCreated() > createdBefore;
  });
  QCOMPARE(h.probeFactory.totalCreated(), createdBefore + 1);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);
}

void SessionCoordinatorTests::
    probeNetworkErrorSetsRecoverableFailureAndIsRetryable() {
  Harness h;
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(networkErrorProbeResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RecoverableFailure;
  });

  const int createdBefore = h.probeFactory.totalCreated();
  h.coordinator->retry();
  pumpEventsUntil([&h, createdBefore] {
    return h.probeFactory.totalCreated() > createdBefore;
  });
  QCOMPARE(h.probeFactory.totalCreated(), createdBefore + 1);
}

// ─── Credential restore ──────────────────────────────────────────────────

void SessionCoordinatorTests::missingTokenBecomesSignedOut() {
  Harness h;
  h.bootToSignedOut();
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::SignedOut);
}

void SessionCoordinatorTests::validTokenWhoAmISuccessBecomesSignedIn() {
  Harness h;
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });

  const QString expectedToken = QStringLiteral("stored-token-abc");
  h.tokenStore.complete(profileId, successReadResult(expectedToken));
  pumpEventsUntil([&h] { return !h.authClient.calls.isEmpty(); });
  QCOMPARE(h.authClient.calls.size(), 1);
  QCOMPARE(h.authClient.calls.first().kind, FakeAuthClient::CallKind::WhoAmI);
  const QString actualToken = h.authClient.calls.first().token;
  QVERIFY(actualToken == expectedToken);

  const QString expectedUsername = QStringLiteral("alice");
  const QString expectedEmail = QStringLiteral("alice@example.test");
  h.authClient.completeUser(1, userSuccess(expectedUsername, expectedEmail));
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  });
  const QString actualUsername = h.coordinator->currentUsername();
  const QString actualEmail = h.coordinator->currentEmail();
  QVERIFY(actualUsername == expectedUsername);
  QVERIFY(actualEmail == expectedEmail);
  // Restoring an already-stored token must never re-save it.
  QCOMPARE(h.tokenStore.calls.size(), 1);
}

void SessionCoordinatorTests::unauthorizedRestoredTokenDeletesThenSignedOut() {
  Harness h;
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  h.tokenStore.complete(profileId,
                        successReadResult(QStringLiteral("stale-token")));
  pumpEventsUntil([&h] { return !h.authClient.calls.isEmpty(); });

  h.authClient.completeUser(1, userUnauthorized());
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  QCOMPARE(h.tokenStore.calls.size(), 2);
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));

  h.tokenStore.complete(profileId, successWriteResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  });
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::SignedOut);
}

void SessionCoordinatorTests::
    unauthorizedRestoredTokenDeletionFailureIsSecureStorageUnavailable() {
  Harness h;
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  h.tokenStore.complete(profileId,
                        successReadResult(QStringLiteral("stale-token")));
  pumpEventsUntil([&h] { return !h.authClient.calls.isEmpty(); });
  h.authClient.completeUser(1, userUnauthorized());
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });

  h.tokenStore.complete(profileId, backendErrorResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  });
  // Never claim signed out while the token might still remain.
  QVERIFY(h.coordinator->state() != SessionCoordinator::State::SignedOut);
}

void SessionCoordinatorTests::
    whoAmITransportFailureDuringRestoreIsRecoverableAndDoesNotDeleteToken() {
  Harness h;
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  h.tokenStore.complete(profileId,
                        successReadResult(QStringLiteral("valid-token")));
  pumpEventsUntil([&h] { return !h.authClient.calls.isEmpty(); });

  h.authClient.completeUser(1, userTransport());
  pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RecoverableFailure;
  });
  // The potentially-valid token must not have been deleted.
  QCOMPARE(h.tokenStore.calls.size(), 1);

  // retry() re-issues whoami, not a fresh read.
  h.coordinator->retry();
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; });
  QCOMPARE(h.authClient.calls.size(), 2);
  QCOMPARE(h.authClient.calls.last().kind, FakeAuthClient::CallKind::WhoAmI);
  QCOMPARE(h.tokenStore.calls.size(), 1);
}

void SessionCoordinatorTests::
    tokenStoreAccessDeniedDuringRestoreIsSecureStorageUnavailable() {
  Harness h;
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  h.tokenStore.complete(profileId, accessDeniedResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  });
  QVERIFY(!h.coordinator->diagnostic().isEmpty());
}

// ─── Sign in / register ──────────────────────────────────────────────────

void SessionCoordinatorTests::signInSuccessValidatesThenSavesThenSignedIn() {
  Harness h;
  h.bootToSignedOut();
  const QString profileId = h.coordinator->selectedProfileId();

  const QString password = QStringLiteral("hunter2");
  const QString email = QStringLiteral("bob@example.test");
  h.coordinator->signIn(email, password);
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::Authenticating);
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; });
  QCOMPARE(h.authClient.calls.first().kind,
           FakeAuthClient::CallKind::Authenticate);

  const QString expectedToken = QStringLiteral("fresh-token-xyz");
  h.authClient.completeToken(1, tokenSuccess(expectedToken));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; });
  QCOMPARE(h.authClient.calls.last().kind, FakeAuthClient::CallKind::WhoAmI);
  const QString actualWhoAmIToken = h.authClient.calls.last().token;
  QVERIFY(actualWhoAmIToken == expectedToken);

  const QString expectedUsername = QStringLiteral("bob");
  h.authClient.completeUser(2, userSuccess(expectedUsername, email));
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("save"));
  const QString actualSavedToken = h.tokenStore.calls.last().token;
  QVERIFY(actualSavedToken == expectedToken);

  h.tokenStore.complete(profileId, successWriteResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  });
  const QString actualUsername = h.coordinator->currentUsername();
  QVERIFY(actualUsername == expectedUsername);
}

void SessionCoordinatorTests::signInAuthFailureReturnsSignedOut() {
  Harness h;
  h.bootToSignedOut();
  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("wrong-password"));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; });
  h.authClient.completeToken(1, tokenUnauthorized());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut &&
           !h.coordinator->diagnostic().isEmpty();
  });
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::SignedOut);
  QVERIFY(!h.coordinator->diagnostic().isEmpty());
}

void SessionCoordinatorTests::
    signInWhoAmIUnauthorizedDoesNotSaveAndReturnsSignedOut() {
  Harness h;
  h.bootToSignedOut();
  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("hunter2"));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; });
  h.authClient.completeToken(1, tokenSuccess(QStringLiteral("rejected-token")));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; });
  h.authClient.completeUser(2, userUnauthorized());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  });
  // No save was ever attempted for a token that failed validation.
  QCOMPARE(h.tokenStore.calls.size(), 1); // only the earlier restore read
}

void SessionCoordinatorTests::signInSaveFailureIsSecureStorageUnavailable() {
  Harness h;
  h.bootToSignedOut();
  const QString profileId = h.coordinator->selectedProfileId();
  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("hunter2"));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; });
  h.authClient.completeToken(1, tokenSuccess(QStringLiteral("fresh-token")));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; });
  h.authClient.completeUser(2, userSuccess(QStringLiteral("bob"),
                                           QStringLiteral("bob@example.test")));
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  h.tokenStore.complete(profileId, backendErrorResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  });
}

void SessionCoordinatorTests::signInNoOpWhenProfileNotYetUsable() {
  Harness h;
  h.coordinator->start(); // still Loading/ProbingCapabilities
  const SessionCoordinator::State stateBefore = h.coordinator->state();
  h.coordinator->signIn(QStringLiteral("a@example.test"), QStringLiteral("pw"));
  QCOMPARE(h.coordinator->state(), stateBefore);
  QCOMPARE(h.authClient.calls.size(), 0);
}

// Regression test for the reentrancy race fixed in this round: signIn()/
// registerAccount() used to be gated on "profile usable" alone, which
// remains true throughout RestoringCredential (it becomes true once the
// probe succeeds and stays true afterwards). That let an interactive
// signIn() call run concurrently with the automatic credential restore's
// own /whoami validation; both flows share the single pending-auth-handle
// slot, so the restore's issueWhoAmI() would silently cancel the
// interactive request via cancelPendingAuthRequest(). Both entry points
// must instead be gated on state() == SignedOut, which excludes
// RestoringCredential.
void SessionCoordinatorTests::signInAndRegisterNoOpDuringCredentialRestore() {
  Harness h;
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::RestoringCredential);

  h.coordinator->signIn(QStringLiteral("a@example.test"), QStringLiteral("pw"));
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::RestoringCredential);
  QCOMPARE(h.authClient.calls.size(), 0);

  h.coordinator->registerAccount(QStringLiteral("a@example.test"),
                                 QStringLiteral("auser"), QStringLiteral("pw"));
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::RestoringCredential);
  QCOMPARE(h.authClient.calls.size(), 0);

  // The credential restore itself is unaffected by the rejected calls
  // above and can still proceed to completion normally.
  h.tokenStore.complete(profileId, notFoundResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  });
}

void SessionCoordinatorTests::registerAccountSuccessFlow() {
  Harness h;
  h.bootToSignedOut();
  const QString profileId = h.coordinator->selectedProfileId();
  h.coordinator->registerAccount(QStringLiteral("new@example.test"),
                                 QStringLiteral("newuser"),
                                 QStringLiteral("s3cret"));
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::Registering);
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; });
  QCOMPARE(h.authClient.calls.first().kind, FakeAuthClient::CallKind::Register);
  h.authClient.completeToken(1, tokenSuccess(QStringLiteral("new-token")));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; });
  h.authClient.completeUser(2, userSuccess(QStringLiteral("newuser"),
                                           QStringLiteral("new@example.test")));
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  h.tokenStore.complete(profileId, successWriteResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  });
}

// ─── Sign out ─────────────────────────────────────────────────────────────

namespace {
// Drives a harness all the way to SignedIn with a known token, for tests
// that need to start from a signed-in state.
void bootToSignedIn(Harness &h, const QString &token) {
  h.bootToSignedOut();
  const QString profileId = h.coordinator->selectedProfileId();
  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("hunter2"));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; });
  h.authClient.completeToken(1, tokenSuccess(token));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; });
  h.authClient.completeUser(2, userSuccess(QStringLiteral("bob"),
                                           QStringLiteral("bob@example.test")));
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  h.tokenStore.complete(profileId, successWriteResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  });
}
} // namespace

void SessionCoordinatorTests::signOutDeletesTokenAndBecomesSignedOut() {
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();
  h.coordinator->signOut();
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  });
  QVERIFY(h.coordinator->currentUsername().isEmpty());
}

void SessionCoordinatorTests::
    signOutDeletionFailurePreservesIdentityAndIsRetryable() {
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();
  const QString expectedUsername = h.coordinator->currentUsername();
  h.coordinator->signOut();
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  h.tokenStore.complete(profileId, backendErrorResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  });
  // Identity preserved; never silently claims signed out.
  const QString actualUsername = h.coordinator->currentUsername();
  QVERIFY(actualUsername == expectedUsername);

  h.coordinator->retry();
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  h.tokenStore.complete(profileId, successWriteResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  });
}

void SessionCoordinatorTests::signOutNoOpWhenNotSignedIn() {
  Harness h;
  h.bootToSignedOut();
  h.coordinator->signOut();
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::SignedOut);
  QCOMPARE(h.tokenStore.calls.size(), 1); // only the earlier restore read
}

// ─── Profile switching / races ────────────────────────────────────────────

void SessionCoordinatorTests::switchProfilePersistsSelectionAndRestartsProbe() {
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  // The profile must already be part of the coordinator's loaded list
  // (as loaded during start()) for switchProfile() to accept it -- adding
  // brand-new profiles is a separate, out-of-scope concern.
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString hostedId = h.coordinator->selectedProfileId();
  pumpEventsUntil([&h, hostedId] { return h.tokenStore.hasPending(hostedId); });
  h.tokenStore.complete(hostedId, notFoundResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  });

  const int createdBefore = h.probeFactory.totalCreated();
  h.coordinator->switchProfile(customProfile->profileId());
  const QString expectedId = customProfile->profileId();
  const QString actualId = h.profileStore.selectedId;
  QVERIFY(actualId == expectedId);
  pumpEventsUntil([&h, createdBefore] {
    return h.probeFactory.totalCreated() > createdBefore;
  });
  QCOMPARE(h.probeFactory.totalCreated(), createdBefore + 1);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);
}

void SessionCoordinatorTests::
    switchProfilePersistenceFailureLeavesCurrentProfileUntouched() {
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString hostedId = h.coordinator->selectedProfileId();
  pumpEventsUntil([&h, hostedId] { return h.tokenStore.hasPending(hostedId); });
  h.tokenStore.complete(hostedId, notFoundResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  });
  const QString originalId = h.coordinator->selectedProfileId();
  h.profileStore.failSaveSelectedId = QStringLiteral("disk full");

  h.coordinator->switchProfile(customProfile->profileId());
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProfileStorageFailure);
  const QString actualId = h.coordinator->selectedProfileId();
  QVERIFY(actualId == originalId);
}

void SessionCoordinatorTests::switchProfileCancelsInFlightAuthRequest() {
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString hostedId = h.coordinator->selectedProfileId();
  pumpEventsUntil([&h, hostedId] { return h.tokenStore.hasPending(hostedId); });
  h.tokenStore.complete(hostedId, notFoundResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  });

  h.coordinator->signIn(QStringLiteral("a@example.test"), QStringLiteral("pw"));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; });
  QVERIFY(h.authClient.hasPendingToken(1));

  h.coordinator->switchProfile(customProfile->profileId());
  pumpEventsUntil([&h] { return !h.authClient.hasPendingToken(1); });
  QVERIFY(!h.authClient.hasPendingToken(1));
  // The stale cancellation must not clobber the new profile's fresh state.
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);
}

void SessionCoordinatorTests::
    rapidSwitchAwayAndBackIgnoresStaleProbeCompletion() {
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  FakeCapabilityProbe *firstProbe = h.probeFactory.current();

  h.coordinator->switchProfile(customProfile->profileId());
  pumpEventsUntil(
      [&h, firstProbe] { return h.probeFactory.current() != firstProbe; });
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);

  // The old (destroyed) probe cannot fire a stale finished() -- it was
  // destroyed by switchProfile(). Complete the CURRENT probe with an
  // Incompatible result, then switch straight back to hosted.
  h.probeFactory.current()->complete(incompatibleProbeResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::Incompatible;
  });

  h.coordinator->switchProfile(hosted.profileId());
  pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::ProbingCapabilities;
  });
  FakeCapabilityProbe *thirdProbe = h.probeFactory.current();
  QVERIFY(thirdProbe != nullptr);
  thirdProbe->complete(compatibleProbeResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RestoringCredential;
  });
  const QString actualSelected = h.coordinator->selectedProfileId();
  const QString expectedSelected = hosted.profileId();
  QVERIFY(actualSelected == expectedSelected);
}

void SessionCoordinatorTests::rapidSwitchDuringWhoAmIIgnoresStaleCompletion() {
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString hostedId = hosted.profileId();
  pumpEventsUntil([&h, hostedId] { return h.tokenStore.hasPending(hostedId); });
  h.tokenStore.complete(hostedId,
                        successReadResult(QStringLiteral("old-token")));
  pumpEventsUntil([&h] { return h.authClient.hasPendingUser(1); });

  // Switch away before whoami resolves.
  h.coordinator->switchProfile(customProfile->profileId());
  pumpEventsUntil([&h] { return !h.authClient.hasPendingUser(1); });

  // The (stale, cancelled) whoami's completion must never flip state back
  // to SignedIn for the now-abandoned hosted profile: the coordinator must
  // already be probing the newly selected custom profile.
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);
  const QString actualSelected = h.coordinator->selectedProfileId();
  const QString expectedSelected = customProfile->profileId();
  QVERIFY(actualSelected == expectedSelected);
}

void SessionCoordinatorTests::perProfileFifoOrdersSaveBeforeLaterDelete() {
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  // Sign out while nothing else is in flight: a plain delete op is
  // enqueued and must run to completion in order.
  h.coordinator->signOut();
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete
  QCOMPARE(h.tokenStore.calls.at(0).kind, QStringLiteral("read"));
  QCOMPARE(h.tokenStore.calls.at(1).kind, QStringLiteral("save"));
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  });
}

void SessionCoordinatorTests::concurrentProfilesHaveIndependentTokenQueues() {
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString hostedId = hosted.profileId();
  pumpEventsUntil([&h, hostedId] { return h.tokenStore.hasPending(hostedId); });

  // Switch to the custom profile while the hosted profile's restore read is
  // still outstanding (uncancellable): both profiles now have independent,
  // non-overlapping pending operations, which FakeTokenStore would flag as
  // a fatal invariant violation if the coordinator ever mixed them up.
  h.coordinator->switchProfile(customProfile->profileId());
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString customId = customProfile->profileId();
  pumpEventsUntil([&h, customId] { return h.tokenStore.hasPending(customId); });

  QVERIFY(h.tokenStore.hasPending(hostedId));
  QVERIFY(h.tokenStore.hasPending(customId));

  h.tokenStore.complete(hostedId, notFoundResult());
  h.tokenStore.complete(customId, notFoundResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  });
  const QString actualSelected = h.coordinator->selectedProfileId();
  const QString expectedSelected = customProfile->profileId();
  QVERIFY(actualSelected == expectedSelected);
}

// ─── Destruction ──────────────────────────────────────────────────────────

void SessionCoordinatorTests::destructionSuppressesProbeCompletion() {
  Harness h;
  h.coordinator->start();
  pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; });
  QPointer<FakeCapabilityProbe> probe = h.probeFactory.current();
  h.coordinator.reset();
  QVERIFY(probe.isNull()); // destroyed together with the coordinator
}

void SessionCoordinatorTests::destructionSuppressesTokenStoreCompletion() {
  Harness h;
  h.bootToSignedOut();
  const QString profileId = h.coordinator->selectedProfileId();
  h.coordinator->signIn(QStringLiteral("a@example.test"), QStringLiteral("pw"));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; });
  h.authClient.completeToken(1, tokenSuccess(QStringLiteral("fresh-token")));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; });
  h.authClient.completeUser(
      2, userSuccess(QStringLiteral("a"), QStringLiteral("a@example.test")));
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });

  // Destroy the coordinator while the save is still outstanding
  // (uncancellable), then release it: FakeTokenStore invokes the stored
  // callback via QueuedConnection, which must be a safe no-op (QPointer
  // guarded) rather than touching the destroyed coordinator.
  h.coordinator.reset();
  h.tokenStore.complete(profileId, successWriteResult());
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  QVERIFY(true); // reaching here without a crash is the assertion
}

void SessionCoordinatorTests::destructionCancelsPendingAuthRequest() {
  Harness h;
  h.bootToSignedOut();
  h.coordinator->signIn(QStringLiteral("a@example.test"), QStringLiteral("pw"));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; });
  QVERIFY(h.authClient.hasPendingToken(1));
  h.coordinator.reset();
  QVERIFY(!h.authClient.hasPendingToken(1));
}

// ─── Secret-free diagnostics ──────────────────────────────────────────────

void SessionCoordinatorTests::diagnosticsAndStateNeverContainSecrets() {
  Harness h;
  const QString secretPassword = QStringLiteral("sentinel-secret-password");
  const QString secretToken = QStringLiteral("sentinel-secret-token");

  h.bootToSignedOut();
  h.coordinator->signIn(QStringLiteral("carol@example.test"), secretPassword);
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; });
  h.authClient.completeToken(1, tokenSuccess(secretToken));
  pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; });
  h.authClient.completeUser(2,
                            userSuccess(QStringLiteral("carol"),
                                        QStringLiteral("carol@example.test")));
  const QString profileId = h.coordinator->selectedProfileId();
  pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); });
  h.tokenStore.complete(profileId, backendErrorResult());
  pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  });

  QVERIFY(!h.coordinator->diagnostic().contains(secretPassword));
  QVERIFY(!h.coordinator->diagnostic().contains(secretToken));
  QVERIFY(!h.coordinator->stateDescription().contains(secretPassword));
  QVERIFY(!h.coordinator->stateDescription().contains(secretToken));
  QVERIFY(!h.coordinator->currentUsername().contains(secretPassword));
  QVERIFY(!h.coordinator->selectedProfileDisplayName().contains(secretToken));
}

QTEST_GUILESS_MAIN(SessionCoordinatorTests)

#include "SessionCoordinatorTests.moc"
