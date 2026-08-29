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

[[nodiscard]] bool pumpEventsUntil(const std::function<bool()> &predicate,
                                   int timeoutMs = 2000) {
  const QDeadlineTimer deadline(timeoutMs);
  while (!predicate() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return predicate();
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
    registerPending(profileId, PendingKind::Read, QString(),
                    std::move(callback));
  }

  void saveToken(const QString &profileId, const QString &token,
                 ResultCallback callback) override {
    calls.append({profileId, QStringLiteral("save"), token});
    registerPending(profileId, PendingKind::Save, token, std::move(callback));
  }

  void deleteToken(const QString &profileId, ResultCallback callback) override {
    calls.append({profileId, QStringLiteral("delete"), QString()});
    registerPending(profileId, PendingKind::Delete, QString(),
                    std::move(callback));
  }

  [[nodiscard]] bool hasPending(const QString &profileId) const {
    return m_pending.contains(profileId);
  }

  // Real per-profile stored-token state, mutated automatically by
  // complete() below whenever a Save/Delete completes with Success. This
  // lets tests assert the genuine end state of the fake secure store --
  // e.g. "the abandoned save's token was truly removed by the
  // compensating cleanup delete" -- rather than only inspecting the
  // |calls| ordering list.
  [[nodiscard]] std::optional<QString>
  storedToken(const QString &profileId) const {
    const auto it = m_stored.find(profileId);
    if (it == m_stored.end()) {
      return std::nullopt;
    }
    return it.value();
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
    PendingOp pending = it.value();
    m_pending.erase(it);
    if (result.outcome == TokenStoreOutcome::Success) {
      if (pending.kind == PendingKind::Save) {
        m_stored.insert(profileId, pending.token);
      } else if (pending.kind == PendingKind::Delete) {
        m_stored.remove(profileId);
      }
    }
    ResultCallback callback = pending.callback;
    // Recorded so replayLastCompletion() below can simulate a duplicate/
    // buggy second invocation of this exact completion after this
    // operation has already finished and the queue has moved on.
    m_lastCompleted.insert(profileId, CompletedRecord{callback, result});
    QMetaObject::invokeMethod(
        this, [callback, result]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }

  // Re-invokes the callback of the most recently completed operation for
  // |profileId| a second time, simulating a duplicate/buggy completion
  // notification from the underlying secure store (e.g. a callback fired
  // twice by a lower-level job). SessionCoordinator itself must be the one
  // to reject this stale/duplicate invocation (via its per-attempt opId/
  // attemptId dispatch matching in startFrontTokenOp()) rather than the
  // fake preventing it from ever happening. Must be called only after a
  // prior complete(profileId, ...) for the same profile.
  void replayLastCompletion(const QString &profileId) {
    const auto it = m_lastCompleted.find(profileId);
    if (it == m_lastCompleted.end()) {
      qFatal("FakeTokenStore: no completed operation recorded for this "
             "profile to replay");
    }
    ResultCallback callback = it->callback;
    TokenStoreResult result = it->result;
    QMetaObject::invokeMethod(
        this, [callback, result]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }

  // Number of times registerPending() observed a second dispatch for a
  // profile ID that already had one outstanding. SessionCoordinator's own
  // per-profile dispatch guard (ProfileTokenDispatch::inFlight) must keep
  // this at zero for the whole test run; it is a plain counter (not a
  // qFatal) specifically so a violation surfaces as a normal, diagnosable
  // assertion failure rather than crashing the test binary before any
  // assertion output is produced.
  [[nodiscard]] int overlappingDispatchCount() const {
    return m_overlappingDispatchCount;
  }

private:
  enum class PendingKind { Read, Save, Delete };
  struct PendingOp {
    PendingKind kind;
    QString token; // only meaningful for Save
    ResultCallback callback;
  };
  struct CompletedRecord {
    ResultCallback callback;
    TokenStoreResult result;
  };

  void registerPending(const QString &profileId, PendingKind kind,
                       QString token, ResultCallback callback) {
    if (m_pending.contains(profileId)) {
      // SessionCoordinator's per-profile FIFO invariant requires exactly
      // one in-flight ITokenStore operation per profile ID at a time.
      // This must never happen if SessionCoordinator's dispatch guard is
      // correct; recorded via a counter (see overlappingDispatchCount())
      // rather than qFatal so a regression is a normal assertion failure.
      ++m_overlappingDispatchCount;
    }
    m_pending.insert(profileId,
                     PendingOp{kind, std::move(token), std::move(callback)});
  }

  QHash<QString, PendingOp> m_pending;
  QHash<QString, QString> m_stored;
  QHash<QString, CompletedRecord> m_lastCompleted;
  int m_overlappingDispatchCount{0};
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
    QVERIFY(
        pumpEventsUntil([this] { return probeFactory.current() != nullptr; }));
    probeFactory.current()->complete(compatibleProbeResult());
    const QString profileId = coordinator->selectedProfileId();
    QVERIFY(pumpEventsUntil(
        [this, profileId] { return tokenStore.hasPending(profileId); }));
    tokenStore.complete(profileId, notFoundResult());
    QVERIFY(pumpEventsUntil([this] {
      return coordinator->state() == SessionCoordinator::State::SignedOut;
    }));
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
  void reentrantStartDiscardsPriorProbeAndRestartsCleanly();

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
  void duplicateSignOutCallsDispatchExactlyOneDelete();
  void
  directReentrantSignOutDuringSigningOutEmissionDispatchesExactlyOneDelete();
  void signOutPendingSwitchAwayDeleteFailureReturnBlockedRetrySuccessNoToken();

  // Profile switching / races
  void switchProfilePersistsSelectionAndRestartsProbe();
  void switchProfilePersistenceFailureLeavesCurrentProfileUntouched();
  void switchProfileCancelsInFlightAuthRequest();
  void rapidSwitchAwayAndBackIgnoresStaleProbeCompletion();
  void rapidSwitchDuringWhoAmIIgnoresStaleCompletion();
  void perProfileFifoOrdersSaveBeforeLaterDelete();
  void immediateNewAuthForSameProfileQueuesBehindReservedCleanupDelete();
  void deleteFailureDurablyBlocksProfileUntilRetrySucceeds();
  void deleteFailureRemainsAcrossSwitchAwayAndStartRestart();
  void staleCompensatingCleanupCannotDeleteLaterSave();
  void concurrentProfilesHaveIndependentTokenQueues();
  void
  retryCannotConcurrentlyRedispatchStalledDeleteAndDuplicateCallbackCannotDequeueNextOp();

  // Destruction
  void destructionSuppressesProbeCompletion();
  void destructionSuppressesTokenStoreCompletion();
  void destructionCancelsPendingAuthRequest();

  // Direct-connection reentrancy (Qt property signals are synchronous)
  void directStateChangedReentrancyDuringSignInSendsNoStalePasswordRequest();
  void
  directStateChangedReentrancyDuringProbeDoesNotDereferenceDestroyedProbe();
  void directCurrentUserChangedReentrancyCannotSignInOldProfileOrLeakSave();
  void coordinatorDestructionDuringStateChangedEmissionIsSafe();
  void
  directCurrentUserChangedReentrancyDuringStartFromSignedInDestroysSafely();

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

void SessionCoordinatorTests::
    reentrantStartDiscardsPriorProbeAndRestartsCleanly() {
  // start() is a public slot reachable from QML/re-entrant signal delivery.
  // A second call while the first boot's probe is still in flight must not
  // let that stale probe's later completion mutate state: it discards the
  // prior probe and begins an entirely fresh boot sequence instead.
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  // Track the first probe's liveness with a QPointer rather than comparing
  // its raw address later: once destroyed, that address may be reused by
  // an unrelated allocation (e.g. the very next FakeCapabilityProbe), which
  // would make a dangling-pointer comparison flaky/undefined rather than a
  // reliable destruction proof.
  const QPointer<FakeCapabilityProbe> firstProbe = h.probeFactory.current();
  QVERIFY(!firstProbe.isNull());
  const int createdBefore = h.probeFactory.totalCreated();

  h.coordinator->start();
  QVERIFY(pumpEventsUntil([&h, createdBefore] {
    return h.probeFactory.totalCreated() > createdBefore;
  }));

  // The first probe was genuinely destroyed (QPointer self-clears), and a
  // fresh probe instance was created for the restarted boot.
  QVERIFY(firstProbe.isNull());
  QCOMPARE(h.probeFactory.totalCreated(), createdBefore + 1);
  QVERIFY(h.probeFactory.current() != nullptr);

  // The restarted boot still completes normally end-to-end, proving this
  // is a real restart rather than a stuck/broken state.
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::SignedOut);
}

// ─── Capability probing ──────────────────────────────────────────────────

void SessionCoordinatorTests::compatibleProbeProceedsToCredentialRestore() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  QCOMPARE(h.probeFactory.current()->probeCallCount(), 1);
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
               SessionCoordinator::State::RestoringCredential ||
           !h.tokenStore.calls.isEmpty();
  }));
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::RestoringCredential);
  QCOMPARE(h.tokenStore.calls.size(), 1);
  QCOMPARE(h.tokenStore.calls.first().kind, QStringLiteral("read"));
}

void SessionCoordinatorTests::legacyFallbackProbeProceedsToCredentialRestore() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(legacyFallbackProbeResult());
  QVERIFY(pumpEventsUntil([&h] { return !h.tokenStore.calls.isEmpty(); }));
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::RestoringCredential);
}

void SessionCoordinatorTests::
    incompatibleProbeSetsIncompatibleStateAndIsRetryable() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(incompatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::Incompatible;
  }));
  QVERIFY(!h.coordinator->diagnostic().isEmpty());

  // retry() re-probes: a new probe instance is created.
  const int createdBefore = h.probeFactory.totalCreated();
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil([&h, createdBefore] {
    return h.probeFactory.totalCreated() > createdBefore;
  }));
  QCOMPARE(h.probeFactory.totalCreated(), createdBefore + 1);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);
}

void SessionCoordinatorTests::
    probeNetworkErrorSetsRecoverableFailureAndIsRetryable() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(networkErrorProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RecoverableFailure;
  }));

  const int createdBefore = h.probeFactory.totalCreated();
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil([&h, createdBefore] {
    return h.probeFactory.totalCreated() > createdBefore;
  }));
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
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));

  const QString expectedToken = QStringLiteral("stored-token-abc");
  h.tokenStore.complete(profileId, successReadResult(expectedToken));
  QVERIFY(pumpEventsUntil([&h] { return !h.authClient.calls.isEmpty(); }));
  QCOMPARE(h.authClient.calls.size(), 1);
  QCOMPARE(h.authClient.calls.first().kind, FakeAuthClient::CallKind::WhoAmI);
  const QString actualToken = h.authClient.calls.first().token;
  QVERIFY(actualToken == expectedToken);

  const QString expectedUsername = QStringLiteral("alice");
  const QString expectedEmail = QStringLiteral("alice@example.test");
  h.authClient.completeUser(1, userSuccess(expectedUsername, expectedEmail));
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
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
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId,
                        successReadResult(QStringLiteral("stale-token")));
  QVERIFY(pumpEventsUntil([&h] { return !h.authClient.calls.isEmpty(); }));

  h.authClient.completeUser(1, userUnauthorized());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 2);
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));

  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::SignedOut);
}

void SessionCoordinatorTests::
    unauthorizedRestoredTokenDeletionFailureIsSecureStorageUnavailable() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId,
                        successReadResult(QStringLiteral("stale-token")));
  QVERIFY(pumpEventsUntil([&h] { return !h.authClient.calls.isEmpty(); }));
  h.authClient.completeUser(1, userUnauthorized());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));

  h.tokenStore.complete(profileId, backendErrorResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  // Never claim signed out while the token might still remain.
  QVERIFY(h.coordinator->state() != SessionCoordinator::State::SignedOut);
  QCOMPARE(h.tokenStore.calls.size(), 2);

  // retry() must not be a silent no-op here: it re-attempts exactly the
  // same durable deletion, and a successful retry completes the original
  // "become signed out only after deletion succeeds" contract.
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::SignedOut);
}

void SessionCoordinatorTests::
    whoAmITransportFailureDuringRestoreIsRecoverableAndDoesNotDeleteToken() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId,
                        successReadResult(QStringLiteral("valid-token")));
  QVERIFY(pumpEventsUntil([&h] { return !h.authClient.calls.isEmpty(); }));

  h.authClient.completeUser(1, userTransport());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RecoverableFailure;
  }));
  // The potentially-valid token must not have been deleted.
  QCOMPARE(h.tokenStore.calls.size(), 1);

  // retry() re-issues whoami, not a fresh read.
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  QCOMPARE(h.authClient.calls.size(), 2);
  QCOMPARE(h.authClient.calls.last().kind, FakeAuthClient::CallKind::WhoAmI);
  QCOMPARE(h.tokenStore.calls.size(), 1);
}

void SessionCoordinatorTests::
    tokenStoreAccessDeniedDuringRestoreIsSecureStorageUnavailable() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, accessDeniedResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
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
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  QCOMPARE(h.authClient.calls.first().kind,
           FakeAuthClient::CallKind::Authenticate);

  const QString expectedToken = QStringLiteral("fresh-token-xyz");
  h.authClient.completeToken(1, tokenSuccess(expectedToken));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  QCOMPARE(h.authClient.calls.last().kind, FakeAuthClient::CallKind::WhoAmI);
  const QString actualWhoAmIToken = h.authClient.calls.last().token;
  QVERIFY(actualWhoAmIToken == expectedToken);

  const QString expectedUsername = QStringLiteral("bob");
  h.authClient.completeUser(2, userSuccess(expectedUsername, email));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("save"));
  const QString actualSavedToken = h.tokenStore.calls.last().token;
  QVERIFY(actualSavedToken == expectedToken);

  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
  const QString actualUsername = h.coordinator->currentUsername();
  QVERIFY(actualUsername == expectedUsername);
}

void SessionCoordinatorTests::signInAuthFailureReturnsSignedOut() {
  Harness h;
  h.bootToSignedOut();
  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("wrong-password"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  h.authClient.completeToken(1, tokenUnauthorized());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut &&
           !h.coordinator->diagnostic().isEmpty();
  }));
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::SignedOut);
  QVERIFY(!h.coordinator->diagnostic().isEmpty());
}

void SessionCoordinatorTests::
    signInWhoAmIUnauthorizedDoesNotSaveAndReturnsSignedOut() {
  Harness h;
  h.bootToSignedOut();
  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  h.authClient.completeToken(1, tokenSuccess(QStringLiteral("rejected-token")));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  h.authClient.completeUser(2, userUnauthorized());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  // No save was ever attempted for a token that failed validation.
  QCOMPARE(h.tokenStore.calls.size(), 1); // only the earlier restore read
}

void SessionCoordinatorTests::signInSaveFailureIsSecureStorageUnavailable() {
  Harness h;
  h.bootToSignedOut();
  const QString profileId = h.coordinator->selectedProfileId();
  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  h.authClient.completeToken(1, tokenSuccess(QStringLiteral("fresh-token")));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  h.authClient.completeUser(2, userSuccess(QStringLiteral("bob"),
                                           QStringLiteral("bob@example.test")));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, backendErrorResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  // bootToSignedOut() already performed one "read" call; the failed save
  // attempt above is the second call.
  QCOMPARE(h.tokenStore.calls.size(), 2);

  // retry() must not be a silent no-op: the already-validated token is
  // still available and only the durable save needs to be retried, so a
  // successful retry completes sign-in without re-authenticating.
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("save"));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::SignedIn);
  // No re-authentication occurred; the retry reused the already-validated
  // token.
  QCOMPARE(h.authClient.calls.size(), 2);
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
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
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
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::registerAccountSuccessFlow() {
  Harness h;
  h.bootToSignedOut();
  const QString profileId = h.coordinator->selectedProfileId();
  h.coordinator->registerAccount(QStringLiteral("new@example.test"),
                                 QStringLiteral("newuser"),
                                 QStringLiteral("s3cret"));
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::Registering);
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  QCOMPARE(h.authClient.calls.first().kind, FakeAuthClient::CallKind::Register);
  h.authClient.completeToken(1, tokenSuccess(QStringLiteral("new-token")));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  h.authClient.completeUser(2, userSuccess(QStringLiteral("newuser"),
                                           QStringLiteral("new@example.test")));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
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
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  h.authClient.completeToken(1, tokenSuccess(token));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  h.authClient.completeUser(2, userSuccess(QStringLiteral("bob"),
                                           QStringLiteral("bob@example.test")));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
}
} // namespace

void SessionCoordinatorTests::signOutDeletesTokenAndBecomesSignedOut() {
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();
  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(h.coordinator->currentUsername().isEmpty());
}

void SessionCoordinatorTests::
    signOutDeletionFailurePreservesIdentityAndIsRetryable() {
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();
  const QString expectedUsername = h.coordinator->currentUsername();
  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, backendErrorResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  // Identity preserved; never silently claims signed out.
  const QString actualUsername = h.coordinator->currentUsername();
  QVERIFY(actualUsername == expectedUsername);

  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::signOutNoOpWhenNotSignedIn() {
  Harness h;
  h.bootToSignedOut();
  h.coordinator->signOut();
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::SignedOut);
  QCOMPARE(h.tokenStore.calls.size(), 1); // only the earlier restore read
}

void SessionCoordinatorTests::duplicateSignOutCallsDispatchExactlyOneDelete() {
  // An ordinary duplicate signOut() call (no reentrancy involved -- just
  // calling it twice in a row, e.g. a double-click) must not enqueue a
  // second delete: the first call's synchronous transition to SigningOut
  // already makes m_state != SignedIn by the time the second/third call
  // is evaluated.
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  h.coordinator->signOut();
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::SigningOut);
  h.coordinator->signOut();
  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete -- just one
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 3); // still just the one delete
}

void SessionCoordinatorTests::
    directReentrantSignOutDuringSigningOutEmissionDispatchesExactlyOneDelete() {
  // A directly-connected stateChanged() handler that reentrantly calls
  // signOut() again from inside the SigningOut transition's own emission
  // must be rejected exactly like the ordinary duplicate-call case above:
  // m_state is already SigningOut (set before the emission in setState())
  // by the time the reentrant call is evaluated.
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::stateChanged,
      h.coordinator.get(),
      [&h, &handled] {
        if (handled ||
            h.coordinator->state() != SessionCoordinator::State::SigningOut) {
          return;
        }
        handled = true;
        h.coordinator->signOut();
      },
      Qt::DirectConnection);

  h.coordinator->signOut();
  QVERIFY(handled);
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete -- just one
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
}

void SessionCoordinatorTests::
    signOutPendingSwitchAwayDeleteFailureReturnBlockedRetrySuccessNoToken() {
  // A signOut() deletion that is still pending (uncancellable) when the
  // UI switches away must remain a durable obligation for the abandoned
  // profile: a later failure must still stall it (regardless of the by
  // -then-stale generation it was issued under), returning to that
  // profile must immediately surface the still-unresolved block, and only
  // a successful retry may finally clear it -- leaving no token behind.
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete

  // Switch away while the deletion is still pending: the outgoing
  // profile's delete obligation is not tied to it remaining the current
  // profile.
  h.coordinator->switchProfile(customProfile->profileId());
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  const QString actualSelected = h.coordinator->selectedProfileId();
  QVERIFY(actualSelected == customProfile->profileId());

  // The delete now fails, arriving for a session/generation that is no
  // longer current. It must still durably stall this profile's FIFO.
  h.tokenStore.complete(profileId, backendErrorResult());
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);

  // Returning to the original profile must immediately surface the
  // still-unresolved required deletion rather than silently proceeding.
  h.coordinator->switchProfile(profileId);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 3); // no new dispatch yet; still blocked

  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());

  // The queued restore Read (enqueued behind the stuck delete while
  // returning to this profile) may now finally dispatch.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 5);
  QCOMPARE(h.tokenStore.calls.at(4).kind, QStringLiteral("read"));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());
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
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString hostedId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, hostedId] { return h.tokenStore.hasPending(hostedId); }));
  h.tokenStore.complete(hostedId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  const int createdBefore = h.probeFactory.totalCreated();
  h.coordinator->switchProfile(customProfile->profileId());
  const QString expectedId = customProfile->profileId();
  const QString actualId = h.profileStore.selectedId;
  QVERIFY(actualId == expectedId);
  QVERIFY(pumpEventsUntil([&h, createdBefore] {
    return h.probeFactory.totalCreated() > createdBefore;
  }));
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
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString hostedId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, hostedId] { return h.tokenStore.hasPending(hostedId); }));
  h.tokenStore.complete(hostedId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
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
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString hostedId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, hostedId] { return h.tokenStore.hasPending(hostedId); }));
  h.tokenStore.complete(hostedId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  h.coordinator->signIn(QStringLiteral("a@example.test"), QStringLiteral("pw"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  QVERIFY(h.authClient.hasPendingToken(1));

  h.coordinator->switchProfile(customProfile->profileId());
  QVERIFY(pumpEventsUntil([&h] { return !h.authClient.hasPendingToken(1); }));
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
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  QPointer<FakeCapabilityProbe> firstProbe(h.probeFactory.current());
  const int createdBeforeSwitch = h.probeFactory.totalCreated();
  QVERIFY(!firstProbe.isNull());

  h.coordinator->switchProfile(customProfile->profileId());
  // Never compare a destroyed probe's raw address against a new instance:
  // once switchProfile() destroys the old probe (m_probe.reset()), a
  // subsequent heap allocation for the replacement probe may legitimately
  // reuse the exact same address, making an address-equality check
  // silently pass even when the coordinator is still (incorrectly)
  // holding the OLD instance. A monotonically increasing creation count
  // from the factory, plus the QPointer for liveness, cannot be fooled by
  // allocator reuse.
  QVERIFY(pumpEventsUntil([&h, createdBeforeSwitch] {
    return h.probeFactory.totalCreated() > createdBeforeSwitch;
  }));
  QVERIFY(firstProbe.isNull());
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);

  // The old (destroyed) probe cannot fire a stale finished() -- it was
  // destroyed by switchProfile(). Complete the CURRENT probe with an
  // Incompatible result, then switch straight back to hosted.
  h.probeFactory.current()->complete(incompatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::Incompatible;
  }));

  h.coordinator->switchProfile(hosted.profileId());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::ProbingCapabilities;
  }));
  FakeCapabilityProbe *thirdProbe = h.probeFactory.current();
  QVERIFY(thirdProbe != nullptr);
  thirdProbe->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RestoringCredential;
  }));
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
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString hostedId = hosted.profileId();
  QVERIFY(pumpEventsUntil(
      [&h, hostedId] { return h.tokenStore.hasPending(hostedId); }));
  h.tokenStore.complete(hostedId,
                        successReadResult(QStringLiteral("old-token")));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.hasPendingUser(1); }));

  // Switch away before whoami resolves.
  h.coordinator->switchProfile(customProfile->profileId());
  QVERIFY(pumpEventsUntil([&h] { return !h.authClient.hasPendingUser(1); }));

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
  // Genuine FIFO overlap: an ITokenStore Save is dispatched (in flight,
  // uncancellable) for a profile, THEN that profile is switched away from
  // before the Save completes. invalidateProfileCredential() must reserve
  // a compensating Delete BEHIND the still-in-flight Save (never
  // dispatching it early), and that Delete must only actually run once
  // the Save finishes -- proving the abandoned token is truly cleaned up
  // rather than left stored.
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  h.bootToSignedOut();
  const QString profileId = h.coordinator->selectedProfileId();

  const QString sessionToken = QStringLiteral("session-token");
  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  h.authClient.completeToken(1, tokenSuccess(sessionToken));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  h.authClient.completeUser(2, userSuccess(QStringLiteral("bob"),
                                           QStringLiteral("bob@example.test")));

  // The validated token's Save has now been enqueued AND dispatched (it is
  // the sole/front op for this profile's FIFO) -- it is in flight and
  // uncancellable.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 2); // read, save
  QCOMPARE(h.tokenStore.calls.at(1).kind, QStringLiteral("save"));

  // Switch away WHILE the Save is still outstanding. This must reserve a
  // compensating Delete behind it -- not dispatch it now (only one
  // ITokenStore op may be in flight per profile at a time; FakeTokenStore
  // would fatally assert on an overlap) and not skip it either.
  h.coordinator->switchProfile(customProfile->profileId());
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);
  QVERIFY(h.tokenStore.hasPending(profileId)); // still the original Save
  QCOMPARE(h.tokenStore.calls.size(), 2);      // no delete dispatched yet

  // Release the Save. Its own continuation is generation-gated and must
  // not resurrect SignedIn for the now-abandoned profile, but the FIFO
  // must advance to dispatch the reserved cleanup Delete regardless.
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));

  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return !h.tokenStore.hasPending(profileId); }));

  // The abandoned Save's token must have been truly removed by the
  // compensating cleanup Delete -- not left stored.
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());
  // Switching away must not have been derailed by any of this.
  const QString actualSelected = h.coordinator->selectedProfileId();
  const QString expectedSelected = customProfile->profileId();
  QVERIFY(actualSelected == expectedSelected);
}

void SessionCoordinatorTests::
    immediateNewAuthForSameProfileQueuesBehindReservedCleanupDelete() {
  // After a Save crosses the ITokenStore boundary and is then invalidated
  // (switch away), switching straight back to the SAME profile enqueues a
  // fresh credential-restore Read for it. That Read must queue BEHIND the
  // reserved compensating Delete -- it must never overtake cleanup and
  // observe a token that is about to be deleted.
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  h.bootToSignedOut();
  const QString profileId = h.coordinator->selectedProfileId();

  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  h.authClient.completeToken(1, tokenSuccess(QStringLiteral("session-token")));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  h.authClient.completeUser(2, userSuccess(QStringLiteral("bob"),
                                           QStringLiteral("bob@example.test")));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 2); // read, save (in flight)

  // Switch away (reserves a cleanup Delete behind the in-flight Save),
  // then immediately switch straight back to the same profile.
  h.coordinator->switchProfile(customProfile->profileId());
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.coordinator->switchProfile(profileId);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());

  // startCredentialRestore() enqueues a new Read for profileId; it must
  // sit behind [Save (in flight), Delete (reserved)] rather than being
  // dispatched now.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(),
           2); // still just read, save -- no new read yet
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::RestoringCredential);

  // Release the stale Save: the reserved Delete must dispatch next, NOT
  // the newly queued Read.
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));

  // Release the Delete: only now may the queued Read finally dispatch.
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("read"));
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());

  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    deleteFailureDurablyBlocksProfileUntilRetrySucceeds() {
  // A failed REQUIRED deletion (here: a plain sign-out delete) must be
  // left un-dequeued at the head of its profile's FIFO so it durably
  // blocks every later same-profile operation -- including a credential
  // restore triggered by switching back to it -- until
  // retryStuckProfileTokenOp() actually succeeds. This is never abandoned
  // just because the UI/session moved on in the meantime.
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, backendErrorResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));

  // Switch away and back to the SAME profile while the required deletion
  // is still stuck: the switch itself must never silently proceed past
  // it, and a fresh credential restore must immediately surface the
  // still-unresolved block rather than trying (or worse, appearing) to
  // read a token that is supposed to be deleted.
  h.coordinator->switchProfile(customProfile->profileId());
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.coordinator->switchProfile(profileId);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  // The stuck delete must not have been silently dequeued/abandoned: no
  // new "read" call has been dispatched yet, and the fake store never
  // received a second delete attempt either.
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete (all so far)

  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());

  // Retry succeeded: the queued restore Read (enqueued behind the stuck
  // delete while switching back) may now finally dispatch.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 5);
  QCOMPARE(h.tokenStore.calls.at(4).kind, QStringLiteral("read"));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());
}

void SessionCoordinatorTests::
    deleteFailureRemainsAcrossSwitchAwayAndStartRestart() {
  // The same required-deletion failure must remain recorded/actionable
  // even across a full start() restart (not just switchProfile()): a
  // stale UI/session generation must never be used as an excuse to
  // silently drop a durable cleanup obligation.
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, accessDeniedResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));

  // A full restart must not pretend the deletion succeeded, nor may it
  // try to read a token that is still pending deletion.
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete -- no more yet

  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.at(4).kind, QStringLiteral("read"));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    retryCannotConcurrentlyRedispatchStalledDeleteAndDuplicateCallbackCannotDequeueNextOp() {
  // Exact regression from review: a required Delete fails and stalls; a
  // retry is dispatched but held (not yet resolved); a SECOND retry call
  // while the first attempt is still outstanding must be a complete no-op
  // (no second store dispatch, no overlap recorded). Only after the held
  // attempt completes may the queued restore Read dispatch -- and a
  // replayed/duplicate delivery of the (now stale) delete-success
  // callback must not be able to dequeue/corrupt that Read.
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, backendErrorResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete

  // Switch away and back while the deletion is still stuck: this enqueues
  // a fresh restore Read behind the stalled delete, giving us a genuine
  // "next op" that must not be corrupted by a later duplicate/stale
  // delete callback.
  h.coordinator->switchProfile(customProfile->profileId());
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.coordinator->switchProfile(profileId);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 3); // still no new dispatch: still stuck

  // First retry: dispatches attempt #2 of the same logical delete op, but
  // holds it unresolved.
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("delete"));

  // A second (and third) retry call while the first attempt is still
  // outstanding must be entirely refused by the central dispatch guard:
  // no additional store call, and the fake never observes an overlapping
  // dispatch for this profile.
  h.coordinator->retry();
  h.coordinator->retry();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);

  // Complete the held retry attempt successfully: the FIFO may now finally
  // advance and dispatch the restore Read that had been queued behind the
  // stuck delete this whole time.
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 5);
  QCOMPARE(h.tokenStore.calls.at(4).kind, QStringLiteral("read"));
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);

  // A duplicate/replayed delivery of the (already-processed) delete
  // completion must not be able to touch the queue at all now that
  // dispatch has moved on to the Read: it must not dequeue the Read, feed
  // it a stale result, or register as an overlapping dispatch.
  h.tokenStore.replayLastCompletion(profileId);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  QCOMPARE(h.tokenStore.calls.size(), 5); // unchanged: no new dispatch
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::SecureStorageUnavailable); // untouched

  // The genuine, still-outstanding Read completes normally and the
  // coordinator reaches its correct final state.
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);
}

void SessionCoordinatorTests::staleCompensatingCleanupCannotDeleteLaterSave() {
  // After an abandoned save has been fully compensated for by a cleanup
  // delete (queue fully drained), a brand-new, independent sign-in cycle
  // for the same profile must persist its own fresh token normally: the
  // earlier (now long-completed) cleanup must have no lingering effect on
  // a save that starts an entirely new lifecycle.
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  h.bootToSignedOut();
  const QString profileId = h.coordinator->selectedProfileId();

  // Abandon a save mid-flight (as in perProfileFifoOrdersSaveBeforeLaterDelete)
  // and let its compensating cleanup delete run to completion.
  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  h.authClient.completeToken(1,
                             tokenSuccess(QStringLiteral("abandoned-token")));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  h.authClient.completeUser(2, userSuccess(QStringLiteral("bob"),
                                           QStringLiteral("bob@example.test")));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.coordinator->switchProfile(customProfile->profileId());
  h.tokenStore.complete(profileId, successWriteResult()); // abandoned save
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult()); // cleanup delete
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return !h.tokenStore.hasPending(profileId); }));
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());

  // Switch back and start an entirely fresh, independent sign-in.
  h.coordinator->switchProfile(profileId);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  const QString freshToken = QStringLiteral("fresh-token");
  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 3; }));
  h.authClient.completeToken(3, tokenSuccess(freshToken));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 4; }));
  h.authClient.completeUser(4, userSuccess(QStringLiteral("bob"),
                                           QStringLiteral("bob@example.test")));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));

  const std::optional<QString> actualStored =
      h.tokenStore.storedToken(profileId);
  QVERIFY(actualStored.has_value());
  QVERIFY(*actualStored == freshToken);
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
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString hostedId = hosted.profileId();
  QVERIFY(pumpEventsUntil(
      [&h, hostedId] { return h.tokenStore.hasPending(hostedId); }));

  // Switch to the custom profile while the hosted profile's restore read is
  // still outstanding (uncancellable): both profiles now have independent,
  // non-overlapping pending operations, which FakeTokenStore would flag as
  // a fatal invariant violation if the coordinator ever mixed them up.
  h.coordinator->switchProfile(customProfile->profileId());
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString customId = customProfile->profileId();
  QVERIFY(pumpEventsUntil(
      [&h, customId] { return h.tokenStore.hasPending(customId); }));

  QVERIFY(h.tokenStore.hasPending(hostedId));
  QVERIFY(h.tokenStore.hasPending(customId));

  h.tokenStore.complete(hostedId, notFoundResult());
  h.tokenStore.complete(customId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  const QString actualSelected = h.coordinator->selectedProfileId();
  const QString expectedSelected = customProfile->profileId();
  QVERIFY(actualSelected == expectedSelected);
}

// ─── Destruction ──────────────────────────────────────────────────────────

void SessionCoordinatorTests::destructionSuppressesProbeCompletion() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  QPointer<FakeCapabilityProbe> probe = h.probeFactory.current();
  h.coordinator.reset();
  QVERIFY(probe.isNull()); // destroyed together with the coordinator
}

void SessionCoordinatorTests::destructionSuppressesTokenStoreCompletion() {
  Harness h;
  h.bootToSignedOut();
  const QString profileId = h.coordinator->selectedProfileId();
  h.coordinator->signIn(QStringLiteral("a@example.test"), QStringLiteral("pw"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  h.authClient.completeToken(1, tokenSuccess(QStringLiteral("fresh-token")));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  h.authClient.completeUser(
      2, userSuccess(QStringLiteral("a"), QStringLiteral("a@example.test")));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));

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
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  QVERIFY(h.authClient.hasPendingToken(1));
  h.coordinator.reset();
  QVERIFY(!h.authClient.hasPendingToken(1));
}

// ─── Direct-connection reentrancy ─────────────────────────────────────────
//
// Qt property-changed signals are delivered SYNCHRONOUSLY. A directly
// connected slot can therefore reenter the coordinator (switchProfile(),
// start(), signOut(), or even destruction) from the middle of an emission
// that the coordinator itself triggered, before the emitting method's own
// continuation resumes. These tests connect with Qt::DirectConnection
// (QObject::connect's default for same-thread objects) and verify the
// coordinator never dispatches a request for an abandoned profile/session,
// never dereferences a stale/destroyed object, and never emits a stale
// result -- the existing sequential "reentrantStart" test above only
// covers calling start() twice in a row, not reentrancy from inside an
// emission, which is a fundamentally different hazard.

void SessionCoordinatorTests::
    directStateChangedReentrancyDuringSignInSendsNoStalePasswordRequest() {
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  h.bootToSignedOut();
  const QString customId = customProfile->profileId();

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::stateChanged,
      h.coordinator.get(),
      [&h, &handled, customId] {
        if (handled || h.coordinator->state() !=
                           SessionCoordinator::State::Authenticating) {
          return;
        }
        handled = true;
        // Reentrantly abandon this profile from inside signIn()'s own
        // setState(Authenticating) emission, before signIn() has had a
        // chance to dispatch the network request.
        h.coordinator->switchProfile(customId);
      },
      Qt::DirectConnection);

  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("hunter2"));

  QVERIFY(handled);
  // The reentrant switch must have fully superseded signIn(): zero
  // authenticate requests were ever dispatched for the abandoned profile.
  QVERIFY(h.authClient.calls.isEmpty());
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);
  const QString actualSelected = h.coordinator->selectedProfileId();
  QVERIFY(actualSelected == customId);

  // The new profile's own flow still proceeds normally afterward.
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, customId] { return h.tokenStore.hasPending(customId); }));
  h.tokenStore.complete(customId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    directStateChangedReentrancyDuringProbeDoesNotDereferenceDestroyedProbe() {
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();

  bool handled = false;
  const int createdBeforeStart = h.probeFactory.totalCreated();
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::stateChanged,
      h.coordinator.get(),
      [&h, &handled, &customProfile] {
        if (handled || h.coordinator->state() !=
                           SessionCoordinator::State::ProbingCapabilities) {
          return;
        }
        handled = true;
        // Reentrantly destroy/replace m_probe (via switchProfile()) from
        // inside startProbe()'s own setState(ProbingCapabilities)
        // emission, before startProbe() has dereferenced m_probe to call
        // probe() on it. Without the QPointer+generation+non-null guard,
        // this segfaults.
        h.coordinator->switchProfile(customProfile->profileId());
      },
      Qt::DirectConnection);

  h.coordinator->start();

  QVERIFY(handled);
  // Exactly two probes were ever created: the original (superseded, never
  // dereferenced again) and the one from the reentrant switch -- the
  // superseded startProbe() call must not have gone on to create or
  // dispatch a third, redundant probe for the abandoned profile.
  QCOMPARE(h.probeFactory.totalCreated(), createdBeforeStart + 2);
  const QString actualSelected = h.coordinator->selectedProfileId();
  const QString expectedSelected = customProfile->profileId();
  QVERIFY(actualSelected == expectedSelected);

  // The surviving (current) probe still drives the flow normally.
  QVERIFY(h.probeFactory.current() != nullptr);
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RestoringCredential;
  }));
}

void SessionCoordinatorTests::
    directCurrentUserChangedReentrancyCannotSignInOldProfileOrLeakSave() {
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  h.bootToSignedOut();
  const QString hostedId = h.coordinator->selectedProfileId();
  const QString customId = customProfile->profileId();

  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  h.authClient.completeToken(1, tokenSuccess(QStringLiteral("fresh-token")));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::currentUserChanged,
      h.coordinator.get(),
      [&h, &handled, customId] {
        if (handled || h.coordinator->currentUsername().isEmpty()) {
          return;
        }
        handled = true;
        // Reentrantly abandon this profile from inside
        // handleWhoAmIResult()'s applyCurrentUser() emission, BEFORE the
        // freshly validated token would otherwise be saved.
        h.coordinator->switchProfile(customId);
      },
      Qt::DirectConnection);

  h.authClient.completeUser(2, userSuccess(QStringLiteral("bob"),
                                           QStringLiteral("bob@example.test")));
  QVERIFY(pumpEventsUntil([&handled] { return handled; }));

  QVERIFY(handled);
  // The reentrant switch must have superseded the whoami success
  // continuation entirely: the freshly obtained token is never saved for
  // the abandoned profile (only the original restore "read" call exists),
  // and the coordinator must not end up SignedIn for it.
  QCOMPARE(h.tokenStore.calls.size(), 1);
  QCOMPARE(h.tokenStore.calls.first().kind, QStringLiteral("read"));
  QVERIFY(!h.tokenStore.hasPending(hostedId));
  const QString actualSelected = h.coordinator->selectedProfileId();
  QVERIFY(actualSelected == customId);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);
}

void SessionCoordinatorTests::
    directCurrentUserChangedReentrancyDuringStartFromSignedInDestroysSafely() {
  // Exact regression from review: start() calls clearCurrentUser() (whose
  // currentUserChanged() emission is synchronous) and previously used
  // `this` in the following setState(State::Loading) call without an
  // intervening QPointer/generation checkpoint. A directly-connected
  // handler that destroys the coordinator during that emission must never
  // crash and must never observe any further state emission from the
  // (by-then-dangling) object.
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::currentUserChanged,
      h.coordinator.get(),
      [&h, &handled] {
        if (handled) {
          return;
        }
        handled = true;
        // Destroy the coordinator from inside start()'s clearCurrentUser()
        // emission, before setState(State::Loading) would otherwise run.
        h.coordinator.reset();
      },
      Qt::DirectConnection);

  h.coordinator->start();

  QVERIFY(handled);
  QVERIFY(h.coordinator == nullptr);
  // Reaching this line at all (no crash/UB, no dangling-pointer emission
  // of stateChanged() after destruction) is the assertion.
}

void SessionCoordinatorTests::
    coordinatorDestructionDuringStateChangedEmissionIsSafe() {
  Harness h;
  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::stateChanged,
      h.coordinator.get(),
      [&h, &handled] {
        if (handled || h.coordinator->state() !=
                           SessionCoordinator::State::ProbingCapabilities) {
          return;
        }
        handled = true;
        // Destroy the coordinator itself from inside its own
        // stateChanged() emission. Every guard checkpoint uses a
        // QPointer<SessionCoordinator>, so the continuation that
        // triggered this emission must recognize destruction and return
        // without ever touching `this` again -- this must not crash.
        h.coordinator.reset();
      },
      Qt::DirectConnection);

  h.coordinator->start();

  QVERIFY(handled);
  QVERIFY(h.coordinator == nullptr);
  // Reaching this line at all (no crash/UB) is the assertion.
}

// ─── Secret-free diagnostics ──────────────────────────────────────────────

void SessionCoordinatorTests::diagnosticsAndStateNeverContainSecrets() {
  Harness h;
  const QString secretPassword = QStringLiteral("sentinel-secret-password");
  const QString secretToken = QStringLiteral("sentinel-secret-token");

  h.bootToSignedOut();
  h.coordinator->signIn(QStringLiteral("carol@example.test"), secretPassword);
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  h.authClient.completeToken(1, tokenSuccess(secretToken));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  h.authClient.completeUser(2,
                            userSuccess(QStringLiteral("carol"),
                                        QStringLiteral("carol@example.test")));
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, backendErrorResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));

  QVERIFY(!h.coordinator->diagnostic().contains(secretPassword));
  QVERIFY(!h.coordinator->diagnostic().contains(secretToken));
  QVERIFY(!h.coordinator->stateDescription().contains(secretPassword));
  QVERIFY(!h.coordinator->stateDescription().contains(secretToken));
  QVERIFY(!h.coordinator->currentUsername().contains(secretPassword));
  QVERIFY(!h.coordinator->selectedProfileDisplayName().contains(secretToken));
}

QTEST_GUILESS_MAIN(SessionCoordinatorTests)

#include "SessionCoordinatorTests.moc"
