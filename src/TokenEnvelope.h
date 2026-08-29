#pragma once

#include <QString>

namespace Arkham {

// Discriminated result of parsing a raw secure-store payload previously
// read back for a profile's token entry. See serializeTokenEnvelope() for
// the paired writer and the exact wire format both sides agree on, and
// QtKeychainTokenStore::readToken() for how each outcome maps onto
// TokenStoreOutcome.
enum class TokenEnvelopeParseOutcome {
  Parsed, ///< A structurally valid version-1 envelope; endpointIdentity and
          ///< token are populated. The caller is still responsible for
          ///< comparing endpointIdentity against whatever endpoint it
          ///< actually expects -- parsing success alone does not imply the
          ///< token is bound to the right server.
  LegacyUnbound, ///< The raw payload does not begin with this format's
                 ///< recognised magic prefix at all -- i.e. a token stored
                 ///< by a release that predates endpoint binding (or any
                 ///< other foreign/unrecognised data). Its origin cannot be
                 ///< proven, so it is never trusted or exposed as a usable
                 ///< token.
  Malformed,     ///< The payload begins with this format's magic prefix but
                 ///< fails strict structural validation (bad/unsupported
                 ///< version, corrupt length field, truncated content, or a
                 ///< token portion that is empty, whitespace-only, has
                 ///< leading/trailing whitespace, or contains any control
                 ///< character -- see parseTokenEnvelope()'s own comment on
                 ///< why this grammar check lives here rather than being
                 ///< left to a caller). Never trusted or exposed.
};

struct TokenEnvelopeParseResult {
  TokenEnvelopeParseOutcome outcome{TokenEnvelopeParseOutcome::Malformed};
  // Populated only when outcome == Parsed; both are empty for any other
  // outcome so a caller can never accidentally read a partially-parsed
  // fragment.
  QString endpointIdentity;
  QString token;
};

// Serializes |token| bound to |endpointIdentity| (see
// ServerProfile::credentialEndpointIdentity()) into the exact opaque
// string that is persisted as a single secure-store entry's payload.
// |endpointIdentity| and |token| are both assumed non-empty -- callers
// are expected to have already rejected blank input (see
// QtKeychainTokenStore's own validation) before reaching this function.
//
// Wire format (version 1):
//   "AHKV1:" <decimal endpointIdentity length> ":" <endpointIdentity,
//   exactly that many QChar code units> <token, the remainder of the
//   string>.
//
// Every field boundary is determined purely by the explicit decimal
// length prefix -- never by searching for a delimiter character -- so
// nothing inside either field's content (including any ':' or other
// punctuation the identity or token may happen to contain) can ever be
// misread as a boundary, and no escaping scheme is required.
[[nodiscard]] QString serializeTokenEnvelope(const QString &endpointIdentity,
                                             const QString &token);

// Strictly parses a raw secure-store payload previously produced by
// serializeTokenEnvelope() -- or, for TokenEnvelopeParseOutcome::
// LegacyUnbound, a pre-envelope raw token predating this format, or any
// other foreign/tampered data. Never throws; always returns exactly one
// of the three discriminated outcomes above, with no partial/ambiguous
// result. |raw| must already be known non-empty/non-whitespace-only --
// callers are expected to have handled that degenerate case themselves
// (see QtKeychainTokenStore::readToken(), which maps an entirely blank
// stored value to a BackendError before this function is ever called).
[[nodiscard]] TokenEnvelopeParseResult parseTokenEnvelope(const QString &raw);

} // namespace Arkham
