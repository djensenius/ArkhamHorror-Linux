#pragma once

#include <QString>

namespace Arkham {

// The single, shared definition of "structurally valid bearer token
// content", used at every trust boundary a token crosses: decoding an
// authentication response (AuthModels.cpp's AuthToken::fromJson()),
// admitting a token into an outgoing whoAmI() request/Authorization header
// (NetworkAuthenticationClient), a coordinator's own handling of an
// AuthenticationClient result before ever issuing a request or a durable
// save (SessionCoordinator::handleFreshTokenResult()), a secure-store save
// (QtKeychainTokenStore::saveToken()), and the durable envelope's own
// writer/reader (TokenEnvelope.cpp's serializeTokenEnvelope()/
// parseTokenEnvelope()).
//
// Requiring every one of these call sites to route through this exact
// function -- rather than each independently reimplementing "looks like a
// token" -- is what guarantees the writer can never persist a token its own
// reader would reject: a value saveToken() accepts is, by construction, a
// value parseTokenEnvelope() will also accept on the very next read, and a
// value the coordinator/whoAmI() ever admits into a real request is
// guaranteed to be exactly the same value a fresh restore would later
// re-validate identically.
//
// Accepts only non-empty "visible ASCII": every QChar's UTF-16 code unit
// must fall in the printable, non-space ASCII range U+0021 ("!") through
// U+007E ("~") inclusive. Rejected by construction (there is no allow-list
// of individually-named exclusions to keep in sync):
//   - an entirely empty token;
//   - the ASCII space character (U+0020), anywhere in the token (leading,
//     trailing, or embedded) -- this also covers a whitespace-only token;
//   - every C0 control character (U+0000-U+001F), including tab, LF, and
//     CR -- an embedded CR/LF must never reach an
//     `Authorization: Token <token>` header verbatim, since that would be
//     an HTTP header/request-splitting injection;
//   - the ASCII DEL control character (U+007F);
//   - every character above U+007E, which covers every C1 control
//     character, every Unicode whitespace character beyond plain ASCII
//     space (e.g. NBSP U+00A0, the various U+2000-range spaces), every
//     zero-width/format character (e.g. ZWSP U+200B, ZWNJ/ZWJ, BOM), and
//     any other non-ASCII code point -- all of which either have no
//     canonical meaning in this backend's token grammar or would be
//     lossily/ambiguously converted when placed into an HTTP header via
//     toUtf8()/toLatin1().
//
// Deliberately does NOT validate any particular internal structure (e.g.
// an exact three-segment, dot-separated JWT compact-serialization shape):
// the backend contract permits any opaque bearer token (including
// self-hosted/legacy servers that may not issue JWTs), so this function
// only enforces the character-level grammar every accepted token -- JWT or
// otherwise -- must satisfy, never a specific token format.
[[nodiscard]] bool isValidTokenContent(const QString &token);

} // namespace Arkham
