#pragma once

#include <QString>
#include <functional>

namespace Arkham {

// Discriminated outcome of a single token-store operation.
enum class TokenStoreOutcome {
  Success,      ///< Operation completed; for readToken() the token field of
                ///< the result is populated.
  NotFound,     ///< No token is stored for this profile.  Distinct from a
                ///< secure-store failure: this is an expected, non-error
                ///< state (e.g. the user has never signed in).
  AccessDenied, ///< The secure store exists but is locked, or the user or OS
                ///< policy denied access to this entry.
  Unavailable,  ///< No supported secure-storage backend is available in this
                ///< session (e.g. no Secret Service/KWallet provider on this
                ///< Linux desktop, including SteamOS Gaming Mode).  Never a
                ///< silent success; the caller must surface this explicitly.
  BackendError, ///< The secure-store backend reported another failure.
  InvalidInput, ///< The supplied profile ID is not a valid non-nil UUID, or
                ///< (for saveToken()) the token is empty or whitespace-only.
                ///< Rejected before any secure-store operation is attempted.
};

// Result of a single ITokenStore operation.
//
// diagnostic is always a static, human-readable, secret-free description of
// the outcome.  It never contains the token value, the profile ID's origin
// (e.g. a server URL), or any raw backend-reported error text, so it is safe
// to log or display.
struct TokenStoreResult {
  TokenStoreOutcome outcome{TokenStoreOutcome::BackendError};
  QString diagnostic;
  // Populated only when outcome == Success and the operation was a read.
  QString token;
};

// Injectable, asynchronous secure token store keyed by canonical
// ServerProfile::profileId().
//
// Contract:
//   - Exactly one token is retained per profile ID.  Hosted and custom
//     profiles use distinct, non-colliding profile IDs (see ServerProfile),
//     so they can never read or overwrite each other's token.
//   - Every callback is invoked exactly once, asynchronously (never
//     reentrantly from within the call that requested the operation), while
//     the store instance is alive.
//   - Destroying the store before an operation's callback has fired
//     suppresses that callback; no stale callback is ever invoked after
//     destruction.
//   - Implementations must never fall back to QSettings, a file, an
//     environment variable, or any other plaintext storage.  Unavailable or
//     locked secure storage is reported via TokenStoreOutcome::Unavailable /
//     AccessDenied; it is never treated as "no token" (NotFound) and never
//     silently degrades to an insecure store.
//   - profileId is validated as a non-nil UUID and token (on save) as
//     non-empty after trimming; invalid input yields
//     TokenStoreOutcome::InvalidInput without touching the backing store.
class ITokenStore {
public:
  using ResultCallback = std::function<void(TokenStoreResult)>;

  virtual ~ITokenStore() = default;

  // Reads the token stored for |profileId|.  Result outcome is Success with
  // TokenStoreResult::token populated, NotFound if no entry exists, or a
  // failure outcome otherwise.
  virtual void readToken(const QString &profileId, ResultCallback callback) = 0;

  // Stores |token| for |profileId|, replacing any existing entry.
  virtual void saveToken(const QString &profileId, const QString &token,
                         ResultCallback callback) = 0;

  // Deletes the token stored for |profileId|.  Deleting a profile with no
  // stored token is reported as Success (idempotent), not NotFound.
  virtual void deleteToken(const QString &profileId,
                           ResultCallback callback) = 0;
};

} // namespace Arkham
