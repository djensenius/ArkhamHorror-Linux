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
  case State::Incompatible:
    return QStringLiteral("Server incompatible");
  case State::SecureStorageUnavailable:
    return QStringLiteral("Secure storage unavailable");
  case State::ProfileStorageFailure:
    return QStringLiteral("Profile storage error");
  case State::RecoverableFailure:
    return QStringLiteral("Connection problem");
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
  setState(State::Loading);

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
  // profile being switched away from.
  m_probe.reset();
  ++m_generation;
  m_retryAction = nullptr;
  clearCurrentUser();

  m_selectedProfileId = profileId;
  m_currentProfile = *it;
  emit selectedProfileChanged();

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
  setState(State::ProbingCapabilities);
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
  setState(State::RestoringCredential);
  const quint64 generation = m_generation;
  const QString profileId = m_currentProfile->profileId();
  QPointer<SessionCoordinator> self(this);
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
    clearCurrentUser();
    setState(State::SignedOut);
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
    applyCurrentUser(*result.value);
    if (purpose == WhoAmIPurpose::RestoreExisting) {
      // Already durably stored; nothing more to persist.
      setState(State::SignedIn);
      return;
    }
    // Freshly obtained token: persist it before declaring signed in. The
    // admission check happens immediately before enqueuing the write.
    if (generation != m_generation) {
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
      clearCurrentUser();
      setState(State::SignedOut,
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
    clearCurrentUser();
    setState(State::SignedOut, result.diagnostic);
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
                   if (generation != self->m_generation) {
                     return;
                   }
                   if (deleteResult.outcome == TokenStoreOutcome::Success) {
                     self->clearCurrentUser();
                     self->setState(State::SignedOut);
                   } else {
                     self->m_retryAction = [self, generation, profileId] {
                       if (!self) {
                         return;
                       }
                       self->deleteRestoredUnauthorizedToken(generation,
                                                             profileId);
                     };
                     self->setState(State::SecureStorageUnavailable,
                                    deleteResult.diagnostic);
                   }
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
  setState(State::Authenticating);
  QPointer<SessionCoordinator> self(this);
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
  setState(State::Registering);
  QPointer<SessionCoordinator> self(this);
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
    clearCurrentUser();
    setState(State::SignedOut, result.diagnostic);
    return;
  }
  issueWhoAmI(generation, profileId, result.value->token,
              WhoAmIPurpose::FreshlyObtained);
}

// ─── Sign out ────────────────────────────────────────────────────────────

void SessionCoordinator::signOut() {
  if (m_state != State::SignedIn || !m_currentProfile.has_value()) {
    return;
  }
  cancelPendingAuthRequest();
  ++m_generation;
  const quint64 generation = m_generation;
  const QString profileId = m_currentProfile->profileId();
  QPointer<SessionCoordinator> self(this);
  enqueueTokenOp(profileId, TokenOpKind::Delete, QString(),
                 [self, generation](TokenStoreResult result) {
                   if (!self) {
                     return;
                   }
                   if (generation != self->m_generation) {
                     return;
                   }
                   self->handleSignOutDeletionResult(generation, result);
                 });
}

void SessionCoordinator::handleSignOutDeletionResult(
    quint64 generation, const TokenStoreResult &result) {
  if (result.outcome == TokenStoreOutcome::Success) {
    clearCurrentUser();
    setState(State::SignedOut);
    return;
  }
  // Never claim signed out while the token might still remain in the
  // secure store: report an explicit, retryable failure instead. The
  // signed-in identity is preserved (not cleared) so the UI can still show
  // who is signed in while the user retries.
  const QString profileId =
      m_currentProfile.has_value() ? m_currentProfile->profileId() : QString();
  m_retryAction = [this, generation, profileId] {
    if (generation != m_generation) {
      return;
    }
    QPointer<SessionCoordinator> self(this);
    enqueueTokenOp(profileId, TokenOpKind::Delete, QString(),
                   [self, generation](TokenStoreResult retryResult) {
                     if (!self) {
                       return;
                     }
                     if (generation != self->m_generation) {
                       return;
                     }
                     self->handleSignOutDeletionResult(generation, retryResult);
                   });
  };
  setState(State::SecureStorageUnavailable, result.diagnostic);
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
  queue.enqueue(TokenOp{kind, std::move(token), std::move(onComplete)});
  if (wasEmpty) {
    startFrontTokenOp(profileId);
  }
}

void SessionCoordinator::startFrontTokenOp(const QString &profileId) {
  const auto queueIt = m_tokenQueues.find(profileId);
  if (queueIt == m_tokenQueues.end() || queueIt->isEmpty()) {
    return;
  }
  const TokenOp &op = queueIt->head();
  const TokenOpKind kind = op.kind;
  const QString token = op.token;

  QPointer<SessionCoordinator> self(this);
  auto onResult = [self, profileId](TokenStoreResult result) {
    if (!self) {
      return;
    }
    const auto it = self->m_tokenQueues.find(profileId);
    if (it == self->m_tokenQueues.end() || it->isEmpty()) {
      return; // should not happen; defensive
    }
    TokenOp finishedOp = it->dequeue();
    const bool hasMore = !it->isEmpty();
    if (it->isEmpty()) {
      self->m_tokenQueues.erase(it);
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

} // namespace Arkham
