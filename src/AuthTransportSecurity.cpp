#include "AuthTransportSecurity.h"

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// Matches a single strict decimal octet: 0-255, no leading zero (except the
// literal single-character "0"), no sign, no non-digit characters.
bool isStrictDecimalOctet(QStringView text) {
  if (text.isEmpty() || text.size() > 3) {
    return false;
  }
  // Ascii-only digits: reject unicode digit look-alikes that QString::toInt
  // might otherwise accept.
  for (const QChar c : text) {
    if (c.unicode() < u'0' || c.unicode() > u'9') {
      return false;
    }
  }
  if (text.size() > 1 && text[0] == u'0') {
    return false; // Leading zero, e.g. "01", "007".
  }
  bool ok = false;
  const int value = text.toString().toInt(&ok);
  return ok && value >= 0 && value <= 255;
}

// Matches exactly "A.B.C.D" where A, B, C, D are each strict decimal
// octets (see isStrictDecimalOctet) -- i.e. ANY canonical, unambiguous
// four-octet dotted-decimal IPv4 literal, regardless of its value (not
// restricted to the 127.0.0.0/8 loopback range). Used to distinguish a
// legitimate, unambiguous dotted-decimal IPv4 host from an "alternate
// numeric IP" spelling (shortened form, bare 32-bit integer, octal/hex
// octet) that this project's raw-authority policy rejects outright for
// every host, not just loopback ones.
bool isStrictDottedDecimalIPv4Text(QStringView text) {
  const qsizetype firstDot = text.indexOf(u'.');
  if (firstDot < 0) {
    return false;
  }
  const QStringView octetA = text.left(firstDot);
  const QStringView afterA = text.mid(firstDot + 1);
  const qsizetype secondDot = afterA.indexOf(u'.');
  if (secondDot < 0) {
    return false;
  }
  const QStringView octetB = afterA.left(secondDot);
  const QStringView afterB = afterA.mid(secondDot + 1);
  const qsizetype thirdDot = afterB.indexOf(u'.');
  if (thirdDot < 0) {
    return false;
  }
  const QStringView octetC = afterB.left(thirdDot);
  const QStringView octetD = afterB.mid(thirdDot + 1);
  return isStrictDecimalOctet(octetA) && isStrictDecimalOctet(octetB) &&
         isStrictDecimalOctet(octetC) && isStrictDecimalOctet(octetD);
}

// Matches exactly "127.A.B.C" where A, B, C are each strict decimal octets
// (see isStrictDecimalOctet) -- i.e. the canonical, unambiguous dotted-
// decimal spelling of a 127.0.0.0/8 loopback address, and nothing else
// (no shortened forms, no octal/hex, no extra components).
bool isCanonicalLoopbackIPv4Text(QStringView text) {
  static constexpr QStringView kPrefix = u"127.";
  return text.startsWith(kPrefix) && isStrictDottedDecimalIPv4Text(text);
}

// Extracts the raw authority substring (userinfo@host:port, as literally
// spelled in the input, before any QUrl normalisation) from a "scheme://..."
// URL string, stopping at the first path/query/fragment delimiter. Returns
// an empty, default-constructed QStringView if no "://" separator is found.
QStringView extractRawAuthority(QStringView rawUrlText) {
  static constexpr QStringView kSeparator = u"://";
  const qsizetype sepIdx = rawUrlText.indexOf(kSeparator);
  if (sepIdx < 0) {
    return {};
  }
  const qsizetype authorityStart = sepIdx + kSeparator.size();
  QStringView rest = rawUrlText.mid(authorityStart);
  qsizetype end = rest.size();
  for (const QChar delim : {u'/', u'?', u'#'}) {
    const qsizetype idx = rest.indexOf(delim);
    if (idx >= 0 && idx < end) {
      end = idx;
    }
  }
  return rest.left(end);
}

// Returns true iff |text| is a syntactically valid, in-range port: one or
// more ASCII digits (RFC 3986 port = *DIGIT), no sign, no percent-escapes,
// no other non-digit characters (which also rules out further ":"
// separators or embedded control characters), representing a decimal value
// in 1..65535 inclusive. Unlike RFC 3986's own grammar (which technically
// permits an empty port, meaning "use the default"), this policy requires
// a non-empty, in-range port whenever a ":" is present at all: an empty or
// zero port (e.g. "host:", "host:0") is never a meaningful destination and
// must not be silently treated as "no port specified".
bool isValidRawPort(QStringView text) {
  if (text.isEmpty()) {
    return false;
  }
  for (const QChar c : text) {
    if (c.unicode() < u'0' || c.unicode() > u'9') {
      return false;
    }
  }
  bool ok = false;
  const qlonglong value = text.toString().toLongLong(&ok);
  return ok && value >= 1 && value <= 65535;
}

// Extracts the raw host substring from a raw authority substring (as
// produced by extractRawAuthority), handling a bracketed IPv6 literal or a
// bare host optionally followed by ":port". Returns an empty QStringView on
// any ambiguity (e.g. an unterminated "[", or a ":" not followed by a
// syntactically valid, in-range port -- see isValidRawPort()).
QStringView extractRawHostFromAuthority(QStringView authority) {
  if (authority.startsWith(u'[')) {
    const qsizetype closeIdx = authority.indexOf(u']');
    if (closeIdx < 0) {
      return {};
    }
    // An IPv6-literal authority's closing bracket must be immediately
    // followed by either nothing (no port) or ":" plus a valid port -- any
    // other trailing text (e.g. "[::1]evil", "[::1]:9000evil", "[::1]:",
    // "[::1]:evil") is not a valid authority at all and must never be
    // silently truncated into what looks like a safe, exact "::1" host.
    // Rejecting it here (returning an empty view, which the caller treats
    // as "fail closed") closes that bypass.
    const QStringView remainder = authority.mid(closeIdx + 1);
    if (!remainder.isEmpty() &&
        (!remainder.startsWith(u':') || !isValidRawPort(remainder.mid(1)))) {
      return {};
    }
    return authority.mid(1, closeIdx - 1);
  }
  const qsizetype colonIdx = authority.indexOf(u':');
  if (colonIdx < 0) {
    return authority; // No ":" at all: no port, nothing further to check.
  }
  // A bare host's ":" must also be followed by a valid port (e.g.
  // "localhost:", "localhost:evil", "localhost:%39", and
  // "localhost::9000" are all rejected here) -- otherwise the host text
  // before the ":" (e.g. the exact string "localhost") could be silently
  // treated as safe while the full authority is not actually a valid
  // "host[:port]" form at all.
  if (!isValidRawPort(authority.mid(colonIdx + 1))) {
    return {};
  }
  return authority.left(colonIdx);
}

// Returns true iff every code unit in |text| is a plain 7-bit ASCII
// character (U+0000-U+007F). Used to guard the "localhost" comparison
// below: Qt::CaseInsensitive performs full Unicode case folding, which can
// equate a non-ASCII look-alike letter with an ASCII one (see
// isAsciiCaseInsensitiveLocalhost() below), so any hostText containing a
// non-ASCII code point must be rejected before that comparison is even
// attempted.
bool isAsciiOnly(QStringView text) {
  for (const QChar c : text) {
    if (c.unicode() > 0x7F) {
      return false;
    }
  }
  return true;
}

// Returns true iff |text| is EXACTLY the ASCII string "localhost", using
// ASCII-only case folding (plain 'A'-'Z' <-> 'a'-'z') rather than
// QString::compare(..., Qt::CaseInsensitive). Qt's case-insensitive
// comparison performs full Unicode case folding, which equates code
// points well beyond ASCII with their lowercase counterparts -- most
// notably U+017F LATIN SMALL LETTER LONG S ("ſ"), which case-folds to
// plain ASCII 's', so the raw text "localhoſt" would otherwise compare
// equal to "localhost" under Qt::CaseInsensitive (confirmed empirically).
// Rejecting any non-ASCII code point outright, before any letter-case
// comparison, closes that bypass and any similar one (fullwidth forms,
// other Unicode case-fold collisions, etc.) at the source, rather than
// enumerating look-alikes one at a time.
bool isAsciiCaseInsensitiveLocalhost(QStringView text) {
  static constexpr QStringView kLocalhost = u"localhost";
  if (text.size() != kLocalhost.size() || !isAsciiOnly(text)) {
    return false;
  }
  for (qsizetype i = 0; i < text.size(); ++i) {
    ushort c = text[i].unicode();
    if (c >= u'A' && c <= u'Z') {
      c = static_cast<ushort>(c + (u'a' - u'A'));
    }
    if (c != kLocalhost[i].unicode()) {
      return false;
    }
  }
  return true;
}

// Returns true iff every code unit in |text| is either a C0 control
// character (U+0000-U+001F), DELETE (U+007F), a C1 control
// (U+0080-U+009F), or a backslash -- i.e. any character this project's
// raw-authority host policy never permits, regardless of position.
// Checked separately from isAsciiOnly() (backslash is plain ASCII, and a
// non-ASCII code point is rejected on its own merits by that check) so
// each rejection reason stays independently testable.
bool containsBackslashOrControlCharacter(QStringView text) {
  for (const QChar c : text) {
    if (c == u'\\' || c.category() == QChar::Other_Control) {
      return true;
    }
  }
  return false;
}

// Returns true iff |component| (one '.'-separated label from a candidate
// numeric host, with the dots already stripped by the caller) is itself a
// literal that SOME real numeric-address parser (inet_aton/strtoul-with-
// base-0-style rules, as used by many libc/proxy/URL implementations)
// would actually interpret as a number: either a run of one or more
// plain ASCII decimal digits (covers both ordinary decimal and, via a
// leading zero, octal interpretation), or an explicit "0x"/"0X" prefix
// followed by one or more hex digits. An empty component (from a leading,
// trailing, or doubled '.') is never numeric.
bool numericIpAddressComponentText(QStringView component) {
  if (component.isEmpty()) {
    return false;
  }
  if (component.size() >= 3 && (component[0] == u'0') &&
      (component[1] == u'x' || component[1] == u'X')) {
    for (qsizetype i = 2; i < component.size(); ++i) {
      const ushort u = component[i].unicode();
      const bool isHexDigit = (u >= u'0' && u <= u'9') ||
                              (u >= u'a' && u <= u'f') ||
                              (u >= u'A' && u <= u'F');
      if (!isHexDigit) {
        return false;
      }
    }
    return true; // size >= 3 guarantees at least one hex digit after "0x".
  }
  for (const QChar c : component) {
    const ushort u = c.unicode();
    if (!(u >= u'0' && u <= u'9')) {
      return false;
    }
  }
  return true;
}

// Returns true iff |text|, split on '.', is composed ENTIRELY of
// components each independently recognisable as a genuine numeric
// literal (see numericIpAddressComponentText) -- i.e. |text| really is
// some alternate numeric IP spelling (a shortened form like "127.1", a
// bare 32-bit integer like "2130706433", an octal-looking octet like
// "0177.0.0.1", or a "0x"-prefixed hex octet/address) and not merely a
// hostname that happens to share some characters with hex digits.
//
// Round-9+ review (MEDIUM): a hex LETTER ('a'-'f'/'A'-'F') is only ever
// actually numeric when it follows a proper "0x"/"0X" prefix -- no real
// host-parsing implementation (inet_aton, strtoul, glibc's own resolver)
// ever interprets a bare label like "cafe" or "decaf" as a number just
// because every character in it happens to be a valid hex digit. The
// previous implementation classified ANY host composed solely of
// digits/dots/hex-letters/'x' as "numeric-ish", which wrongly rejected
// entirely valid, ordinary DNS names such as "decaf.cafe", "abc.de", or
// even a single label like "abc" purely because their letters overlap
// hex's alphabet. This function instead only recognises the specific,
// narrow set of textual forms an address parser could actually be
// confused by, per component.
bool looksLikeNumericIshHostText(QStringView text) {
  if (text.isEmpty()) {
    return false;
  }
  // A host containing any character outside the digit/dot/hex-letter/'x'
  // set can never be any kind of numeric spelling at all: fast-reject it
  // here before doing the per-component split/parse below (also keeps
  // the previous, still-correct broad character-class contract that
  // callers may already depend on).
  for (const QChar c : text) {
    const ushort u = c.unicode();
    const bool isDigit = u >= u'0' && u <= u'9';
    const bool isHexLetter =
        (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
    const bool isDot = u == u'.';
    const bool isXLetter = u == u'x' || u == u'X';
    if (!isDigit && !isHexLetter && !isDot && !isXLetter) {
      return false;
    }
  }
  const QList<QStringView> components = text.split(u'.');
  for (const QStringView component : components) {
    if (!numericIpAddressComponentText(component)) {
      return false;
    }
  }
  return true;
}

// Returns true iff every character in a bracketed IPv6 literal's inner
// text (already stripped of its '[' and ']' by
// extractRawHostFromAuthority()) is a hex digit, ':', or '.' (the last
// permitted only for an IPv4-mapped/embedded form, e.g.
// "::ffff:127.0.0.1") -- i.e. the only characters a syntactically valid
// IPv6 literal can ever contain. Rejects a percent-escape, a zone/scope
// id ("%eth0"), or any other stray character smuggled inside the
// brackets.
bool isPlausibleIPv6LiteralText(QStringView text) {
  for (const QChar c : text) {
    const ushort u = c.unicode();
    const bool isHexDigit = (u >= u'0' && u <= u'9') ||
                            (u >= u'a' && u <= u'f') ||
                            (u >= u'A' && u <= u'F');
    if (!isHexDigit && u != u':' && u != u'.') {
      return false;
    }
  }
  return true;
}

} // namespace

bool isCanonicalLoopbackHostText(const QStringView hostText) {
  if (isAsciiCaseInsensitiveLocalhost(hostText)) {
    return true;
  }
  if (isCanonicalLoopbackIPv4Text(hostText)) {
    return true;
  }
  return hostText.compare(u"::1", Qt::CaseSensitive) == 0;
}

bool isCleartextAuthAllowedForRawInput(const QString &scheme,
                                       const QString &rawUrlText) {
  if (scheme == "https"_L1) {
    // https is never cleartext, regardless of host: no loopback
    // restriction applies here.
    return true;
  }
  if (scheme != "http"_L1) {
    // Any scheme other than "http"/"https" (e.g. a future caller passing
    // an unvalidated or unexpected scheme) is not a cleartext HTTP
    // transport this policy was designed to reason about at all -- fail
    // closed rather than silently answering "allowed" for a case this
    // function was never meant to classify. validateCustomUrl() itself
    // already rejects any non-http/https scheme before this is ever
    // called with one, so this is defense-in-depth for this public
    // helper against future misuse, not a path exercised today.
    return false;
  }
  const QStringView authority = extractRawAuthority(rawUrlText);
  if (authority.isEmpty()) {
    return false; // Could not identify an authority; fail closed.
  }
  if (authority.contains(u'@')) {
    // A userinfo-like construct is present in the raw text. Regardless of
    // how QUrl itself would later parse userinfo vs. host, fail closed:
    // this policy never lets userinfo influence which host is checked.
    return false;
  }
  const QStringView hostText = extractRawHostFromAuthority(authority);
  if (hostText.isEmpty()) {
    return false;
  }
  return isCanonicalLoopbackHostText(hostText);
}

bool isSecureOrLoopbackAuthTransport(const QUrl &url) {
  if (!url.userInfo().isEmpty()) {
    return false;
  }
  // Fail closed on a missing host regardless of scheme (e.g.
  // "https:///missing-host"): this is a public transport-safety
  // predicate, not merely an internal helper for already-validated
  // ServerProfile URLs, so it must not report "secure" for a URL that
  // does not actually identify a host at all.
  if (url.host().isEmpty()) {
    return false;
  }
  if (url.scheme() == "https"_L1) {
    return true;
  }
  if (url.scheme() == "http"_L1) {
    return isCanonicalLoopbackHostText(url.host());
  }
  return false;
}

bool rawAuthorityHostIsUnambiguousAndWellFormed(const QString &rawUrlText) {
  const QStringView authority = extractRawAuthority(rawUrlText);
  if (authority.isEmpty()) {
    return false; // Could not identify an authority; fail closed.
  }
  if (authority.contains(u'@')) {
    // A userinfo-like construct is present in the raw text -- see
    // isCleartextAuthAllowedForRawInput()'s identical check for why this
    // is rejected independent of how QUrl itself would later parse
    // userinfo vs. host.
    return false;
  }
  const bool isBracketedIPv6 = authority.startsWith(u'[');
  const QStringView hostText = extractRawHostFromAuthority(authority);
  if (hostText.isEmpty()) {
    return false; // Empty host, or a malformed/empty/out-of-range port
                  // (see extractRawHostFromAuthority()'s own contract).
  }
  if (hostText.contains(u'%')) {
    return false; // Percent-escape in the host: never legitimate here.
  }
  if (!isAsciiOnly(hostText)) {
    return false; // No Unicode/IDNA hostnames: see this function's own
                  // doc comment for the Unicode case-fold rationale.
  }
  if (containsBackslashOrControlCharacter(hostText)) {
    return false;
  }
  if (isBracketedIPv6) {
    // A bracketed IPv6 literal's inner text must contain only characters
    // that can ever legitimately appear in one.
    return isPlausibleIPv6LiteralText(hostText);
  }
  if (looksLikeNumericIshHostText(hostText) &&
      !isStrictDottedDecimalIPv4Text(hostText)) {
    // "Numeric-ish" (digits/dots/hex-letters/'x' only) but not itself a
    // strict, canonical four-octet dotted-decimal IPv4 literal: an
    // ambiguous/alternate numeric IP spelling -- see
    // looksLikeNumericIshHostText()'s doc comment.
    return false;
  }
  return true;
}

} // namespace Arkham
