#include "SessionCoordinator.h"

#include "TokenValidation.h"

#include <QPointer>
#include <QSet>

#include <algorithm>
#include <utility>

namespace Arkham {

SessionCoordinator::SessionCoordinator(IProfileStore &profileStore,
                                       ProbeFactory probeFactory,
                                       ITokenStore &tokenStore,
                                       IAuthenticationClient &authClient,
                                       QObject *parent)
    : QObject(parent), m_profileStore(profileStore),
      m_probeFactory(std::move(probeFactory)), m_tokenStore(tokenStore),
      m_authClient(authClient) {}

SessionCoordinator::~SessionCoordinator() {
  cancelPendingAuthRequest();
  // Destroying the probe suppresses its pending finished() signal (see
  // ICapabilityProbe's contract); this must happen before any member this
  // destructor doesn't otherwise touch is torn down by the derived class.
  m_probe.reset();
  // Any token-store operation already dispatched to the backing store is
  // uncancellable and keeps running, but every stored continuation is
  // guarded by a QPointer to this, so it can never touch a destroyed
  // coordinator. Bumping the generation here is purely for readability/
  // defense-in-depth; the QPointer check alone is already sufficient.
  ++m_generation;
}

SessionCoordinator::State SessionCoordinator::state() const noexcept {
  return hasBlockingOrphanCleanup() ? State::SecureStorageUnavailable : m_state;
}

QString SessionCoordinator::diagnostic() const {
  if (hasBlockingOrphanCleanup()) {
    return m_profileFifoStalled.value(m_stalledProfileOrder.first());
  }
  return m_diagnostic;
}

// stateDescription() is a static label FOR state() (see its own Q_PROPERTY
// NOTIFY, the same stateChanged() as state() itself), so it must switch on
// the same possibly-overridden value state() returns -- never the raw
// m_state -- or it could report e.g. "Signed in" while state() itself
// simultaneously reports SecureStorageUnavailable for an orphaned
// profile's unresolved cleanup.
QString SessionCoordinator::stateDescription() const {
  switch (state()) {
  case State::Loading:
    return QStringLiteral("Loading");
  case State::ProbingCapabilities:
    return QStringLiteral("Checking server");
  case State::RestoringCredential:
    return QStringLiteral("Restoring session");
  case State::SignedOut:
    return QStringLiteral("Signed out");
  case State::Authenticating:
    return QStringLiteral("Signing in");
  case State::Registering:
    return QStringLiteral("Creating account");
  case State::SignedIn:
    return QStringLiteral("Signed in");
  case State::SigningOut:
    return QStringLiteral("Signing out");
  case State::Incompatible:
    return QStringLiteral("Server incompatible");
  case State::SecureStorageUnavailable:
    return QStringLiteral("Secure storage unavailable");
  case State::ProfileStorageFailure:
    return QStringLiteral("Profile storage error");
  case State::RecoverableFailure:
    return QStringLiteral("Temporary problem");
  }
  return QStringLiteral("Unknown");
}

QString SessionCoordinator::selectedProfileDisplayName() const {
  return m_currentProfile ? m_currentProfile->displayName() : QString();
}

QString SessionCoordinator::selectedProfileBaseUrl() const {
  return m_currentProfile ? m_currentProfile->baseUrl().toString() : QString();
}

QString SessionCoordinator::currentUsername() const {
  return m_currentUser ? m_currentUser->username : QString();
}

QString SessionCoordinator::currentEmail() const {
  return m_currentUser ? m_currentUser->email : QString();
}

bool SessionCoordinator::currentUserBeta() const {
  return m_currentUser && m_currentUser->beta;
}

bool SessionCoordinator::currentUserAdmin() const {
  return m_currentUser && m_currentUser->admin;
}

void SessionCoordinator::mutateState(State state, QString diagnostic) {
  if (m_state == state && m_diagnostic == diagnostic) {
    // Reassigning the identical raw (state, diagnostic) tuple can never
    // change what is externally observable either --
    // hasBlockingOrphanCleanup()'s masking decision depends only on
    // m_stalledProfileOrder/ m_currentProfile, never on m_state/m_diagnostic
    // themselves -- so this is not a real transition and must never create a
    // fresh notification obligation (this is what stops an unguarded
    // stateChanged handler that unconditionally calls start() from
    // recursing forever).
    return;
  }
  const State oldExposedState = this->state();
  const QString oldExposedDiagnostic = this->diagnostic();
  m_state = state;
  m_diagnostic = std::move(diagnostic);
  // A raw transition that happens while an orphan-cleanup obligation is
  // currently masking state()/diagnostic() (see hasBlockingOrphanCleanup())
  // produces the exact same externally observable tuple before and after
  // -- e.g. a reselected/re-added profile's own probe legitimately moving
  // its real internal state from Loading to Incompatible/
  // RecoverableFailure while its (or some other profile's) required
  // deletion is still unresolved. Such a transition is real and necessary
  // internally -- the profile's own flow must keep progressing so it is
  // ready to be shown the instant the mask lifts -- but it must never
  // itself be announced, and must never appear to have "published over"
  // the still-owed cleanup failure. Only bump the revision -- and
  // therefore only ever emit stateChanged() -- when the EXPOSED tuple
  // genuinely changed as a result of this specific assignment.
  if (this->state() != oldExposedState ||
      this->diagnostic() != oldExposedDiagnostic) {
    ++m_stateRevision.mutation;
  }
}

void SessionCoordinator::mutateCurrentUser(std::optional<CurrentUser> user) {
  const bool unchanged =
      m_currentUser.has_value() == user.has_value() &&
      (!user.has_value() || (m_currentUser->username == user->username &&
                             m_currentUser->email == user->email &&
                             m_currentUser->beta == user->beta &&
                             m_currentUser->admin == user->admin));
  if (unchanged) {
    return;
  }
  m_currentUser = std::move(user);
  ++m_currentUserRevision.mutation;
}

void SessionCoordinator::mutateSelectedProfile(const QString &profileId,
                                               const ServerProfile &profile) {
  if (profileId == m_selectedProfileId && m_currentProfile.has_value()) {
    // The SAME profile ID may still have been reloaded with genuinely
    // different content: start() unconditionally reloads every profile
    // record from storage on every restart (see start()'s call to
    // mutateSelectedProfile() with whatever the profile store currently
    // returns for the persisted selection), and stable IDs are explicitly
    // supported via ServerProfile::customWithId(), so the same UUID can
    // legitimately come back with an updated displayName and/or baseUrl.
    // switchProfile()'s own top-of-function early return already excludes
    // the "user re-selects the still-identical current profile" case
    // before ever reaching here, so a same-ID call that DOES reach this
    // point with different content only ever comes from a reload, never
    // from switchProfile() re-targeting itself.
    //
    // Credential-endpoint invalidation for THIS id (and every other
    // retained/removed id) is handled exclusively, and unconditionally of
    // selection, by reconcileAllProfileCredentialsOnReload(), called by
    // start() BEFORE m_profiles is replaced and therefore strictly before
    // this function ever runs (see its own doc comment for why the two
    // must never both invalidate the same id). This function is left
    // concerned only with observable-equality bookkeeping for QML
    // notification purposes.
    const ServerProfile &previous = *m_currentProfile;
    const bool observableUnchanged =
        previous.displayName() == profile.displayName() &&
        previous.baseUrl() == profile.baseUrl();
    if (observableUnchanged) {
      return;
    }
    m_currentProfile = profile;
    ++m_selectedProfileRevision.mutation;
    return;
  }
  if (profileId == m_selectedProfileId) {
    // No previous profile existed to compare against (e.g. the very first
    // selection); matches switchProfile()'s pre-existing convention.
    return;
  }
  m_selectedProfileId = profileId;
  m_currentProfile = profile;
  ++m_selectedProfileRevision.mutation;
}

void SessionCoordinator::setState(State state, QString diagnostic) {
  mutateState(state, std::move(diagnostic));
  QPointer<SessionCoordinator> self(this);
  publishDirtyProperties(self);
}

void SessionCoordinator::applyCurrentUser(const CurrentUser &user) {
  mutateCurrentUser(user);
  QPointer<SessionCoordinator> self(this);
  publishDirtyProperties(self);
}

// ─── Boot / profile loading ─────────────────────────────────────────────

void SessionCoordinator::start() {
  // Defensive guard against re-entrant/duplicate invocation (see the
  // start() doc comment in SessionCoordinator.h): cancelling any pending
  // auth request, discarding the current probe, and bumping the generation
  // makes every in-flight async completion from a prior start() stale, so
  // it can never mutate state after this restart. If a fresh-token save
  // for the profile being abandoned has already crossed the ITokenStore
  // boundary, invalidateProfileCredential() reserves a compensating
  // cleanup delete for it before any later same-profile auth/save can run.
  cancelPendingAuthRequest();
  m_probe.reset();
  if (m_currentProfile.has_value()) {
    invalidateProfileCredential(m_currentProfile->profileId());
  }
  ++m_generation;
  const quint64 generation = m_generation;
  m_retryAction = nullptr;

  // Commit the complete new (Loading, cleared-identity) snapshot together
  // BEFORE either notification is published below (see
  // publishDirtyProperties()'s contract): a directly-connected handler
  // reentrantly calling switchProfile()/start()/signOut() from either
  // emission must see state()==Loading and currentUser()==nil already
  // coherent together, never a stale SignedIn alongside an already-
  // cleared identity (which previously let a reentrant signOut() during
  // this exact window proceed as if the session were still fully signed
  // in). Reassigning identical values here (e.g. a reentrant restart
  // finding itself already at (Loading, nil)) is a no-op under
  // mutate*(): no duplicate notification is created.
  mutateCurrentUser(std::nullopt);
  mutateState(State::Loading, {});

  QPointer<SessionCoordinator> self(this);
  if (!publishDirtyProperties(self)) {
    return; // destroyed while a still-owed signal was being delivered
  }
  if (generation != m_generation) {
    return; // superseded by a nested transition; side effects only
  }

  const auto profilesResult = m_profileStore.loadProfiles();
  if (!profilesResult) {
    m_retryAction = [this] { start(); };
    setState(State::ProfileStorageFailure, profilesResult.error());
    return;
  }

  const auto selectedIdResult = m_profileStore.loadSelectedProfileId();
  if (!selectedIdResult) {
    m_retryAction = [this] { start(); };
    setState(State::ProfileStorageFailure, selectedIdResult.error());
    return;
  }

  QList<ServerProfile> profiles = *profilesResult;
  QString selectedId = *selectedIdResult;

  if (profiles.isEmpty()) {
    // Genuinely empty first run: deterministically seed the hosted default
    // and persist both the profile list and its selection. Any failure to
    // persist is an explicit ProfileStorageFailure, never a silent
    // in-memory-only "first run success".
    const ServerProfile hosted = ServerProfile::hostedDefault();
    const auto saveProfilesResult = m_profileStore.saveProfiles({hosted});
    if (!saveProfilesResult) {
      m_retryAction = [this] { start(); };
      setState(State::ProfileStorageFailure, saveProfilesResult.error());
      return;
    }
    profiles = {hosted};
    selectedId.clear();
  }

  if (selectedId.isEmpty()) {
    // Either nothing was ever selected, or the branch above just seeded the
    // list. Deterministically select the first profile and persist that
    // selection rather than defaulting silently in memory only.
    selectedId = profiles.first().profileId();
    const auto saveSelectionResult =
        m_profileStore.saveSelectedProfileId(selectedId);
    if (!saveSelectionResult) {
      m_retryAction = [this] { start(); };
      setState(State::ProfileStorageFailure, saveSelectionResult.error());
      return;
    }
  }

  const auto it = std::find_if(
      profiles.cbegin(), profiles.cend(),
      [&](const ServerProfile &p) { return p.profileId() == selectedId; });
  if (it == profiles.cend()) {
    // A persisted selection that matches no loaded profile is explicit,
    // repairable corruption -- never an arbitrary silent fallback to some
    // other profile.
    m_retryAction = [this] { start(); };
    setState(
        State::ProfileStorageFailure,
        QStringLiteral("The selected server profile is missing from storage."));
    return;
  }

  // Copy the matched profile out before moving |profiles| into m_profiles:
  // std::move()-ing the container invalidates iterators into it, so |it|
  // must never be dereferenced after the move below.
  const ServerProfile selectedProfile = *it;
  // Reconcile EVERY retained/removed profile's credential against this
  // reload BEFORE m_profiles is overwritten below, while the OLD records
  // are still available for comparison via the current value of
  // m_profiles itself -- and strictly before mutateSelectedProfile() just
  // below can ever run, so a profile that is ABOUT TO become newly
  // selected in this very reload is reconciled here too, not skipped (see
  // reconcileAllProfileCredentialsOnReload()'s own doc comment). This
  // ordering is what guarantees a required Delete for such a profile is
  // always reserved ahead of any later restore Read for it, regardless of
  // whether it was previously selected, is newly selected here, or is
  // never selected at all in this process run.
  reconcileAllProfileCredentialsOnReload(m_profiles, profiles);
  m_profiles = std::move(profiles);
  mutateSelectedProfile(selectedId, selectedProfile);
  if (!publishDirtyProperties(self)) {
    return; // destroyed while a still-owed signal was being delivered
  }
  if (generation != m_generation) {
    return; // superseded while selectedProfileChanged() was being delivered
  }

  startProbe();
}

void SessionCoordinator::switchProfile(const QString &profileId) {
  if (profileId == m_selectedProfileId) {
    return;
  }
  const auto it = std::find_if(
      m_profiles.cbegin(), m_profiles.cend(),
      [&](const ServerProfile &p) { return p.profileId() == profileId; });
  if (it == m_profiles.cend()) {
    m_retryAction = nullptr;
    setState(State::ProfileStorageFailure,
             QStringLiteral("Unknown server profile."));
    return;
  }

  // Persist the new selection BEFORE using it. A failure here must never
  // pretend the switch succeeded: the selected profile and any existing
  // session/identity remain unchanged, while the failure is reported via
  // an observable ProfileStorageFailure state/diagnostic transition below.
  const auto saveResult = m_profileStore.saveSelectedProfileId(profileId);
  if (!saveResult) {
    m_retryAction = [this, profileId] { switchProfile(profileId); };
    setState(State::ProfileStorageFailure, saveResult.error());
    return;
  }

  cancelPendingAuthRequest();
  // Destroying the probe suppresses any pending finished() signal from the
  // profile being switched away from. If a fresh-token save for that
  // profile has already crossed the ITokenStore boundary,
  // invalidateProfileCredential() reserves a compensating cleanup delete
  // for it before any later same-profile auth/save can run.
  m_probe.reset();
  if (m_currentProfile.has_value()) {
    invalidateProfileCredential(m_currentProfile->profileId());
  }
  ++m_generation;
  const quint64 generation = m_generation;
  m_retryAction = nullptr;

  // Assign the ENTIRE new observable snapshot -- transitional (non-
  // SignedIn) state, cleared identity, AND the new selected profile --
  // together, BEFORE any notification is emitted below. A previous
  // version of this function assigned these in three separate steps
  // (clear identity -> emit; set state -> emit; reassign profile ->
  // emit), which meant a directly-connected handler reentrantly observing
  // an EARLY emission (e.g. currentUserChanged()) could still see the OLD
  // profile alongside the already-cleared identity/still-SignedIn state
  // -- and if that handler called signOut(), it could bump the generation
  // and abort the rest of this function AFTER the new selection had
  // already been persisted to storage above, leaving the persisted
  // selection (the new profile) permanently split from the in-memory
  // selection (reverted to the old profile). With every field already
  // assigned here, EVERY notification below -- stateChanged(),
  // currentUserChanged(), and selectedProfileChanged() -- carries the
  // complete new snapshot from the very first one: a reentrant observer
  // of any of them sees state()==Loading, currentUser()==nil, and
  // selectedProfileId() already the NEW profile, all together. signOut()
  // (which requires state()==SignedIn) can therefore never fire
  // reentrantly from within this transition at all, so it can never
  // observe -- or act on -- a hybrid of old and new identity. All three
  // mutate*() calls below happen before the single publishDirtyProperties()
  // call that follows, so every property that is genuinely dirty (state
  // and currentUser always; selectedProfile always, since this function's
  // own top-of-function early return already excludes the no-op
  // re-selection case) is announced together from the very first signal.
  mutateCurrentUser(std::nullopt);
  mutateState(State::Loading, {});
  mutateSelectedProfile(profileId, *it);

  QPointer<SessionCoordinator> self(this);
  if (!publishDirtyProperties(self)) {
    return; // destroyed while a still-owed signal was being delivered
  }
  if (generation != m_generation) {
    return; // superseded by a nested transition; side effects only
  }

  startProbe();
}

// ─── Capability probing ─────────────────────────────────────────────────

void SessionCoordinator::startProbe() {
  m_probe.reset();
  m_probe = m_probeFactory ? m_probeFactory() : nullptr;
  if (!m_probe) {
    // A misconfigured/empty ProbeFactory must never crash the coordinator:
    // surface it as an explicit, retryable failure instead of dereferencing
    // a null pointer below.
    m_retryAction = [this] { startProbe(); };
    setState(State::RecoverableFailure,
             QStringLiteral(
                 "The capability-probe factory did not produce a probe."));
    return;
  }
  const quint64 generation = m_generation;
  connect(m_probe.get(), &ICapabilityProbe::finished, this,
          [this, generation](ProbeResult result) {
            onProbeFinished(generation, std::move(result));
          });
  QPointer<SessionCoordinator> self(this);
  setState(State::ProbingCapabilities);
  if (!self || generation != self->m_generation || !self->m_probe) {
    // A directly-connected stateChanged() handler reentrantly called
    // switchProfile()/start()/signOut() (which reset m_probe and bumped
    // the generation) or destroyed the coordinator while this emission
    // was being delivered: m_probe may already be null or a different
    // instance entirely, so it must never be dereferenced below.
    return;
  }
  m_probe->probe(*m_currentProfile);
}

void SessionCoordinator::onProbeFinished(quint64 generation,
                                         ProbeResult result) {
  if (generation != m_generation) {
    return; // superseded by a profile switch or destruction
  }
  switch (result.outcome) {
  case ProbeOutcome::Compatible:
  case ProbeOutcome::LegacyFallback:
    startCredentialRestore();
    return;
  case ProbeOutcome::Incompatible:
    m_retryAction = [this] { startProbe(); };
    setState(State::Incompatible, result.diagnostic);
    return;
  case ProbeOutcome::NetworkError:
  case ProbeOutcome::MalformedJson:
  case ProbeOutcome::HttpError:
  case ProbeOutcome::InvalidProfile:
    m_retryAction = [this] { startProbe(); };
    setState(State::RecoverableFailure, result.diagnostic);
    return;
  }
}

// ─── Credential restore ─────────────────────────────────────────────────

void SessionCoordinator::startCredentialRestore() {
  // Capture generation/profileId BEFORE emitting: setState() below is a
  // synchronous stateChanged() emission, and a directly-connected handler
  // could reentrantly call switchProfile()/start()/signOut() (which would
  // change m_currentProfile/m_generation) while it is being delivered.
  // Re-deriving these from members only *after* the emission would enqueue
  // a read for the wrong profile/generation -- a credential read must
  // never be issued for a profile whose own capability probe has not
  // itself just completed.
  const quint64 generation = m_generation;
  const QString profileId = m_currentProfile->profileId();
  const QString expectedEndpointIdentity =
      m_currentProfile->credentialEndpointIdentity();
  const bool stalled = m_profileFifoStalled.contains(profileId);
  QPointer<SessionCoordinator> self(this);
  setState(stalled ? State::SecureStorageUnavailable
                   : State::RestoringCredential,
           stalled ? m_profileFifoStalled.value(profileId) : QString());
  if (!self || generation != self->m_generation) {
    return; // superseded while stateChanged() was being delivered
  }
  ITokenStore::ResultCallback onComplete =
      [self, generation, profileId](TokenStoreResult result) {
        if (!self) {
          return;
        }
        if (generation != self->m_generation) {
          return;
        }
        self->handleRestoreReadResult(generation, profileId, result);
      };
  if (stalled) {
    // A previously-failed required deletion for this profile is still
    // durably blocking its FIFO (see startFrontTokenOp()); retry() now
    // always resolves the oldest outstanding deletion obligation across
    // every profile before falling back to m_retryAction (see retry()'s
    // own doc comment and m_stalledProfileOrder), so this profile's
    // obligation -- already recorded there the moment it first failed --
    // remains fully actionable via retry() without needing its own
    // dedicated m_retryAction assignment here.
  }

  // Enforce at most ONE logical restore Read queued or in flight for this
  // profile at any time, in EVERY FIFO state -- not merely while stalled
  // behind a failed Delete. Repeated start()/switchProfile() calls back
  // to the same profile while its queue head is still an in-flight Read,
  // or a Save/Delete that has not (yet, or ever) failed, would otherwise
  // each enqueue ANOTHER Read behind whatever is running, causing
  // unbounded queue growth and duplicate secure-store I/O once the head
  // eventually completes. At most one Read admitted for the CURRENT
  // endpoint epoch can ever be present; search for it (it may be the
  // in-flight head itself, or queued behind a still-pending Save/Delete
  // chain) and rebind its continuation to this call's (current)
  // generation instead of enqueuing a second one.
  //
  // Critically, a Read is only EVER eligible for this rebind if its
  // captured admissionEndpointEpoch (see TokenOp's own doc comment) still
  // matches profileEndpointEpoch(profileId) right now. A Read admitted
  // BEFORE mutateSelectedProfile() detected an endpoint change for this
  // profileId() (see invalidateProfileCredentialForEndpointChange(),
  // which bumps this narrower epoch) was targeting the OLD endpoint --
  // rebinding it to a continuation for the current (possibly NEW)
  // endpoint would let whatever token it eventually reads (which may
  // still be the old-endpoint token, since the required compensating
  // Delete reserved for it is only guaranteed to run BEFORE a fresh Read,
  // never before this stale one) be delivered to a whoami call against
  // the new endpoint, entirely bypassing that required Delete. Such a
  // stale Read is deliberately left un-rebound here: it remains in the
  // FIFO with its ORIGINAL (now permanently stale-generation) onComplete,
  // which will discard its own result the moment it fires (see the
  // generation check inside that closure, captured above), and a FRESH
  // Read -- admitted under the CURRENT endpoint epoch -- is enqueued
  // below, strictly behind whatever required Delete(s)
  // invalidateProfileCredentialForEndpointChange() already reserved,
  // guaranteeing the deterministic old-Read -> required-Delete -> fresh-
  // Read ordering the endpoint-change invariant depends on.
  //
  // Rebinding only ever mutates TokenOp::onComplete -- never opId or
  // attemptId -- so it cannot affect startFrontTokenOp()'s dispatch
  // guard or a completion's own attempt matching. This is also safe to do
  // while the Read is ALREADY dispatched to the real store: the
  // in-flight completion closure (see startFrontTokenOp()) captures only
  // (profileId, opId, attemptId), and looks up the queue's CURRENT head
  // op's onComplete only at the moment it actually completes -- never a
  // continuation captured by value back when the op was first dispatched
  // -- so a rebind applied here after dispatch, but before completion,
  // still takes effect. The previously-installed continuation's own
  // generation is necessarily stale by now -- this function is only
  // reached again via a fresh start()/switchProfile() that re-derives
  // |generation| from the (already possibly bumped) m_generation -- so it
  // would have been a silent no-op once dispatched anyway.
  const auto queueIt = m_tokenQueues.find(profileId);
  if (queueIt != m_tokenQueues.end()) {
    const quint64 currentEndpointEpoch = profileEndpointEpoch(profileId);
    for (int i = 0; i < queueIt->size(); ++i) {
      if ((*queueIt)[i].kind == TokenOpKind::Read &&
          (*queueIt)[i].admissionEndpointEpoch == currentEndpointEpoch) {
        (*queueIt)[i].onComplete = std::move(onComplete);
        return;
      }
    }
  }
  enqueueTokenOp(profileId, TokenOpKind::Read, QString(),
                 expectedEndpointIdentity, std::move(onComplete));
}

void SessionCoordinator::handleRestoreReadResult(
    quint64 generation, const QString &profileId,
    const TokenStoreResult &result) {
  switch (result.outcome) {
  case TokenStoreOutcome::NotFound:
    clearCurrentUserAndSetStateIfCurrent(generation, State::SignedOut);
    return;
  case TokenStoreOutcome::Success:
    issueWhoAmI(generation, profileId, result.token,
                WhoAmIPurpose::RestoreExisting);
    return;
  case TokenStoreOutcome::BindingMismatch:
  case TokenStoreOutcome::LegacyUnbound:
  case TokenStoreOutcome::Malformed:
    // The entry that was read cannot be trusted for this profile's
    // CURRENT endpoint: either it is durably bound to a different one
    // (BindingMismatch, e.g. this profileId() was reused or its
    // persisted URL changed since the credential was saved), it predates
    // endpoint binding entirely (LegacyUnbound), or it failed strict
    // structural parsing (Malformed). In every case its origin cannot be
    // proven, so it is never used, never auto-migrated, and never
    // exposed as a token -- it is durably deleted and this profile is
    // settled to SignedOut on success, exactly like an unauthorized
    // restored token (see deleteUntrustedRestoredToken()).
    deleteUntrustedRestoredToken(generation, profileId);
    return;
  case TokenStoreOutcome::AccessDenied:
  case TokenStoreOutcome::Unavailable:
  case TokenStoreOutcome::BackendError:
  case TokenStoreOutcome::InvalidInput:
    m_retryAction = [this] { startCredentialRestore(); };
    setState(State::SecureStorageUnavailable, result.diagnostic);
    return;
  }
}

// ─── whoami validation (shared by restore and fresh sign-in/register) ───

void SessionCoordinator::issueWhoAmI(quint64 generation,
                                     const QString &profileId,
                                     const QString &token,
                                     WhoAmIPurpose purpose) {
  cancelPendingAuthRequest();
  QPointer<SessionCoordinator> self(this);
  m_pendingAuthHandle = m_authClient.whoAmI(
      *m_currentProfile, token,
      [self, generation, profileId, token,
       purpose](AuthResult<CurrentUser> result) {
        if (!self) {
          return;
        }
        // Check generation BEFORE clearing m_pendingAuthHandle: a stale
        // completion (e.g. the Cancelled result delivered after
        // cancelPendingAuthRequest()) must never clobber the handle of a
        // newer, still-in-flight request that a later signIn()/
        // registerAccount()/credential-restore call may have already
        // installed under the bumped generation.
        if (generation != self->m_generation) {
          return;
        }
        self->m_pendingAuthHandle = AuthRequestHandle{};
        self->handleWhoAmIResult(generation, profileId, token, purpose, result);
      });
}

void SessionCoordinator::handleWhoAmIResult(
    quint64 generation, const QString &profileId, const QString &token,
    WhoAmIPurpose purpose, const AuthResult<CurrentUser> &result) {
  if (result.outcome == AuthOutcome::Success) {
    if (purpose == WhoAmIPurpose::RestoreExisting) {
      // Already durably stored; nothing more to persist.
      applyCurrentUserAndSetStateIfCurrent(generation, *result.value,
                                           State::SignedIn);
      return;
    }
    // Freshly obtained token: persist it before declaring signed in. The
    // admission check happens immediately before enqueuing the write.
    // applyCurrentUser()'s currentUserChanged() emission is synchronous
    // and could reentrantly trigger switchProfile()/start()/signOut(), so
    // |generation| is re-checked afterward before ever touching the
    // secure store for a session that may no longer be current.
    QPointer<SessionCoordinator> self(this);
    applyCurrentUser(*result.value);
    if (!self || generation != self->m_generation) {
      return;
    }
    saveFreshlyObtainedToken(generation, profileId, token);
    return;
  }

  if (result.outcome == AuthOutcome::Unauthorized) {
    if (purpose == WhoAmIPurpose::RestoreExisting) {
      // A restored token the server no longer accepts must be durably
      // deleted before the coordinator is allowed to claim signed out.
      deleteUntrustedRestoredToken(generation, profileId);
    } else {
      // Nothing was ever saved for a freshly rejected token; just report
      // signed out. This path is shared by signIn() and registerAccount(),
      // so the diagnostic must not claim it was specifically a sign-in.
      clearCurrentUserAndSetStateIfCurrent(
          generation, State::SignedOut,
          QStringLiteral("The server rejected the freshly issued token."));
    }
    return;
  }

  // Transport / NonHttpResponse / MalformedPayload / UnexpectedStatus /
  // Cancelled / InvalidInput.
  if (purpose == WhoAmIPurpose::RestoreExisting) {
    // A potentially valid token must never be deleted just because we
    // could not currently reach the server to validate it.
    m_retryAction = [this, generation, profileId, token] {
      if (generation != m_generation) {
        return;
      }
      issueWhoAmI(generation, profileId, token, WhoAmIPurpose::RestoreExisting);
    };
    setState(State::RecoverableFailure, result.diagnostic);
  } else {
    clearCurrentUserAndSetStateIfCurrent(generation, State::SignedOut,
                                         result.diagnostic);
  }
}

// A freshly obtained (validated by /whoami) token must be durably saved
// before the coordinator declares itself signed in. Defined as a reusable,
// generation-guarded step (rather than an inline lambda) so a save
// failure can install a retry action that re-invokes exactly this same
// step; without that, retry() would be a silent no-op while stuck in the
// resulting SecureStorageUnavailable state, even though a validated token
// is already available and only the durable write needs to be retried.
void SessionCoordinator::saveFreshlyObtainedToken(quint64 generation,
                                                  const QString &profileId,
                                                  const QString &token) {
  if (generation != m_generation) {
    return;
  }
  // Safe to compute here (rather than threading a new parameter through
  // issueWhoAmI()/handleWhoAmIResult() above): |generation| still matching
  // m_generation at this point implies m_currentProfile is the SAME
  // profile object that was active throughout this entire sign-in/
  // register call chain, since only start()/switchProfile() ever change
  // m_currentProfile, and both unconditionally bump m_generation when
  // they do.
  const QString endpointIdentity =
      m_currentProfile->credentialEndpointIdentity();
  QPointer<SessionCoordinator> self(this);
  enqueueTokenOp(
      profileId, TokenOpKind::Save, token, endpointIdentity,
      [self, generation, profileId, token](TokenStoreResult saveResult) {
        if (!self) {
          return;
        }
        if (generation != self->m_generation) {
          return;
        }
        if (saveResult.outcome == TokenStoreOutcome::Success) {
          self->setState(State::SignedIn);
        } else {
          self->m_retryAction = [self, generation, profileId, token] {
            if (!self) {
              return;
            }
            self->saveFreshlyObtainedToken(generation, profileId, token);
          };
          self->setState(State::SecureStorageUnavailable,
                         saveResult.diagnostic);
        }
      });
}

// A restored/freshly-authenticated token that cannot be trusted -- rejected
// by the server (AuthOutcome::Unauthorized) or found by ITokenStore to be
// bound to a different endpoint, pre-envelope legacy, or structurally
// malformed (TokenStoreOutcome::BindingMismatch / LegacyUnbound /
// Malformed) -- must be durably deleted before the
// coordinator is allowed to claim signed out. Defined as a reusable,
// generation-guarded step (rather than an inline lambda) so a deletion
// failure can install a retry action that re-invokes exactly this same
// step; without that, retry() would be a silent no-op while stuck in the
// resulting SecureStorageUnavailable state.
void SessionCoordinator::deleteUntrustedRestoredToken(
    quint64 generation, const QString &profileId) {
  if (generation != m_generation) {
    return;
  }
  QPointer<SessionCoordinator> self(this);
  enqueueTokenOp(profileId, TokenOpKind::Delete, QString(), QString(),
                 [self, generation, profileId](TokenStoreResult deleteResult) {
                   if (!self) {
                     return;
                   }
                   // NotFound (the token was already absent) achieves the
                   // exact same postcondition as Success here -- no token
                   // remains under this profileId() -- so both are
                   // treated identically; see the shared deleteFailed
                   // check in startFrontTokenOp() for why NotFound must
                   // never itself be mistaken for a failure.
                   if (deleteResult.outcome == TokenStoreOutcome::Success ||
                       deleteResult.outcome == TokenStoreOutcome::NotFound) {
                     self->clearCurrentUserAndSetStateIfCurrent(
                         generation, State::SignedOut);
                   }
                   // On any other failure, nothing further to do here: the
                   // central FIFO dispatch (see startFrontTokenOp()) has
                   // already left the delete stalled and, if this profile
                   // is still the coordinator's current one, has already
                   // reinstalled an actionable retry() and surfaced
                   // SecureStorageUnavailable -- unconditionally,
                   // regardless of whether |generation| (the session that
                   // originally triggered this cleanup) is still current.
                 });
}

// ─── Sign in / register ─────────────────────────────────────────────────

void SessionCoordinator::signIn(const QString &email, const QString &password) {
  // Gated on state == SignedOut: signIn() must never run concurrently
  // with an in-flight credential restore (state == RestoringCredential),
  // since issueWhoAmI() for the restored token would cancel this
  // interactive request via cancelPendingAuthRequest() and the two flows
  // would race to set the final state.
  if (m_state != State::SignedOut || m_pendingAuthHandle.id != 0 ||
      !m_currentProfile.has_value()) {
    return;
  }
  const quint64 generation = m_generation;
  const QString profileId = m_currentProfile->profileId();
  const ServerProfile profile = *m_currentProfile;
  QPointer<SessionCoordinator> self(this);
  setState(State::Authenticating);
  if (!self || generation != self->m_generation) {
    // A directly-connected stateChanged() handler reentrantly switched
    // profile, restarted, or signed out while this emission was being
    // delivered: the captured |profile|/|profileId| are for a session
    // that is no longer current. Issuing the request below would send a
    // password request for the wrong (abandoned) profile, so this must
    // produce zero network calls instead.
    return;
  }
  m_pendingAuthHandle = m_authClient.authenticate(
      profile, AuthenticateRequest{email, password},
      [self, generation, profileId](AuthResult<AuthToken> result) {
        if (!self) {
          return;
        }
        // See issueWhoAmI()'s callback: check generation before clearing
        // m_pendingAuthHandle so a stale (e.g. cancelled) completion never
        // clobbers a newer in-flight request's handle.
        if (generation != self->m_generation) {
          return;
        }
        self->m_pendingAuthHandle = AuthRequestHandle{};
        self->handleFreshTokenResult(generation, profileId, result);
      });
  // password/email are never retained in a member: they are only used to
  // build the immediate request above.
}

void SessionCoordinator::registerAccount(const QString &email,
                                         const QString &username,
                                         const QString &password) {
  // Same reentrancy guard as signIn(): must not run during
  // RestoringCredential (or any other non-SignedOut state).
  if (m_state != State::SignedOut || m_pendingAuthHandle.id != 0 ||
      !m_currentProfile.has_value()) {
    return;
  }
  const quint64 generation = m_generation;
  const QString profileId = m_currentProfile->profileId();
  const ServerProfile profile = *m_currentProfile;
  QPointer<SessionCoordinator> self(this);
  setState(State::Registering);
  if (!self || generation != self->m_generation) {
    // See signIn()'s identical guard: never send a stale-profile request.
    return;
  }
  m_pendingAuthHandle = m_authClient.registerAccount(
      profile, RegisterRequest{email, username, password},
      [self, generation, profileId](AuthResult<AuthToken> result) {
        if (!self) {
          return;
        }
        // See issueWhoAmI()'s callback: check generation before clearing
        // m_pendingAuthHandle so a stale (e.g. cancelled) completion never
        // clobbers a newer in-flight request's handle.
        if (generation != self->m_generation) {
          return;
        }
        self->m_pendingAuthHandle = AuthRequestHandle{};
        self->handleFreshTokenResult(generation, profileId, result);
      });
}

void SessionCoordinator::handleFreshTokenResult(
    quint64 generation, const QString &profileId,
    const AuthResult<AuthToken> &result) {
  if (result.outcome != AuthOutcome::Success) {
    clearCurrentUserAndSetStateIfCurrent(generation, State::SignedOut,
                                         result.diagnostic);
    return;
  }
  // Defense-in-depth, independent of which IAuthenticationClient
  // implementation produced |result|: a production
  // NetworkAuthenticationClient's AuthToken::fromJson() already rejects
  // any token failing isValidTokenContent() (see TokenValidation.h)
  // before ever reporting Success here, but an alternate/fake client (as
  // used throughout this coordinator's own test suite, and any future
  // alternate implementation) could still hand back a Success result
  // carrying a token that never went through that decoder at all. Every
  // other trust boundary this same token could next cross --
  // NetworkAuthenticationClient::whoAmI()'s own admission check and
  // QtKeychainTokenStore::saveToken() -- enforces this exact identical
  // shared check, so re-validating it here, BEFORE ever calling
  // issueWhoAmI() or saveFreshlyObtainedToken(), guarantees the same
  // invariant holds no matter which client implementation is wired in: no
  // request is ever sent, and nothing is ever saved, for a token this
  // coordinator's own secure-store layer would immediately classify as
  // Malformed (and durably delete) on the very next restore.
  if (!isValidTokenContent(result.value->token)) {
    clearCurrentUserAndSetStateIfCurrent(
        generation, State::SignedOut,
        QStringLiteral(
            "authentication response did not contain a usable token"));
    return;
  }
  issueWhoAmI(generation, profileId, result.value->token,
              WhoAmIPurpose::FreshlyObtained);
}

// ─── Sign out ────────────────────────────────────────────────────────────

void SessionCoordinator::signOut() {
  if (m_state != State::SignedIn || !m_currentProfile.has_value()) {
    // Also covers a signOut() already in progress: the very first call to
    // reach this point transitions to SigningOut below (after the delete
    // is already reserved) so a reentrant call (from within that
    // transition's stateChanged() emission) or a merely duplicate call
    // sees m_state != SignedIn here and is rejected as a safe, idempotent
    // no-op -- it can never bump the generation again or enqueue a second
    // deletion. This also covers a pathological synchronously-completing
    // ITokenStore (see below): clearCurrentUserAndSetStateIfCurrent()
    // assigns the target state (e.g. SignedOut) and clears identity
    // TOGETHER before emitting anything, so even a reentrant call from
    // within that nested completion's own currentUserChanged() emission
    // observes state() already != SignedIn -- never a stale SignedIn
    // alongside an already-cleared identity -- and is rejected here too.
    return;
  }
  cancelPendingAuthRequest();
  const QString profileId = m_currentProfile->profileId();
  // Defensive: signOut() itself is about to enqueue a Delete for this
  // profile, so there is no separate in-flight Save to compensate for at
  // this exact instant, but bumping the credential epoch here keeps the
  // invariant uniform ("every session-invalidating transition invalidates
  // the outgoing profile's credential epoch") regardless of how signOut()
  // is reached in the future.
  invalidateProfileCredential(profileId);
  ++m_generation;
  const quint64 generation = m_generation;
  QPointer<SessionCoordinator> self(this);

  // Reserve the durable deletion BEFORE any notification is emitted.
  // enqueueTokenOp() is a plain, non-signal-emitting FIFO mutation (and
  // ITokenStore's own dispatch is itself asynchronous per its contract),
  // so recording the obligation here -- rather than after the SigningOut
  // transition below, as a previous version of this function did --
  // guarantees it survives even if a directly-connected observer of that
  // transition reentrantly calls start()/switchProfile()/signOut() again
  // and bumps the generation further. Durable cleanup must never depend
  // on the generation surviving a notification it would otherwise be
  // emitted alongside; the required-delete failure path itself is also no
  // longer generation-gated (see startFrontTokenOp()'s central retry
  // reinstatement), so this obligation remains actionable regardless.
  enqueueTokenOp(profileId, TokenOpKind::Delete, QString(), QString(),
                 [self, generation, profileId](TokenStoreResult result) {
                   if (!self) {
                     return;
                   }
                   self->handleSignOutDeletionResult(generation, profileId,
                                                     result);
                 });

  // A pathological (test-only) synchronously-completing ITokenStore may
  // already have invoked the callback above and moved m_state past
  // SignedIn (e.g. straight to SignedOut on immediate success, or to
  // SecureStorageUnavailable via the central FIFO dispatcher on immediate
  // failure) before this line ever runs -- see
  // clearCurrentUserAndSetStateIfCurrent()'s coherent-snapshot guarantee,
  // which is exactly what makes this safe: nothing observable ever shows
  // a stale SigningOut/SignedIn once that has happened. Only publish the
  // SigningOut transition if that has not happened, so this call can
  // never clobber an already-correct, more-current state with a stale
  // "still signing out" label.
  if (!self || generation != self->m_generation ||
      self->m_state != State::SignedIn) {
    return;
  }
  setState(State::SigningOut);
}

void SessionCoordinator::handleSignOutDeletionResult(
    quint64 generation, const QString &profileId,
    const TokenStoreResult &result) {
  // NotFound (the token was already absent) achieves the exact same
  // postcondition as Success here -- no token remains under this
  // profileId() -- so both are treated identically; see the shared
  // deleteFailed check in startFrontTokenOp() for why NotFound must never
  // itself be mistaken for a genuine failure.
  if (result.outcome == TokenStoreOutcome::Success ||
      result.outcome == TokenStoreOutcome::NotFound) {
    clearCurrentUserAndSetStateIfCurrent(generation, State::SignedOut);
  }
  // On any other failure, nothing further to do here: the central FIFO dispatch
  // (see startFrontTokenOp()) has already left the delete stalled at the
  // head of |profileId|'s queue and, if that profile is still the
  // coordinator's current one, has already reinstalled an actionable
  // retry() action and surfaced SecureStorageUnavailable -- unconditionally,
  // regardless of whether |generation| (the session that originally
  // called signOut()) is still current. Retryability for a required
  // delete must never depend on that original generation surviving (see
  // the class-level FIFO/durability comments in SessionCoordinator.h).
}

// ─── Retry ───────────────────────────────────────────────────────────────

void SessionCoordinator::retry() {
  if (!m_stalledProfileOrder.isEmpty()) {
    // An outstanding required-deletion obligation for ANY profile always
    // takes priority over m_retryAction below, regardless of whether that
    // profile is currently selected: see m_stalledProfileOrder's own doc
    // comment and retry()'s declaration in the header. Deliberately does
    // NOT consume/touch m_retryAction here, so whatever single action it
    // currently holds for the CURRENT profile's own, unrelated failure
    // (if any) remains intact underneath, unaffected by this profile's
    // resolution.
    retryStuckProfileTokenOp(m_stalledProfileOrder.first());
    return;
  }
  auto action = std::exchange(m_retryAction, nullptr);
  if (action) {
    action();
  }
}

// ─── Auth-request cancellation ──────────────────────────────────────────

void SessionCoordinator::cancelPendingAuthRequest() {
  if (m_pendingAuthHandle.id != 0) {
    m_authClient.cancel(m_pendingAuthHandle);
    m_pendingAuthHandle = AuthRequestHandle{};
  }
}

// ─── Per-profile FIFO token-store queue ─────────────────────────────────

void SessionCoordinator::enqueueTokenOp(
    const QString &profileId, TokenOpKind kind, QString token,
    QString endpointIdentity, ITokenStore::ResultCallback onComplete) {
  auto &queue = m_tokenQueues[profileId];
  const bool wasEmpty = queue.isEmpty();
  TokenOp op{kind, std::move(token), std::move(onComplete)};
  op.admissionEpoch = profileCredentialEpoch(profileId);
  op.admissionEndpointEpoch = profileEndpointEpoch(profileId);
  op.opId = m_nextTokenOpId++;
  op.endpointIdentity = std::move(endpointIdentity);
  queue.enqueue(std::move(op));
  if (wasEmpty) {
    startFrontTokenOp(profileId);
  }
}

void SessionCoordinator::startFrontTokenOp(const QString &profileId) {
  const auto queueIt = m_tokenQueues.find(profileId);
  if (queueIt == m_tokenQueues.end() || queueIt->isEmpty()) {
    return;
  }

  // Central dispatch guard: refuse to issue a second real ITokenStore call
  // for this profile while one is already outstanding. Without this, a
  // duplicate retryStuckProfileTokenOp() call (e.g. two rapid retry()
  // invocations, or start()/switchProfile() re-reaching the same stalled
  // retry path before the first attempt's callback has fired) could
  // dispatch the same stalled head operation twice concurrently.
  ProfileTokenDispatch &dispatch = m_tokenDispatch[profileId];
  if (dispatch.inFlight) {
    return;
  }

  const TokenOp &op = queueIt->head();

  if (op.kind == TokenOpKind::Save &&
      op.admissionEpoch != profileCredentialEpoch(profileId)) {
    // This Save was queued behind an operation that has since invalidated
    // the profile's credential epoch (profile switch/restart/sign-out
    // occurred before this Save ever crossed the ITokenStore boundary).
    // It never needs to run -- and must not, since a subsequent
    // compensating Delete may already be queued right behind it -- so it
    // is skipped without ever touching the real secure store. Its
    // onComplete is intentionally never invoked: nothing in the
    // coordinator depends on it firing for an op that was never admitted
    // (the caller's own generation guard already treats this the same as
    // a stale/never-observed completion).
    queueIt->dequeue();
    const bool hasMore = !queueIt->isEmpty();
    if (queueIt->isEmpty()) {
      m_tokenQueues.erase(queueIt);
      // Also drop this profile's dispatch-tracking entry: it was never
      // marked in-flight on this path (the guard above returns before
      // dispatch.inFlight is ever set true), so nothing depends on it
      // surviving, and leaving a stale default-constructed entry behind
      // in the map would be unbounded per-profile bookkeeping growth for
      // no benefit.
      m_tokenDispatch.remove(profileId);
    }
    if (hasMore) {
      startFrontTokenOp(profileId);
    }
    return;
  }

  const TokenOpKind kind = op.kind;
  const QString token = op.token;
  const QString endpointIdentity = op.endpointIdentity;
  const quint64 opId = op.opId;
  const quint64 attemptId = m_nextTokenAttemptId++;
  dispatch.inFlight = true;
  dispatch.opId = opId;
  dispatch.attemptId = attemptId;

  QPointer<SessionCoordinator> self(this);
  auto onResult = [self, profileId, opId, attemptId](TokenStoreResult result) {
    if (!self) {
      return;
    }
    // Match BOTH the dispatch record (proving this is the specific attempt
    // that was actually issued, not a stale/duplicate invocation of an
    // older attempt or an attempt for an op that has since been
    // superseded) AND the queue's current head opId (proving the op this
    // attempt was for is still the one waiting to be completed, not
    // already dequeued/replaced by a prior, correctly-processed
    // completion). Either mismatch means this callback must be silently
    // discarded: it must never pop/advance the queue or feed its result
    // to a different operation's continuation.
    const auto dispatchIt = self->m_tokenDispatch.find(profileId);
    if (dispatchIt == self->m_tokenDispatch.end() || !dispatchIt->inFlight ||
        dispatchIt->opId != opId || dispatchIt->attemptId != attemptId) {
      return;
    }
    const auto it = self->m_tokenQueues.find(profileId);
    if (it == self->m_tokenQueues.end() || it->isEmpty() ||
        it->head().opId != opId) {
      return; // defensive; should not happen given the guard above
    }

    // Clear the in-flight record exactly once, before any further
    // processing below, so a genuinely new dispatch (e.g. a retry issued
    // from within the continuation invoked further down) is free to
    // proceed and so a subsequent duplicate/stale invocation of this same
    // completion is rejected by the guard above rather than reprocessed.
    dispatchIt->inFlight = false;

    const TokenOpKind finishedKind = it->head().kind;
    // NotFound is treated as an equally successful outcome for a Delete:
    // deleting an already-absent entry achieves the exact same
    // postcondition (no token remains under this profileId()) as deleting
    // one that was present, so it must never be mistaken for a genuine
    // failure requiring a durable stall/retry. This matters most for a
    // required Delete reserved defensively (e.g.
    // invalidateProfileCredentialForEndpointChange()'s unconditional
    // cleanup) against a profile that may never have had a token saved at
    // all.
    const bool deleteFailed = finishedKind == TokenOpKind::Delete &&
                              result.outcome != TokenStoreOutcome::Success &&
                              result.outcome != TokenStoreOutcome::NotFound;
    if (deleteFailed) {
      // A required Delete must never be silently abandoned: leave it at
      // the head of the FIFO (un-dequeued) so it durably blocks every
      // later same-profile operation until retryStuckProfileTokenOp()
      // successfully re-dispatches this exact op (same opId, new
      // attemptId).
      const QString stallDiagnostic =
          result.diagnostic.isEmpty()
              ? QStringLiteral("A required secure-store deletion failed "
                               "and must be retried.")
              : result.diagnostic;

      // Copy the head op's continuation BEFORE any signal emission below:
      // markProfileCleanupStalled()/setState() below may emit
      // synchronously, and a directly-connected handler could destroy
      // this coordinator entirely, which would invalidate
      // `self->m_tokenQueues` (and therefore `it`, an iterator into it)
      // out from under us. Everything this branch still needs from the
      // queue is captured here, up front, while `self` and `it` are both
      // still known-good.
      const ITokenStore::ResultCallback onComplete = it->head().onComplete;

      // Track this profile's cleanup obligation in FIFO order,
      // unconditionally of whether it is the coordinator's currently
      // selected profile: this alone is what keeps an orphaned/unselected
      // profile's failure visible (via state()/diagnostic()'s
      // hasBlockingOrphanCleanup() override) and retryable (via retry()),
      // survives any later start()/switchProfile() call for a *different*
      // profile without ever being lost, and derives its retryability
      // from this durable per-profile FIFO/stalled state rather than
      // whatever UI generation originally enqueued the op (an op's
      // onComplete continuation below is a fixed closure captured once at
      // enqueue time and would otherwise silently lose the ability to
      // retry if the coordinator's generation has since moved on -- e.g.
      // the profile was switched away from and back, or start()
      // restarted while this retry was in flight).
      self->markProfileCleanupStalled(profileId, stallDiagnostic);
      if (!self) {
        return;
      }

      if (self->m_currentProfile.has_value() &&
          self->m_currentProfile->profileId() == profileId) {
        // Also synchronize the REAL, raw m_state/m_diagnostic to this
        // exact obligation when this profile happens to be the one
        // currently selected. hasBlockingOrphanCleanup() now masks
        // state()/diagnostic() to the oldest obligation UNCONDITIONALLY
        // (regardless of selection -- see its own doc comment), so this
        // call changes nothing externally observable by itself; its
        // purpose is purely to keep the raw fields from silently drifting
        // out of sync with what has already been published while this
        // profile remains masked, so that mutateState()'s exposed-value
        // comparison and clearProfileCleanupStalled()'s own before/after
        // comparison never misfire an extra reveal-then-immediately-
        // overwritten pair of signals the moment this exact obligation is
        // the one that eventually resolves.
        self->setState(State::SecureStorageUnavailable, stallDiagnostic);
      }

      // setState() above may have reentrantly destroyed the coordinator
      // (or superseded this profile in some other way); re-check `self`
      // before touching it any further. `onComplete` was already safely
      // copied above and does not depend on `self`/`it` remaining valid.
      if (!self) {
        return;
      }

      // The original per-call onComplete may still react to this failure
      // for its own generation-gated purposes (e.g. identity handling);
      // it must never be relied upon as the sole source of truth for
      // whether a retry action exists (see above).
      if (onComplete) {
        onComplete(result);
      }
      return;
    }

    TokenOp finishedOp = it->dequeue();
    const bool hasMore = !it->isEmpty();
    if (it->isEmpty()) {
      self->m_tokenQueues.erase(it);
      self->m_tokenDispatch.remove(profileId);
    }
    // Clear this profile's cleanup obligation (if any) only now that `it`
    // is no longer needed for anything below: clearProfileCleanupStalled()
    // may itself publish a notification, and a directly-connected handler
    // could reentrantly mutate self->m_tokenQueues[profileId] again (e.g.
    // enqueue a fresh operation), which must never be allowed to
    // invalidate an iterator this function still depends on. `finishedOp`
    // and `hasMore`, already captured above as plain values, remain valid
    // regardless.
    self->clearProfileCleanupStalled(profileId);
    if (!self) {
      return; // destroyed while a resolved-obligation notification was
              // delivered
    }
    // Invoke the completed operation's continuation before starting the
    // next queued operation for this profile, so completion order always
    // matches issue order even if the continuation itself enqueues another
    // operation for the same profile.
    if (finishedOp.onComplete) {
      finishedOp.onComplete(std::move(result));
    }
    // The continuation above is itself QPointer-guarded, but if it somehow
    // led to this coordinator's destruction, `self` is now null: never
    // dereference it to start the next queued operation.
    if (!self) {
      return;
    }
    if (hasMore) {
      self->startFrontTokenOp(profileId);
    }
  };

  switch (kind) {
  case TokenOpKind::Read:
    m_tokenStore.readToken(profileId, endpointIdentity, std::move(onResult));
    break;
  case TokenOpKind::Save:
    m_tokenStore.saveToken(profileId, token, endpointIdentity,
                           std::move(onResult));
    break;
  case TokenOpKind::Delete:
    m_tokenStore.deleteToken(profileId, std::move(onResult));
    break;
  }
}

void SessionCoordinator::retryStuckProfileTokenOp(const QString &profileId) {
  // Only meaningful for a profile whose head op is actually stalled (a
  // failed required Delete); a stray call otherwise is a safe no-op.
  if (!m_profileFifoStalled.contains(profileId)) {
    return;
  }
  // The stuck op's original onComplete closure is still installed at the
  // head of the queue (it was never dequeued on failure); re-dispatching
  // the front op re-runs the exact same logical operation (same opId,
  // fresh attemptId) with the exact same continuation -- no need to
  // reconstruct or re-enqueue it. startFrontTokenOp()'s own inFlight guard
  // ensures that if a previous retry attempt for this same stalled op is
  // still outstanding, this call is a no-op rather than a second
  // concurrent dispatch.
  startFrontTokenOp(profileId);
}

void SessionCoordinator::markProfileCleanupStalled(
    const QString &profileId, const QString &failureDiagnostic) {
  const State oldExposedState = state();
  const QString oldExposedDiagnostic = diagnostic();
  m_profileFifoStalled.insert(profileId, failureDiagnostic);
  if (!m_stalledProfileOrder.contains(profileId)) {
    m_stalledProfileOrder.append(profileId);
  }
  if (state() == oldExposedState && diagnostic() == oldExposedDiagnostic) {
    // Either this profile was already tracked and remains behind an
    // earlier, still-unresolved obligation for a DIFFERENT profile (which
    // stays displayed/retried first, unaffected), or it is the very first
    // obligation AND the currently selected profile, in which case the
    // caller's own companion setState() call (made alongside this one --
    // see startFrontTokenOp()) keeps the raw fields synchronized with
    // what THIS call already published, so that call itself detects no
    // further exposed change either. Either way, nothing externally
    // observable changed as a direct result of THIS call.
    return;
  }
  ++m_stateRevision.mutation;
  QPointer<SessionCoordinator> self(this);
  publishDirtyProperties(self);
}

void SessionCoordinator::clearProfileCleanupStalled(const QString &profileId) {
  if (!m_profileFifoStalled.contains(profileId)) {
    return; // not tracked; nothing to clear, nothing to notify
  }
  const State oldExposedState = state();
  const QString oldExposedDiagnostic = diagnostic();
  m_profileFifoStalled.remove(profileId);
  m_stalledProfileOrder.removeOne(profileId);
  if (state() == oldExposedState && diagnostic() == oldExposedDiagnostic) {
    // Resolving a non-head obligation (queued behind an earlier,
    // still-unresolved failure for a different profile) is deliberately
    // silent: the head remains displayed/retried first, unaffected. This
    // is also the outcome when the CURRENT profile's own head stall
    // resolves and at least one other obligation remains queued behind
    // it (that next obligation becomes the new head, an already-published
    // value, so nothing changes here either); the case where this WAS the
    // very last outstanding obligation and belonged to the current
    // profile is handled by the branch below instead.
    return;
  }
  // This WAS the head of an orphan-cleanup override: either a NEXT
  // obligation now becomes visible in its place, or -- if none remain --
  // the current profile's own real, already-progressed underlying state
  // becomes visible again, automatically, with no further action needed.
  ++m_stateRevision.mutation;
  QPointer<SessionCoordinator> self(this);
  publishDirtyProperties(self);
}

void SessionCoordinator::invalidateProfileCredential(const QString &profileId) {
  const quint64 oldEpoch = m_profileCredentialEpoch.value(profileId, 0);
  m_profileCredentialEpoch.insert(profileId, oldEpoch + 1);

  const auto queueIt = m_tokenQueues.find(profileId);
  if (queueIt == m_tokenQueues.end() || queueIt->isEmpty()) {
    return;
  }
  const TokenOp &front = queueIt->head();
  if (front.kind != TokenOpKind::Save || front.admissionEpoch != oldEpoch) {
    // Either there is no in-flight Save for this profile, or the front op
    // was already stamped with a newer epoch (already compensated for by
    // an earlier invalidation) -- nothing further to reserve.
    return;
  }
  // The front Save was admitted under the epoch we just invalidated. Since
  // it is at the head of the FIFO, it has already been dispatched to the
  // real ITokenStore (queued-but-undispatched Saves are never at the
  // head -- only the op actually in flight occupies that position) and is
  // therefore uncancellable: it may still succeed and durably persist a
  // token for a profile/session that is no longer current. Reserve a
  // compensating Delete behind it now, before any later same-profile
  // auth/save can be enqueued, so that FIFO ordering alone guarantees the
  // cleanup always runs after whatever the in-flight Save ends up doing.
  enqueueTokenOp(profileId, TokenOpKind::Delete, QString(), QString(),
                 [](TokenStoreResult) {
                   // No profile-specific reaction needed here: this
                   // compensating cleanup exists purely to remove a
                   // possibly-leaked token. A failure here still leaves
                   // the profile's FIFO durably stalled (see
                   // startFrontTokenOp()) -- startFrontTokenOp()'s own
                   // central dispatch immediately surfaces
                   // SecureStorageUnavailable and reinstalls retry() if
                   // this profile already is the current one at the
                   // moment of failure, or otherwise the obligation is
                   // surfaced the next time this profile becomes current
                   // again (see startCredentialRestore()'s stalled check).
                 });
}

void SessionCoordinator::invalidateProfileCredentialForEndpointChange(
    const QString &profileId) {
  // Bump the NARROW endpoint epoch FIRST, before anything else below:
  // this is what makes any Read already queued or in flight for this
  // profileId() -- necessarily admitted before this call, hence under the
  // OLD endpoint epoch -- permanently ineligible for
  // startCredentialRestore()'s rebind-dedup logic from this point
  // forward (see TokenOp::admissionEndpointEpoch and
  // startCredentialRestore()'s own comment). Doing this before the
  // required Delete below is enqueued keeps every TokenOp enqueued from
  // here on stamped with the new epoch, for consistency, even though only
  // Read currently consults this particular counter.
  m_profileEndpointEpoch.insert(profileId,
                                m_profileEndpointEpoch.value(profileId, 0) + 1);

  // Perform the ordinary (coarser) epoch bump plus in-flight-Save
  // compensation shared with every other credential-invalidating
  // transition (switching away from a profile entirely, signing out,
  // restarting): an auth/token Save that already crossed the ITokenStore
  // boundary under the OLD epoch must still be compensated for if it
  // later persists a token, exactly as when abandoning a profile.
  invalidateProfileCredential(profileId);

  // Unlike invalidateProfileCredential()'s own conditional compensating
  // Delete (which fires only when there is a stale in-flight Save at the
  // head), an endpoint change must ALSO unconditionally reserve a Delete
  // for whatever token may ALREADY be durably stored for this profile's
  // UUID from a PRIOR session at the old endpoint, even when nothing is
  // currently in flight. That token (if any) was authenticated against
  // the OLD endpoint and must never be read, sent, or saved against the
  // new one. Enqueueing it here appends it to the tail of this profile's
  // FIFO, so it is guaranteed to run to completion -- or durably stall,
  // blocking every later same-profile operation via the shared
  // required-delete-failure retry path, see startFrontTokenOp() -- BEFORE
  // any later-enqueued restore Read or auth Save for this profile,
  // regardless of what (if anything) is already queued ahead of it.
  enqueueTokenOp(profileId, TokenOpKind::Delete, QString(), QString(),
                 [](TokenStoreResult) {
                   // No profile-specific reaction needed: this exists
                   // purely to guarantee no token from the old endpoint
                   // survives. A NotFound outcome (no prior token ever
                   // existed for this profile) is treated as a successful,
                   // idempotent delete by the shared FIFO completion logic
                   // (see startFrontTokenOp()'s deleteFailed check); any
                   // genuine failure still leaves the profile's FIFO
                   // durably stalled and is surfaced/retried exactly like
                   // any other required delete.
                 });
}

void SessionCoordinator::reconcileAllProfileCredentialsOnReload(
    const QList<ServerProfile> &previousProfiles,
    const QList<ServerProfile> &newProfiles) {
  QHash<QString, ServerProfile> previousById;
  previousById.reserve(previousProfiles.size());
  for (const ServerProfile &previous : previousProfiles) {
    previousById.insert(previous.profileId(), previous);
  }

  QSet<QString> newIds;
  newIds.reserve(newProfiles.size());
  for (const ServerProfile &fresh : newProfiles) {
    const QString id = fresh.profileId();
    newIds.insert(id);
    const auto previousIt = previousById.constFind(id);
    if (previousIt == previousById.constEnd()) {
      // Brand-new or re-added ID: no prior in-memory record exists to
      // compare against, so there is nothing here to invalidate. A
      // re-added ID whose UUID was previously used for a different
      // endpoint relies purely on ITokenStore's own durable
      // envelope-binding check (see ITokenStore.h) the next time it is
      // actually selected and its credential is read.
      continue;
    }
    if (!previousIt->hasEquivalentEndpoint(fresh)) {
      invalidateProfileCredentialForEndpointChange(id);
    }
  }

  for (const ServerProfile &previous : previousProfiles) {
    const QString id = previous.profileId();
    if (newIds.contains(id)) {
      continue;
    }
    // Removed from persisted storage entirely: its secure-store entry (if
    // any) is now unreachable through the ordinary selected-profile
    // restore flow and would otherwise be orphaned forever, so it is
    // durably cleaned up here exactly like a detected endpoint change.
    invalidateProfileCredentialForEndpointChange(id);
  }
}

bool SessionCoordinator::publishDirtyProperties(
    QPointer<SessionCoordinator> &self) {
  if (m_stateRevision.notified != m_stateRevision.mutation) {
    // Mark |notified| BEFORE emitting (not after): if this emission
    // synchronously triggers a nested transition that mutates the SAME
    // property group further, the nested call's own publish step sees
    // notified==mutation(old) as already stale relative to ITS newer
    // mutation and correctly announces the newest value itself; when
    // control returns here, this frame's own now-superseded obligation is
    // already satisfied and must not re-fire.
    m_stateRevision.notified = m_stateRevision.mutation;
    emit stateChanged();
    if (!self) {
      return false; // destroyed while stateChanged() was being delivered
    }
  }
  if (m_currentUserRevision.notified != m_currentUserRevision.mutation) {
    m_currentUserRevision.notified = m_currentUserRevision.mutation;
    emit currentUserChanged();
    if (!self) {
      return false; // destroyed while currentUserChanged() was delivered
    }
  }
  if (m_selectedProfileRevision.notified !=
      m_selectedProfileRevision.mutation) {
    m_selectedProfileRevision.notified = m_selectedProfileRevision.mutation;
    emit selectedProfileChanged();
    if (!self) {
      return false; // destroyed while selectedProfileChanged() was delivered
    }
  }
  return true;
}

bool SessionCoordinator::clearCurrentUserAndSetStateIfCurrent(
    quint64 generation, State state, QString diagnostic) {
  if (generation != m_generation) {
    return false;
  }
  // Commit the complete new (state, cleared-identity) snapshot together,
  // BEFORE either signal is published: see publishDirtyProperties()'s
  // contract. This is what makes a synchronously-completing ITokenStore
  // (see signOut()'s deletion path) safe -- a reentrant handler observing
  // either emission below sees the target state (e.g. SignedOut) and the
  // cleared identity together, never a stale SignedIn alongside an
  // already-cleared identity.
  mutateCurrentUser(std::nullopt);
  mutateState(state, std::move(diagnostic));
  QPointer<SessionCoordinator> self(this);
  if (!publishDirtyProperties(self)) {
    return false;
  }
  return generation == self->m_generation;
}

bool SessionCoordinator::applyCurrentUserAndSetStateIfCurrent(
    quint64 generation, const CurrentUser &user, State state) {
  if (generation != m_generation) {
    return false;
  }
  // Commit the complete new (state, identity) snapshot together, BEFORE
  // either signal is published, for the same coherence reason as
  // clearCurrentUserAndSetStateIfCurrent() above.
  mutateCurrentUser(user);
  mutateState(state, {});
  QPointer<SessionCoordinator> self(this);
  if (!publishDirtyProperties(self)) {
    return false;
  }
  return generation == self->m_generation;
}

} // namespace Arkham
