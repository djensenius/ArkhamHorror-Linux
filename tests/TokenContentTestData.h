#pragma once

// Shared canonical test-data table for the single, shared bearer-token
// "structurally valid content" grammar enforced by
// Arkham::isValidTokenContent() (see src/TokenValidation.h).
//
// This table is consumed by every test surface that exercises one of the
// trust boundaries that function guards, so the exact set of accepted/
// rejected token content can never silently diverge between them:
//   - tests/TokenStoreTests.cpp, which drives every row directly through
//     isValidTokenContent() in isolation, through
//     parseTokenEnvelope(serializeTokenEnvelope(identity, token)), and
//     through the real QtKeychainTokenStore::saveToken()/readToken()
//     round trip (fake keychain jobs, no real backend);
//   - tests/AuthClientTests.cpp, which drives every row through
//     NetworkAuthenticationClient::whoAmI()'s admission check (rejected
//     rows must create zero HTTP requests) and through
//     AuthToken::fromJson() decoding a `{"token": ...}` response body
//     (rejected rows must decode as AuthOutcome::MalformedPayload); and
//   - tests/SessionCoordinatorTests.cpp, which drives a rejected row
//     through a fake IAuthenticationClient's Success result to prove the
//     coordinator's own defense-in-depth check (independent of whichever
//     client implementation is wired in) rejects it before ever issuing
//     a /whoami request or a secure-store save.
//
// See src/TokenValidation.h for the exact, single-source-of-truth
// definition this table is a concrete instantiation of: non-empty visible
// ASCII (U+0021-U+007E inclusive) only.

#include <QString>
#include <QVector>

namespace Arkham::Test {

struct TokenContentRow {
  const char *name;
  QString token;
  bool expectValid;
};

// Table currently has 8 valid and 14 invalid rows (22 total). Every
// invalid row is definitively-invalid token content in the sense
// TokenValidation.h documents -- never a merely differently-shaped but
// still legitimate token -- so every consuming test may assert the exact
// same "rejected, and rejected safely (no request/no persistence/no
// secret echoed)" outcome for it.
inline QVector<TokenContentRow> tokenContentRows() {
  return {
      // ── Valid: non-empty visible ASCII, no spaces/controls ──────────
      {"valid-simple-alphanumeric", QStringLiteral("SIMPLEOPAQUETOKEN123"),
       true},
      {"valid-single-character", QStringLiteral("a"), true},
      {"valid-jwt-like-punctuation", QStringLiteral("abc123.DEF456-ghi_789"),
       true},
      {"valid-jwt-three-segments",
       QStringLiteral("eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjMifQ."
                      "dGhpc19pc19hX3NpZ25hdHVyZQ"),
       true},
      {"valid-opaque-legacy-hex",
       QStringLiteral("0123456789abcdef0123456789abcdef"), true},
      {"valid-all-visible-punctuation",
       QStringLiteral("!#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"), true},
      {"valid-mixed-case", QStringLiteral("MixedCase-Token_123"), true},
      {"valid-uuid-like-opaque",
       QStringLiteral("550e8400-e29b-41d4-a716-446655440000"), true},

      // ── Invalid: rejected by isValidTokenContent() ───────────────────
      {"empty", QString(), false},
      {"whitespace-only", QStringLiteral("   "), false},
      {"leading-space", QStringLiteral(" token"), false},
      {"trailing-space", QStringLiteral("token "), false},
      {"embedded-space", QStringLiteral("tok en"), false},
      {"embedded-tab", QStringLiteral("tok\ten"), false},
      {"embedded-newline", QStringLiteral("tok\nen"), false},
      {"embedded-carriage-return", QStringLiteral("tok\ren"), false},
      {"c0-control-null",
       QStringLiteral("tok") + QChar(QChar::Null) + QStringLiteral("en"),
       false},
      {"c1-control-nel",
       QStringLiteral("tok") + QChar(0x0085) + QStringLiteral("en"), false},
      {"ascii-delete",
       QStringLiteral("tok") + QChar(0x007F) + QStringLiteral("en"), false},
      {"nbsp", QStringLiteral("tok") + QChar(0x00A0) + QStringLiteral("en"),
       false},
      {"zero-width-space",
       QStringLiteral("tok") + QChar(0x200B) + QStringLiteral("en"), false},
      {"non-ascii-letter", QStringLiteral("tok\u00E9n"), false},
  };
}

} // namespace Arkham::Test
