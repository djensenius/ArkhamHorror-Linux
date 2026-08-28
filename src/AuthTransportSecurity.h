#pragma once

#include <QString>
#include <QStringView>
#include <QUrl>

namespace Arkham {

// Returns true iff |hostText|, taken verbatim with no QUrl-style host
// normalisation applied, is EXACTLY one of the three canonical loopback
// spellings this policy recognises:
//   - "localhost" (case-insensitive; no trailing dot, no subdomain);
//   - a canonical four-component dotted-decimal IPv4 literal whose first
//     component is the literal text "127" and whose remaining three
//     components are each a plain decimal 0-255 with no leading zero, no
//     octal/hex/base-0 syntax, and no shortened (fewer than four
//     component) form; or
//   - the exact unscoped native IPv6 literal "::1" (no zone/scope id, no
//     expanded/alternate spelling, no IPv4-mapped form).
//
// This is a strict allow-list match on literal text, not a numeric/address
// comparison: any input that is not spelled EXACTLY as above is rejected,
// even if an address library would resolve it to the same loopback address
// (e.g. "127.1", "0177.0.0.1", "0x7f.0.0.1", "2130706433", "127.000.000.001",
// "0:0:0:0:0:0:0:1", "::ffff:127.0.0.1", "::1%eth0" are all rejected).
[[nodiscard]] bool isCanonicalLoopbackHostText(QStringView hostText);

// Returns true if cleartext HTTP traffic that may carry a password or bearer
// token is permitted to the host lexically named in |rawUrlText|, given its
// already-validated |scheme| ("http" or "https").
//
// - "https": always true (any host permitted).
// - "http": true only if the RAW, non-QUrl-normalised authority-host
//   substring extracted from |rawUrlText| is exactly one of the canonical
//   loopback spellings recognised by isCanonicalLoopbackHostText(). Any
//   userinfo-like construct in the raw authority (an '@' present) causes an
//   unconditional false, independent of how QUrl itself would later parse
//   userinfo/host.
// - If the raw authority contains a ":" after the host (or after a
//   bracketed IPv6 literal's closing "]"), everything following that ":"
//   must be a syntactically valid, in-range port: one or more ASCII
//   digits only (no sign, no percent-escapes, no further ":" separators,
//   no control characters), with a decimal value in 1..65535. An empty
//   port (e.g. "localhost:", "[::1]:") or port 0 (e.g. "localhost:0") is
//   rejected even though QUrl itself treats both as syntactically valid --
//   an empty or all-zero port is never a meaningful destination and must
//   not be silently treated the same as "no port specified".
//
// Why this must run against the RAW pre-QUrl text: QUrl itself silently
// canonicalises many alternate, ambiguous numeric encodings of the loopback
// address (e.g. "127.1", octal/hex octets, a bare 32-bit integer) into the
// single string "127.0.0.1" at parse time, irrecoverably destroying the
// original spelling. By the time a QUrl's host() is inspected, there is no
// way to tell whether the caller actually typed the canonical form or one of
// those alternate encodings. Callers MUST therefore invoke this function
// against the original, not-yet-QUrl-parsed input text, at the earliest
// possible point (URL validation during ServerProfile construction) -- not
// against an already-parsed QUrl's host(), which by then may already have
// laundered an unsafe spelling into an accepted-looking canonical string.
[[nodiscard]] bool isCleartextAuthAllowedForRawInput(const QString &scheme,
                                                     const QString &rawUrlText);

// Returns true if it is safe to send authentication traffic (a request or
// response that may carry a password or bearer token) to |url| without
// encryption.
//
// Permitted:
//   - https, for any host.
//   - http, but only to a loopback host whose QUrl-parsed host() text is
//     exactly one of the canonical spellings recognised by
//     isCanonicalLoopbackHostText() (exactly "localhost", canonical
//     127.x.y.z dotted-decimal, or exactly "::1"). This narrow exception
//     exists solely to preserve local development/self-hosting without ever
//     allowing a LAN or public host to receive a password or bearer token
//     in cleartext.
// Rejected:
//   - Any other scheme.
//   - http to any non-canonical-loopback host, including a hostname that
//     merely resembles a loopback address without being one (e.g.
//     "localhost.evil.com", "127.0.0.1.evil.com" -- these are different DNS
//     names, not the local machine).
//   - Any URL with a userinfo component (credentials embedded in the URL),
//     regardless of scheme or host: userinfo must never influence which
//     host is treated as "the" host for this decision.
//
// NOTE: this function operates on an already-parsed QUrl, so -- per the
// isCleartextAuthAllowedForRawInput() doc comment above -- it cannot itself
// distinguish an originally-ambiguous loopback spelling (e.g. "127.1") from
// one typed in canonical form, because QUrl has already normalised both to
// the same host() string by the time this runs. The authoritative rejection
// of non-canonical spellings happens earlier, in validateCustomUrl(), which
// calls isCleartextAuthAllowedForRawInput() against the pristine raw input
// before any QUrl-based normalisation occurs. This function remains a
// defense-in-depth request-time check: ServerProfile no longer exposes any
// public constructor that can carry an unvalidated URL (its only public
// constructor is the argument-less default one, which is always invalid),
// and NetworkAuthenticationClient additionally rejects any profile whose
// ServerProfile::hasValidatedProvenance() is false, so this check only
// matters if some future internal code path ever bypassed those structural
// guarantees -- and it still correctly rejects LAN/public hosts and
// non-matching text over http in that case.
//
// This function has no knowledge of redirects: callers that may follow
// redirects must not rely on this check alone to keep a redirected request
// secure. NetworkAuthenticationClient never follows redirects (every 3xx is
// a typed failure), so this exception can never be leveraged to smuggle a
// loopback request to an insecure remote origin.
[[nodiscard]] bool isSecureOrLoopbackAuthTransport(const QUrl &url);

} // namespace Arkham
