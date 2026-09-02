#pragma once

#include <QString>
#include <QStringView>
#include <QUrl>

namespace Arkham {

// Returns true iff |hostText|, taken verbatim with no QUrl-style host
// normalisation applied, is EXACTLY one of the three canonical loopback
// spellings this policy recognises:
//   - "localhost" (ASCII-only case-insensitive; no trailing dot, no
//     subdomain -- see the ASCII-only-comparison note below);
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
//
// The "localhost" comparison is deliberately ASCII-only, not
// QString::compare(..., Qt::CaseInsensitive): Qt's case-insensitive
// comparison performs full Unicode case folding, which equates code points
// well beyond plain ASCII 'A'-'Z' with their ASCII lowercase counterparts
// -- most notably U+017F LATIN SMALL LETTER LONG S ("ſ"), which Unicode
// case-folds to plain ASCII 's', making the raw text "localhoſt" compare
// equal to "localhost" under Qt::CaseInsensitive (confirmed empirically).
// Any hostText containing a code point outside 7-bit ASCII is therefore
// rejected outright before any letter-case comparison is attempted, so no
// Unicode look-alike, fullwidth form, or other case-folding trick can ever
// reach (let alone pass) the "localhost" branch. The IPv4 and "::1"
// branches are unaffected by this class of bug on their own merits: the
// IPv4 octet parser already rejects any non-ASCII-digit code point
// (isStrictDecimalOctet), and the "::1" comparison already uses
// Qt::CaseSensitive (an exact code-unit comparison with no folding at
// all), so neither one needs the same guard to stay strictly ASCII.
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
// - Any other scheme: false (fail closed). validateCustomUrl() only ever
//   calls this after already validating the scheme is "http" or "https",
//   so this case is not reachable through that call site today; it exists
//   so a future caller of this public helper cannot accidentally get
//   "allowed" back for a scheme this policy was never designed to
//   classify.
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

// Cumulative-review finding (PR #18): returns true iff the RAW authority
// (userinfo@host:port, as literally spelled in |rawUrlText|, before any
// QUrl normalisation) is unambiguous and well-formed enough that this
// project's later use of a QUrl-parsed host()/port() cannot silently
// diverge from what a human reading the raw input text would expect --
// regardless of scheme. Unlike isCleartextAuthAllowedForRawInput() (which
// only matters for the http-loopback exception), this check is a blanket
// gate that MUST run for https just as much as for http: QUrl silently
// normalises escaped/percent-encoded hosts, folds certain Unicode
// look-alike characters, and canonicalises alternate numeric IP spellings
// (octal/hex/shortened) at parse time, all before any later code ever
// sees the result -- an https URL is just as capable of smuggling one of
// these ambiguities as an http one, so it must not be exempted from this
// check merely because it is not the loopback-cleartext exception this
// file otherwise concerns itself with.
//
// Rejects (returns false) if the raw authority:
//   - has no identifiable authority section at all (fail closed);
//   - contains a userinfo-like '@' delimiter;
//   - has an empty, or otherwise malformed/out-of-range, port (see
//     isValidRawPort() above -- applies identically here);
//   - has a host substring that is empty, contains a '%' (percent-escape;
//     never legitimate in this project's own configured host text),
//     contains any non-ASCII code point (no Unicode/IDNA hostnames are
//     accepted by this policy at all -- see the class-level rationale in
//     isCanonicalLoopbackHostText()'s doc comment for why ASCII-only
//     comparisons are required to avoid Unicode case-fold/look-alike
//     bypasses), contains a backslash or any control character, or is an
//     "alternate numeric IP" spelling (a host composed only of digits,
//     dots, and/or hex letters/'x' that is not itself a strict, canonical
//     four-octet dotted-decimal IPv4 literal -- e.g. a shortened form
//     like "127.1", a bare 32-bit integer, or an octal/hex-prefixed
//     octet); or
//   - is a bracketed IPv6 literal whose inner text contains any character
//     other than a hex digit, ':', or '.' (the last permitted only for an
//     IPv4-mapped/embedded form).
[[nodiscard]] bool
rawAuthorityHostIsUnambiguousAndWellFormed(const QString &rawUrlText);

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
