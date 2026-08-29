#include "SessionCoordinator.h"

#include <QPointer>

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

QString SessionCoordinator::stateDescription() const {
  switch (m_state) {
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

void SessionCoordinator::setState(State state, QString diagnostic) {
  m_state = state;
  m_diagnostic = std::move(diagnostic);
  emit stateChanged();
}

void SessionCoordinator::applyCurrentUser(const CurrentUser &user) {
  m_currentUser = user;
  emit currentUserChanged();
}

void SessionCoordinator::clearCurrentUser() {
  if (!m_currentUser.has_value()) {
    return;
  }
  m_currentUser.reset();
  emit currentUserChanged();
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

  QPointer<SessionCoordinator> self(this);
  clearCurrentUser();
  if (!self || generation != self->m_generation) {
    // A directly-connected currentUserChanged() handler reentrantly called
    // start()/switchProfile()/signOut(), or destroyed the coordinator,
    // while clearCurrentUser()'s emission was being delivered: `this` may
    // already be dangling and |generation| is no longer current, so
    // setState() below must never run.
    return;
  }
  setState(State::Loading);
  if (!self || generation != self->m_generation) {
    // Same check again for setState()'s own stateChanged() emission: a
    // handler reentrantly triggered by *this* emission could equally
    // destroy the coordinator or supersede |generation| before the
    // remainder of start() runs below.
    return;
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
  m_profiles = std::move(profiles);
  m_selectedProfileId = selectedId;
  m_currentProfile = selectedProfile;
  emit selectedProfileChanged();
  if (!self || generation != self->m_generation) {
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
  // pretend the switch succeeded: the current profile/session are left
  // completely untouched.
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

  QPointer<SessionCoordinator> self(this);
  clearCurrentUser();
  if (!self || generation != self->m_generation) {
    return; // superseded while currentUserChanged() was being delivered
  }

  // Publish a coherent non-SignedIn transitional snapshot (cleared
  // identity, Loading state) BEFORE the selected profile itself changes
  // below. Without this, a reentrant observer of the selectedProfileChanged()
  // emission further down could see the NEW profile while state() still
  // reported the OLD session's SignedIn (setState() was previously only
  // reached later, inside startProbe()) -- letting e.g. a directly
  // connected selectedProfileChanged()/currentUserChanged() handler that
  // calls signOut() delete the NEW profile's token while believing it was
  // still authenticated as the profile being left. Every subsequent
  // notification below re-checks self/generation, so a reentrant handler
  // observes either the complete old snapshot or this complete transition
  // snapshot -- never a hybrid of the two.
  setState(State::Loading);
  if (!self || generation != self->m_generation) {
    return; // superseded while stateChanged() was being delivered
  }

  m_selectedProfileId = profileId;
  m_currentProfile = *it;
  emit selectedProfileChanged();
  if (!self || generation != self->m_generation) {
    return; // superseded while selectedProfileChanged() was being delivered
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
  const bool stalled = m_profileFifoStalled.contains(profileId);
  QPointer<SessionCoordinator> self(this);
  setState(stalled ? State::SecureStorageUnavailable
                   : State::RestoringCredential,
           stalled ? m_profileFifoStalled.value(profileId) : QString());
  if (!self || generation != self->m_generation) {
    return; // superseded while stateChanged() was being delivered
  }
  if (stalled) {
    // A previously-failed required deletion for this profile is still
    // durably blocking its FIFO (see startFrontTokenOp()); enqueuing a
    // fresh read below is still correct (it will simply wait behind that
    // still-queued deletion), but retry() must resolve the deletion, not
    // silently wait forever with no actionable feedback.
    m_retryAction = [this, profileId] { retryStuckProfileTokenOp(profileId); };
  }
  enqueueTokenOp(profileId, TokenOpKind::Read, QString(),
                 [self, generation, profileId](TokenStoreResult result) {
                   if (!self) {
                     return;
                   }
                   if (generation != self->m_generation) {
                     return;
                   }
                   self->handleRestoreReadResult(generation, profileId, result);
                 });
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
      deleteRestoredUnauthorizedToken(generation, profileId);
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
  QPointer<SessionCoordinator> self(this);
  enqueueTokenOp(
      profileId, TokenOpKind::Save, token,
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

// A restored token the server has rejected must be durably deleted before
// the coordinator is allowed to claim signed out. Defined as a reusable,
// generation-guarded step (rather than an inline lambda) so a deletion
// failure can install a retry action that re-invokes exactly this same
// step; without that, retry() would be a silent no-op while stuck in the
// resulting SecureStorageUnavailable state.
void SessionCoordinator::deleteRestoredUnauthorizedToken(
    quint64 generation, const QString &profileId) {
  if (generation != m_generation) {
    return;
  }
  QPointer<SessionCoordinator> self(this);
  enqueueTokenOp(profileId, TokenOpKind::Delete, QString(),
                 [self, generation, profileId](TokenStoreResult deleteResult) {
                   if (!self) {
                     return;
                   }
                   if (deleteResult.outcome == TokenStoreOutcome::Success) {
                     self->clearCurrentUserAndSetStateIfCurrent(
                         generation, State::SignedOut);
                   }
                   // On failure, nothing further to do here: the central
                   // FIFO dispatch (see startFrontTokenOp()) has already
                   // left the delete stalled and, if this profile is
                   // still the coordinator's current one, has already
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
    // deletion.
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
  enqueueTokenOp(profileId, TokenOpKind::Delete, QString(),
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
  // failure) before this line ever runs. Only publish the SigningOut
  // transition if that has not happened, so this call can never clobber
  // an already-correct, more-current state with a stale "still signing
  // out" label.
  if (!self || generation != self->m_generation ||
      self->m_state != State::SignedIn) {
    return;
  }
  setState(State::SigningOut);
}

void SessionCoordinator::handleSignOutDeletionResult(
    quint64 generation, const QString &profileId,
    const TokenStoreResult &result) {
  if (result.outcome == TokenStoreOutcome::Success) {
    clearCurrentUserAndSetStateIfCurrent(generation, State::SignedOut);
  }
  // On failure, nothing further to do here: the central FIFO dispatch
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
    ITokenStore::ResultCallback onComplete) {
  auto &queue = m_tokenQueues[profileId];
  const bool wasEmpty = queue.isEmpty();
  TokenOp op{kind, std::move(token), std::move(onComplete)};
  op.admissionEpoch = profileCredentialEpoch(profileId);
  op.opId = m_nextTokenOpId++;
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
    const bool deleteFailed = finishedKind == TokenOpKind::Delete &&
                              result.outcome != TokenStoreOutcome::Success;
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
      self->m_profileFifoStalled.insert(profileId, stallDiagnostic);

      // Copy the head op's continuation BEFORE any signal emission below:
      // setState() emits stateChanged() synchronously, and a directly-
      // connected handler could destroy this coordinator entirely, which
      // would invalidate `self->m_tokenQueues` (and therefore `it`, an
      // iterator into it) out from under us. Everything this branch still
      // needs from the queue is captured here, up front, while `self` and
      // `it` are both still known-good.
      const ITokenStore::ResultCallback onComplete = it->head().onComplete;

      // Retryability for a required delete must derive from this durable
      // per-profile FIFO/stalled state, not from whatever UI generation
      // originally enqueued the op: an op's onComplete continuation below
      // is a fixed closure captured once at enqueue time, so if the
      // coordinator's generation has since moved on (e.g. the profile was
      // switched away from and back, or start() restarted while this
      // retry was in flight), that continuation's own generation check
      // would otherwise silently drop the retry action forever even
      // though this profile may still be the one currently selected. Make
      // this profile's retry() action authoritative here instead,
      // unconditionally, whenever it is still the current profile --
      // regardless of |generation|. If the UI has moved to a different
      // profile, no state is published here; the obligation itself (via
      // m_profileFifoStalled) remains and is enforced/surfaced again by
      // startCredentialRestore() the next time this profile becomes
      // current.
      if (self->m_currentProfile.has_value() &&
          self->m_currentProfile->profileId() == profileId) {
        self->m_retryAction = [self, profileId] {
          if (!self) {
            return;
          }
          self->retryStuckProfileTokenOp(profileId);
        };
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

    self->m_profileFifoStalled.remove(profileId);
    TokenOp finishedOp = it->dequeue();
    const bool hasMore = !it->isEmpty();
    if (it->isEmpty()) {
      self->m_tokenQueues.erase(it);
      self->m_tokenDispatch.remove(profileId);
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
    m_tokenStore.readToken(profileId, std::move(onResult));
    break;
  case TokenOpKind::Save:
    m_tokenStore.saveToken(profileId, token, std::move(onResult));
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
  enqueueTokenOp(profileId, TokenOpKind::Delete, QString(),
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

bool SessionCoordinator::clearCurrentUserAndSetStateIfCurrent(
    quint64 generation, State state, QString diagnostic) {
  if (generation != m_generation) {
    return false;
  }
  QPointer<SessionCoordinator> self(this);
  clearCurrentUser();
  if (!self || generation != self->m_generation) {
    return false; // superseded while currentUserChanged() was delivered
  }
  self->setState(state, std::move(diagnostic));
  return !self.isNull() && generation == self->m_generation;
}

bool SessionCoordinator::applyCurrentUserAndSetStateIfCurrent(
    quint64 generation, const CurrentUser &user, State state) {
  if (generation != m_generation) {
    return false;
  }
  QPointer<SessionCoordinator> self(this);
  applyCurrentUser(user);
  if (!self || generation != self->m_generation) {
    return false; // superseded while currentUserChanged() was delivered
  }
  self->setState(state);
  return !self.isNull() && generation == self->m_generation;
}

} // namespace Arkham
