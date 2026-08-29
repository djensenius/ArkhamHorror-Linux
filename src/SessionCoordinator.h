#pragma once

#include "AuthModels.h"
#include "IAuthenticationClient.h"
#include "ICapabilityProbe.h"
#include "IProfileStore.h"
#include "ITokenStore.h"
#include "ProbeResult.h"
#include "ServerProfile.h"

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace Arkham {

// Composes the profile store, capability probe, secure token store, and
// authentication client foundations (see IProfileStore, ICapabilityProbe,
// ITokenStore, IAuthenticationClient) into a single deterministic,
// QML-observable session coordinator.
//
// This is headless/foundation-only: no account/server-management QML,
// WebSockets, password reset, account deletion, or gameplay wiring. It only
// establishes the ordering guarantees a future UI can build on.
//
// Never exposes a password, token, or Authorization header through any
// property, signal, QVariant, log, or debug operator. state() returns the
// typed State enum value; stateDescription() is always a fixed, static,
// secret-free label for that value. diagnostic() is secret-free but not
// always static text: for transport/backend/malformed-payload failures it
// forwards the typed, already-secret-free diagnostic produced by
// IAuthenticationClient/ITokenStore/ICapabilityProbe. Those diagnostics are
// safe-category text but not necessarily static strings -- for example
// ICapabilityProbe's NetworkCapabilityProbe implementation includes
// QNetworkReply::errorString()/QJsonParseError::errorString() wording -- so
// diagnostic()'s exact text can vary with the underlying failure while
// never containing request/response bodies, credentials, or an
// Authorization header.
//
// Threading/lifetime: every dependency is borrowed by reference and must
// outlive this coordinator (see composeProductionSession() in
// AppSessionComposition.h for the production ownership order that
// guarantees this).
// Construction performs no I/O; start() must be called explicitly to begin
// loading profiles and probing the selected server. Destroying the
// coordinator cancels the outstanding authentication request (if any) and
// destroys the current capability probe instance (which, per
// ICapabilityProbe's contract, suppresses any pending finished() signal);
// any token-store operation already in flight is uncancellable and is left
// to complete, but its completion is guarded by a QPointer so it can never
// touch a destroyed coordinator.
//
// Durability scope: every guarantee above (uncancellable operations always
// completing, abandoned fresh-token saves being compensated by a durable
// cleanup delete, a failed required deletion permanently blocking later
// same-profile operations until retried) holds for as long as this process
// keeps running. None of it survives an OS-level process kill, crash, or
// power loss between ITokenStore dispatching an operation and this
// coordinator observing its completion: this class keeps no on-disk
// journal of in-flight intent, so a process termination at exactly that
// moment can leave the secure store in whatever state the backend itself
// left it in, with no in-process record to resume or compensate from.
// Nothing here claims otherwise.
class SessionCoordinator final : public QObject {
  Q_OBJECT
  Q_PROPERTY(State state READ state NOTIFY stateChanged)
  Q_PROPERTY(QString stateDescription READ stateDescription NOTIFY stateChanged)
  Q_PROPERTY(QString diagnostic READ diagnostic NOTIFY stateChanged)
  Q_PROPERTY(QString selectedProfileId READ selectedProfileId NOTIFY
                 selectedProfileChanged)
  Q_PROPERTY(QString selectedProfileDisplayName READ selectedProfileDisplayName
                 NOTIFY selectedProfileChanged)
  Q_PROPERTY(QString selectedProfileBaseUrl READ selectedProfileBaseUrl NOTIFY
                 selectedProfileChanged)
  Q_PROPERTY(
      QString currentUsername READ currentUsername NOTIFY currentUserChanged)
  Q_PROPERTY(QString currentEmail READ currentEmail NOTIFY currentUserChanged)
  Q_PROPERTY(
      bool currentUserBeta READ currentUserBeta NOTIFY currentUserChanged)
  Q_PROPERTY(
      bool currentUserAdmin READ currentUserAdmin NOTIFY currentUserChanged)

public:
  // Explicit, non-overlapping states. Deliberately not a bool soup: every
  // stage that a QML view might need to distinguish (see class comment) has
  // its own value. RecoverableFailure covers every retryable failure that
  // is not one of the more specific states below: capability-probe
  // configuration/network/HTTP/malformed-JSON errors (including a null/
  // empty ProbeFactory) and restored-credential whoami transport/malformed/
  // non-HTTP errors. It is not limited to network/transport problems.
  // Incompatible and SecureStorageUnavailable and ProfileStorageFailure are
  // kept distinct because their retry action and user-facing meaning
  // differ.
  enum class State {
    Loading,                  ///< Initial boot: loading profiles/selection.
    ProbingCapabilities,      ///< Capability probe in flight for the current
                              ///< profile.
    RestoringCredential,      ///< Reading/validating a previously stored
                              ///< token for the current profile.
    SignedOut,                ///< No valid session for the current profile.
    Authenticating,           ///< signIn() request in flight.
    Registering,              ///< registerAccount() request in flight.
    SignedIn,                 ///< A validated session is active.
    SigningOut,               ///< signOut() has enqueued a durable token
                              ///< deletion that has not yet succeeded.
                              ///< The deletion is reserved (enqueued)
                              ///< BEFORE this state is published, so a
                              ///< reentrant observer of this transition
                              ///< can never cause the deletion itself to
                              ///< be dropped; the state is then installed
                              ///< only if still SignedIn (a synchronously-
                              ///< completing store may already have moved
                              ///< state further). Once installed, a
                              ///< reentrant or duplicate signOut() call
                              ///< (m_state is no longer SignedIn) is a
                              ///< safe no-op rather than bumping the
                              ///< generation or enqueuing a second
                              ///< deletion.
    Incompatible,             ///< The server failed compatibility evaluation.
    SecureStorageUnavailable, ///< The secure token store is locked,
                              ///< unavailable, denied access, rejected
                              ///< input, or reported a backend error.
    ProfileStorageFailure,    ///< IProfileStore load/save failed, or the
                              ///< persisted selection did not match any
                              ///< loaded profile.
    RecoverableFailure,       ///< A retryable transport failure (capability
                              ///< probe network error, or a restored
                              ///< credential's /whoami transport error).
  };
  Q_ENUM(State)

  // Constructs a fresh ICapabilityProbe instance for a single probe
  // attempt. SessionCoordinator always destroys the previous instance (via
  // this factory's product) before creating a new one for the next attempt
  // (initial probe, retry, or profile switch); because destroying an
  // ICapabilityProbe is documented to suppress any pending finished()
  // signal, this is how the coordinator safely disambiguates attempts even
  // though ICapabilityProbe itself has no per-call request handle. A
  // generation counter is kept as additional defense-in-depth.
  using ProbeFactory = std::function<std::unique_ptr<ICapabilityProbe>()>;

  SessionCoordinator(IProfileStore &profileStore, ProbeFactory probeFactory,
                     ITokenStore &tokenStore, IAuthenticationClient &authClient,
                     QObject *parent = nullptr);
  ~SessionCoordinator() override;

  [[nodiscard]] State state() const noexcept { return m_state; }
  // Static, secret-free, human-readable label for state(). Never derived
  // from any server- or user-supplied text.
  [[nodiscard]] QString stateDescription() const;
  // Secret-free, sanitized diagnostic for the current state. Empty unless
  // the current state represents a failure or an explanation is useful.
  // Not always static text: for transport/backend/malformed-payload
  // failures it may forward non-secret network/parse context (e.g. a
  // QNetworkReply/QJsonParseError errorString()) already sanitized by
  // IAuthenticationClient/ITokenStore/ICapabilityProbe. Never contains a
  // response body, request body, password, token, or Authorization
  // header.
  [[nodiscard]] QString diagnostic() const { return m_diagnostic; }

  [[nodiscard]] QString selectedProfileId() const {
    return m_selectedProfileId;
  }
  [[nodiscard]] QString selectedProfileDisplayName() const;
  [[nodiscard]] QString selectedProfileBaseUrl() const;

  [[nodiscard]] QString currentUsername() const;
  [[nodiscard]] QString currentEmail() const;
  [[nodiscard]] bool currentUserBeta() const;
  [[nodiscard]] bool currentUserAdmin() const;

  // Not a Q_PROPERTY: this slice does not build profile-management QML.
  // Exposed for tests and any future management surface.
  [[nodiscard]] const QList<ServerProfile> &profiles() const {
    return m_profiles;
  }

public slots:
  // Loads profiles and the selected profile ID, seeding and persisting
  // hostedDefault()+its selection on a genuinely empty first run (and
  // deterministically persisting a selection of the first loaded profile if
  // one exists but nothing was previously selected), then starts a
  // capability probe for the selected profile. Any load/save error is an
  // explicit ProfileStorageFailure; it is never treated as first-run
  // success. Intended to be called exactly once per coordinator instance
  // before any other slot is meaningful; however, because this is a public
  // Q_INVOKABLE-reachable slot, a re-entrant/duplicate call is defensively
  // guarded rather than assumed away: it cancels any pending auth request,
  // discards the current probe, and bumps the generation first, so every
  // async completion from a prior start() becomes stale and is safely
  // ignored (identical to switchProfile()/signOut()'s own restart guard).
  void start();

  // Switches to a different loaded profile. Persists the new selection
  // before using it; if persistence fails, the current profile/session is
  // left completely untouched and ProfileStorageFailure is reported (the
  // failed switch never silently "succeeds"). On successful persistence,
  // cancels any in-flight authentication request, discards the current
  // capability probe instance, advances the operation generation (so any
  // still-in-flight, uncancellable token-store completion for the previous
  // profile can never mutate state for the new one), and restarts the
  // capability probe for the new profile. The observable transition itself
  // is published as a coherent snapshot: identity is cleared and state is
  // set to a non-SignedIn transitional value BEFORE the selected profile
  // is reassigned and selectedProfileChanged() is emitted, so a reentrant
  // observer of any of these signals can never see a hybrid of the OLD
  // session's SignedIn state paired with the NEW profile's identity.
  void switchProfile(const QString &profileId);

  // Issues POST /authenticate for the current profile. Only usable while
  // state() == SignedOut (i.e. the profile's capability probe has already
  // completed with Compatible/LegacyFallback AND any credential restore has
  // already finished); a call outside that window is a safe no-op. This
  // is intentionally stricter than "profile usable" alone: running during
  // RestoringCredential would race the restore's own /whoami validation,
  // which cancels any in-flight auth request. On a successful token
  // response, validates the token with /whoami before persisting it via
  // ITokenStore; the coordinator becomes SignedIn only after both the
  // whoami validation and the secure save succeed. The password is never
  // retained beyond this call.
  void signIn(const QString &email, const QString &password);

  // Issues POST /register for the current profile, with the same
  // SignedOut-only guard, validate-then-persist ordering, and
  // password-retention contract as signIn().
  void registerAccount(const QString &email, const QString &username,
                       const QString &password);

  // Only usable while SignedIn. Reserves (enqueues) the durable token
  // deletion for the current profile FIRST, then transitions to
  // SigningOut only if still SignedIn (a synchronously-completing store
  // may already have moved state further) -- never the reverse -- so a
  // reentrant observer of the SigningOut transition can never cause the
  // deletion itself to be dropped. Cancels any in-flight session network
  // work and becomes SignedOut only once that deletion succeeds. Because
  // m_state is no longer SignedIn once the transition is delivered, a
  // reentrant (from within the stateChanged() emission) or merely
  // duplicate signOut() call is a safe, idempotent no-op: it can never
  // bump the generation or enqueue a second deletion. On deletion failure,
  // the signed-in identity is preserved and the coordinator reports
  // SecureStorageUnavailable with an actionable retry(); it never claims
  // SignedOut while a token might still remain in the secure store. If
  // the profile is switched away from while a deletion is pending or has
  // failed, the obligation to delete is retained for that profile (see
  // the FIFO/credential-epoch machinery below) and is enforced again
  // before any future restore/auth/save for it, independent of the
  // coordinator's current generation/selection.
  void signOut();

  // Re-runs whatever stage most recently failed (profile load/save,
  // capability probe, restored-credential read/whoami, or a pending
  // sign-out deletion). A safe no-op if nothing is retryable.
  void retry();

signals:
  void stateChanged();
  void selectedProfileChanged();
  void currentUserChanged();

private:
  enum class TokenOpKind { Read, Save, Delete };
  enum class WhoAmIPurpose { RestoreExisting, FreshlyObtained };

  struct TokenOp {
    TokenOpKind kind{TokenOpKind::Read};
    QString token; // only meaningful for Save
    ITokenStore::ResultCallback onComplete;
    // Captured from profileCredentialEpoch(profileId) at enqueue time.
    // Only ever consulted for Save: a Save whose captured epoch no longer
    // matches the profile's current epoch by the time it reaches the
    // front of the FIFO was abandoned (profile switch, restart, or
    // sign-out) before ever crossing the ITokenStore boundary, and is
    // safely skipped without dispatching it. Read/Delete are never
    // epoch-gated -- see invalidateProfileCredential()'s class comment.
    quint64 admissionEpoch{0};
    // Unique for the lifetime of this logical operation: assigned once at
    // enqueue time and never changed, even across repeated retries of a
    // stalled head op (see retryStuckProfileTokenOp()). Lets a completion
    // callback (see ProfileTokenDispatch/startFrontTokenOp()) prove it is
    // reporting on the op that is actually still at the head of the
    // queue, rather than a later one that has since replaced it.
    quint64 opId{0};
  };

  // Per-profile bookkeeping for the single ITokenStore operation (if any)
  // currently dispatched to the real store for that profile's queue head.
  // inFlight is the central guard that keeps at most one real store call
  // outstanding per profile at a time: startFrontTokenOp() refuses to
  // dispatch anything while it is true, so a duplicate/racing call (e.g.
  // retryStuckProfileTokenOp() invoked twice before the first retry
  // attempt's callback has fired, or start()/switchProfile() re-reaching
  // the same stalled retry path) can never issue a second concurrent
  // operation for the same profile. attemptId is bumped on every dispatch
  // (including re-dispatching the same opId on retry) so a completion
  // callback can distinguish "the attempt I was issued for" from any
  // later attempt, even when the logical op ID is unchanged; a callback
  // whose captured (opId, attemptId) no longer matches this state (or
  // whose opId no longer matches the queue's actual head) is a stale or
  // duplicate invocation and must not dequeue/advance/invoke anything.
  struct ProfileTokenDispatch {
    bool inFlight{false};
    quint64 opId{0};
    quint64 attemptId{0};
  };

  void setState(State state, QString diagnostic = {});

  void startProbe();
  void onProbeFinished(quint64 generation, ProbeResult result);

  void startCredentialRestore();
  void handleRestoreReadResult(quint64 generation, const QString &profileId,
                               const TokenStoreResult &result);

  void issueWhoAmI(quint64 generation, const QString &profileId,
                   const QString &token, WhoAmIPurpose purpose);
  void handleWhoAmIResult(quint64 generation, const QString &profileId,
                          const QString &token, WhoAmIPurpose purpose,
                          const AuthResult<CurrentUser> &result);
  void deleteRestoredUnauthorizedToken(quint64 generation,
                                       const QString &profileId);
  void saveFreshlyObtainedToken(quint64 generation, const QString &profileId,
                                const QString &token);

  void handleFreshTokenResult(quint64 generation, const QString &profileId,
                              const AuthResult<AuthToken> &result);

  void handleSignOutDeletionResult(quint64 generation, const QString &profileId,
                                   const TokenStoreResult &result);

  void cancelPendingAuthRequest();

  // Per-profile FIFO token-store operation queue. Enqueuing an operation
  // for a profile with no operation currently running against the real
  // store dispatches it immediately; otherwise it waits behind whatever is
  // already running/queued for that same profile ID. This is what lets an
  // uncancellable operation (e.g. an in-flight save) always complete before
  // a later operation for the same profile (e.g. a sign-out deletion) is
  // even issued to the backing store, so a stale write can never clobber a
  // newer one and vice versa -- without needing to cancel anything.
  //
  // A Delete that fails is deliberately left un-dequeued at the head of
  // the queue (see startFrontTokenOp()): a required durable deletion can
  // never be silently abandoned merely because the coordinator's UI/
  // session generation moved on. This durably blocks every later
  // same-profile operation (read, save, or another delete) until the
  // deletion is retried (see retryStuckProfileTokenOp()) and succeeds --
  // generation only ever suppresses *state* mutation, never this
  // durable-cleanup bookkeeping. See ProfileTokenDispatch above for how
  // concurrent/duplicate dispatch of that same stalled head is prevented.
  // The actionable retry() action and the visible SecureStorageUnavailable
  // state on such a failure are themselves installed centrally, inside
  // startFrontTokenOp()'s own completion handling, gated only on whether
  // |profileId| is still the coordinator's *current* profile -- never on
  // whatever UI generation happened to be current when the failed op was
  // originally enqueued. A per-call onComplete continuation's own
  // generation guard would otherwise permanently lose the retry action if
  // the same op fails again after the profile was switched away from and
  // back (or start() restarted) in between.
  void enqueueTokenOp(const QString &profileId, TokenOpKind kind, QString token,
                      ITokenStore::ResultCallback onComplete);
  void startFrontTokenOp(const QString &profileId);
  void retryStuckProfileTokenOp(const QString &profileId);

  [[nodiscard]] quint64 profileCredentialEpoch(const QString &profileId) const {
    return m_profileCredentialEpoch.value(profileId, 0);
  }

  // Bumps |profileId|'s credential epoch. If a Save for the previous epoch
  // has already been dispatched to ITokenStore (i.e. is the current
  // in-flight head of that profile's FIFO queue), it cannot be un-sent:
  // this reserves a cleanup Delete immediately behind it, so the now-
  // abandoned token is durably removed the moment that save completes --
  // before any later same-profile Save or Read is ever admitted. Called
  // whenever start(), switchProfile(), or signOut() abandons whatever
  // session activity was in progress for the profile being left. A Save
  // that is only queued (not yet dispatched) for the previous epoch is
  // left alone here; it is safely skipped by the epoch check in
  // startFrontTokenOp() once it reaches the front, without ever touching
  // ITokenStore.
  void invalidateProfileCredential(const QString &profileId);

  // Emits currentUserChanged() (if needed, via clearCurrentUser()) and
  // then setState(state, diagnostic), but only if |generation| is still
  // current both before and after the (possibly reentrancy-triggering)
  // currentUserChanged() emission. clearCurrentUser()/setState() are
  // synchronous Qt signal emissions; a directly-connected handler could
  // reentrantly call switchProfile(), start(), signOut(), or destroy the
  // coordinator while either is being delivered. Returns false (having
  // made no further visible change) if superseded at either point.
  bool clearCurrentUserAndSetStateIfCurrent(quint64 generation, State state,
                                            QString diagnostic = {});

  // Same reentrancy-safety contract as clearCurrentUserAndSetStateIfCurrent
  // above, but for the "apply a freshly-known user, then transition to
  // SignedIn" pattern (credential restore's whoami success path).
  bool applyCurrentUserAndSetStateIfCurrent(quint64 generation,
                                            const CurrentUser &user,
                                            State state);

  void applyCurrentUser(const CurrentUser &user);
  void clearCurrentUser();

  IProfileStore &m_profileStore;
  ProbeFactory m_probeFactory;
  ITokenStore &m_tokenStore;
  IAuthenticationClient &m_authClient;

  State m_state{State::Loading};
  QString m_diagnostic;

  QList<ServerProfile> m_profiles;
  QString m_selectedProfileId;
  std::optional<ServerProfile> m_currentProfile;

  std::optional<CurrentUser> m_currentUser;

  // Bumped on every profile switch, start() restart, and signOut();
  // captured by value in every asynchronous completion lambda so a stale
  // completion (from a profile that is no longer current, or from a
  // session that has since been signed out or restarted) can never mutate
  // visible state. Token-store operations still run to completion when
  // stale (see enqueueTokenOp comment); only the *reaction* to their
  // result is generation-gated -- durable cleanup (see
  // invalidateProfileCredential(), profileCredentialEpoch) is not.
  quint64 m_generation{0};

  // Per-profile credential epoch: independent of m_generation (which is a
  // single coordinator-wide UI/session counter). See
  // invalidateProfileCredential() and TokenOp::admissionEpoch.
  QHash<QString, quint64> m_profileCredentialEpoch;

  // Profiles whose FIFO is durably blocked behind a Delete that has failed
  // at least once and not yet been retried successfully (see
  // startFrontTokenOp()'s non-dequeue-on-failure behavior and
  // retryStuckProfileTokenOp()). Maps profileId to a static, secret-free
  // diagnostic describing the block, so a later switch back to that
  // profile (or a fresh credential restore for it) can immediately
  // surface it rather than silently waiting behind it forever.
  QHash<QString, QString> m_profileFifoStalled;

  std::function<void()> m_retryAction;

  std::unique_ptr<ICapabilityProbe> m_probe;
  AuthRequestHandle m_pendingAuthHandle;

  QHash<QString, QQueue<TokenOp>> m_tokenQueues;

  // Monotonically increasing counters used to stamp every TokenOp/dispatch
  // attempt with a unique identity (see TokenOp::opId and
  // ProfileTokenDispatch above). Coordinator-wide rather than per-profile:
  // uniqueness only needs to hold within a single profile's queue, but a
  // single shared counter is simpler and still trivially sufficient.
  quint64 m_nextTokenOpId{1};
  quint64 m_nextTokenAttemptId{1};

  // Per-profile record of the single ITokenStore call (if any) currently
  // outstanding for that profile's queue head. See ProfileTokenDispatch
  // and startFrontTokenOp() for the concurrency guard this provides.
  QHash<QString, ProfileTokenDispatch> m_tokenDispatch;
};

} // namespace Arkham
