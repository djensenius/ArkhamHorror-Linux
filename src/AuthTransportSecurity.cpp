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

// Matches exactly "127.A.B.C" where A, B, C are each strict decimal octets
// (see isStrictDecimalOctet) -- i.e. the canonical, unambiguous dotted-
// decimal spelling of a 127.0.0.0/8 loopback address, and nothing else
// (no shortened forms, no octal/hex, no extra components).
bool isCanonicalLoopbackIPv4Text(QStringView text) {
  static constexpr QStringView kPrefix = u"127.";
  if (!text.startsWith(kPrefix)) {
    return false;
  }
  const QStringView rest = text.mid(kPrefix.size());
  const qsizetype firstDot = rest.indexOf(u'.');
  if (firstDot < 0) {
    return false;
  }
  const QStringView octetB = rest.left(firstDot);
  const QStringView afterB = rest.mid(firstDot + 1);
  const qsizetype secondDot = afterB.indexOf(u'.');
  if (secondDot < 0) {
    return false;
  }
  const QStringView octetC = afterB.left(secondDot);
  const QStringView octetD = afterB.mid(secondDot + 1);
  return isStrictDecimalOctet(octetB) && isStrictDecimalOctet(octetC) &&
         isStrictDecimalOctet(octetD);
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

} // namespace

bool isCanonicalLoopbackHostText(const QStringView hostText) {
  if (hostText.compare(u"localhost", Qt::CaseInsensitive) == 0) {
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

} // namespace Arkham
