#pragma once

#include <QString>
#include <functional>

namespace Arkham {

// Discriminated outcome of a single token-store operation.
enum class TokenStoreOutcome {
  Success,         ///< Operation completed; for readToken() the token field of
                   ///< the result is populated.
  NotFound,        ///< No token is stored for this profile.  Distinct from a
                   ///< secure-store failure: this is an expected, non-error
                   ///< state (e.g. the user has never signed in).
  AccessDenied,    ///< The secure store exists but is locked, or the user or OS
                   ///< policy denied access to this entry.
  Unavailable,     ///< No supported secure-storage backend is available in this
                   ///< session (e.g. no Secret Service/KWallet provider on this
                   ///< Linux desktop, including SteamOS Gaming Mode).  Never a
                   ///< silent success; the caller must surface this explicitly.
  BackendError,    ///< The secure-store backend reported another failure.
  InvalidInput,    ///< The supplied profile ID is not a valid non-nil UUID, the
                   ///< endpoint identity is empty, or (for saveToken()) the
                   ///< token is empty or whitespace-only. Rejected before any
                   ///< secure-store operation is attempted.
  BindingMismatch, ///< readToken() found a structurally valid, versioned
                   ///< entry, but it is durably bound (see saveToken()) to a
                   ///< DIFFERENT endpoint than the one the caller currently
                   ///< expects for this profile ID. This means the same
                   ///< profile UUID's persisted server address changed (or
                   ///< the UUID was reused by a different profile) since the
                   ///< credential was saved. No token is ever returned for
                   ///< this outcome; it must be treated as cleanup-required
                   ///< (delete, then allow fresh authentication), never as a
                   ///< candidate for /whoami.
  LegacyUnbound,   ///< readToken() found a raw stored value that does not
                   ///< carry this store's endpoint-binding envelope at all
                   ///< (i.e. it predates the versioned envelope format, or is
                   ///< otherwise foreign data). Its origin/endpoint cannot be
                   ///< proven, so -- exactly like BindingMismatch -- no token
                   ///< is ever returned; it must be treated as
                   ///< cleanup-required, never auto-migrated or trusted.
  Malformed,       ///< readToken() found an entry that either declares this
                   ///< store's envelope format but fails strict structural
                   ///< parsing (unsupported version, corrupt framing,
                   ///< truncated content, or a token portion that is empty,
                   ///< whitespace-only, has leading/trailing whitespace, or
                   ///< contains a control character -- see
                   ///< TokenEnvelope.h's own doc comment on why this
                   ///< grammar check is included here), or is an entirely
                   ///< blank/whitespace-only raw payload that predates any
                   ///< envelope framing at all. Both are definitively
                   ///< corrupt/tampered data, never a transient backend
                   ///< issue. No token is ever returned; treated as
                   ///< cleanup-required exactly like BindingMismatch/
                   ///< LegacyUnbound.
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
// Since PR #17, every stored token is wrapped in a durable, versioned,
// endpoint-bound envelope (see TokenEnvelope.h): saveToken() records not
// just the token but the canonical credential-endpoint identity (see
// ServerProfile::credentialEndpointIdentity()) it was saved for, and
// readToken() requires the caller to state the endpoint identity it
// currently expects, verifying the stored envelope against it before ever
// returning a token. This protects a profile UUID's credential across
// profile-store reloads, profile removal/re-addition with a reused UUID,
// and process restarts -- none of which any purely in-memory bookkeeping
// could detect, since the durable envelope itself is the sole source of
// truth for which endpoint a stored credential belongs to.
//
// Contract:
//   - Exactly one token entry is retained per profile ID. Hosted and custom
//     profiles use distinct, non-colliding profile IDs (see ServerProfile),
//     so they can never read or overwrite each other's entry.
//   - Every callback is invoked exactly once, asynchronously (never
//     reentrantly from within the call that requested the operation), while
//     the store instance is alive.
//   - Destroying the store before an operation's callback has fired
//     suppresses that callback; no stale callback is ever invoked after
//     destruction.
//   - Implementations must never fall back to QSettings, a file, an
//     environment variable, or any other plaintext storage. Unavailable or
//     locked secure storage is reported via TokenStoreOutcome::Unavailable /
//     AccessDenied; it is never treated as "no token" (NotFound) and never
//     silently degrades to an insecure store.
//   - profileId is validated as a non-nil UUID, endpointIdentity as
//     non-empty, and token (on save) as non-empty after trimming; invalid
//     input yields TokenStoreOutcome::InvalidInput without touching the
//     backing store.
//   - A mismatched, legacy (pre-envelope), or malformed entry NEVER yields a
//     token: the token field of the result is left empty for
//     BindingMismatch, LegacyUnbound, and Malformed exactly as it is for any
//     other failure outcome.
class ITokenStore {
public:
  using ResultCallback = std::function<void(TokenStoreResult)>;

  virtual ~ITokenStore() = default;

  // Reads the token stored for |profileId|, verified against
  // |expectedEndpointIdentity| (see ServerProfile::
  // credentialEndpointIdentity()). Result outcome is Success (with
  // TokenStoreResult::token populated) only when a structurally valid
  // entry exists AND its durably bound endpoint identity matches
  // |expectedEndpointIdentity| exactly; NotFound if no entry exists at
  // all; BindingMismatch/LegacyUnbound/Malformed if an entry exists but
  // cannot be trusted for this endpoint (see TokenStoreOutcome); or
  // another failure outcome otherwise. |expectedEndpointIdentity| must be
  // non-empty.
  virtual void readToken(const QString &profileId,
                         const QString &expectedEndpointIdentity,
                         ResultCallback callback) = 0;

  // Stores |token| for |profileId|, durably bound to |endpointIdentity|
  // (see ServerProfile::credentialEndpointIdentity()), replacing any
  // existing entry. |endpointIdentity| must be non-empty.
  virtual void saveToken(const QString &profileId, const QString &token,
                         const QString &endpointIdentity,
                         ResultCallback callback) = 0;

  // Deletes the token stored for |profileId|, regardless of what endpoint
  // (if any) it is bound to. Deleting a profile with no stored token is
  // reported as Success (idempotent), not NotFound.
  virtual void deleteToken(const QString &profileId,
                           ResultCallback callback) = 0;
};

} // namespace Arkham
