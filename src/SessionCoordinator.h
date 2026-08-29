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
#include <QPointer>
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
  // before using it; if persistence fails, the selected profile and any
  // existing session/identity are left unchanged (the failed switch never
  // silently "succeeds"), while the failure is itself reported via a
  // ProfileStorageFailure state/diagnostic transition for retry. On
  // successful persistence,
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
    // gated on THIS epoch (which bumps on every restart/switch, endpoint
    // change or not) -- see invalidateProfileCredential()'s class
    // comment. See admissionEndpointEpoch below for the narrower counter
    // that DOES gate a Read.
    quint64 admissionEpoch{0};
    // Captured from profileEndpointEpoch(profileId) at enqueue time: this
    // counter bumps ONLY when mutateSelectedProfile() detects a genuine
    // change of network endpoint for this profileId() (see
    // invalidateProfileCredentialForEndpointChange()), never on an
    // ordinary restart/switch that keeps the same endpoint. This is the
    // credential-admission identity a queued/in-flight Read must match
    // for startCredentialRestore()'s dedup logic to safely rebind its
    // continuation: a Read admitted for an endpoint that has since
    // changed must never be rebound to a continuation targeting the NEW
    // endpoint (its result could otherwise deliver an old-endpoint token
    // to a new-endpoint whoami call, ahead of the required Delete
    // reserved for that old token) -- see startCredentialRestore().
    quint64 admissionEndpointEpoch{0};
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

  // Per-property-group mutation/notification revision tracking. See the
  // three PropertyRevision members below and publishDirtyProperties()'s
  // own comment for the full model: |mutation| is bumped by the
  // corresponding mutate*() helper ONLY when the externally observable
  // getter value(s) it covers actually change (an unchanged reassignment
  // -- e.g. a reentrant restart finding itself already in (Loading, "")
  // -- creates no new obligation at all); |notified| records the highest
  // |mutation| value whose signal has already been emitted. A property
  // group is "dirty" (still owed a notification) exactly when
  // notified < mutation. Both counters are quint64 and, like
  // m_generation/m_nextTokenOpId/m_nextTokenAttemptId elsewhere in this
  // class, rely on the practical impossibility of a single process
  // instance performing 2^64 mutations of the same property within its
  // lifetime rather than any explicit overflow handling.
  struct PropertyRevision {
    quint64 mutation{0};
    quint64 notified{0};
  };

  // Assigns (state, diagnostic) and bumps m_stateRevision.mutation, but
  // ONLY if the new tuple actually differs from the current one. Never
  // emits by itself -- see publishDirtyProperties().
  void mutateState(State state, QString diagnostic);

  // Assigns the (optional) current-user identity (std::nullopt clears it)
  // and bumps m_currentUserRevision.mutation, but ONLY if it actually
  // differs (compared field-by-field: username/email/beta/admin, or
  // has_value() for a clear). Never emits by itself.
  void mutateCurrentUser(std::optional<CurrentUser> user);

  // Assigns the selected profile ID/ServerProfile and bumps
  // m_selectedProfileRevision.mutation, but ONLY if |profileId| actually
  // differs from the current selection (matching switchProfile()'s own
  // pre-existing "already selected" early-return check). Never emits by
  // itself.
  void mutateSelectedProfile(const QString &profileId,
                             const ServerProfile &profile);

  // The SOLE place stateChanged()/currentUserChanged()/
  // selectedProfileChanged() are ever emitted. Every property group whose
  // |mutation| revision is still newer than its |notified| revision at
  // the moment this call reaches it is announced exactly once, in the
  // fixed order state -> currentUser -> selectedProfile, marking
  // |notified| == |mutation| for that group IMMEDIATELY BEFORE emitting
  // its signal (never after). This ordering is what makes both directions
  // of the review's finding impossible at once:
  //
  //  - A committed mutation is never left unannounced: unlike the
  //    previous (buggy) design, staleness of the caller's own
  //    |generation| is NEVER consulted here -- only actual coordinator
  //    destruction (the QPointer going null, checked after every
  //    individual emission) may skip a signal, since a destroyed QObject
  //    cannot safely emit anything and nothing can observe it either.
  //  - A value is never announced twice for the same settled revision: if
  //    an earlier signal in this same call reentrantly triggers a NESTED
  //    mutation/publication (e.g. a directly-connected stateChanged()
  //    handler calling switchProfile() again), the nested call's own
  //    dirty-check for any group this call already marked |notified| for
  //    sees notified == mutation and correctly skips re-announcing it --
  //    while any group the nested call mutates FURTHER (bumping
  //    |mutation| again after this call already marked |notified|) is
  //    left dirty for this call's own later, not-yet-reached check (which
  //    will then announce the newest value, correctly reflecting the
  //    nested call's outcome rather than this call's now-superseded one,
  //    since notify signals carry no value and getters are always read
  //    live).
  //
  // Returns false the moment destruction is observed, at which point
  // nothing further may be touched; true otherwise. Callers must
  // separately compare their own captured |generation| against
  // m_generation (only once this returns true) to decide whether to
  // proceed with any FURTHER asynchronous side effect (e.g. startProbe())
  // -- never to decide whether notification happened, since by the time
  // this returns, it already has, unconditionally, for every property
  // that was genuinely dirty.
  bool publishDirtyProperties(QPointer<SessionCoordinator> &self);

  // Convenience wrapper for the common single-property-group case: calls
  // mutateState() then publishDirtyProperties() via a fresh, local
  // QPointer to `this`, so the majority of call sites that only ever
  // change (state, diagnostic) don't need to construct their own
  // QPointer or call publishDirtyProperties() by hand. A transition that
  // mutates MORE than one property group together (start(),
  // switchProfile(), clearCurrentUserAndSetStateIfCurrent(),
  // applyCurrentUserAndSetStateIfCurrent()) instead calls the raw
  // mutate*() helpers directly for every field in the same coherent
  // batch, followed by exactly one shared publishDirtyProperties() call,
  // so the very first signal already reflects every field's new value
  // together.
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

  // See TokenOp::admissionEndpointEpoch and m_profileEndpointEpoch below.
  [[nodiscard]] quint64 profileEndpointEpoch(const QString &profileId) const {
    return m_profileEndpointEpoch.value(profileId, 0);
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

  // Handles the SAME-profileId, DIFFERENT-endpoint case detected by
  // mutateSelectedProfile(): first bumps this profile's ENDPOINT epoch
  // (see m_profileEndpointEpoch/TokenOp::admissionEndpointEpoch), which
  // immediately makes any already-queued or already-in-flight Read for
  // this profile ineligible for startCredentialRestore()'s rebind-dedup
  // logic -- it can only ever be rebound to a continuation for the SAME
  // endpoint it was admitted under, never a newer one. Then performs
  // invalidateProfileCredential()'s usual (coarser, restart-on-every-call)
  // epoch bump plus in-flight-Save compensation, and ADDITIONALLY
  // unconditionally reserves a Delete for whatever token may already be
  // durably stored for this profileId() from a prior session at the old
  // endpoint (invalidateProfileCredential() alone only reacts to a Save
  // currently in flight; it never reaches into already-persisted storage).
  // Reserving this Delete here -- before mutateSelectedProfile() returns,
  // and therefore strictly before startCredentialRestore() can ever
  // enqueue a Read for this profile -- guarantees via FIFO ordering alone
  // that no credential from the old endpoint is ever read, sent, or saved
  // against the new one, even if a Read for the old endpoint was already
  // in flight when the endpoint changed: it is left orphaned (its
  // captured continuation is stale-generation by the time it fires) and
  // a FRESH Read for the new endpoint is enqueued strictly behind this
  // Delete instead of ever rebinding the orphaned one.
  void invalidateProfileCredentialForEndpointChange(const QString &profileId);

  // Assigns the coherent (state, cleared-user) snapshot via
  // mutateState()/mutateCurrentUser() (so every field that changes as
  // part of this transition is committed BEFORE either signal is
  // emitted), then publishes whichever of the two groups is actually
  // dirty via publishDirtyProperties() above, and finally reports whether
  // |generation| is still current -- but only once every genuinely dirty
  // property has already been announced, per publishDirtyProperties()'s
  // contract. Returns false immediately (without checking |generation|)
  // if destroyed during publication.
  bool clearCurrentUserAndSetStateIfCurrent(quint64 generation, State state,
                                            QString diagnostic = {});

  // Same reentrancy-safety contract as clearCurrentUserAndSetStateIfCurrent
  // above, but for the "apply a freshly-known user, then transition to
  // SignedIn" pattern (credential restore's whoami success path): the new
  // user and the new state are both committed via mutateCurrentUser()/
  // mutateState() before either signal is published together via
  // publishDirtyProperties().
  bool applyCurrentUserAndSetStateIfCurrent(quint64 generation,
                                            const CurrentUser &user,
                                            State state);

  void applyCurrentUser(const CurrentUser &user);

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

  // Per-property-group mutation/notification revisions -- see
  // PropertyRevision's own declaration and publishDirtyProperties()'s
  // comment above for the full model.
  PropertyRevision m_stateRevision;
  PropertyRevision m_currentUserRevision;
  PropertyRevision m_selectedProfileRevision;

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

  // Per-profile ENDPOINT epoch: a strictly narrower counter than
  // m_profileCredentialEpoch above -- it bumps ONLY when
  // invalidateProfileCredentialForEndpointChange() detects that the same
  // profileId() now designates a genuinely different network endpoint
  // (never on an ordinary restart/switch that keeps the same endpoint,
  // unlike m_profileCredentialEpoch, which bumps on every one of those
  // too). This is what lets startCredentialRestore()'s dedup logic tell
  // "a Read admitted before THIS endpoint change" apart from "a Read
  // admitted for the CURRENT endpoint, just during an earlier ordinary
  // restart" -- see TokenOp::admissionEndpointEpoch.
  QHash<QString, quint64> m_profileEndpointEpoch;

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
