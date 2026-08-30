#pragma once

// Shared canonical test-data table for the strict raw-URL / http-loopback-
// only transport policy enforced by UrlValidator::validateCustomUrl() (via
// AuthTransportSecurity.h's isCleartextAuthAllowedForRawInput()).
//
// This table is consumed by BOTH:
//   - tests/NetworkTests.cpp, which drives every row directly through
//     validateCustomUrl() in isolation, and
//   - tests/AuthClientTests.cpp, which drives every row through the full
//     PUBLIC ServerProfile::custom() -> NetworkAuthenticationClient
//     request-construction boundary, proving an accepted row produces
//     exactly one expected request and a rejected row produces none.
//
// Keeping a single shared table (rather than two independently-maintained
// lists) means the two test surfaces can never silently diverge into two
// different definitions of "the policy" under test.

#include <QString>
#include <QVector>

#include "UrlValidator.h"

namespace Arkham::Test {

struct StrictLoopbackUrlRow {
  const char *name;
  QString urlString;
  bool expectAccepted;
  // Meaningful only when !expectAccepted. Stored as int (not
  // Arkham::UrlErrorCode) purely so it can be forwarded straight into a
  // QTest::addColumn<int>("expectedErrorCode") column without registering
  // UrlErrorCode as a QMetaType.
  int expectedErrorCode;
  // Meaningful only when expectAccepted. The full expected /authenticate
  // URL this row must produce when driven through the real
  // ServerProfile::custom() -> NetworkAuthenticationClient boundary
  // (tests/AuthClientTests.cpp's authBoundaryEnforcesStrictLoopbackPolicy()).
  // Deliberately hand-written/independent of ServerProfile::apiUrl() --
  // the same normalisation function the production request path itself
  // calls -- so a bug in apiUrl() (e.g. losing a port, mishandling a base
  // path, or failing to canonicalise "127.1" the way QUrl's own host
  // parser does) cannot make the test trivially self-confirm. Left
  // default-constructed (empty/null QString) for every rejected row: an
  // accepted row's URL always has a non-empty scheme+host, so an empty
  // QString unambiguously means "not applicable" without needing
  // std::optional.
  QString expectedAuthenticateUrl;
};

// Returns the full canonical strict raw-URL / http-loopback policy test
// matrix: every row a caller of validateCustomUrl() / ServerProfile::
// custom() must accept or reject, and (for rejected rows) the exact
// UrlErrorCode it must produce. Table currently has 55 data rows (see
// tests/AuthClientTests.cpp / tests/NetworkTests.cpp for the distinct
// Qt-reported *test invocation* counts, which additionally include each
// binary's own initTestCase()/cleanupTestCase() pseudo-tests).
inline QVector<StrictLoopbackUrlRow> strictLoopbackUrlRows() {
  using Arkham::UrlErrorCode;
  const int invalidUrl = static_cast<int>(UrlErrorCode::InvalidUrl);
  const int controlCharacterPresent =
      static_cast<int>(UrlErrorCode::ControlCharacterPresent);
  const int insecureTransport =
      static_cast<int>(UrlErrorCode::InsecureTransport);
  const int ambiguousAuthority =
      static_cast<int>(UrlErrorCode::AmbiguousAuthority);
  const int credentialsPresent =
      static_cast<int>(UrlErrorCode::CredentialsPresent);
  const int missingHost = static_cast<int>(UrlErrorCode::MissingHost);
  const int unsupportedScheme =
      static_cast<int>(UrlErrorCode::UnsupportedScheme);

  return QVector<StrictLoopbackUrlRow>{
      // ── Accepted: exact canonical loopback spellings over http ────────
      {"http-localhost", QStringLiteral("http://localhost"), true, 0,
       QStringLiteral("http://localhost/api/v1/authenticate")},
      {"http-localhost-port", QStringLiteral("http://localhost:9000"), true, 0,
       QStringLiteral("http://localhost:9000/api/v1/authenticate")},
      {"http-localhost-path", QStringLiteral("http://localhost/selfhosted"),
       true, 0,
       QStringLiteral("http://localhost/selfhosted/api/v1/authenticate")},
      {"http-127.0.0.1", QStringLiteral("http://127.0.0.1"), true, 0,
       QStringLiteral("http://127.0.0.1/api/v1/authenticate")},
      {"http-127.0.0.1-port-path",
       QStringLiteral("http://127.0.0.1:9000/selfhosted"), true, 0,
       QStringLiteral("http://127.0.0.1:9000/selfhosted/api/v1/authenticate")},
      {"http-bracketed-::1", QStringLiteral("http://[::1]"), true, 0,
       QStringLiteral("http://[::1]/api/v1/authenticate")},
      {"http-bracketed-::1-port", QStringLiteral("http://[::1]:9000"), true, 0,
       QStringLiteral("http://[::1]:9000/api/v1/authenticate")},
      {"http-localhost-port-min", QStringLiteral("http://localhost:1"), true, 0,
       QStringLiteral("http://localhost:1/api/v1/authenticate")},
      {"http-localhost-port-max", QStringLiteral("http://localhost:65535"),
       true, 0, QStringLiteral("http://localhost:65535/api/v1/authenticate")},
      {"http-localhost-whitespace-trimmed",
       QStringLiteral("  http://localhost:9000  "), true, 0,
       QStringLiteral("http://localhost:9000/api/v1/authenticate")},

      // ── Accepted: https for any host, including odd literals and base
      //    paths ──────────────────────────────────────────────────────
      {"https-any-host", QStringLiteral("https://example.com"), true, 0,
       QStringLiteral("https://example.com/api/v1/authenticate")},
      {"https-lan-ip", QStringLiteral("https://192.168.1.100:8080/selfhosted"),
       true, 0,
       QStringLiteral(
           "https://192.168.1.100:8080/selfhosted/api/v1/authenticate")},
      {"https-127.1-unrestricted", QStringLiteral("https://127.1"), false,
       ambiguousAuthority, QString()},
      // Cumulative-review finding (PR #18): https is NOT exempt from the
      // raw-authority ambiguity policy merely because it has no loopback
      // restriction -- "127.1" is an alternate/shortened numeric IPv4
      // spelling regardless of scheme, and must be rejected the same way
      // for both http and https (see
      // rawAuthorityHostIsUnambiguousAndWellFormed()).
      {"https-localhost-lookalike",
       QStringLiteral("https://localhost.evil.example"), true, 0,
       QStringLiteral("https://localhost.evil.example/api/v1/authenticate")},
      {"https-base-path", QStringLiteral("https://example.com/arkham"), true, 0,
       QStringLiteral("https://example.com/arkham/api/v1/authenticate")},
      // Round-9+ review (MEDIUM): a hostname composed entirely of
      // characters that happen to overlap hex digits ('a'-'f') must NOT
      // be misclassified as an "alternate numeric IP spelling" merely for
      // that overlap -- see looksLikeNumericIshHostText()'s doc comment.
      // None of these are ever interpreted as numeric by any real
      // host-parsing implementation, since none carries a "0x" prefix.
      {"https-all-hex-letters-dotted", QStringLiteral("https://decaf.cafe"),
       true, 0, QStringLiteral("https://decaf.cafe/api/v1/authenticate")},
      {"https-all-hex-letters-dotted-short", QStringLiteral("https://abc.de"),
       true, 0, QStringLiteral("https://abc.de/api/v1/authenticate")},
      {"https-all-hex-letters-single-label", QStringLiteral("https://abc"),
       true, 0, QStringLiteral("https://abc/api/v1/authenticate")},
      {"https-all-hex-letters-with-x-not-prefix",
       QStringLiteral("https://faced.ace"), true, 0,
       QStringLiteral("https://faced.ace/api/v1/authenticate")},

      // ── Rejected: ambiguous/non-canonical numeric loopback spellings ───
      // Cumulative-review finding (PR #18): every one of these is now
      // rejected by rawAuthorityHostIsUnambiguousAndWellFormed() (an
      // "alternate numeric IP" spelling), which runs BEFORE the
      // http-loopback-specific check below -- so the typed error is
      // AmbiguousAuthority, not InsecureTransport, even though these
      // happen to also be loopback-adjacent hosts over http.
      {"http-127.1", QStringLiteral("http://127.1"), false, ambiguousAuthority,
       QString()},
      {"http-single-integer", QStringLiteral("http://2130706433"), false,
       ambiguousAuthority, QString()},
      {"http-octal-looking", QStringLiteral("http://0177.0.0.1"), false,
       ambiguousAuthority, QString()},
      {"http-hex-looking", QStringLiteral("http://0x7f.0.0.1"), false,
       ambiguousAuthority, QString()},
      {"http-octal-single-integer", QStringLiteral("http://017700000001"),
       false, ambiguousAuthority, QString()},
      {"http-hex-single-integer", QStringLiteral("http://0x7f000001"), false,
       ambiguousAuthority, QString()},
      {"http-leading-zero-full", QStringLiteral("http://127.000.000.001"),
       false, ambiguousAuthority, QString()},
      {"http-leading-zero-last-octet", QStringLiteral("http://127.0.0.01"),
       false, ambiguousAuthority, QString()},
      {"http-shortened-form", QStringLiteral("http://127.0.1"), false,
       ambiguousAuthority, QString()},

      // ── Rejected: IPv4-mapped / expanded / alternate IPv6 spellings ────
      {"http-ipv4-mapped-ipv6", QStringLiteral("http://[::ffff:127.0.0.1]"),
       false, insecureTransport, QString()},
      {"http-ipv6-expanded", QStringLiteral("http://[0:0:0:0:0:0:0:1]"), false,
       insecureTransport, QString()},
      {"http-ipv6-alternate-shortened", QStringLiteral("http://[::0:1]"), false,
       insecureTransport, QString()},
      {"http-ipv6-zone-id", QStringLiteral("http://[::1%25eth0]"), false,
       ambiguousAuthority, QString()},

      // ── Rejected: trailing-dot / subdomain / lookalike localhost forms ─
      {"http-localhost-trailing-dot", QStringLiteral("http://localhost."),
       false, insecureTransport, QString()},
      {"http-localhost-subdomain",
       QStringLiteral("http://localhost.evil.example"), false,
       insecureTransport, QString()},
      {"http-127-lookalike-subdomain",
       QStringLiteral("http://127.0.0.1.evil.example"), false,
       insecureTransport, QString()},
      {"http-notlocalhost", QStringLiteral("http://notlocalhost"), false,
       insecureTransport, QString()},

      // ── Rejected: Unicode case-fold / homoglyph / IDNA / percent-encoded
      //    / mixed lookalikes of "localhost". All of these are now caught
      //    by rawAuthorityHostIsUnambiguousAndWellFormed() (AmbiguousAuthority
      //    -- non-ASCII host text, checked scheme-independently) BEFORE the
      //    http-loopback-specific check ever runs -- except the percent-
      //    encoded row, which QUrl itself rejects as an invalid hostname
      //    even earlier -- so this table asserts the precise,
      //    empirically-confirmed UrlErrorCode for each, not merely
      //    "rejected somehow".
      {"http-localhost-long-s-unicode-casefold",
       QString::fromUtf16(u"http://localho\u017Ft"), false, ambiguousAuthority,
       QString()}, // U+017F LATIN SMALL LETTER LONG S case-folds to
                   // ASCII 's' under Qt::CaseInsensitive.
      {"http-localhost-cyrillic-o-homoglyph",
       QString::fromUtf16(u"http://l\u043Ecalhost"), false, ambiguousAuthority,
       QString()}, // U+043E CYRILLIC SMALL LETTER O looks identical
                   // to Latin 'o'.
      {"http-localhost-fullwidth-idna",
       QString::fromUtf16(
           u"http://\uFF4C\uFF4F\uFF43\uFF41\uFF4C\uFF48\uFF4F\uFF53\uFF54"),
       false, ambiguousAuthority,
       QString()}, // Fullwidth Unicode forms of each Latin
                   // letter in "localhost"; QUrl even
                   // normalises url.host() to plain
                   // "localhost" for this input, which is
                   // exactly why the raw, pre-normalisation
                   // text (not url.host()) must be what this
                   // policy checks.
      {"http-localhost-percent-encoded",
       QStringLiteral("http://%6c%6f%63%61%6c%68%6f%73%74"), false, invalidUrl,
       QString()}, // QUrl::StrictMode itself rejects percent-escapes in
                   // the host as an invalid hostname, before either
                   // authority check is ever reached.
      {"http-localhost-mixed-homoglyph-and-casefold",
       QString::fromUtf16(u"http://l\u043Ecalho\u017Ft"), false,
       ambiguousAuthority,
       QString()}, // Cyrillic 'о' + long-s combined in one host.
      {"http-localhost-explicit-punycode-ace",
       QStringLiteral("http://xn--lcalhost-nbh"), false, insecureTransport,
       QString()},
      // The literal ASCII-Compatible-Encoding (punycode) form QUrl itself
      // produces for the Cyrillic-o row above -- proves the raw-lexical
      // check also fails closed on an already-ACE-encoded lookalike, not
      // only on the pre-encoding Unicode spelling. This form is plain
      // ASCII (punycode is ASCII-Compatible-Encoding by design) and not
      // "numeric-ish", so it passes
      // rawAuthorityHostIsUnambiguousAndWellFormed()
      // and is instead rejected by the http-loopback-specific check below
      // for simply not being the exact text "localhost" -- hence
      // InsecureTransport, not AmbiguousAuthority.

      // ── Rejected: https is NOT exempt from the same raw-authority
      //    ambiguity checks above -- this project's blanket policy rejects
      //    percent-escapes, non-ASCII hosts, and alternate numeric IP
      //    spellings identically regardless of scheme (see
      //    rawAuthorityHostIsUnambiguousAndWellFormed()'s doc comment for
      //    why an https-only exemption from this specific class of
      //    ambiguity would be unsafe even though https itself carries no
      //    cleartext-loopback restriction).
      {"https-octal-looking", QStringLiteral("https://0177.0.0.1"), false,
       ambiguousAuthority, QString()},
      {"https-hex-looking", QStringLiteral("https://0x7f.0.0.1"), false,
       ambiguousAuthority, QString()},
      {"https-single-integer", QStringLiteral("https://2130706433"), false,
       ambiguousAuthority, QString()},
      {"https-shortened-form", QStringLiteral("https://192.168.1"), false,
       ambiguousAuthority, QString()},
      {"https-empty-port", QStringLiteral("https://example.com:"), false,
       ambiguousAuthority, QString()},
      {"https-cyrillic-o-homoglyph",
       QString::fromUtf16(u"https://ex\u0430mple.com"), false,
       ambiguousAuthority,
       QString()}, // U+0430 CYRILLIC SMALL LETTER A looks identical to
                   // Latin 'a'; a public-facing example.com lookalike,
                   // not merely a "localhost" one, since this check is
                   // no longer restricted to the loopback-exception path
                   // at all.

      // ── Rejected: userinfo, malformed, LAN/public addresses over http ──
      // Userinfo is rejected unconditionally (CredentialsPresent), even
      // for an otherwise-canonical loopback host, and is checked before
      // the http/loopback policy itself (see
      // UrlValidator::validateCustomUrl()'s ordering), so this row must
      // produce CredentialsPresent, not InsecureTransport.
      {"http-userinfo-on-loopback",
       QStringLiteral("http://") + QStringLiteral("u") + QStringLiteral("ser") +
           QStringLiteral(":") + QStringLiteral("pass") +
           QStringLiteral("@localhost:9000"),
       false, credentialsPresent, QString()},
      {"http-lan-ip", QStringLiteral("http://192.168.1.100"), false,
       insecureTransport, QString()},
      {"http-public-host", QStringLiteral("http://example.com"), false,
       insecureTransport, QString()},

      // ── Rejected: malformed/absent/out-of-range ports on an otherwise-
      //    exact loopback host or bracketed IPv6 literal. QUrl's own
      //    StrictMode parsing already rejects most malformed port syntax
      //    (non-digit garbage, percent-escapes, double colons, negative,
      //    >65535) as an unparseable URL entirely (UrlErrorCode::
      //    InvalidUrl) before either authority check is ever reached; the
      //    remaining two forms QUrl treats as syntactically valid -- an
      //    empty port ("host:") and port 0 -- are only caught by
      //    rawAuthorityHostIsUnambiguousAndWellFormed()'s own stricter,
      //    in-range (1..65535), non-empty port rule (UrlErrorCode::
      //    AmbiguousAuthority, since that scheme-independent check now
      //    runs before the http-loopback-specific one). Both groups are
      //    asserted here so a regression in either QUrl's parsing
      //    assumptions or the port rule itself is caught.
      {"http-localhost-empty-port", QStringLiteral("http://localhost:"), false,
       ambiguousAuthority, QString()},
      {"http-localhost-zero-port", QStringLiteral("http://localhost:0"), false,
       ambiguousAuthority, QString()},
      {"http-bracketed-::1-empty-port", QStringLiteral("http://[::1]:"), false,
       ambiguousAuthority, QString()},
      {"http-localhost-non-numeric-port",
       QStringLiteral("http://localhost:evil"), false, invalidUrl, QString()},
      {"http-localhost-percent-escaped-port",
       QStringLiteral("http://localhost:%39"), false, invalidUrl, QString()},
      {"http-localhost-double-colon-port",
       QStringLiteral("http://localhost::9000"), false, invalidUrl, QString()},
      {"http-localhost-multiple-port-segments",
       QStringLiteral("http://localhost:9000:9000"), false, invalidUrl,
       QString()},
      {"http-bracketed-::1-non-numeric-port",
       QStringLiteral("http://[::1]:evil"), false, invalidUrl, QString()},
      {"http-localhost-port-overflow", QStringLiteral("http://localhost:99999"),
       false, invalidUrl, QString()},
      {"http-localhost-port-negative", QStringLiteral("http://localhost:-1"),
       false, invalidUrl, QString()},

      // ── Rejected: control characters in the ORIGINAL input, which must
      //    not be laundered away by trimmed() before this policy ever
      //    sees them.
      {"http-localhost-trailing-tab-after-port",
       QStringLiteral("http://localhost:9000\t"), false,
       controlCharacterPresent, QString()},
      {"http-localhost-tab-before-colon",
       QStringLiteral("http://localhost\t:9000"), false,
       controlCharacterPresent, QString()},
      {"http-localhost-trailing-newline", QStringLiteral("http://localhost\n"),
       false, controlCharacterPresent, QString()},

      // ── Rejected: hostless and unexpected schemes, so this shared
      //    boundary table -- not merely validateCustomUrl()'s isolated
      //    tests -- also proves these fail before any request is built.
      {"https-hostless-scheme", QStringLiteral("https:///nohost"), false,
       missingHost, QString()},
      {"ftp-unexpected-scheme", QStringLiteral("ftp://localhost"), false,
       unsupportedScheme, QString()},
  };
}

} // namespace Arkham::Test
