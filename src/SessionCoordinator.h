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
// property, signal, QVariant, log, or debug operator. state() is always a
// fixed, static description of the enum value. diagnostic() is secret-free
// but not always static text: for transport/backend/malformed-payload
// failures it forwards the typed, already-secret-free diagnostic produced
// by IAuthenticationClient/ITokenStore/ICapabilityProbe (safe categories
// and static messages only, per their own contracts), so its exact string
// can vary with the underlying failure while never containing request/
// response bodies, credentials, or low-level exception text.
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
  // its own value. RecoverableFailure covers every retryable transport
  // failure (capability-probe network/HTTP/malformed errors and restored-
  // credential whoami transport errors); Incompatible and
  // SecureStorageUnavailable and ProfileStorageFailure are kept distinct
  // because their retry action and user-facing meaning differ.
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
  // Static, secret-free diagnostic for the current state. Empty unless the
  // current state represents a failure or an explanation is useful (e.g.
  // "sign-in was rejected"). Never contains a response body, request body,
  // password, token, or Authorization header.
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
  // success. Must be called exactly once per coordinator instance before
  // any other slot is meaningful.
  void start();

  // Switches to a different loaded profile. Persists the new selection
  // before using it; if persistence fails, the current profile/session is
  // left completely untouched and ProfileStorageFailure is reported (the
  // failed switch never silently "succeeds"). On successful persistence,
  // cancels any in-flight authentication request, discards the current
  // capability probe instance, advances the operation generation (so any
  // still-in-flight, uncancellable token-store completion for the previous
  // profile can never mutate state for the new one), and restarts the
  // capability probe for the new profile.
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

  // Only usable while SignedIn. Cancels any in-flight session network work,
  // enqueues a durable token deletion for the current profile, and becomes
  // SignedOut only once that deletion succeeds. On deletion failure, the
  // signed-in identity is preserved and the coordinator reports
  // SecureStorageUnavailable with an actionable retry(); it never claims
  // SignedOut while a token might still remain in the secure store.
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

  void handleSignOutDeletionResult(quint64 generation,
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
  void enqueueTokenOp(const QString &profileId, TokenOpKind kind, QString token,
                      ITokenStore::ResultCallback onComplete);
  void startFrontTokenOp(const QString &profileId);

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

  // Bumped on every profile switch and on signOut(); captured by value in
  // every asynchronous completion lambda so a stale completion (from a
  // profile that is no longer current, or from a session that has since
  // been signed out) can never mutate visible state. Token-store
  // operations still run to completion when stale (see enqueueTokenOp
  // comment); only the *reaction* to their result is generation-gated.
  quint64 m_generation{0};

  std::function<void()> m_retryAction;

  std::unique_ptr<ICapabilityProbe> m_probe;
  AuthRequestHandle m_pendingAuthHandle;

  QHash<QString, QQueue<TokenOp>> m_tokenQueues;
};

} // namespace Arkham
