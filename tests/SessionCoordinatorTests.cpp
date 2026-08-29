// Deterministic tests for SessionCoordinator, driven entirely through fake
// IProfileStore/ICapabilityProbe/ITokenStore/IAuthenticationClient
// implementations. No live service, keyring, or network connection is used.
//
// FakeTokenStore doubles as an invariant checker: it records (via a plain
// overlappingDispatchCount() counter, not a fatal error) whenever two
// operations are ever in flight for the same profile ID at once, which
// would violate the per-profile FIFO guarantee SessionCoordinator must
// uphold (see SessionCoordinator::enqueueTokenOp / startFrontTokenOp).

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
    // Only meaningful for "read"/"save" (see ITokenStore.h); empty for
    // "delete", which is endpoint-agnostic.
    QString endpointIdentity;
  };
  QList<Call> calls;

  // When set, the NEXT deleteToken() call invokes its callback
  // synchronously (reentrantly, from within the very call that requested
  // the delete) instead of registering a pending op for complete() to
  // release later. ITokenStore's documented contract (see ITokenStore.h)
  // explicitly requires every callback to be invoked asynchronously and
  // never reentrantly -- a real, contract-compliant backend must never do
  // this. This fake deliberately VIOLATES that contract here, on purpose,
  // to harden SessionCoordinator against a hypothetical buggy/non-
  // compliant backend implementation: it proves the coordinator's own
  // internal ordering (reserve-before-notify, atomic state+identity
  // assignment) stays safe even in that disallowed case, rather than
  // relying on an unenforced assumption that no backend could ever
  // misbehave this way. Cleared automatically after firing once.
  std::optional<TokenStoreResult> synchronousDeleteResult;

  void readToken(const QString &profileId,
                 const QString &expectedEndpointIdentity,
                 ResultCallback callback) override {
    calls.append({profileId, QStringLiteral("read"), QString(),
                  expectedEndpointIdentity});
    registerPending(profileId, PendingKind::Read, QString(),
                    std::move(callback));
  }

  void saveToken(const QString &profileId, const QString &token,
                 const QString &endpointIdentity,
                 ResultCallback callback) override {
    calls.append({profileId, QStringLiteral("save"), token, endpointIdentity});
    registerPending(profileId, PendingKind::Save, token, std::move(callback));
  }

  void deleteToken(const QString &profileId, ResultCallback callback) override {
    calls.append({profileId, QStringLiteral("delete"), QString(), QString()});
    if (synchronousDeleteResult.has_value()) {
      const TokenStoreResult result = *synchronousDeleteResult;
      synchronousDeleteResult.reset();
      if (result.outcome == TokenStoreOutcome::Success) {
        m_stored.remove(profileId);
      }
      m_lastCompleted.insert(profileId, CompletedRecord{callback, result});
      callback(result); // synchronous/reentrant, unlike complete() below
      return;
    }
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

// The following three outcomes model a restored token whose durable
// endpoint binding (see TokenEnvelope.h) does not vouch for the profile's
// CURRENT endpoint -- either because the endpoint genuinely changed since
// the token was saved (BindingMismatch), the token predates envelope
// binding entirely (LegacyUnbound), or the stored payload is corrupt
// (Malformed). All three are untrusted-origin outcomes that the
// coordinator must treat identically: never used for auth, and cleaned up
// via deleteUntrustedRestoredToken().
TokenStoreResult bindingMismatchResult() {
  return TokenStoreResult{TokenStoreOutcome::BindingMismatch,
                          QStringLiteral("endpoint binding mismatch"),
                          QString()};
}

TokenStoreResult legacyUnboundResult() {
  return TokenStoreResult{TokenStoreOutcome::LegacyUnbound,
                          QStringLiteral("legacy unbound token"), QString()};
}

TokenStoreResult malformedEnvelopeResult() {
  return TokenStoreResult{TokenStoreOutcome::Malformed,
                          QStringLiteral("malformed envelope"), QString()};
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
  void repeatedStartWhileStalledQueuesAtMostOneRestoreRead();
  void repeatedStartWhileInitialReadInFlightQueuesAtMostOneRestoreRead();
  void
  repeatedStartWhileOrdinaryDeleteInFlightQueuesAtMostOneRestoreReadBehindIt();
  void
  repeatedStartWhileSaveThenCompensatingDeleteChainInFlightDedupsRestoreRead();
  void staleReadAttemptCallbackCannotCompleteNewerReadAttempt();

  // Profile reload / endpoint identity (same profileId, changed content)
  void reloadedProfileNameOnlyChangeUpdatesNameKeepsEndpointAndToken();
  void reloadedProfileEndpointChangeDeletesOldTokenBeforeRestoreRead();
  void reloadedProfileEndpointChangeDuringInFlightSaveCompensatesExactlyOnce();
  void reloadedProfileEndpointChangeDeleteFailureBlocksThenRetrySucceeds();
  void
  reloadedProfileCanonicallyEquivalentUrlIsNotEndpointChangeButPathCaseIs();
  void reloadedProfileExactUnchangedContentEmitsNoSignals();
  void
  reloadedProfileEndpointChangeStaleInFlightReadCannotBypassRequiredDelete();
  void
  reloadedProfileEndpointChangeRequiredDeleteFailureAfterStaleReadBlocksThenRetrySucceeds();
  void
  reloadedProfileEndpointChangeQueuedNotInFlightOldReadCannotBypassRequiredDelete();
  void
  reloadedProfileEndpointChangeRepeatedStartDedupesOnlyFreshReadAndStaleReplayCannotCorruptIt();

  // Durable endpoint-bound credential envelope (protects unselected
  // profiles, removed/re-added UUIDs, and cross-process-restart state
  // that in-memory epochs alone cannot see).
  void restoreReadReceivesExpectedEndpointIdentityMatchingSelectedProfile();
  void
  restoreBindingMismatchDeletesUntrustedTokenBeforeAnyAuthThenAllowsFreshSignIn();
  void restoreLegacyUnboundTokenIsDeletedBeforeAnyAuthThenAllowsFreshSignIn();
  void restoreMalformedEnvelopeIsDeletedBeforeAnyAuthThenAllowsFreshSignIn();
  void restoreUntrustedTokenDeleteFailureBlocksNewAuthUntilRetrySucceeds();
  void unselectedProfileEndpointChangeReservesCleanupBeforeItIsEverSelected();
  void removedProfileOnReloadReservesOrphanCredentialCleanup();
  void
  reAddedSameUuidWithDifferentEndpointDoesNotTrustPriorSelectedProfileEpoch();
  void reAddedSameUuidWithSameEndpointRestoresTokenNormally();
  void
  multipleProfilesChangedOrRemovedOnSingleReloadEachReserveExactlyOneCleanup();
  void selectedProfileEndpointChangeDoesNotDoubleInvalidateViaReconcileHelper();
  void freshSaveAfterEndpointChangeBindsNewTokenToNewEndpointIdentityOnly();
  void staleUntrustedTokenDeleteCallbackReplayCannotCorruptSubsequentRestore();
  void diagnosticsNeverContainEndpointIdentityForBindingOutcomes();

  // Centralized all-profile reconciliation covering the NEWLY selected ID
  // (not just old-current/unselected) on a single reload.
  void
  newlySelectedProfileEndpointChangeInSameReloadStillReservesRequiredDeleteBeforeFreshRead();
  void
  newlySelectedProfileEndpointChangeQueuedNotInFlightOldReadStillBlockedByRequiredDelete();
  void
  multipleChangedProfilesOnReloadThatAlsoSelectsOneOfThemReserveCleanupDeterministically();
  void
  repeatedStartWhileNewlySelectedProfileEndpointChangeDedupesOnlyFreshRead();

  // Orphan credential-cleanup-failure visibility/retry independent of
  // current selection.
  void
  removedProfileDeleteFailureIsVisibleAndRetryableWhileDifferentProfileRemainsCurrent();
  void
  twoRemovedProfilesWithIndependentDeleteFailuresRetryFifoOrderAndStaleCallbackCannotClearWrongOne();

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
  void
  directStateChangedReentrancyDuringSigningOutSwitchProfileCannotAbandonDelete();
  void directStateChangedReentrancyDuringSigningOutStartCannotAbandonDelete();
  void
  directSelectedProfileChangedReentrancySignOutCannotDeleteWrongProfileDuringSwitch();
  void
  directCurrentUserChangedReentrancyDuringSwitchSignsOutOldProfileNotNewProfile();
  void
  directCurrentUserChangedReentrancyDuringSwitchToThirdProfileLeavesAAndBTokensUntouched();
  void
  directCurrentUserChangedReentrancyDuringSwitchRestartLeavesSelectionCoherent();
  void
  directCurrentUserChangedReentrancyDuringStartSignOutIsNoOpAndStartProceeds();
  void
  synchronousTokenStoreDeleteDuringSignOutExposesCoherentSignedOutNeverStaleSignedIn();
  void coordinatorDestructionDuringRequiredDeleteFailureStateChangedIsSafe();
  void repeatedRetryFailuresRemainActionableUntilEventualSuccess();
  void
  directStateChangedReentrancyDuringStartNestedRestartStillDeliversCurrentUserChanged();
  void
  directStateChangedReentrancyDuringSwitchNestedSwitchStillDeliversAllOwedSignals();
  void
  coordinatorDestructionDuringSelectedProfileChangedEmissionDuringSwitchIsSafe();
  void unguardedStateChangedHandlerCallingStartWheneverLoadingDoesNotRecurse();
  void repeatedIdenticalRetryFailureDiagnosticEmitsStateChangedOnlyOnce();
  void unchangedSwitchProfileReselectionEmitsNoSignals();

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
  // longer current. It must still durably stall this profile's FIFO --
  // and, per this round's fix, that failure must be visible/actionable
  // even though the profile that failed is NOT the one currently
  // selected: state()/diagnostic() report the orphaned obligation
  // (SecureStorageUnavailable) rather than the currently-selected
  // profile's own, genuinely still-progressing ProbingCapabilities.
  h.tokenStore.complete(profileId, backendErrorResult());
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::SecureStorageUnavailable);
  QVERIFY(h.coordinator->selectedProfileId() == customProfile->profileId());

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
    repeatedStartWhileStalledQueuesAtMostOneRestoreRead() {
  // Exact regression from review: startCredentialRestore() previously
  // enqueued a brand-new TokenOpKind::Read every single time it ran, even
  // while a required deletion was still durably stalling this profile's
  // FIFO. Repeated start()/switchProfile() calls back to the same stalled
  // profile (e.g. a user mashing retry/restart while the delete was
  // failing) would therefore silently accumulate multiple queued reads
  // behind the stuck delete -- unbounded queue growth and duplicate
  // secure-store I/O once the delete was eventually retried. Prove that no
  // matter how many times start() is called while stalled, only ONE
  // restore read is ever actually dispatched to the real store once the
  // delete succeeds -- not one per repeated start() call.
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
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete

  // Repeatedly restart while the profile remains stalled behind the
  // failed, still-undispatched delete. Each call reaches
  // startCredentialRestore() again with a freshly-bumped generation.
  for (int i = 0; i < 5; ++i) {
    h.coordinator->start();
    QVERIFY(
        pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
    h.probeFactory.current()->complete(compatibleProbeResult());
    QVERIFY(pumpEventsUntil([&h] {
      return h.coordinator->state() ==
             SessionCoordinator::State::SecureStorageUnavailable;
    }));
  }
  // None of the five repeated restarts touched the real store: it is
  // still stalled behind the one still-undispatched delete, so the call
  // log must be unchanged.
  QCOMPARE(h.tokenStore.calls.size(), 3);

  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());

  // Exactly ONE restore read must now dispatch to the real store -- not
  // five -- proving the five stalled start() calls were deduplicated to a
  // single queued read rather than each enqueuing their own.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 5);
  QCOMPARE(h.tokenStore.calls.at(4).kind, QStringLiteral("read"));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  // If a duplicate read had still been queued behind the deduplicated
  // one, completing the single read above would have immediately
  // dispatched it too (FIFO dispatch on dequeue is synchronous), leaving
  // another pending op needing its own complete() call. Confirm the FIFO
  // is fully drained with no extra dispatch and no extra call recorded.
  QVERIFY(!h.tokenStore.hasPending(profileId));
  QCOMPARE(h.tokenStore.calls.size(), 5);
}

void SessionCoordinatorTests::
    repeatedStartWhileInitialReadInFlightQueuesAtMostOneRestoreRead() {
  // Exact regression from review: the prior dedup fix (see
  // repeatedStartWhileStalledQueuesAtMostOneRestoreRead() above) only
  // applied while the profile's FIFO was "stalled" behind a failed
  // required Delete. Repeated start() calls while the INITIAL restore
  // Read is still merely in flight (never having failed at all) were
  // still each enqueuing another Read behind it, unboundedly. Prove that
  // however many times start() is called while that one Read remains
  // outstanding, only ONE real store read is ever dispatched, and only
  // the NEWEST generation's continuation actually mutates state once it
  // completes -- an earlier (now-discarded) generation's continuation
  // must never fire and must never block the newest one from ever
  // completing.
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 1);
  QCOMPARE(h.tokenStore.calls.first().kind, QStringLiteral("read"));

  // Repeatedly restart while that same Read remains outstanding. Each
  // call reaches startCredentialRestore() again with a freshly-bumped
  // generation and must rebind the SAME already-dispatched Read's
  // continuation rather than enqueue a new one.
  for (int i = 0; i < 5; ++i) {
    h.coordinator->start();
    QVERIFY(
        pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
    h.probeFactory.current()->complete(compatibleProbeResult());
    QVERIFY(pumpEventsUntil([&h] {
      return h.coordinator->state() ==
             SessionCoordinator::State::RestoringCredential;
    }));
  }
  // None of the five repeated restarts touched the real store again: the
  // one Read dispatched at the very start is still the only one ever
  // issued.
  QCOMPARE(h.tokenStore.calls.size(), 1);
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);

  // Complete the single dispatched Read. If the five repeated restarts had
  // each enqueued their own duplicate Read, this completion would either
  // (a) invoke a now-stale generation's continuation that silently no-ops
  // (leaving state stuck at RestoringCredential forever, so the
  // pumpEventsUntil below would time out and fail), or (b) leave further
  // duplicate Reads pending after this one dispatches (caught by the
  // hasPending()/calls.size() assertions below). Only the deduplication
  // fix makes this complete() call resolve the CURRENT (latest)
  // generation's flow.
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(!h.tokenStore.hasPending(profileId));
  QCOMPARE(h.tokenStore.calls.size(), 1);
}

void SessionCoordinatorTests::
    repeatedStartWhileOrdinaryDeleteInFlightQueuesAtMostOneRestoreReadBehindIt() {
  // Same defect as above, but for a Delete that is in flight and has NOT
  // (yet, or ever) failed -- so the profile is never marked "stalled" at
  // all (m_profileFifoStalled never gets populated), meaning the OLD
  // stalled-only dedup check would never have applied here even once.
  // Repeated start() calls while an ordinary sign-out Delete is still
  // outstanding must still dedup the restore Read queued behind it.
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));

  // Repeatedly restart while the delete remains outstanding (never
  // failed, never stalled).
  for (int i = 0; i < 4; ++i) {
    h.coordinator->start();
    QVERIFY(
        pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
    h.probeFactory.current()->complete(compatibleProbeResult());
    QVERIFY(pumpEventsUntil([&h] {
      return h.coordinator->state() ==
             SessionCoordinator::State::RestoringCredential;
    }));
  }
  // The delete is still the only thing ever dispatched; the deduplicated
  // restore Read is merely queued behind it, never touching the real
  // store yet.
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);

  h.tokenStore.complete(profileId, successWriteResult());
  // Exactly ONE restore read now dispatches -- not five (one per repeated
  // start() call).
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("read"));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(!h.tokenStore.hasPending(profileId));
  QCOMPARE(h.tokenStore.calls.size(), 4);
}

void SessionCoordinatorTests::
    repeatedStartWhileSaveThenCompensatingDeleteChainInFlightDedupsRestoreRead() {
  // Same defect as above, but behind a genuine Save-then-compensating-
  // Delete chain (see perProfileFifoOrdersSaveBeforeLaterDelete()): an
  // in-flight fresh-token Save is abandoned by switching away, reserving
  // a compensating cleanup Delete behind it, and repeated restarts back
  // to that same profile (while the Save is still outstanding, well
  // before the Delete even dispatches) must still dedup the restore Read
  // queued behind the whole two-op chain to exactly one.
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  h.bootToSignedOut();
  const QString profileId = h.coordinator->selectedProfileId(); // A
  const QString customId = customProfile->profileId();          // B

  h.coordinator->signIn(QStringLiteral("bob@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  h.authClient.completeToken(1, tokenSuccess(QStringLiteral("session-token")));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  h.authClient.completeUser(2, userSuccess(QStringLiteral("bob"),
                                           QStringLiteral("bob@example.test")));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 2); // read, save (in flight for A)

  // Abandon A's in-flight Save by switching to B: reserves a compensating
  // Delete behind it for A, without dispatching it yet.
  h.coordinator->switchProfile(customId);
  QVERIFY(h.tokenStore.hasPending(profileId)); // still A's original Save
  QCOMPARE(h.tokenStore.calls.size(), 2);      // no delete dispatched yet

  // Switch back to A repeatedly (via start(), since switchProfile() to
  // the already-selected profile is a no-op) while A's Save+Delete chain
  // remains entirely unresolved. Each call reaches startCredentialRestore()
  // for A again and must dedup its restore Read against the chain's tail.
  h.coordinator->switchProfile(profileId);
  for (int i = 0; i < 3; ++i) {
    h.coordinator->start();
    QVERIFY(
        pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
    h.probeFactory.current()->complete(compatibleProbeResult());
    QVERIFY(pumpEventsUntil([&h] {
      return h.coordinator->state() ==
             SessionCoordinator::State::RestoringCredential;
    }));
  }
  // Still nothing new dispatched to the real store: A's Save is still the
  // only in-flight operation, with its compensating Delete and exactly
  // one deduplicated restore Read both merely queued behind it.
  QCOMPARE(h.tokenStore.calls.size(), 2);
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);

  // Release the Save: the FIFO must advance to dispatch the compensating
  // Delete next.
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));

  // Release the Delete: exactly ONE restore Read now dispatches -- not
  // four (one per repeated restart back to A).
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("read"));

  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(!h.tokenStore.hasPending(profileId));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  // The abandoned Save's token was truly removed by the compensating
  // cleanup Delete, and the final restore correctly found nothing left.
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());
}

void SessionCoordinatorTests::
    staleReadAttemptCallbackCannotCompleteNewerReadAttempt() {
  // Complements the dedup tests above: proves the SAME per-attempt
  // opId/attemptId dispatch guard that protects Delete retries (see
  // retryCannotConcurrentlyRedispatchStalledDeleteAndDuplicateCallbackCannotDequeueNextOp())
  // also protects a Read that fails and is retried as an entirely NEW
  // attempt (a fresh TokenOp with its own opId, since a failed Read --
  // unlike a failed Delete -- is dequeued normally rather than left
  // stalled at the head; see startFrontTokenOp()). Replaying the FIRST,
  // now-stale attempt's completion after the SECOND attempt has already
  // been dispatched must never be able to dequeue/complete that second,
  // still-outstanding attempt.
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 1); // attempt 1, dispatched

  // Fail attempt 1: a failed Read is dequeued normally (not left stalled
  // -- only a failed Delete blocks the FIFO), surfacing
  // SecureStorageUnavailable with an actionable retry().
  h.tokenStore.complete(profileId, accessDeniedResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 1);

  // retry() re-enters startCredentialRestore(), which enqueues (and
  // dispatches, since the queue is now empty) an entirely NEW Read --
  // attempt 2, with its own opId and attemptId.
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 2);
  QCOMPARE(h.tokenStore.calls.at(1).kind, QStringLiteral("read"));
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::RestoringCredential);

  // Replay attempt 1's stale (AccessDenied) completion -- captured when it
  // completed above and not yet overwritten, since attempt 2 has not
  // completed yet. Its (opId, attemptId) no longer matches the profile's
  // current dispatch record (which now belongs to attempt 2), so
  // startFrontTokenOp()'s guard must silently reject it: no state change,
  // no dequeue, and attempt 2 must remain fully able to complete
  // afterward.
  h.tokenStore.replayLastCompletion(profileId);
  // state() staying RestoringCredential is trivially true whether or not
  // this replay was actually processed and rejected, so a
  // pumpEventsUntil() on that predicate would be vacuously satisfied
  // immediately without draining the fake's queued callback at all. Drain
  // unconditionally instead; the subsequent assertions (state still
  // RestoringCredential, no extra store call/dequeue, and attempt 2 still
  // able to complete normally below) are what decisively prove the stale
  // replay was actually delivered and rejected, not merely never
  // processed.
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  // The stale replay changed nothing: still RestoringCredential (not
  // reverted to SecureStorageUnavailable), and attempt 2 is still the
  // sole outstanding real-store operation.
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::RestoringCredential);
  QCOMPARE(h.tokenStore.calls.size(), 2);
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);

  // Attempt 2 completes normally and drives the real transition -- proof
  // the stale replay above never consumed/corrupted it.
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(!h.tokenStore.hasPending(profileId));
  QCOMPARE(h.tokenStore.calls.size(), 2);
}

void SessionCoordinatorTests::
    retryCannotConcurrentlyRedispatchStalledDeleteAndDuplicateCallbackCannotDequeueNextOp() {
  // Exact regression from review: a required Delete fails and stalls; a
  // retry is dispatched but held (not yet resolved). While that attempt is
  // STILL outstanding, a genuine production path (start() -> probe
  // completion -> startCredentialRestore()'s stalled check) re-installs
  // the profile's retry() action and queues a restore Read behind the
  // still-stuck delete -- this is deliberate: a previous version of this
  // test merely called retry() a second/third time immediately, which
  // found m_retryAction already null (retry() always consumes it via
  // std::exchange) and therefore never even reached
  // retryStuckProfileTokenOp()/startFrontTokenOp()'s real inFlight guard,
  // making the "no second dispatch" assertions vacuously true. This
  // version's second retry() call is only possible because a real code
  // path put a fresh action in place, so reaching the inFlight guard --
  // and being rejected by it -- is what this test actually exercises.
  //
  // The decisive part (redesigned per review: the previous version here
  // only checked that state()/calls.size() were UNCHANGED after replaying
  // a stale attempt-1 callback, which is true whether or not the replay
  // was correctly rejected, since the target state was already
  // SecureStorageUnavailable either way): after replaying attempt-1's
  // stale, already-processed failure callback, this test immediately
  // completes the REAL, still-outstanding attempt #2 with SUCCESS and
  // REQUIRES the queued restore Read to actually dispatch as a result. If
  // attemptId matching were removed (leaving only opId matching, which
  // the stale replay would still satisfy since it is the same logical
  // delete op), the stale replay would incorrectly clear
  // ProfileTokenDispatch::inFlight; attempt #2's genuine success
  // completion would then be wrongly rejected by the "!dispatchIt->inFlight"
  // guard (since inFlight had already been wrongly cleared), the Read
  // would never dispatch, and this test would fail via pumpEventsUntil()
  // timing out -- a decisive, non-vacuous signal. (Verified locally: with
  // the attemptId comparison temporarily removed, this exact test fails;
  // production code was restored immediately afterward.)
  Harness h;
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
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete (attempt #1)

  // First retry: dispatches attempt #2 of the same logical delete op
  // (same opId, new attemptId), but it is held unresolved for most of
  // this test.
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("delete"));

  // A genuine production path re-installs the retry action AND queues a
  // restore Read behind the still-stuck delete, while attempt #2 is still
  // outstanding: start() restarts the whole session (a fresh probe,
  // re-selecting the SAME still-current profile), and once that new probe
  // reports Compatible, startCredentialRestore() observes this profile is
  // still stalled and re-installs m_retryAction plus queues the Read --
  // without touching the real ITokenStore at all, since the Read simply
  // queues behind the still-in-flight delete.
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 4); // no new store dispatch yet

  // The second retry() call now genuinely reaches
  // retryStuckProfileTokenOp()/startFrontTokenOp(): it must be rejected by
  // the real dispatch.inFlight guard (attempt #2 is still outstanding), so
  // no additional store call is issued and no overlapping dispatch is ever
  // observed by the fake. If the guard (or its opId/attemptId matching)
  // were removed, this would instead dispatch a concurrent second delete.
  h.coordinator->retry();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);

  // A stale/duplicate replay of attempt #1's ALREADY-PROCESSED failure
  // callback (captured before attempt #2 was ever dispatched) must be
  // rejected by attemptId matching: it must not touch the queue or
  // corrupt the still-outstanding attempt #2. No NEW store call is issued
  // by this replay either way (the deleteFailed branch never dispatches),
  // so this drain-and-check is a sanity check only -- the decisive proof
  // is what happens to attempt #2's genuine completion immediately below.
  h.tokenStore.replayLastCompletion(profileId);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  QCOMPARE(h.tokenStore.calls.size(), 4); // unchanged: no new dispatch

  // DECISIVE: complete the REAL, still-outstanding attempt #2 with
  // SUCCESS and require the queued restore Read to actually dispatch.
  // This is what the stale replay above must NOT have been able to
  // prevent: if attemptId matching had been wrongly bypassed by the
  // stale replay, this genuine completion would be silently discarded
  // (see the guard analysis in this test's header comment) and the
  // Read would never dispatch, so pumpEventsUntil() below would time out
  // and fail this test.
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.tokenStore.calls.size() == 5 &&
           h.tokenStore.calls.last().kind == QStringLiteral("read");
  }));
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());

  // Once the Read is genuinely current/in-flight, replay the delete's own
  // (now most-recently-completed) callback again: it must be rejected by
  // the opId check alone (the queue head is now the Read op, a different
  // opId entirely), so it cannot dequeue, complete, or otherwise corrupt
  // the still-outstanding Read.
  h.tokenStore.replayLastCompletion(profileId);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  QCOMPARE(h.tokenStore.calls.size(), 5); // unchanged: still just the Read
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::SecureStorageUnavailable); // untouched

  // The genuine, still-outstanding Read completes with its own distinct
  // result, and the coordinator reaches its correct final state.
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);
}

void SessionCoordinatorTests::
    repeatedRetryFailuresRemainActionableUntilEventualSuccess() {
  // Complements the redesigned stale-attempt/inFlight-guard test above,
  // which focuses on a single retry plus a queued Read racing a stale
  // callback. This test instead proves REPEATED retry failures (no
  // profile switching, no queued Read behind the delete) each remain
  // genuinely actionable -- decisively, via the NEXT retry() call's
  // dispatch actually reaching the real store -- rather than via a
  // same-state check that would pass whether or not the failure was
  // actually processed (state() stays SecureStorageUnavailable across
  // every one of these failures regardless of whether the review's bug
  // is present).
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, backendErrorResult()); // failure #1
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete #1

  for (int attempt = 2; attempt <= 4; ++attempt) {
    // Decisive: retry() only dispatches a new store call if the PRIOR
    // failure was fully processed and genuinely reinstated the retry
    // action.
    h.coordinator->retry();
    QVERIFY(pumpEventsUntil(
        [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
    QCOMPARE(h.tokenStore.calls.size(), attempt + 2);
    QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
    h.tokenStore.complete(profileId, backendErrorResult());
    // state() stays SecureStorageUnavailable across every one of these
    // failures regardless of whether this exact completion was actually
    // processed yet, so pumpEventsUntil() on that predicate would be
    // vacuously true immediately without ever draining the fake's queued
    // callback. Drain unconditionally instead; the NEXT iteration's
    // retry() dispatch (checked above) is what decisively proves this
    // failure was fully processed and genuinely reinstated the retry
    // action.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCOMPARE(h.coordinator->state(),
             SessionCoordinator::State::SecureStorageUnavailable);
  }
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);

  // The final retry succeeds: the coordinator reaches SignedOut directly,
  // and the token is genuinely gone.
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());
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

void SessionCoordinatorTests::
    directStateChangedReentrancyDuringSigningOutSwitchProfileCannotAbandonDelete() {
  // Exact regression from review: signOut() previously emitted
  // setState(SigningOut) BEFORE enqueuing the required delete, so a
  // directly-connected stateChanged() handler observing SigningOut could
  // call switchProfile() (bumping the generation) and cause the
  // post-emission guard to return before enqueueTokenOp(Delete) ever ran
  // -- silently dropping the deletion forever. The fix reserves the
  // delete BEFORE the SigningOut notification, so it must survive
  // regardless of what a reentrant observer does afterward.
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();
  const QString customId = customProfile->profileId();

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::stateChanged,
      h.coordinator.get(),
      [&h, &handled, customId] {
        if (handled ||
            h.coordinator->state() != SessionCoordinator::State::SigningOut) {
          return;
        }
        handled = true;
        // Reentrantly abandon this profile from inside signOut()'s own
        // setState(SigningOut) emission.
        h.coordinator->switchProfile(customId);
      },
      Qt::DirectConnection);

  h.coordinator->signOut();

  QVERIFY(handled);
  // Exactly one delete was ever dispatched for the abandoned profile,
  // despite the reentrant switch -- the obligation was reserved before
  // the notification that triggered the switch.
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));
  const QString actualDeletedProfile = h.tokenStore.calls.at(2).profileId;
  QVERIFY(actualDeletedProfile == profileId);

  // The reentrant switch fully superseded signOut(): the new profile is
  // selected.
  const QString actualSelected = h.coordinator->selectedProfileId();
  QVERIFY(actualSelected == customId);

  // Completing the reserved delete removes the old profile's token --
  // returning to it later cannot find a restored credential.
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return !h.tokenStore.hasPending(profileId); }));
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());

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

  // Returning to the abandoned profile confirms the deletion was durable:
  // there is no token left to restore.
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
}

void SessionCoordinatorTests::
    directStateChangedReentrancyDuringSigningOutStartCannotAbandonDelete() {
  // Same hazard as the switchProfile() variant above, but the reentrant
  // observer calls start() (a full restart) instead: the required delete
  // must still survive.
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();
  const int probesBefore = h.probeFactory.totalCreated();

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
        // Reentrantly restart the whole session from inside signOut()'s
        // own setState(SigningOut) emission.
        h.coordinator->start();
      },
      Qt::DirectConnection);

  h.coordinator->signOut();

  QVERIFY(handled);
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));
  const QString actualDeletedProfile = h.tokenStore.calls.at(2).profileId;
  QVERIFY(actualDeletedProfile == profileId);
  // start() discarded the old probe and created its own fresh one.
  QCOMPARE(h.probeFactory.totalCreated(), probesBefore + 1);

  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return !h.tokenStore.hasPending(profileId); }));
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());

  // The restarted session's own probe/restore flow completes cleanly,
  // finding no token (since it was just durably deleted above).
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    directSelectedProfileChangedReentrancySignOutCannotDeleteWrongProfileDuringSwitch() {
  // Exact regression from review: switchProfile() previously mutated
  // m_selectedProfileId/m_currentProfile and emitted selectedProfileChanged()
  // for the NEW profile while state() still reported the OLD profile's
  // SignedIn (setState() to a non-SignedIn value only happened later,
  // inside startProbe()). A directly-connected selectedProfileChanged()
  // handler that called signOut() could therefore observe the NEW
  // profile's ID alongside the stale SignedIn state and delete the NEW
  // profile's token -- even though it had never authenticated. The fix
  // publishes a coherent non-SignedIn transitional snapshot (Loading,
  // nil user) BEFORE the profile itself changes, so this reentrant
  // signOut() must now see state() != SignedIn and be rejected as a
  // no-op, regardless of which profile selectedProfileId() reports.
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  const QString token = QStringLiteral("session-token");
  bootToSignedIn(h, token);
  const QString profileId = h.coordinator->selectedProfileId();
  const QString customId = customProfile->profileId();

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::selectedProfileChanged,
      h.coordinator.get(),
      [&h, &handled] {
        if (handled) {
          return;
        }
        handled = true;
        h.coordinator->signOut();
      },
      Qt::DirectConnection);

  h.coordinator->switchProfile(customId);

  QVERIFY(handled);
  // No delete was ever dispatched: signOut() observed a coherent
  // non-SignedIn transitional state (never a hybrid of the old SignedIn
  // state with the new profile's identity) and rejected the call as a
  // no-op.
  QCOMPARE(h.tokenStore.calls.size(), 2); // just the original read + save
  const std::optional<QString> actualStored =
      h.tokenStore.storedToken(profileId);
  QVERIFY(actualStored.has_value());
  QVERIFY(*actualStored == token);

  // The switch itself still proceeds normally, and the persisted
  // selection agrees with the in-memory one -- no split.
  const QString actualSelected = h.coordinator->selectedProfileId();
  QVERIFY(actualSelected == customId);
  QVERIFY(h.profileStore.selectedId == customId);
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
    directCurrentUserChangedReentrancyDuringSwitchSignsOutOldProfileNotNewProfile() {
  // Exact regression from review: a previous version of switchProfile()
  // assigned m_currentProfile/m_selectedProfileId only AFTER emitting
  // currentUserChanged() (which fired while state() still reported the
  // OLD profile's stale SignedIn). A directly-connected currentUserChanged()
  // handler that called signOut() therefore observed a coherent OLD
  // snapshot and correctly deleted the OLD profile's token -- but the
  // generation bump this caused made the OUTER switchProfile() call abort
  // AFTER it had already persisted the NEW selection to storage above,
  // leaving the persisted selection (NEW profile) permanently split from
  // the in-memory selection (reverted to the OLD profile).
  //
  // The fix assigns the ENTIRE new snapshot -- Loading state, cleared
  // identity, AND the new selected profile -- together BEFORE any
  // notification (including this currentUserChanged() emission) is
  // fired. A reentrant signOut() from here therefore now observes
  // state() == Loading (never SignedIn) and is rejected as a safe no-op,
  // exactly like the selectedProfileChanged() case above: neither the
  // OLD nor the NEW profile's token is ever deleted, and the persisted
  // selection can never diverge from the in-memory one.
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  const QString token = QStringLiteral("session-token");
  bootToSignedIn(h, token);
  const QString profileId = h.coordinator->selectedProfileId();
  const QString customId = customProfile->profileId();

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::currentUserChanged,
      h.coordinator.get(),
      [&h, &handled] {
        if (handled) {
          return;
        }
        handled = true;
        h.coordinator->signOut();
      },
      Qt::DirectConnection);

  h.coordinator->switchProfile(customId);

  QVERIFY(handled);
  // No delete was ever dispatched: signOut() observed a coherent
  // non-SignedIn transitional state (Loading, already the NEW profile
  // selected) and rejected the call as a no-op.
  QCOMPARE(h.tokenStore.calls.size(), 2); // just the original read + save
  const std::optional<QString> actualStored =
      h.tokenStore.storedToken(profileId);
  QVERIFY(actualStored.has_value());
  QVERIFY(*actualStored == token);

  // The switch proceeds normally, and -- critically -- the in-memory
  // selection and the PERSISTED selection agree: there is no split.
  const QString actualSelected = h.coordinator->selectedProfileId();
  QVERIFY(actualSelected == customId);
  QVERIFY(h.profileStore.selectedId == customId);

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
    directCurrentUserChangedReentrancyDuringSwitchToThirdProfileLeavesAAndBTokensUntouched() {
  // Extends the regression above to a THIRD profile: a directly-connected
  // currentUserChanged() handler fires while switchProfile(B) is in
  // progress and reentrantly calls switchProfile(C) instead of signOut().
  // Because the complete new (Loading, cleared identity, profile B)
  // snapshot is already assigned before this emission, the reentrant
  // switchProfile(C) call itself runs to completion coherently (persisting
  // C, bumping the generation again, and starting C's own probe), and the
  // OUTER switchProfile(B) call is cleanly superseded immediately
  // afterward -- never touching A's or B's token, and never leaving the
  // persisted selection different from the final in-memory one (C).
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto profileB = ServerProfile::custom(
      QStringLiteral("B"), QStringLiteral("https://b.example.test"));
  const auto profileC = ServerProfile::custom(
      QStringLiteral("C"), QStringLiteral("https://c.example.test"));
  QVERIFY(profileB.has_value());
  QVERIFY(profileC.has_value());
  h.profileStore.profiles = {hosted, *profileB, *profileC};
  h.profileStore.selectedId = hosted.profileId();
  const QString token = QStringLiteral("session-token");
  bootToSignedIn(h, token);
  const QString profileIdA = h.coordinator->selectedProfileId();
  const QString idB = profileB->profileId();
  const QString idC = profileC->profileId();

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::currentUserChanged,
      h.coordinator.get(),
      [&h, &handled, idC] {
        if (handled) {
          return;
        }
        handled = true;
        h.coordinator->switchProfile(idC);
      },
      Qt::DirectConnection);

  h.coordinator->switchProfile(idB);

  QVERIFY(handled);
  // Neither A's nor B's token was ever touched: no delete was dispatched
  // for either, and B never even had a token to begin with.
  QCOMPARE(h.tokenStore.calls.size(), 2); // just the original read + save
  const std::optional<QString> actualStoredA =
      h.tokenStore.storedToken(profileIdA);
  QVERIFY(actualStoredA.has_value());
  QVERIFY(*actualStoredA == token);
  QVERIFY(!h.tokenStore.storedToken(idB).has_value());

  // The reentrant switch to C fully superseded the outer switch to B: the
  // final in-memory selection and the persisted selection both agree on
  // C, never on B or a split between the two.
  const QString actualSelected = h.coordinator->selectedProfileId();
  QVERIFY(actualSelected == idC);
  QVERIFY(h.profileStore.selectedId == idC);

  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idC] { return h.tokenStore.hasPending(idC); }));
  h.tokenStore.complete(idC, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    directCurrentUserChangedReentrancyDuringSwitchRestartLeavesSelectionCoherent() {
  // Same window as above, but the reentrant call is start() instead of
  // switchProfile()/signOut(): a full restart reentered from within
  // switchProfile(B)'s currentUserChanged() emission. Because the new
  // selection (B) was already persisted BEFORE any notification was
  // emitted, the reentrant start() reloads profiles/selection fresh and
  // finds B already persisted -- so it restarts cleanly onto B, and the
  // outer switchProfile(B) call is superseded immediately afterward
  // without ever re-persisting or touching A's token.
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto profileB = ServerProfile::custom(
      QStringLiteral("B"), QStringLiteral("https://b.example.test"));
  QVERIFY(profileB.has_value());
  h.profileStore.profiles = {hosted, *profileB};
  h.profileStore.selectedId = hosted.profileId();
  const QString token = QStringLiteral("session-token");
  bootToSignedIn(h, token);
  const QString profileIdA = h.coordinator->selectedProfileId();
  const QString idB = profileB->profileId();

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::currentUserChanged,
      h.coordinator.get(),
      [&h, &handled] {
        if (handled) {
          return;
        }
        handled = true;
        h.coordinator->start();
      },
      Qt::DirectConnection);

  h.coordinator->switchProfile(idB);

  QVERIFY(handled);
  // A's token was never touched by any delete.
  QCOMPARE(h.tokenStore.calls.size(), 2); // just the original read + save
  const std::optional<QString> actualStoredA =
      h.tokenStore.storedToken(profileIdA);
  QVERIFY(actualStoredA.has_value());
  QVERIFY(*actualStoredA == token);

  // The reentrant restart lands on B, and the persisted selection agrees.
  const QString actualSelected = h.coordinator->selectedProfileId();
  QVERIFY(actualSelected == idB);
  QVERIFY(h.profileStore.selectedId == idB);

  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  h.tokenStore.complete(idB, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    directCurrentUserChangedReentrancyDuringStartSignOutIsNoOpAndStartProceeds() {
  // Mirrors the switchProfile() coverage above, but for start(): a
  // directly-connected currentUserChanged() handler fires during start()'s
  // own coherent-snapshot publication (state already Loading, identity
  // already cleared) and reentrantly calls signOut(). Because state() is
  // already Loading (never SignedIn) at that point, signOut() must be
  // rejected as a no-op -- the restart proceeds to completion normally,
  // and the existing token is never deleted.
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::currentUserChanged,
      h.coordinator.get(),
      [&h, &handled] {
        if (handled) {
          return;
        }
        handled = true;
        h.coordinator->signOut();
      },
      Qt::DirectConnection);

  h.coordinator->start();

  QVERIFY(handled);
  // signOut() observed state() == Loading (not SignedIn) and was rejected
  // as a no-op: no delete was ever dispatched, and the existing token is
  // untouched.
  QCOMPARE(h.tokenStore.calls.size(), 2); // just the original read + save
  const std::optional<QString> actualStored =
      h.tokenStore.storedToken(profileId);
  QVERIFY(actualStored.has_value());
  QVERIFY(*actualStored == QStringLiteral("session-token"));

  // start() proceeds normally to completion.
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    synchronousTokenStoreDeleteDuringSignOutExposesCoherentSignedOutNeverStaleSignedIn() {
  // Exact regression from review: signOut()'s reserved delete may resolve
  // SYNCHRONOUSLY (a pathological, but not contractually impossible,
  // ITokenStore backend), reentering handleSignOutDeletionResult() ->
  // clearCurrentUserAndSetStateIfCurrent() from within the very call to
  // enqueueTokenOp() inside signOut(), before signOut()'s own later
  // setState(SigningOut) line ever runs. A directly-connected
  // currentUserChanged() handler observing THIS emission must never see a
  // stale state()==SignedIn (which would let a reentrant signOut() pass
  // its "already signing out" guard and enqueue a duplicate delete):
  // clearCurrentUserAndSetStateIfCurrent() assigns the target state
  // (SignedOut, on synchronous success) and the cleared identity together
  // BEFORE emitting anything, so state() is already coherent at the very
  // first notification.
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  bool sawStaleSignedInDuringEmission = false;
  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::currentUserChanged,
      h.coordinator.get(),
      [&h, &sawStaleSignedInDuringEmission, &handled] {
        if (h.coordinator->state() == SessionCoordinator::State::SignedIn) {
          sawStaleSignedInDuringEmission = true;
        }
        if (handled) {
          return;
        }
        handled = true;
        h.coordinator->signOut(); // reentrant duplicate attempt
      },
      Qt::DirectConnection);

  h.tokenStore.synchronousDeleteResult = successWriteResult();
  h.coordinator->signOut();

  QVERIFY(handled);
  // The coherent-snapshot guarantee held: no directly-connected observer
  // ever saw the stale SignedIn state alongside the already-cleared
  // identity.
  QVERIFY(!sawStaleSignedInDuringEmission);
  // Exactly one delete was ever dispatched: the reentrant signOut() saw
  // state() already != SignedIn (SignedOut, published synchronously) and
  // was rejected as a no-op, so it could never enqueue a duplicate.
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, delete
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));
  QVERIFY(h.coordinator->state() == SessionCoordinator::State::SignedOut);
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());
  QVERIFY(h.coordinator->currentUsername().isEmpty());
}

void SessionCoordinatorTests::
    coordinatorDestructionDuringRequiredDeleteFailureStateChangedIsSafe() {
  // Exact regression from review: on a required-delete failure, the
  // central FIFO dispatcher copies the head op's onComplete callback,
  // then calls setState(SecureStorageUnavailable, ...) (a synchronous
  // stateChanged() emission), then invokes the copied callback. A
  // directly-connected stateChanged() handler observing that transition
  // can destroy the coordinator entirely; the dispatcher must re-check
  // `self` after setState() and never dereference the (now invalidated)
  // token-queue iterator or `self` itself again -- only the
  // already-copied callback value, and only if `self` is still alive.
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::stateChanged,
      h.coordinator.get(),
      [&h, &handled] {
        if (handled ||
            h.coordinator->state() !=
                SessionCoordinator::State::SecureStorageUnavailable) {
          return;
        }
        handled = true;
        // Destroy the coordinator from inside the very stateChanged()
        // emission that the deleteFailed branch triggers. This must not
        // crash/UB even though the dispatcher still has an iterator into
        // (and a raw `self`/`this` pointer to) the coordinator being
        // destroyed.
        h.coordinator.reset();
      },
      Qt::DirectConnection);

  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, backendErrorResult());
  QVERIFY(pumpEventsUntil([&handled] { return handled; }));

  QVERIFY(handled);
  QVERIFY(h.coordinator == nullptr);
  // Reaching this line at all (no crash/UB from dereferencing an iterator
  // into a destroyed coordinator's token-queue map) is the assertion.
}

void SessionCoordinatorTests::
    directStateChangedReentrancyDuringStartNestedRestartStillDeliversCurrentUserChanged() {
  // Exact regression from review: publishDirtyProperties() (formerly
  // publishTransitionSnapshot()) uses per-property mutation/notification
  // revisions rather than a coarse "was this property part of THIS batch"
  // boolean. A directly-connected stateChanged() handler that reentrantly
  // calls start() again bumps the generation WHILE the OUTER start()'s
  // own stateChanged() emission is still being delivered; by the time
  // control returns to the outer call, m_currentUser has already been
  // committed to nil (by the outer assignment; the inner restart's own
  // mutateCurrentUser(nullopt) is then a no-op since the value is already
  // nil, so it bumps no further revision). Prove currentUserChanged() is
  // still delivered EXACTLY ONCE for the whole nested batch -- never
  // zero (the old bug: dropped because |generation| already changed) and
  // never twice (a coarser fix that re-announces an already-settled
  // value would also be wrong).
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  QSignalSpy currentUserSpy(h.coordinator.get(),
                            &SessionCoordinator::currentUserChanged);
  QVERIFY(currentUserSpy.isValid());

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::stateChanged,
      h.coordinator.get(),
      [&h, &handled] {
        if (handled) {
          return;
        }
        handled = true;
        h.coordinator->start();
      },
      Qt::DirectConnection);

  h.coordinator->start();

  QVERIFY(handled);
  // The identity transition (signed-in user -> nil) was genuinely
  // committed exactly once (the inner restart's own attempt to clear it
  // again is a no-op) and must be announced exactly once, regardless of
  // the nested restart changing the generation mid-batch.
  QCOMPARE(currentUserSpy.count(), 1);
  QVERIFY(h.coordinator->currentUsername().isEmpty());
  // Existing token untouched: neither the outer nor the inner restart
  // ever calls signOut()/deletes anything.
  const std::optional<QString> actualStored =
      h.tokenStore.storedToken(profileId);
  QVERIFY(actualStored.has_value());
  QVERIFY(*actualStored == QStringLiteral("session-token"));

  // The nested restart proceeds to completion normally.
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    directStateChangedReentrancyDuringSwitchNestedSwitchStillDeliversAllOwedSignals() {
  // Same defect as above, exercised through switchProfile(B), whose own
  // transition also commits a NEW selected profile as part of the same
  // coherent snapshot. A directly-connected stateChanged() handler
  // reentrantly calls switchProfile(C) (to a THIRD profile) while the
  // OUTER switchProfile(B)'s own stateChanged() emission is still being
  // delivered -- the earliest possible interruption point, putting BOTH
  // of the outer call's remaining owed signals (currentUserChanged() and
  // selectedProfileChanged()) at risk. Under the revision-tracking
  // model: the outer's mutateSelectedProfile(B) bumps the profile
  // revision but has not yet been marked notified (outer is still stuck
  // inside its own stateChanged() emission); the nested switchProfile(C)
  // call then bumps the SAME revision further (B -> C) and its OWN
  // publishDirtyProperties() call marks/emits it, delivering C exactly
  // once. When the outer frame resumes and reaches its own selectedProfile
  // check, that revision is already notified, so it is NOT re-emitted.
  // Prove selectedProfileChanged() fires EXACTLY ONCE for the final
  // settled value C (never twice, and never zero), and currentUserChanged
  // fires EXACTLY ONCE for the identity clear (the nested call's own
  // re-clear is a no-op).
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto profileB = ServerProfile::custom(
      QStringLiteral("B"), QStringLiteral("https://b.example.test"));
  const auto profileC = ServerProfile::custom(
      QStringLiteral("C"), QStringLiteral("https://c.example.test"));
  QVERIFY(profileB.has_value());
  QVERIFY(profileC.has_value());
  h.profileStore.profiles = {hosted, *profileB, *profileC};
  h.profileStore.selectedId = hosted.profileId();
  const QString token = QStringLiteral("session-token");
  bootToSignedIn(h, token);
  const QString profileIdA = h.coordinator->selectedProfileId();
  const QString idB = profileB->profileId();
  const QString idC = profileC->profileId();

  QSignalSpy currentUserSpy(h.coordinator.get(),
                            &SessionCoordinator::currentUserChanged);
  QSignalSpy selectedProfileSpy(h.coordinator.get(),
                                &SessionCoordinator::selectedProfileChanged);
  QSignalSpy stateSpy(h.coordinator.get(), &SessionCoordinator::stateChanged);
  QVERIFY(currentUserSpy.isValid());
  QVERIFY(selectedProfileSpy.isValid());
  QVERIFY(stateSpy.isValid());

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::stateChanged,
      h.coordinator.get(),
      [&h, &handled, idC] {
        if (handled) {
          return;
        }
        handled = true;
        h.coordinator->switchProfile(idC);
      },
      Qt::DirectConnection);

  h.coordinator->switchProfile(idB);

  QVERIFY(handled);
  // Both signals were genuinely committed exactly once each (identity
  // cleared once; selection settled on C, with B's transient intermediate
  // value never externally announced) despite the nested switch changing
  // the generation mid-batch.
  QCOMPARE(currentUserSpy.count(), 1);
  QCOMPARE(selectedProfileSpy.count(), 1);
  // stateChanged: the outer's own (SignedIn -> Loading) is the first
  // genuine transition; the nested call's own attempt to reassign
  // (Loading, "") is a no-op under mutateState()'s dirty-check and
  // contributes no emission -- but the nested switchProfile(C) call runs
  // all the way to its own startProbe(), whose setState(ProbingCapabilities)
  // IS a genuinely new (state, diagnostic) value, producing a second,
  // real emission before the outer frame ever resumes. Exactly 2, never
  // 3 (no duplicate Loading) and never 1 (ProbingCapabilities must not be
  // silently dropped either).
  QCOMPARE(stateSpy.count(), 2);

  // Neither A's nor B's token was ever touched, and the final settled
  // selection is C -- with persisted storage agreeing (no split).
  QCOMPARE(h.tokenStore.calls.size(), 2); // just the original read + save
  const std::optional<QString> actualStoredA =
      h.tokenStore.storedToken(profileIdA);
  QVERIFY(actualStoredA.has_value());
  QVERIFY(*actualStoredA == token);
  QVERIFY(!h.tokenStore.storedToken(idB).has_value());
  const QString actualSelected = h.coordinator->selectedProfileId();
  QVERIFY(actualSelected == idC);
  QVERIFY(h.profileStore.selectedId == idC);

  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idC] { return h.tokenStore.hasPending(idC); }));
  h.tokenStore.complete(idC, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    coordinatorDestructionDuringSelectedProfileChangedEmissionDuringSwitchIsSafe() {
  // Destruction safety for the THIRD signal in switchProfile()'s
  // publishDirtyProperties() batch (selectedProfileChanged(), the last
  // one delivered since selectedProfile is always checked last): destroying
  // the coordinator from directly within this specific emission must
  // never crash or dereference `this`/`self` afterward. stateChanged() and
  // currentUserChanged() destruction safety are already covered by
  // coordinatorDestructionDuringStateChangedEmissionIsSafe() (the FIRST
  // signal) and the synchronous-delete test elsewhere; this completes the
  // audit for every signal in the batch.
  Harness h;
  const ServerProfile hosted = ServerProfile::hostedDefault();
  const auto customProfile = ServerProfile::custom(
      QStringLiteral("Custom"), QStringLiteral("https://example.test"));
  QVERIFY(customProfile.has_value());
  h.profileStore.profiles = {hosted, *customProfile};
  h.profileStore.selectedId = hosted.profileId();
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString customId = customProfile->profileId();

  bool handled = false;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::selectedProfileChanged,
      h.coordinator.get(),
      [&h, &handled] {
        if (handled) {
          return;
        }
        handled = true;
        h.coordinator.reset();
      },
      Qt::DirectConnection);

  h.coordinator->switchProfile(customId);

  QVERIFY(handled);
  QVERIFY(h.coordinator == nullptr);
  // Reaching this line at all (no crash/UB) is the assertion.
}

// ─── Property-revision notification model ────────────────────────────────

void SessionCoordinatorTests::
    unguardedStateChangedHandlerCallingStartWheneverLoadingDoesNotRecurse() {
  // Exact regression from review: under the OLD unconditional-emit
  // setState(), a directly-connected stateChanged() handler that
  // unconditionally calls start() whenever it observes state()==Loading
  // would recurse forever -- every nested start() reassigns (Loading, "")
  // even though the value never actually changes, and the old code
  // re-emitted stateChanged() regardless, re-triggering the handler
  // endlessly. Under mutateState()'s dirty-check, a reassignment of the
  // IDENTICAL (state, diagnostic) tuple is a genuine no-op: it bumps no
  // revision and therefore produces no emission, so the handler is never
  // re-invoked for the same Loading value. This is the ONLY thing that
  // makes this handler pattern -- deliberately written with NO "handled"
  // guard, unlike every other reentrancy test in this file -- terminate
  // at all: reaching this test's end (rather than a stack overflow/hang)
  // is itself part of the assertion.
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();
  const int createdBefore = h.probeFactory.totalCreated();

  QSignalSpy stateSpy(h.coordinator.get(), &SessionCoordinator::stateChanged);
  QVERIFY(stateSpy.isValid());

  int handlerInvocations = 0;
  QObject::connect(
      h.coordinator.get(), &SessionCoordinator::stateChanged,
      h.coordinator.get(),
      [&h, &handlerInvocations] {
        if (h.coordinator->state() == SessionCoordinator::State::Loading) {
          ++handlerInvocations;
          h.coordinator->start();
        }
      },
      Qt::DirectConnection);

  h.coordinator->start();

  // The outer start() commits (SignedIn -> Loading): the handler fires
  // once, calls start() again; the nested start()'s own reassignment of
  // (Loading, "") is a no-op, so the handler is NOT invoked a second time
  // for the same value. The nested call proceeds all the way to a
  // genuinely NEW state (ProbingCapabilities), at which point the
  // handler's own `state() == Loading` condition no longer holds, so it
  // does nothing further. Exactly one handler invocation; exactly two
  // real stateChanged emissions (Loading once, ProbingCapabilities once);
  // exactly one probe created (only the nested call ever reaches
  // startProbe() -- the outer call is superseded by the generation bump
  // before it would reach its own startProbe()).
  QCOMPARE(handlerInvocations, 1);
  QCOMPARE(stateSpy.count(), 2);
  QCOMPARE(h.probeFactory.totalCreated(), createdBefore + 1);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::ProbingCapabilities);

  // The nested restart proceeds to completion normally.
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    repeatedIdenticalRetryFailureDiagnosticEmitsStateChangedOnlyOnce() {
  // Exact requirement: assigning the IDENTICAL (state, diagnostic) tuple
  // must never create a new notification obligation. A profile whose
  // required delete fails repeatedly with the EXACT SAME diagnostic text
  // (backendErrorResult() always returns the static string
  // "backend failure") reassigns (SecureStorageUnavailable,
  // "backend failure") on every single failure -- under the OLD
  // unconditional-emit setState(), each of these value-IDENTICAL
  // reassignments re-emitted stateChanged() every time; the fix must
  // emit it only once, for the first genuine transition into that
  // (state, diagnostic) pair, with every subsequent identical failure
  // producing zero further emissions.
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));

  QSignalSpy stateSpy(h.coordinator.get(), &SessionCoordinator::stateChanged);
  QVERIFY(stateSpy.isValid());

  h.tokenStore.complete(profileId, backendErrorResult()); // failure #1
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  QCOMPARE(stateSpy.count(), 1);
  const QString firstDiagnostic = h.coordinator->diagnostic();

  for (int attempt = 0; attempt < 3; ++attempt) {
    h.coordinator->retry();
    QVERIFY(pumpEventsUntil(
        [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
    h.tokenStore.complete(profileId, backendErrorResult()); // identical
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCOMPARE(h.coordinator->state(),
             SessionCoordinator::State::SecureStorageUnavailable);
    QCOMPARE(h.coordinator->diagnostic(), firstDiagnostic);
  }
  // Every one of the 3 repeated identical failures reassigned the EXACT
  // SAME (state, diagnostic) tuple: zero additional stateChanged()
  // emissions beyond the first genuine transition.
  QCOMPARE(stateSpy.count(), 1);

  // The eventual success is still a genuinely NEW (state, diagnostic)
  // pair and must still be announced.
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QCOMPARE(stateSpy.count(), 2);
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());
}

void SessionCoordinatorTests::
    unchangedSwitchProfileReselectionEmitsNoSignals() {
  // Exact requirement: reassigning an IDENTICAL value must never create a
  // new notification obligation. switchProfile() re-selecting the
  // already-selected profile is the clearest public-API scenario in
  // which EVERY observable property genuinely stays byte-for-byte
  // unchanged (state remains SignedIn with the same diagnostic,
  // currentUser remains the same identity, selectedProfile remains the
  // same profile): it must fire NONE of the three notify signals, not
  // merely "no signal for whichever property happens to differ".
  Harness h;
  bootToSignedIn(h, QStringLiteral("session-token"));
  const QString profileId = h.coordinator->selectedProfileId();

  QSignalSpy stateSpy(h.coordinator.get(), &SessionCoordinator::stateChanged);
  QSignalSpy currentUserSpy(h.coordinator.get(),
                            &SessionCoordinator::currentUserChanged);
  QSignalSpy selectedProfileSpy(h.coordinator.get(),
                                &SessionCoordinator::selectedProfileChanged);
  QVERIFY(stateSpy.isValid());
  QVERIFY(currentUserSpy.isValid());
  QVERIFY(selectedProfileSpy.isValid());

  h.coordinator->switchProfile(profileId); // already selected: true no-op

  QCOMPARE(stateSpy.count(), 0);
  QCOMPARE(currentUserSpy.count(), 0);
  QCOMPARE(selectedProfileSpy.count(), 0);
  QCOMPARE(h.coordinator->state(), SessionCoordinator::State::SignedIn);
  QCOMPARE(h.coordinator->selectedProfileId(), profileId);
}

// ─── Profile reload / endpoint identity ──────────────────────────────────

void SessionCoordinatorTests::
    reloadedProfileNameOnlyChangeUpdatesNameKeepsEndpointAndToken() {
  // start() unconditionally reloads every profile record from storage on
  // every restart. A display-name-only change for the SAME stable
  // profileId() (see ServerProfile::customWithId()) must update the
  // observable name, fire selectedProfileChanged() EXACTLY ONCE, and keep
  // using the SAME endpoint/token -- never treat this as a credential-
  // invalidating endpoint change.
  Harness h;
  const QString profileId =
      QStringLiteral("22222222-2222-2222-2222-222222222222");
  const QString url = QStringLiteral("https://old.example.test");
  const auto v1 =
      ServerProfile::customWithId(profileId, QStringLiteral("Old Name"), url);
  QVERIFY(v1.has_value());
  h.profileStore.profiles = {*v1};
  h.profileStore.selectedId = profileId;

  const QString token = QStringLiteral("session-token");
  bootToSignedIn(h, token);
  QCOMPARE(h.tokenStore.calls.size(), 2); // read, save
  QCOMPARE(h.coordinator->selectedProfileDisplayName(),
           QStringLiteral("Old Name"));

  const auto v2 =
      ServerProfile::customWithId(profileId, QStringLiteral("New Name"), url);
  QVERIFY(v2.has_value());
  h.profileStore.profiles[0] = *v2;

  QSignalSpy selectedProfileSpy(h.coordinator.get(),
                                &SessionCoordinator::selectedProfileChanged);
  QVERIFY(selectedProfileSpy.isValid());

  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));

  // No delete was ever issued for a name-only change: the op dispatched
  // right after the reload is the credential-restore Read, not a Delete.
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("read"));

  h.tokenStore.complete(profileId, successReadResult(token));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 3; }));
  QCOMPARE(h.authClient.calls.at(2).kind, FakeAuthClient::CallKind::WhoAmI);
  const QString sentToken = h.authClient.calls.at(2).token;
  QVERIFY(sentToken == token);
  h.authClient.completeUser(3, userSuccess(QStringLiteral("bob"),
                                           QStringLiteral("bob@example.test")));
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));

  QCOMPARE(selectedProfileSpy.count(), 1);
  QCOMPARE(h.coordinator->selectedProfileDisplayName(),
           QStringLiteral("New Name"));
  QCOMPARE(h.coordinator->selectedProfileBaseUrl(), url);
  QCOMPARE(h.probeFactory.current()->lastProfile().baseUrl().toString(), url);
  QCOMPARE(h.authClient.calls.at(2).profile.baseUrl().toString(), url);
}

void SessionCoordinatorTests::
    reloadedProfileEndpointChangeDeletesOldTokenBeforeRestoreRead() {
  // The same stable profileId() reloaded with a DIFFERENT canonical
  // endpoint must never let the OLD token be read, sent, or saved against
  // the NEW endpoint: a required Delete must precede any restore Read.
  Harness h;
  const QString profileId =
      QStringLiteral("22222222-2222-2222-2222-222222222222");
  const QString oldUrl = QStringLiteral("https://old.example.test");
  const QString newUrl = QStringLiteral("https://new.example.test");
  const auto v1 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), oldUrl);
  QVERIFY(v1.has_value());
  h.profileStore.profiles = {*v1};
  h.profileStore.selectedId = profileId;

  const QString oldToken = QStringLiteral("old-token");
  bootToSignedIn(h, oldToken);
  QCOMPARE(h.tokenStore.calls.size(), 2); // read, save
  QVERIFY(h.tokenStore.storedToken(profileId).has_value());
  const int authCallsBeforeReload = h.authClient.calls.size();

  const auto v2 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), newUrl);
  QVERIFY(v2.has_value());
  h.profileStore.profiles[0] = *v2;

  h.coordinator->start();

  // The full new snapshot -- including the new endpoint -- is committed
  // synchronously, before any asynchronous probe/token-store completion.
  QCOMPARE(h.coordinator->selectedProfileBaseUrl(), newUrl);

  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());

  // The required Delete for the OLD endpoint's token is dispatched ahead
  // of any restore Read.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));

  h.tokenStore.complete(profileId, successWriteResult());

  // The restore Read now dispatches, strictly after the Delete.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("read"));

  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  // The old token was truly deleted; the probe used the new endpoint; and
  // -- restricting the check to calls made AFTER the reload, since the
  // ORIGINAL bootToSignedIn() legitimately validated oldToken via its own
  // whoami call before this reload ever happened -- no whoami call made
  // as part of the reload/restore-after-reload flow ever carried the old
  // token.
  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());
  QCOMPARE(h.probeFactory.current()->lastProfile().baseUrl().toString(),
           newUrl);
  for (int i = authCallsBeforeReload; i < h.authClient.calls.size(); ++i) {
    if (h.authClient.calls.at(i).kind == FakeAuthClient::CallKind::WhoAmI) {
      const QString sentToken = h.authClient.calls.at(i).token;
      QVERIFY(sentToken != oldToken);
    }
  }

  // A subsequent explicit sign-in uses the new URL only.
  h.coordinator->signIn(QStringLiteral("carol@example.test"),
                        QStringLiteral("hunter2"));
  QCOMPARE(h.authClient.calls.size(), 3);
  QCOMPARE(h.authClient.calls.last().kind,
           FakeAuthClient::CallKind::Authenticate);
  QCOMPARE(h.authClient.calls.last().profile.baseUrl().toString(), newUrl);
}

void SessionCoordinatorTests::
    reloadedProfileEndpointChangeDuringInFlightSaveCompensatesExactlyOnce() {
  // An auth/token Save already dispatched (in flight, uncancellable) under
  // the OLD endpoint's epoch must be compensated for by exactly one
  // cleanup Delete, and the endpoint change's OWN unconditional required
  // Delete must still separately run, leaving no orphan/cross-origin
  // token behind.
  Harness h;
  const QString profileId =
      QStringLiteral("22222222-2222-2222-2222-222222222222");
  const QString oldUrl = QStringLiteral("https://old.example.test");
  const QString newUrl = QStringLiteral("https://new.example.test");
  const auto v1 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), oldUrl);
  QVERIFY(v1.has_value());
  h.profileStore.profiles = {*v1};
  h.profileStore.selectedId = profileId;
  h.bootToSignedOut();

  h.coordinator->signIn(QStringLiteral("dave@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  h.authClient.completeToken(1, tokenSuccess(QStringLiteral("stale-token")));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 2; }));
  h.authClient.completeUser(
      2,
      userSuccess(QStringLiteral("dave"), QStringLiteral("dave@example.test")));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  // The Save is now dispatched (in flight, uncancellable) under the OLD
  // endpoint's credential epoch.
  QCOMPARE(h.tokenStore.calls.size(), 2); // read, save
  QCOMPARE(h.tokenStore.calls.at(1).kind, QStringLiteral("save"));

  const auto v2 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), newUrl);
  QVERIFY(v2.has_value());
  h.profileStore.profiles[0] = *v2;

  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());

  // The stale Save is still the only in-flight op; nothing new is
  // dispatched until it completes (only one op may be in flight per
  // profile at a time -- FakeTokenStore would fatally assert otherwise).
  QCOMPARE(h.tokenStore.calls.size(), 2);

  h.tokenStore.complete(profileId, successWriteResult()); // stale save lands

  // Exactly one compensating Delete for the abandoned save.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());

  // The endpoint change's OWN unconditional required Delete follows.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, notFoundResult()); // already removed above

  // Exactly two deletes total: never a duplicate compensation.
  int deleteCount = 0;
  for (const auto &call : h.tokenStore.calls) {
    if (call.kind == QStringLiteral("delete")) {
      ++deleteCount;
    }
  }
  QCOMPARE(deleteCount, 2);

  // The queued restore Read now dispatches, strictly after both deletes.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 5);
  QCOMPARE(h.tokenStore.calls.at(4).kind, QStringLiteral("read"));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  QVERIFY(!h.tokenStore.storedToken(profileId).has_value());
  QCOMPARE(h.probeFactory.current()->lastProfile().baseUrl().toString(),
           newUrl);
}

void SessionCoordinatorTests::
    reloadedProfileEndpointChangeDeleteFailureBlocksThenRetrySucceeds() {
  // A genuine failure of the endpoint-change's required Delete must
  // durably block any later read/auth for this profile -- never silently
  // proceed with (or leak) the old endpoint's token -- until retry()
  // succeeds.
  Harness h;
  const QString profileId =
      QStringLiteral("22222222-2222-2222-2222-222222222222");
  const QString oldUrl = QStringLiteral("https://old.example.test");
  const QString newUrl = QStringLiteral("https://new.example.test");
  const auto v1 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), oldUrl);
  QVERIFY(v1.has_value());
  h.profileStore.profiles = {*v1};
  h.profileStore.selectedId = profileId;
  bootToSignedIn(h, QStringLiteral("old-token"));

  const auto v2 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), newUrl);
  QVERIFY(v2.has_value());
  h.profileStore.profiles[0] = *v2;

  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());

  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, backendErrorResult()); // genuine failure

  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));

  // The old token is still technically present in the fake store (the
  // Delete failed) but must never be sent anywhere while blocked: no
  // further tokenStore call, and no auth request, occurs while stalled.
  QVERIFY(h.tokenStore.storedToken(profileId).has_value());
  QCOMPARE(h.tokenStore.calls.size(), 3);
  const int authCallsBeforeBlockedSignIn = h.authClient.calls.size();
  h.coordinator->signIn(QStringLiteral("erin@example.test"),
                        QStringLiteral("hunter2"));
  QCOMPARE(h.authClient.calls.size(), authCallsBeforeBlockedSignIn);

  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult()); // retry succeeds

  // The queued restore Read now finally dispatches.
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
    reloadedProfileCanonicallyEquivalentUrlIsNotEndpointChangeButPathCaseIs() {
  // Direct unit-level check of the canonical-endpoint-identity comparison
  // itself: an explicit-vs-default port spelling is the SAME endpoint;
  // an otherwise-identical path that differs only in case is NOT (URL
  // paths are case-sensitive, unlike scheme/host, which QUrl already
  // lowercases during parsing -- see UrlValidator::validateCustomUrl()).
  const QString profileId =
      QStringLiteral("22222222-2222-2222-2222-222222222222");
  const auto portOmitted =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"),
                                  QStringLiteral("https://example.test"));
  const auto portExplicitDefault =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"),
                                  QStringLiteral("https://example.test:443"));
  QVERIFY(portOmitted.has_value());
  QVERIFY(portExplicitDefault.has_value());
  QVERIFY(portOmitted->hasEquivalentEndpoint(*portExplicitDefault));

  const auto pathLower =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"),
                                  QStringLiteral("https://example.test/app"));
  const auto pathUpper =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"),
                                  QStringLiteral("https://example.test/App"));
  QVERIFY(pathLower.has_value());
  QVERIFY(pathUpper.has_value());
  QVERIFY(!pathLower->hasEquivalentEndpoint(*pathUpper));

  // End-to-end behavioral proof, part 1: the port-spelling-only reload
  // above changes the EXPOSED baseUrl() representation (an explicit
  // ":443" is textually different from an implicit default, even though
  // hasEquivalentEndpoint() says they are the same credential-scoped
  // endpoint) and so must still fire selectedProfileChanged() exactly
  // once and use the fresh representation for the probe/whoami calls --
  // but must never trigger a token delete, since the underlying
  // credential endpoint has not actually changed.
  {
    Harness h;
    h.profileStore.profiles = {*portOmitted};
    h.profileStore.selectedId = profileId;
    const QString token = QStringLiteral("session-token");
    bootToSignedIn(h, token);
    QCOMPARE(h.tokenStore.calls.size(), 2); // read, save

    h.profileStore.profiles[0] = *portExplicitDefault;
    QSignalSpy selectedProfileSpy(h.coordinator.get(),
                                  &SessionCoordinator::selectedProfileChanged);
    QVERIFY(selectedProfileSpy.isValid());

    h.coordinator->start();
    // The new (":443") representation is already the coherent, exposed
    // snapshot synchronously, before any asynchronous probe/token-store
    // completion -- matching every other transition's coherent-snapshot
    // publication contract.
    QCOMPARE(selectedProfileSpy.count(), 1);
    QCOMPARE(h.coordinator->selectedProfileBaseUrl(),
             portExplicitDefault->baseUrl().toString());
    QVERIFY(
        pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
    QCOMPARE(h.probeFactory.current()->lastProfile().baseUrl().toString(),
             portExplicitDefault->baseUrl().toString());
    h.probeFactory.current()->complete(compatibleProbeResult());
    QVERIFY(pumpEventsUntil(
        [&h, profileId] { return h.tokenStore.hasPending(profileId); }));

    // No delete was ever issued: the op dispatched right after the
    // reload is the credential-restore Read, not a Delete, and exactly
    // one selectedProfileChanged() was ever fired for this reload.
    QCOMPARE(h.tokenStore.calls.size(), 3);
    QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("read"));
    QCOMPARE(selectedProfileSpy.count(), 1);
    h.tokenStore.complete(profileId, successReadResult(token));
    QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 3; }));
    QCOMPARE(h.authClient.calls.at(2).profile.baseUrl().toString(),
             portExplicitDefault->baseUrl().toString());
    h.authClient.completeUser(
        3,
        userSuccess(QStringLiteral("fay"), QStringLiteral("fay@example.test")));
    QVERIFY(pumpEventsUntil([&h] {
      return h.coordinator->state() == SessionCoordinator::State::SignedIn;
    }));
    QVERIFY(h.tokenStore.storedToken(profileId).has_value());
    QCOMPARE(selectedProfileSpy.count(), 1);
  }

  // End-to-end behavioral proof, part 2: the path-case-only reload above
  // MUST trigger the required-delete-before-restore-Read sequence exactly
  // like any other endpoint change.
  {
    Harness h;
    h.profileStore.profiles = {*pathLower};
    h.profileStore.selectedId = profileId;
    bootToSignedIn(h, QStringLiteral("session-token"));
    QCOMPARE(h.tokenStore.calls.size(), 2); // read, save

    h.profileStore.profiles[0] = *pathUpper;
    h.coordinator->start();
    QVERIFY(
        pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
    h.probeFactory.current()->complete(compatibleProbeResult());
    QVERIFY(pumpEventsUntil(
        [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
    QCOMPARE(h.tokenStore.calls.size(), 3);
    QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));
    h.tokenStore.complete(profileId, successWriteResult());
    QVERIFY(pumpEventsUntil(
        [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
    QCOMPARE(h.tokenStore.calls.size(), 4);
    QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("read"));
    h.tokenStore.complete(profileId, notFoundResult());
    QVERIFY(pumpEventsUntil([&h] {
      return h.coordinator->state() == SessionCoordinator::State::SignedOut;
    }));
    QVERIFY(!h.tokenStore.storedToken(profileId).has_value());
  }
}

void SessionCoordinatorTests::
    reloadedProfileExactUnchangedContentEmitsNoSignals() {
  // start() unconditionally reloads every profile record from storage on
  // every restart. When the freshly loaded record for the SAME profileId()
  // is byte-for-byte unchanged (same displayName, same canonical URL),
  // mutateSelectedProfile() must not create a new notification obligation
  // -- selectedProfileChanged() must fire zero times, and no credential
  // action of any kind (delete or otherwise) may be taken.
  Harness h;
  const QString profileId =
      QStringLiteral("22222222-2222-2222-2222-222222222222");
  const QString url = QStringLiteral("https://example.test");
  const auto v1 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), url);
  QVERIFY(v1.has_value());
  h.profileStore.profiles = {*v1};
  h.profileStore.selectedId = profileId;
  const QString token = QStringLiteral("session-token");
  bootToSignedIn(h, token);
  QCOMPARE(h.tokenStore.calls.size(), 2); // read, save

  // Reload with an INDEPENDENTLY constructed but byte-for-byte identical
  // record for the same profileId().
  const auto v2 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), url);
  QVERIFY(v2.has_value());
  h.profileStore.profiles[0] = *v2;

  QSignalSpy selectedProfileSpy(h.coordinator.get(),
                                &SessionCoordinator::selectedProfileChanged);
  QVERIFY(selectedProfileSpy.isValid());

  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));

  QCOMPARE(selectedProfileSpy.count(), 0);
  QCOMPARE(h.tokenStore.calls.size(), 3); // read, save, restore-read only
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("read"));
  h.tokenStore.complete(profileId, successReadResult(token));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 3; }));
  h.authClient.completeUser(
      3,
      userSuccess(QStringLiteral("gina"), QStringLiteral("gina@example.test")));
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
}

void SessionCoordinatorTests::
    reloadedProfileEndpointChangeStaleInFlightReadCannotBypassRequiredDelete() {
  // Exact race from review: an in-flight restore Read admitted for the
  // OLD endpoint must never be rebound to a continuation for a NEW
  // endpoint once the same profileId() reloads with a different URL --
  // it must remain a permanently stale, un-rebound op whose eventual
  // result is silently discarded, while a FRESH Read (for the new
  // endpoint) is enqueued strictly BEHIND the required Delete reserved
  // for the old endpoint's token.
  Harness h;
  const QString profileId =
      QStringLiteral("22222222-2222-2222-2222-222222222222");
  const QString oldUrl = QStringLiteral("https://old.example.test");
  const QString newUrl = QStringLiteral("https://new.example.test/app");
  const auto v1 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), oldUrl);
  QVERIFY(v1.has_value());
  h.profileStore.profiles = {*v1};
  h.profileStore.selectedId = profileId;

  // Start once: the initial restore Read dispatches and is held in
  // flight (never completed yet).
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 1);
  QCOMPARE(h.tokenStore.calls.first().kind, QStringLiteral("read"));

  // Persisted storage now reports a DIFFERENT canonical endpoint (host
  // AND path both changed) for the SAME stable UUID.
  const auto v2 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), newUrl);
  QVERIFY(v2.has_value());
  h.profileStore.profiles[0] = *v2;

  // Restart while the old-endpoint Read is STILL in flight. This must
  // invalidate the endpoint epoch and reserve a required Delete behind
  // the still-outstanding old Read, then -- once the fresh probe for the
  // new endpoint completes BEFORE the old Read ever does, exactly the
  // review's ordering -- enqueue a FRESH Read behind that Delete,
  // without ever rebinding the stale one.
  h.coordinator->start();
  QCOMPARE(h.coordinator->selectedProfileBaseUrl(), newUrl);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RestoringCredential;
  }));
  // Nothing new has touched the real store yet: the required Delete and
  // fresh Read are both merely queued behind the still-in-flight old
  // Read.
  QCOMPARE(h.tokenStore.calls.size(), 1);

  // NOW the old-endpoint Read finally completes, returning a token that
  // was scoped to the OLD endpoint.
  const QString oldToken = QStringLiteral("old-endpoint-token");
  h.tokenStore.complete(profileId, successReadResult(oldToken));

  // The stale Read's own (now-superseded-generation) continuation must
  // discard this result entirely: no whoami call of any kind -- to
  // either endpoint -- may ever be issued carrying it. The required
  // Delete for the old endpoint's token dispatches next.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 2);
  QCOMPARE(h.tokenStore.calls.at(1).kind, QStringLiteral("delete"));
  QVERIFY(h.authClient.calls.isEmpty());

  // A duplicate/replayed completion of the stale old Read (simulating a
  // buggy backend notifying twice) must be equally inert: its captured
  // (opId, attemptId) no longer matches the profile's current dispatch
  // record (which now belongs to the required Delete), so it must be
  // silently rejected rather than corrupt the Delete's own in-flight
  // dispatch.
  h.tokenStore.replayLastCompletion(profileId);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  QCOMPARE(h.tokenStore.calls.size(), 2);
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);
  QVERIFY(h.authClient.calls.isEmpty());

  // The required Delete for the old endpoint's token completes...
  h.tokenStore.complete(profileId, successWriteResult());

  // ...and ONLY NOW does the distinct fresh Read (for the NEW endpoint)
  // ever dispatch.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("read"));
  QVERIFY(h.authClient.calls.isEmpty());

  const QString newToken = QStringLiteral("new-endpoint-token");
  h.tokenStore.complete(profileId, successReadResult(newToken));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  QCOMPARE(h.authClient.calls.first().kind, FakeAuthClient::CallKind::WhoAmI);
  const QString sentToken = h.authClient.calls.first().token;
  QVERIFY(sentToken == newToken);
  QCOMPARE(h.authClient.calls.first().profile.baseUrl().toString(), newUrl);
  h.authClient.completeUser(
      1,
      userSuccess(QStringLiteral("hank"), QStringLiteral("hank@example.test")));
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
  QCOMPARE(h.coordinator->selectedProfileBaseUrl(), newUrl);
}

void SessionCoordinatorTests::
    reloadedProfileEndpointChangeRequiredDeleteFailureAfterStaleReadBlocksThenRetrySucceeds() {
  // Same race as above, but the required Delete for the old endpoint's
  // token fails first: the coordinator must durably block (never fall
  // through to the fresh Read) until retry() succeeds.
  Harness h;
  const QString profileId =
      QStringLiteral("22222222-2222-2222-2222-222222222222");
  const QString oldUrl = QStringLiteral("https://old.example.test");
  const QString newUrl = QStringLiteral("https://new.example.test/app");
  const auto v1 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), oldUrl);
  QVERIFY(v1.has_value());
  h.profileStore.profiles = {*v1};
  h.profileStore.selectedId = profileId;

  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 1);

  const auto v2 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), newUrl);
  QVERIFY(v2.has_value());
  h.profileStore.profiles[0] = *v2;

  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RestoringCredential;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 1);

  const QString oldToken = QStringLiteral("old-endpoint-token");
  h.tokenStore.complete(profileId, successReadResult(oldToken));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 2);
  QCOMPARE(h.tokenStore.calls.at(1).kind, QStringLiteral("delete"));

  // The required delete fails: this must durably block -- the fresh
  // Read must never be attempted before it succeeds.
  h.tokenStore.complete(profileId, backendErrorResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 2);
  QVERIFY(h.authClient.calls.isEmpty());

  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());

  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("read"));

  const QString newToken = QStringLiteral("new-endpoint-token");
  h.tokenStore.complete(profileId, successReadResult(newToken));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  QCOMPARE(h.authClient.calls.first().kind, FakeAuthClient::CallKind::WhoAmI);
  const QString sentToken = h.authClient.calls.first().token;
  QVERIFY(sentToken == newToken);
  h.authClient.completeUser(1, userSuccess(QStringLiteral("ivy"),
                                           QStringLiteral("ivy@example.test")));
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
}

void SessionCoordinatorTests::
    reloadedProfileEndpointChangeQueuedNotInFlightOldReadCannotBypassRequiredDelete() {
  // Variant of the race where the stale-epoch Read is enqueued but NOT
  // yet dispatched (an ordinary sign-out Delete is the one actually in
  // flight when the endpoint changes). The same defensive discard must
  // apply once it does eventually dispatch.
  Harness h;
  const QString profileId =
      QStringLiteral("22222222-2222-2222-2222-222222222222");
  const QString oldUrl = QStringLiteral("https://old.example.test");
  const QString newUrl = QStringLiteral("https://new.example.test/app");
  const auto v1 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), oldUrl);
  QVERIFY(v1.has_value());
  h.profileStore.profiles = {*v1};
  h.profileStore.selectedId = profileId;

  const QString firstToken = QStringLiteral("first-token");
  bootToSignedIn(h, firstToken);
  QCOMPARE(h.tokenStore.calls.size(), 2); // read, save

  // Sign out: an ordinary Delete dispatches and is held in flight.
  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));

  // Restart (same endpoint, no reload yet): the restore Read is enqueued
  // behind the still in-flight ordinary Delete -- QUEUED, never
  // dispatched.
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RestoringCredential;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 3);

  // NOW the persisted record for the SAME UUID reports a different
  // endpoint.
  const auto v2 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), newUrl);
  QVERIFY(v2.has_value());
  h.profileStore.profiles[0] = *v2;

  h.coordinator->start();
  QCOMPARE(h.coordinator->selectedProfileBaseUrl(), newUrl);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RestoringCredential;
  }));
  // Nothing new dispatched yet: the ordinary Delete is still the only
  // in-flight op; the stale queued Read, the required Delete, and the
  // fresh Read are all still merely queued behind it.
  QCOMPARE(h.tokenStore.calls.size(), 3);

  // The in-flight ordinary Delete completes; the queued (never
  // dispatched) stale-epoch Read now dispatches...
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 4);
  QCOMPARE(h.tokenStore.calls.at(3).kind, QStringLiteral("read"));

  // ...but completing it must never issue a whoami: its continuation's
  // captured generation is stale by now, so the required Delete for the
  // old endpoint's token dispatches next instead.
  h.tokenStore.complete(profileId, successReadResult(firstToken));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 5);
  QCOMPARE(h.tokenStore.calls.at(4).kind, QStringLiteral("delete"));
  QCOMPARE(h.authClient.calls.size(), 2); // unchanged: just the original
                                          // sign-in's authenticate+whoami

  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 6);
  QCOMPARE(h.tokenStore.calls.at(5).kind, QStringLiteral("read"));

  const QString newToken = QStringLiteral("new-endpoint-token");
  h.tokenStore.complete(profileId, successReadResult(newToken));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 3; }));
  QCOMPARE(h.authClient.calls.at(2).kind, FakeAuthClient::CallKind::WhoAmI);
  const QString sentToken = h.authClient.calls.at(2).token;
  QVERIFY(sentToken == newToken);
  QCOMPARE(h.authClient.calls.at(2).profile.baseUrl().toString(), newUrl);
  h.authClient.completeUser(
      3,
      userSuccess(QStringLiteral("iris"), QStringLiteral("iris@example.test")));
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
  QCOMPARE(h.coordinator->selectedProfileBaseUrl(), newUrl);
}

void SessionCoordinatorTests::
    reloadedProfileEndpointChangeRepeatedStartDedupesOnlyFreshReadAndStaleReplayCannotCorruptIt() {
  // After an endpoint change reserves a required Delete and a fresh
  // Read, calling start() AGAIN (same new endpoint, no further reload)
  // while that fresh Read is still in flight must dedup onto the SAME
  // fresh Read -- never create a third Read -- proving the endpoint-
  // epoch match still correctly handles legitimate repeated-start dedup
  // even immediately after an endpoint change.
  Harness h;
  const QString profileId =
      QStringLiteral("22222222-2222-2222-2222-222222222222");
  const QString oldUrl = QStringLiteral("https://old.example.test");
  const QString newUrl = QStringLiteral("https://new.example.test/app");
  const auto v1 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), oldUrl);
  QVERIFY(v1.has_value());
  h.profileStore.profiles = {*v1};
  h.profileStore.selectedId = profileId;

  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 1);

  const auto v2 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), newUrl);
  QVERIFY(v2.has_value());
  h.profileStore.profiles[0] = *v2;

  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RestoringCredential;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 1);

  const QString oldToken = QStringLiteral("old-endpoint-token");
  h.tokenStore.complete(profileId, successReadResult(oldToken));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 2);
  QCOMPARE(h.tokenStore.calls.at(1).kind, QStringLiteral("delete"));

  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("read"));

  // The fresh Read for the new endpoint is now in flight. Repeatedly
  // restart (same endpoint, no reload) while it remains outstanding.
  for (int i = 0; i < 3; ++i) {
    h.coordinator->start();
    QVERIFY(
        pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
    h.probeFactory.current()->complete(compatibleProbeResult());
    QVERIFY(pumpEventsUntil([&h] {
      return h.coordinator->state() ==
             SessionCoordinator::State::RestoringCredential;
    }));
  }
  // No third (or fourth, fifth...) Read was ever dispatched: still just
  // the original stale-old Read (already discarded) and this ONE fresh
  // Read.
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);

  // A duplicate replay of the required Delete's (long-since-completed)
  // callback -- the most recent completion recorded before the fresh
  // Read was ever dispatched -- must still be inert and must not disturb
  // the fresh Read's dedup chain or its own in-flight dispatch in any
  // way: its (opId, attemptId) no longer matches the profile's current
  // dispatch record, which now belongs to the fresh Read.
  h.tokenStore.replayLastCompletion(profileId);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QVERIFY(h.authClient.calls.isEmpty());

  const QString newToken = QStringLiteral("new-endpoint-token");
  h.tokenStore.complete(profileId, successReadResult(newToken));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  QCOMPARE(h.authClient.calls.first().kind, FakeAuthClient::CallKind::WhoAmI);
  const QString sentToken = h.authClient.calls.first().token;
  QVERIFY(sentToken == newToken);
  h.authClient.completeUser(
      1,
      userSuccess(QStringLiteral("jack"), QStringLiteral("jack@example.test")));
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
  QCOMPARE(h.coordinator->selectedProfileBaseUrl(), newUrl);
}

// ─── Durable endpoint-bound credential envelope ──────────────────────────
//
// The in-memory per-profile endpoint epoch (m_profileEndpointEpoch) only
// ever protects the CURRENTLY SELECTED profile within ONE process run. The
// durable, versioned envelope binding stored inside ITokenStore itself
// (see TokenEnvelope.h/ITokenStore.h) is what additionally protects: an
// unselected profile whose endpoint changes, a profile removed and later
// re-added with the same UUID, and any endpoint change that happened while
// the app was not running at all (a fresh process starts with every
// in-memory epoch map empty). These tests exercise SessionCoordinator's
// reaction to the ITokenStore-level outcomes that model that durable
// verification (BindingMismatch/LegacyUnbound/Malformed), and its
// all-profile reload comparison (reconcileAllProfileCredentialsOnReload).

void SessionCoordinatorTests::
    restoreReadReceivesExpectedEndpointIdentityMatchingSelectedProfile() {
  Harness h;
  const QString profileId =
      QStringLiteral("33333333-3333-3333-3333-333333333333");
  const QString url = QStringLiteral("https://example.test/app");
  const auto profile =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), url);
  QVERIFY(profile.has_value());
  h.profileStore.profiles = {*profile};
  h.profileStore.selectedId = profileId;

  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));

  QCOMPARE(h.tokenStore.calls.size(), 1);
  QCOMPARE(h.tokenStore.calls.first().kind, QStringLiteral("read"));
  QVERIFY(!h.tokenStore.calls.first().endpointIdentity.isEmpty());
  QCOMPARE(h.tokenStore.calls.first().endpointIdentity,
           profile->credentialEndpointIdentity());

  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    restoreBindingMismatchDeletesUntrustedTokenBeforeAnyAuthThenAllowsFreshSignIn() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 1);
  QCOMPARE(h.tokenStore.calls.first().kind, QStringLiteral("read"));

  h.tokenStore.complete(profileId, bindingMismatchResult());

  // A binding-mismatched entry must never reach whoami: cleanup deletes
  // it directly from the Read result itself.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QVERIFY(h.authClient.calls.isEmpty());
  QCOMPARE(h.tokenStore.calls.size(), 2);
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));

  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(h.authClient.calls.isEmpty());

  // A fresh, explicit sign-in afterward proceeds entirely normally.
  h.coordinator->signIn(QStringLiteral("erin@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  QCOMPARE(h.authClient.calls.first().kind,
           FakeAuthClient::CallKind::Authenticate);
}

void SessionCoordinatorTests::
    restoreLegacyUnboundTokenIsDeletedBeforeAnyAuthThenAllowsFreshSignIn() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));

  h.tokenStore.complete(profileId, legacyUnboundResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QVERIFY(h.authClient.calls.isEmpty());
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));

  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(h.authClient.calls.isEmpty());

  h.coordinator->signIn(QStringLiteral("erin@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  QCOMPARE(h.authClient.calls.first().kind,
           FakeAuthClient::CallKind::Authenticate);
}

void SessionCoordinatorTests::
    restoreMalformedEnvelopeIsDeletedBeforeAnyAuthThenAllowsFreshSignIn() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));

  h.tokenStore.complete(profileId, malformedEnvelopeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QVERIFY(h.authClient.calls.isEmpty());
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));

  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(h.authClient.calls.isEmpty());

  h.coordinator->signIn(QStringLiteral("erin@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  QCOMPARE(h.authClient.calls.first().kind,
           FakeAuthClient::CallKind::Authenticate);
}

void SessionCoordinatorTests::
    restoreUntrustedTokenDeleteFailureBlocksNewAuthUntilRetrySucceeds() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, bindingMismatchResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));

  h.tokenStore.complete(profileId, backendErrorResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));
  QVERIFY(h.coordinator->state() != SessionCoordinator::State::SignedOut);
  QVERIFY(h.authClient.calls.isEmpty());

  // No new auth may proceed while the untrusted token might still remain.
  h.coordinator->signIn(QStringLiteral("erin@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(h.authClient.calls.isEmpty());

  h.coordinator->retry();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(h.authClient.calls.isEmpty());
}

void SessionCoordinatorTests::
    unselectedProfileEndpointChangeReservesCleanupBeforeItIsEverSelected() {
  Harness h;
  const QString idA = QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
  const QString idB = QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
  const auto profileA = ServerProfile::customWithId(
      idA, QStringLiteral("A"), QStringLiteral("https://a.example.test"));
  const auto profileB1 = ServerProfile::customWithId(
      idB, QStringLiteral("B"), QStringLiteral("https://b-old.example.test"));
  QVERIFY(profileA.has_value());
  QVERIFY(profileB1.has_value());
  h.profileStore.profiles = {*profileA, *profileB1};
  h.profileStore.selectedId = idA;
  h.bootToSignedOut();
  QCOMPARE(h.tokenStore.calls.size(), 1); // one read, for A; B untouched.

  // B's persisted endpoint changes to a different host while B was never
  // selected in this process -- no in-memory epoch for B was ever primed.
  const auto profileB2 = ServerProfile::customWithId(
      idB, QStringLiteral("B"), QStringLiteral("https://b-new.example.test"));
  QVERIFY(profileB2.has_value());
  h.profileStore.profiles[1] = *profileB2;

  h.coordinator->start();
  // The all-profile reload comparison runs synchronously inside start(),
  // before A's own (unrelated, unchanged) probe/restore even begins: B's
  // cleanup Delete is already dispatched by the time start() returns.
  QVERIFY(h.tokenStore.hasPending(idB));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
  QCOMPARE(h.tokenStore.calls.last().profileId, idB);
  h.tokenStore.complete(idB, notFoundResult());

  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idA] { return h.tokenStore.hasPending(idA); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("read"));
  QCOMPARE(h.tokenStore.calls.last().profileId, idA);
  h.tokenStore.complete(idA, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  // B is now selected for the first time in this process. Its stale
  // old-endpoint record was already proactively cleaned up above, so a
  // fresh restore Read for B (carrying B's NEW endpoint identity) finds
  // nothing and proceeds normally -- never resurrecting anything from the
  // old endpoint.
  h.coordinator->switchProfile(idB);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("read"));
  QCOMPARE(h.tokenStore.calls.last().endpointIdentity,
           profileB2->credentialEndpointIdentity());
  h.tokenStore.complete(idB, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QCOMPARE(h.coordinator->selectedProfileId(), idB);
}

void SessionCoordinatorTests::
    removedProfileOnReloadReservesOrphanCredentialCleanup() {
  Harness h;
  const QString idA = QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
  const QString idB = QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
  const auto profileA = ServerProfile::customWithId(
      idA, QStringLiteral("A"), QStringLiteral("https://a.example.test"));
  const auto profileB = ServerProfile::customWithId(
      idB, QStringLiteral("B"), QStringLiteral("https://b.example.test"));
  QVERIFY(profileA.has_value());
  QVERIFY(profileB.has_value());
  h.profileStore.profiles = {*profileA, *profileB};
  h.profileStore.selectedId = idA;
  h.bootToSignedOut();
  QCOMPARE(h.tokenStore.calls.size(), 1);

  // B is removed from persisted storage entirely (e.g. deleted from
  // another session/device) while A remains selected and unchanged.
  h.profileStore.profiles = {*profileA};

  h.coordinator->start();
  // B's now-orphaned secure entry (if any) can never again be reached
  // through the ordinary selected-profile restore flow, so it is
  // proactively, durably deleted here.
  QVERIFY(h.tokenStore.hasPending(idB));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
  QCOMPARE(h.tokenStore.calls.last().profileId, idB);
  h.tokenStore.complete(idB, notFoundResult());

  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idA] { return h.tokenStore.hasPending(idA); }));
  h.tokenStore.complete(idA, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QCOMPARE(h.coordinator->selectedProfileId(), idA);
}

void SessionCoordinatorTests::
    reAddedSameUuidWithDifferentEndpointDoesNotTrustPriorSelectedProfileEpoch() {
  // Simulates a profileId() being removed on one reload and later
  // re-added (e.g. re-imported, or re-created by the server) with a
  // DIFFERENT endpoint. Across that gap,
  // reconcileAllProfileCredentialsOnReload() deliberately has no prior
  // in-memory record to compare against for a "new" ID (see its own doc
  // comment) -- so NO in-memory epoch ever protects this profile. Only the
  // durable per-entry envelope binding inside ITokenStore (modelled here by the
  // fake returning BindingMismatch) can still prevent a stale entry from that
  // old endpoint from ever being trusted.
  Harness h;
  const QString idA = QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
  const QString idB = QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
  const auto profileA = ServerProfile::customWithId(
      idA, QStringLiteral("A"), QStringLiteral("https://a.example.test"));
  const auto profileB1 = ServerProfile::customWithId(
      idB, QStringLiteral("B"), QStringLiteral("https://b-old.example.test"));
  QVERIFY(profileA.has_value());
  QVERIFY(profileB1.has_value());
  h.profileStore.profiles = {*profileA, *profileB1};
  h.profileStore.selectedId = idA;
  h.bootToSignedOut();

  // Reload 1: B is removed. This reserves and dispatches B's orphan
  // cleanup Delete immediately (see
  // removedProfileOnReloadReservesOrphanCredentialCleanup()); it must be
  // completed here so B's queue is empty again before reload 2 re-adds
  // it, otherwise a later Read for B would merely queue behind this
  // still-in-flight Delete rather than ever reaching the fake store.
  h.profileStore.profiles = {*profileA};
  h.coordinator->start();
  QVERIFY(h.tokenStore.hasPending(idB));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
  h.tokenStore.complete(idB, notFoundResult());
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idA] { return h.tokenStore.hasPending(idA); }));
  h.tokenStore.complete(idA, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  // Reload 2: B is re-added under the SAME UUID with a DIFFERENT
  // endpoint. reconcileAllProfileCredentialsOnReload() sees this as a
  // brand-new ID (no previous record survived reload 1's removal), so it
  // intentionally does not bump any in-memory epoch for B here.
  const auto profileB2 = ServerProfile::customWithId(
      idB, QStringLiteral("B"), QStringLiteral("https://b-new.example.test"));
  QVERIFY(profileB2.has_value());
  h.profileStore.profiles = {*profileA, *profileB2};
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idA] { return h.tokenStore.hasPending(idA); }));
  h.tokenStore.complete(idA, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  // B is now selected for the very first time in this process -- its
  // in-memory epoch state is entirely fresh/default.
  h.coordinator->switchProfile(idB);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("read"));
  QCOMPARE(h.tokenStore.calls.last().endpointIdentity,
           profileB2->credentialEndpointIdentity());

  // The backing store still holds an orphaned entry durably bound to B's
  // OLD endpoint identity (simulating an install that predates this
  // profileId() being reused) -- it must never be trusted, regardless of
  // any in-memory epoch (which never even existed for B in this run).
  h.tokenStore.complete(idB, bindingMismatchResult());
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QVERIFY(h.authClient.calls.isEmpty());
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
  h.tokenStore.complete(idB, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(h.authClient.calls.isEmpty());
}

void SessionCoordinatorTests::
    reAddedSameUuidWithSameEndpointRestoresTokenNormally() {
  Harness h;
  const QString idA = QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
  const QString idB = QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
  const QString sharedUrl = QStringLiteral("https://b.example.test");
  const auto profileA = ServerProfile::customWithId(
      idA, QStringLiteral("A"), QStringLiteral("https://a.example.test"));
  const auto profileB1 =
      ServerProfile::customWithId(idB, QStringLiteral("B"), sharedUrl);
  QVERIFY(profileA.has_value());
  QVERIFY(profileB1.has_value());
  h.profileStore.profiles = {*profileA, *profileB1};
  h.profileStore.selectedId = idA;
  h.bootToSignedOut();

  h.profileStore.profiles = {*profileA};
  h.coordinator->start();
  QVERIFY(h.tokenStore.hasPending(idB));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
  h.tokenStore.complete(idB, notFoundResult());
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idA] { return h.tokenStore.hasPending(idA); }));
  h.tokenStore.complete(idA, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  // Re-added under the SAME UUID with the SAME endpoint: a token durably
  // bound to that (unchanged) endpoint identity is expected to still
  // restore and authenticate normally.
  const auto profileB2 =
      ServerProfile::customWithId(idB, QStringLiteral("B"), sharedUrl);
  QVERIFY(profileB2.has_value());
  h.profileStore.profiles = {*profileA, *profileB2};
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idA] { return h.tokenStore.hasPending(idA); }));
  h.tokenStore.complete(idA, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  h.coordinator->switchProfile(idB);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("read"));
  QCOMPARE(h.tokenStore.calls.last().endpointIdentity,
           profileB2->credentialEndpointIdentity());

  const QString token = QStringLiteral("still-valid-token");
  h.tokenStore.complete(idB, successReadResult(token));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  QCOMPARE(h.authClient.calls.first().kind, FakeAuthClient::CallKind::WhoAmI);
  const QString sentToken = h.authClient.calls.first().token;
  QVERIFY(sentToken == token);
  h.authClient.completeUser(
      1,
      userSuccess(QStringLiteral("gail"), QStringLiteral("gail@example.test")));
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
}

void SessionCoordinatorTests::
    multipleProfilesChangedOrRemovedOnSingleReloadEachReserveExactlyOneCleanup() {
  Harness h;
  const QString idA = QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
  const QString idB = QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
  const QString idC = QStringLiteral("cccccccc-cccc-cccc-cccc-cccccccccccc");
  const QString idD = QStringLiteral("dddddddd-dddd-dddd-dddd-dddddddddddd");
  const QString sharedUrlD = QStringLiteral("https://d.example.test");
  const auto profileA = ServerProfile::customWithId(
      idA, QStringLiteral("A"), QStringLiteral("https://a.example.test"));
  const auto profileB1 = ServerProfile::customWithId(
      idB, QStringLiteral("B"), QStringLiteral("https://b-old.example.test"));
  const auto profileC = ServerProfile::customWithId(
      idC, QStringLiteral("C"), QStringLiteral("https://c.example.test"));
  const auto profileD1 = ServerProfile::customWithId(
      idD, QStringLiteral("D-old-name"), sharedUrlD);
  QVERIFY(profileA.has_value());
  QVERIFY(profileB1.has_value());
  QVERIFY(profileC.has_value());
  QVERIFY(profileD1.has_value());
  h.profileStore.profiles = {*profileA, *profileB1, *profileC, *profileD1};
  h.profileStore.selectedId = idA;
  h.bootToSignedOut();
  QCOMPARE(h.tokenStore.calls.size(), 1); // only A touched so far.

  const auto profileB2 = ServerProfile::customWithId(
      idB, QStringLiteral("B"), QStringLiteral("https://b-new.example.test"));
  const auto profileD2 = ServerProfile::customWithId(
      idD, QStringLiteral("D-new-name"), sharedUrlD); // name only
  QVERIFY(profileB2.has_value());
  QVERIFY(profileD2.has_value());
  // C is removed entirely; B's endpoint changes; D's display name only
  // changes (same endpoint) -- A is unchanged.
  h.profileStore.profiles = {*profileA, *profileB2, *profileD2};

  h.coordinator->start();
  QVERIFY(h.tokenStore.hasPending(idB));
  QVERIFY(h.tokenStore.hasPending(idC));
  QVERIFY(!h.tokenStore.hasPending(idD));
  int bDeletes = 0;
  int cDeletes = 0;
  int dOps = 0;
  for (const auto &call : h.tokenStore.calls) {
    if (call.profileId == idB) {
      QCOMPARE(call.kind, QStringLiteral("delete"));
      ++bDeletes;
    }
    if (call.profileId == idC) {
      QCOMPARE(call.kind, QStringLiteral("delete"));
      ++cDeletes;
    }
    if (call.profileId == idD) {
      ++dOps;
    }
  }
  QCOMPARE(bDeletes, 1);
  QCOMPARE(cDeletes, 1);
  QCOMPARE(dOps, 0);
  h.tokenStore.complete(idB, notFoundResult());
  h.tokenStore.complete(idC, notFoundResult());

  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idA] { return h.tokenStore.hasPending(idA); }));
  h.tokenStore.complete(idA, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    selectedProfileEndpointChangeDoesNotDoubleInvalidateViaReconcileHelper() {
  Harness h;
  const QString profileId =
      QStringLiteral("22222222-2222-2222-2222-222222222222");
  const QString oldUrl = QStringLiteral("https://old.example.test");
  const QString newUrl = QStringLiteral("https://new.example.test");
  const auto v1 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), oldUrl);
  QVERIFY(v1.has_value());
  h.profileStore.profiles = {*v1};
  h.profileStore.selectedId = profileId;
  bootToSignedIn(h, QStringLiteral("old-token"));
  QCOMPARE(h.tokenStore.calls.size(), 2); // read, save

  const auto v2 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), newUrl);
  QVERIFY(v2.has_value());
  h.profileStore.profiles[0] = *v2;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());

  // Exactly ONE required Delete is reserved for this endpoint change, even
  // though this profile IS the (about-to-remain) selected one:
  // reconcileAllProfileCredentialsOnReload() now centrally covers EVERY
  // retained ID -- including the selected one -- so mutateSelectedProfile()
  // was deliberately stripped of its own, now-redundant endpoint-change
  // detection to guarantee this single source of truth never
  // double-invalidates the same change.
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  int deleteCount = 0;
  for (const auto &call : h.tokenStore.calls) {
    if (call.profileId == profileId && call.kind == QStringLiteral("delete")) {
      ++deleteCount;
    }
  }
  QCOMPARE(deleteCount, 1);
  h.tokenStore.complete(profileId, successWriteResult());

  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("read"));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
}

void SessionCoordinatorTests::
    freshSaveAfterEndpointChangeBindsNewTokenToNewEndpointIdentityOnly() {
  Harness h;
  const QString profileId =
      QStringLiteral("22222222-2222-2222-2222-222222222222");
  const QString oldUrl = QStringLiteral("https://old.example.test");
  const QString newUrl = QStringLiteral("https://new.example.test");
  const auto v1 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), oldUrl);
  QVERIFY(v1.has_value());
  h.profileStore.profiles = {*v1};
  h.profileStore.selectedId = profileId;
  bootToSignedIn(h, QStringLiteral("old-token"));

  const auto v2 =
      ServerProfile::customWithId(profileId, QStringLiteral("Name"), newUrl);
  QVERIFY(v2.has_value());
  h.profileStore.profiles[0] = *v2;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("read"));
  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  h.coordinator->signIn(QStringLiteral("carol@example.test"),
                        QStringLiteral("hunter2"));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 3; }));
  h.authClient.completeToken(3, tokenSuccess(QStringLiteral("fresh-token")));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 4; }));
  h.authClient.completeUser(4,
                            userSuccess(QStringLiteral("carol"),
                                        QStringLiteral("carol@example.test")));
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("save"));
  // The fresh token is bound to the NEW endpoint identity only -- never
  // to the old one this profileId() used to designate.
  QCOMPARE(h.tokenStore.calls.last().endpointIdentity,
           v2->credentialEndpointIdentity());
  QVERIFY(h.tokenStore.calls.last().endpointIdentity !=
          v1->credentialEndpointIdentity());
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
}

void SessionCoordinatorTests::
    staleUntrustedTokenDeleteCallbackReplayCannotCorruptSubsequentRestore() {
  Harness h;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  const QString profileId = h.coordinator->selectedProfileId();
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 1);
  h.tokenStore.complete(profileId, bindingMismatchResult());

  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 2);
  QCOMPARE(h.tokenStore.calls.at(1).kind, QStringLiteral("delete"));
  h.tokenStore.complete(profileId, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 2);

  // Restart: a genuinely new credential-restore Read now dispatches, with
  // its own distinct opId/attemptId entirely unrelated to the completed
  // delete above.
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.calls.at(2).kind, QStringLiteral("read"));

  // Replay the DELETE's stale, long-since-consumed completion. Its
  // (opId, attemptId) no longer matches the profile's current dispatch
  // record, which now belongs to the fresh Read.
  h.tokenStore.replayLastCompletion(profileId);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  QCOMPARE(h.tokenStore.calls.size(), 3);
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);
  QVERIFY(h.authClient.calls.isEmpty());
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::RestoringCredential);

  h.tokenStore.complete(profileId, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QCOMPARE(h.tokenStore.calls.size(), 3);
}

void SessionCoordinatorTests::
    diagnosticsNeverContainEndpointIdentityForBindingOutcomes() {
  Harness h;
  const QString profileId =
      QStringLiteral("55555555-5555-5555-5555-555555555555");
  const QString sentinelUrl =
      QStringLiteral("https://sentinel-secret-host.example.test/sentinel-path");
  const auto profile = ServerProfile::customWithId(
      profileId, QStringLiteral("Name"), sentinelUrl);
  QVERIFY(profile.has_value());
  h.profileStore.profiles = {*profile};
  h.profileStore.selectedId = profileId;

  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, bindingMismatchResult());
  QVERIFY(pumpEventsUntil(
      [&h, profileId] { return h.tokenStore.hasPending(profileId); }));
  h.tokenStore.complete(profileId, backendErrorResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::SecureStorageUnavailable;
  }));

  // No matter what transient/terminal state this settles into, the
  // profile's endpoint identity (which embeds its full URL) must never
  // leak into the coordinator's own observable diagnostic.
  QVERIFY(!h.coordinator->diagnostic().contains(sentinelUrl));
  QVERIFY(!h.coordinator->diagnostic().contains(
      profile->credentialEndpointIdentity()));
}

// ─── Centralized all-profile reconciliation: newly-selected ID ──────────
//
// reconcileAllProfileCredentialsOnReload() runs over EVERY retained ID --
// including one that is ABOUT to become newly selected in this very
// reload -- strictly before mutateSelectedProfile() ever assigns
// m_currentProfile/m_selectedProfileId to it. These tests exercise the
// exact race from review: an in-flight restore Read for a profile that
// was PREVIOUSLY selected (then switched away from) must never be
// rebound to a fresh continuation once that same profileId() reloads
// with BOTH a changed endpoint AND renewed selection in one reload.

void SessionCoordinatorTests::
    newlySelectedProfileEndpointChangeInSameReloadStillReservesRequiredDeleteBeforeFreshRead() {
  Harness h;
  const QString idA = QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
  const QString idB = QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
  const QString oldUrlB = QStringLiteral("https://b-old.example.test");
  const QString newUrlB = QStringLiteral("https://b-new.example.test/app");
  const auto profileA = ServerProfile::customWithId(
      idA, QStringLiteral("A"), QStringLiteral("https://a.example.test"));
  const auto profileB1 =
      ServerProfile::customWithId(idB, QStringLiteral("B"), oldUrlB);
  QVERIFY(profileA.has_value());
  QVERIFY(profileB1.has_value());
  h.profileStore.profiles = {*profileA, *profileB1};
  h.profileStore.selectedId = idB;

  // Boot with B selected: its restore Read dispatches and is held in
  // flight (never completed).
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.size(), 1);
  QCOMPARE(h.tokenStore.calls.first().kind, QStringLiteral("read"));
  QCOMPARE(h.tokenStore.calls.first().profileId, idB);

  // The UI switches to A while B's Read is still outstanding. B's Read is
  // never cancelled/aborted -- it remains queued/in-flight exactly as
  // before, now simply abandoned by selection.
  h.coordinator->switchProfile(idA);
  QCOMPARE(h.coordinator->selectedProfileId(), idA);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idA] { return h.tokenStore.hasPending(idA); }));
  h.tokenStore.complete(idA, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  // ONE persisted reload both changes B's canonical endpoint AND
  // re-selects B -- exactly the review's trigger.
  const auto profileB2 =
      ServerProfile::customWithId(idB, QStringLiteral("B"), newUrlB);
  QVERIFY(profileB2.has_value());
  h.profileStore.profiles[1] = *profileB2;
  h.profileStore.selectedId = idB;
  // Baseline: A's own read (call index 1) has already completed by now;
  // only calls made from this point on are relevant to the race below.
  const int baseline = h.tokenStore.calls.size();
  QCOMPARE(baseline, 2);

  h.coordinator->start();
  QCOMPARE(h.coordinator->selectedProfileId(), idB);
  QCOMPARE(h.coordinator->selectedProfileBaseUrl(), newUrlB);

  // The new B probe (for the NEW endpoint) completes BEFORE the old B
  // Read ever does.
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RestoringCredential;
  }));
  // Nothing new has touched the real store yet: the required Delete and
  // any fresh Read are both merely queued behind the still-in-flight old
  // Read.
  QCOMPARE(h.tokenStore.calls.size(), baseline);
  QVERIFY(h.authClient.calls.isEmpty());

  // NOW the old (stale) B Read finally completes, returning a token that
  // was scoped to B's OLD endpoint.
  const QString oldToken = QStringLiteral("old-b-endpoint-token");
  h.tokenStore.complete(idB, successReadResult(oldToken));

  // Zero whoami/auth requests of any kind may ever carry this token: the
  // stale Read's own (permanently superseded-generation) continuation
  // must discard it. The required Delete for B's old-endpoint token
  // dispatches next.
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.size(), baseline + 1);
  QCOMPARE(h.tokenStore.calls.at(baseline).kind, QStringLiteral("delete"));
  QCOMPARE(h.tokenStore.calls.at(baseline).profileId, idB);
  QVERIFY(h.authClient.calls.isEmpty());

  // The required Delete succeeds...
  h.tokenStore.complete(idB, successWriteResult());

  // ...and ONLY NOW does a distinct FRESH Read (for B's new endpoint)
  // ever dispatch.
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.size(), baseline + 2);
  QCOMPARE(h.tokenStore.calls.at(baseline + 1).kind, QStringLiteral("read"));
  QCOMPARE(h.tokenStore.calls.at(baseline + 1).endpointIdentity,
           profileB2->credentialEndpointIdentity());
  QVERIFY(h.authClient.calls.isEmpty());

  const QString newToken = QStringLiteral("new-b-endpoint-token");
  h.tokenStore.complete(idB, successReadResult(newToken));
  QVERIFY(pumpEventsUntil([&h] { return h.authClient.calls.size() == 1; }));
  QCOMPARE(h.authClient.calls.first().kind, FakeAuthClient::CallKind::WhoAmI);
  QVERIFY(h.authClient.calls.first().token == newToken);
  QCOMPARE(h.authClient.calls.first().profile.baseUrl().toString(), newUrlB);
  h.authClient.completeUser(
      1,
      userSuccess(QStringLiteral("ivan"), QStringLiteral("ivan@example.test")));
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedIn;
  }));
}

void SessionCoordinatorTests::
    newlySelectedProfileEndpointChangeQueuedNotInFlightOldReadStillBlockedByRequiredDelete() {
  // Same trigger, but B's stale Read is only QUEUED (never yet
  // dispatched to the real store) at the moment of the reload, because a
  // required Delete for B (from an earlier, already-resolved sign-out)
  // occupies the FIFO head. The centralized reconciliation must still
  // reserve exactly one NEW required Delete for the endpoint change
  // behind that stale queued Read, never in front of it.
  Harness h;
  const QString idA = QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
  const QString idB = QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
  const QString oldUrlB = QStringLiteral("https://b-old.example.test");
  const QString newUrlB = QStringLiteral("https://b-new.example.test");
  const auto profileA = ServerProfile::customWithId(
      idA, QStringLiteral("A"), QStringLiteral("https://a.example.test"));
  const auto profileB1 =
      ServerProfile::customWithId(idB, QStringLiteral("B"), oldUrlB);
  QVERIFY(profileA.has_value());
  QVERIFY(profileB1.has_value());
  h.profileStore.profiles = {*profileA, *profileB1};
  h.profileStore.selectedId = idB;
  bootToSignedIn(h, QStringLiteral("b-token"));
  QCOMPARE(h.tokenStore.calls.size(), 2); // read, save
  const int authBaseline = h.authClient.calls.size();

  // Sign out while B is selected: this dispatches a Delete (which we hold
  // in flight -- never completed) and queues a fresh restore Read behind
  // it.
  h.coordinator->signOut();
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));

  // Switch to A while B's Delete is in flight and B's restore Read is
  // merely queued behind it (never dispatched).
  h.coordinator->switchProfile(idA);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idA] { return h.tokenStore.hasPending(idA); }));
  h.tokenStore.complete(idA, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  // ONE reload both changes B's endpoint and re-selects B, while B's
  // sign-out Delete is STILL in flight and its queued restore Read has
  // STILL never dispatched.
  const auto profileB2 =
      ServerProfile::customWithId(idB, QStringLiteral("B"), newUrlB);
  QVERIFY(profileB2.has_value());
  h.profileStore.profiles[1] = *profileB2;
  h.profileStore.selectedId = idB;
  const int baseline = h.tokenStore.calls.size();
  h.coordinator->start();
  QCOMPARE(h.coordinator->selectedProfileId(), idB);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());

  // No new store call is dispatched yet: B's original sign-out Delete is
  // still occupying the FIFO head. The endpoint-change required Delete
  // and the fresh Read are both merely queued behind it, never
  // dispatched early.
  QCOMPARE(h.tokenStore.calls.size(), baseline);

  // The original sign-out Delete completes...
  h.tokenStore.complete(idB, successWriteResult());
  // ...and the endpoint-change required Delete dispatches next -- NOT the
  // stale queued restore Read.
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.size(), baseline + 1);
  QCOMPARE(h.tokenStore.calls.at(baseline).kind, QStringLiteral("delete"));
  QCOMPARE(h.authClient.calls.size(), authBaseline);
  h.tokenStore.complete(idB, notFoundResult());

  // Only now does a fresh Read (for B's new endpoint) ever dispatch.
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.size(), baseline + 2);
  QCOMPARE(h.tokenStore.calls.at(baseline + 1).kind, QStringLiteral("read"));
  QCOMPARE(h.tokenStore.calls.at(baseline + 1).endpointIdentity,
           profileB2->credentialEndpointIdentity());
  h.tokenStore.complete(idB, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QCOMPARE(h.authClient.calls.size(), authBaseline);
}

void SessionCoordinatorTests::
    multipleChangedProfilesOnReloadThatAlsoSelectsOneOfThemReserveCleanupDeterministically() {
  // Multiple profiles change endpoint on the same reload, one of them
  // (C) also becoming newly selected in that same reload. Every changed
  // ID must reserve exactly one required Delete, processed in
  // deterministic (persisted-list) order, regardless of which one is
  // about to become selected.
  Harness h;
  const QString idA = QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
  const QString idB = QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
  const QString idC = QStringLiteral("cccccccc-cccc-cccc-cccc-cccccccccccc");
  const auto profileA = ServerProfile::customWithId(
      idA, QStringLiteral("A"), QStringLiteral("https://a.example.test"));
  const auto profileB1 = ServerProfile::customWithId(
      idB, QStringLiteral("B"), QStringLiteral("https://b-old.example.test"));
  const auto profileC1 = ServerProfile::customWithId(
      idC, QStringLiteral("C"), QStringLiteral("https://c-old.example.test"));
  QVERIFY(profileA.has_value());
  QVERIFY(profileB1.has_value());
  QVERIFY(profileC1.has_value());
  h.profileStore.profiles = {*profileA, *profileB1, *profileC1};
  h.profileStore.selectedId = idA;
  h.bootToSignedOut();
  QCOMPARE(h.tokenStore.calls.size(), 1); // only A touched so far.

  const auto profileB2 = ServerProfile::customWithId(
      idB, QStringLiteral("B"), QStringLiteral("https://b-new.example.test"));
  const auto profileC2 = ServerProfile::customWithId(
      idC, QStringLiteral("C"), QStringLiteral("https://c-new.example.test"));
  QVERIFY(profileB2.has_value());
  QVERIFY(profileC2.has_value());
  // Both B and C change endpoint; C ALSO becomes newly selected; A is
  // unchanged and stays deselected.
  h.profileStore.profiles = {*profileA, *profileB2, *profileC2};
  h.profileStore.selectedId = idC;

  h.coordinator->start();
  QCOMPARE(h.coordinator->selectedProfileId(), idC);
  // Both B and C reserve exactly one required Delete each, dispatched in
  // persisted-list order (B before C) -- deterministic regardless of
  // which one is newly selected.
  QVERIFY(h.tokenStore.hasPending(idB));
  QVERIFY(h.tokenStore.hasPending(idC));
  int bDeletes = 0;
  int cDeletes = 0;
  int firstDeleteIndexB = -1;
  int firstDeleteIndexC = -1;
  for (int i = 0; i < h.tokenStore.calls.size(); ++i) {
    const auto &call = h.tokenStore.calls.at(i);
    if (call.profileId == idB) {
      QCOMPARE(call.kind, QStringLiteral("delete"));
      ++bDeletes;
      if (firstDeleteIndexB < 0) {
        firstDeleteIndexB = i;
      }
    }
    if (call.profileId == idC) {
      QCOMPARE(call.kind, QStringLiteral("delete"));
      ++cDeletes;
      if (firstDeleteIndexC < 0) {
        firstDeleteIndexC = i;
      }
    }
  }
  QCOMPARE(bDeletes, 1);
  QCOMPARE(cDeletes, 1);
  QVERIFY(firstDeleteIndexB < firstDeleteIndexC);

  // C's own selection/probe/restore must not proceed ahead of its own
  // required Delete: no whoami/auth call may exist yet.
  QVERIFY(h.authClient.calls.isEmpty());

  h.tokenStore.complete(idB, notFoundResult());
  h.tokenStore.complete(idC, successWriteResult());

  // C's fresh restore Read (for its NEW endpoint) only dispatches after
  // its own required Delete resolves, once C's own probe (dispatched
  // for the newly-selected profile) also completes.
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idC] {
    return h.tokenStore.calls.size() == 4 &&
           h.tokenStore.calls.last().profileId == idC;
  }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("read"));
  QCOMPARE(h.tokenStore.calls.last().endpointIdentity,
           profileC2->credentialEndpointIdentity());
  h.tokenStore.complete(idC, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(h.authClient.calls.isEmpty());
}

void SessionCoordinatorTests::
    repeatedStartWhileNewlySelectedProfileEndpointChangeDedupesOnlyFreshRead() {
  // Once the newly-selected profile's stale Read/required-Delete/fresh-
  // Read chain is established, calling start() again repeatedly must
  // dedupe: no additional stale Read is created, and at most one fresh
  // (current-epoch) restore Read is ever queued for it.
  Harness h;
  const QString idA = QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
  const QString idB = QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
  const QString oldUrlB = QStringLiteral("https://b-old.example.test");
  const QString newUrlB = QStringLiteral("https://b-new.example.test");
  const auto profileA = ServerProfile::customWithId(
      idA, QStringLiteral("A"), QStringLiteral("https://a.example.test"));
  const auto profileB1 =
      ServerProfile::customWithId(idB, QStringLiteral("B"), oldUrlB);
  QVERIFY(profileA.has_value());
  QVERIFY(profileB1.has_value());
  h.profileStore.profiles = {*profileA, *profileB1};
  h.profileStore.selectedId = idB;
  h.coordinator->start();
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.size(), 1); // stale old Read, held in flight

  h.coordinator->switchProfile(idA);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idA] { return h.tokenStore.hasPending(idA); }));
  h.tokenStore.complete(idA, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));

  const auto profileB2 =
      ServerProfile::customWithId(idB, QStringLiteral("B"), newUrlB);
  QVERIFY(profileB2.has_value());
  h.profileStore.profiles[1] = *profileB2;
  h.profileStore.selectedId = idB;
  const int baseline = h.tokenStore.calls.size();

  // Call start() repeatedly for the same reload -- each call re-derives
  // the same (already-applied) endpoint change; it must not reserve a
  // second required Delete nor enqueue a second fresh Read.
  h.coordinator->start();
  h.coordinator->start();
  h.coordinator->start();
  QCOMPARE(h.coordinator->selectedProfileId(), idB);
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() ==
           SessionCoordinator::State::RestoringCredential;
  }));
  QCOMPARE(h.tokenStore.calls.size(), baseline); // still only the stale Read

  h.tokenStore.complete(idB, notFoundResult()); // stale Read resolves inertly
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.size(), baseline + 1);
  QCOMPARE(h.tokenStore.calls.at(baseline).kind, QStringLiteral("delete"));
  h.tokenStore.complete(idB, successWriteResult());

  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.size(), baseline + 2);
  QCOMPARE(h.tokenStore.calls.at(baseline + 1).kind, QStringLiteral("read"));
  h.tokenStore.complete(idB, notFoundResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  // Exactly one fresh Read for B's NEW endpoint, despite three redundant
  // start() calls (the one earlier stale Read, for the OLD endpoint, is
  // the only other read ever issued for B).
  int freshReadsForB = 0;
  for (int i = baseline; i < h.tokenStore.calls.size(); ++i) {
    if (h.tokenStore.calls.at(i).profileId == idB &&
        h.tokenStore.calls.at(i).kind == QStringLiteral("read")) {
      ++freshReadsForB;
    }
  }
  QCOMPARE(freshReadsForB, 1);
}

// ─── Orphan credential-cleanup-failure visibility independent of
// current selection ───────────────────────────────────────────────────────
//
// A required Delete's failure must be visible/actionable via
// state()/diagnostic() and retry() even when the profile that failed is
// NOT the one currently selected -- tracked via m_stalledProfileOrder,
// surfaced by hasBlockingOrphanCleanup() only while it is non-empty and
// its head differs from the current selection (see state()/diagnostic()
// and markProfileCleanupStalled()/clearProfileCleanupStalled() in
// SessionCoordinator.cpp for the exact mechanism).

void SessionCoordinatorTests::
    removedProfileDeleteFailureIsVisibleAndRetryableWhileDifferentProfileRemainsCurrent() {
  Harness h;
  const QString idA = QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
  const QString idB = QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
  const auto profileA = ServerProfile::customWithId(
      idA, QStringLiteral("A"), QStringLiteral("https://a.example.test"));
  const auto profileB = ServerProfile::customWithId(
      idB, QStringLiteral("B"), QStringLiteral("https://b.example.test"));
  QVERIFY(profileA.has_value());
  QVERIFY(profileB.has_value());
  h.profileStore.profiles = {*profileA, *profileB};
  h.profileStore.selectedId = idA;
  h.bootToSignedOut();
  QCOMPARE(h.tokenStore.calls.size(), 1); // only A's own restore so far.

  // B is removed from persisted storage while A remains selected: its
  // orphaned secure entry is proactively, durably deleted.
  h.profileStore.profiles = {*profileA};
  h.coordinator->start();
  QVERIFY(h.tokenStore.hasPending(idB));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
  QCOMPARE(h.tokenStore.calls.last().profileId, idB);

  // The Delete for the REMOVED, non-current profile fails. This must be
  // visible via state()/diagnostic() -- not silently swallowed just
  // because A (not B) is the current selection.
  h.tokenStore.complete(idB, backendErrorResult());
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::SecureStorageUnavailable);
  QVERIFY(!h.coordinator->diagnostic().isEmpty());
  // The actual selection is untouched by this override.
  QCOMPARE(h.coordinator->selectedProfileId(), idA);

  // Switching away and back, and calling start() again, must not lose
  // this obligation. Restarting A's own flow (a side effect of start()
  // unconditionally re-probing the current profile) must still be
  // allowed to proceed normally in the background -- it is only its
  // VISIBILITY that the still-pending B obligation masks.
  h.coordinator->start();
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::SecureStorageUnavailable);
  QCOMPARE(h.coordinator->selectedProfileId(), idA);
  // No duplicate Delete dispatch was created by the repeated start().
  int bDeleteCount = 0;
  for (const auto &call : h.tokenStore.calls) {
    if (call.profileId == idB) {
      ++bDeleteCount;
    }
  }
  QCOMPARE(bDeleteCount, 1);

  // Let A's own (masked, but still genuinely progressing) restart reach
  // SignedOut in the background.
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idA] { return h.tokenStore.hasPending(idA); }));
  h.tokenStore.complete(idA, notFoundResult());
  // A's real underlying state is now SignedOut, but it stays masked:
  // the still-unresolved B obligation is still the oldest/head one.
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::SecureStorageUnavailable);

  // retry() targets the exact orphaned profile (B), not A's own
  // (unrelated, already-resolved) flow.
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.last().kind, QStringLiteral("delete"));
  QCOMPARE(h.tokenStore.calls.last().profileId, idB);

  // Repeated failure stays actionable -- it does not silently clear or
  // get replaced.
  h.tokenStore.complete(idB, backendErrorResult());
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::SecureStorageUnavailable);

  // A final retry succeeds: the obligation clears, and A's own
  // already-progressed flow (which was never structurally blocked in the
  // background) becomes visible/resumes.
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  h.tokenStore.complete(idB, successWriteResult());
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(h.authClient.calls.isEmpty());
  QCOMPARE(h.coordinator->selectedProfileId(), idA);
}

void SessionCoordinatorTests::
    twoRemovedProfilesWithIndependentDeleteFailuresRetryFifoOrderAndStaleCallbackCannotClearWrongOne() {
  Harness h;
  const QString idA = QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
  const QString idB = QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
  const QString idC = QStringLiteral("cccccccc-cccc-cccc-cccc-cccccccccccc");
  const auto profileA = ServerProfile::customWithId(
      idA, QStringLiteral("A"), QStringLiteral("https://a.example.test"));
  const auto profileB = ServerProfile::customWithId(
      idB, QStringLiteral("B"), QStringLiteral("https://b.example.test"));
  const auto profileC = ServerProfile::customWithId(
      idC, QStringLiteral("C"), QStringLiteral("https://c.example.test"));
  QVERIFY(profileA.has_value());
  QVERIFY(profileB.has_value());
  QVERIFY(profileC.has_value());
  h.profileStore.profiles = {*profileA, *profileB, *profileC};
  h.profileStore.selectedId = idA;
  h.bootToSignedOut();
  QCOMPARE(h.tokenStore.calls.size(), 1);

  // Both B and C are removed on the same reload; A remains selected.
  // This restart also causes A's own (unrelated) capability probe to
  // reissue -- drain and complete it here so A's own background progress
  // is not confused with this test's B/C-focused assertions below.
  h.profileStore.profiles = {*profileA};
  h.coordinator->start();
  QVERIFY(h.tokenStore.hasPending(idB));
  QVERIFY(h.tokenStore.hasPending(idC));
  QCOMPARE(h.tokenStore.calls.size(),
           3); // A's earlier read, B delete, C delete
  QCOMPARE(h.tokenStore.calls.at(1).profileId, idB);
  QCOMPARE(h.tokenStore.calls.at(2).profileId, idC);

  // Both fail independently.
  h.tokenStore.complete(idB, backendErrorResult());
  h.tokenStore.complete(idC, backendErrorResult());
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::SecureStorageUnavailable);
  QCOMPARE(h.coordinator->selectedProfileId(), idA);

  // Let A's own (masked, but still genuinely progressing) restart reach
  // SignedOut in the background, so it is ready to become visible again
  // the instant both B and C's obligations resolve below.
  QVERIFY(
      pumpEventsUntil([&h] { return h.probeFactory.current() != nullptr; }));
  h.probeFactory.current()->complete(compatibleProbeResult());
  QVERIFY(pumpEventsUntil([&h, idA] { return h.tokenStore.hasPending(idA); }));
  h.tokenStore.complete(idA, notFoundResult());
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::SecureStorageUnavailable);

  // retry() must resolve B (the FIFO head, first to fail) before C: a
  // stale replayed callback for the already-completed FIRST B attempt
  // must not clear C's still-pending obligation, nor B's own NEW attempt.
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil([&h, idB] { return h.tokenStore.hasPending(idB); }));
  QCOMPARE(h.tokenStore.calls.last().profileId, idB);
  QVERIFY(!h.tokenStore.hasPending(idC)); // C's retry has not begun yet

  // Replay B's FIRST (already-completed, stale) failure callback: this
  // must be inert -- it cannot corrupt B's new in-flight retry attempt,
  // nor clear any obligation.
  h.tokenStore.replayLastCompletion(idB);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  QCOMPARE(h.tokenStore.overlappingDispatchCount(), 0);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::SecureStorageUnavailable);
  QVERIFY(h.tokenStore.hasPending(idB)); // B's genuine new attempt unaffected

  // B's genuine new retry attempt now succeeds: B's obligation clears,
  // but C's independent obligation remains, still fully visible.
  h.tokenStore.complete(idB, successWriteResult());
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  QCOMPARE(h.coordinator->state(),
           SessionCoordinator::State::SecureStorageUnavailable);

  // retry() now targets C.
  h.coordinator->retry();
  QVERIFY(pumpEventsUntil([&h, idC] { return h.tokenStore.hasPending(idC); }));
  QCOMPARE(h.tokenStore.calls.last().profileId, idC);
  h.tokenStore.complete(idC, successWriteResult());

  // Both obligations resolved: A's own already-progressed flow becomes
  // visible/resumes.
  QVERIFY(pumpEventsUntil([&h] {
    return h.coordinator->state() == SessionCoordinator::State::SignedOut;
  }));
  QVERIFY(h.authClient.calls.isEmpty());
  QCOMPARE(h.coordinator->selectedProfileId(), idA);
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
