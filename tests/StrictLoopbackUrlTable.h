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
      {"https-127.1-unrestricted", QStringLiteral("https://127.1"), true, 0,
       QStringLiteral("https://127.0.0.1/api/v1/authenticate")},
      {"https-localhost-lookalike",
       QStringLiteral("https://localhost.evil.example"), true, 0,
       QStringLiteral("https://localhost.evil.example/api/v1/authenticate")},
      {"https-base-path", QStringLiteral("https://example.com/arkham"), true, 0,
       QStringLiteral("https://example.com/arkham/api/v1/authenticate")},

      // ── Rejected: ambiguous/non-canonical numeric loopback spellings ───
      {"http-127.1", QStringLiteral("http://127.1"), false, insecureTransport,
       QString()},
      {"http-single-integer", QStringLiteral("http://2130706433"), false,
       insecureTransport, QString()},
      {"http-octal-looking", QStringLiteral("http://0177.0.0.1"), false,
       insecureTransport, QString()},
      {"http-hex-looking", QStringLiteral("http://0x7f.0.0.1"), false,
       insecureTransport, QString()},
      {"http-octal-single-integer", QStringLiteral("http://017700000001"),
       false, insecureTransport, QString()},
      {"http-leading-zero-full", QStringLiteral("http://127.000.000.001"),
       false, insecureTransport, QString()},
      {"http-leading-zero-last-octet", QStringLiteral("http://127.0.0.01"),
       false, insecureTransport, QString()},
      {"http-shortened-form", QStringLiteral("http://127.0.1"), false,
       insecureTransport, QString()},

      // ── Rejected: IPv4-mapped / expanded / alternate IPv6 spellings ────
      {"http-ipv4-mapped-ipv6", QStringLiteral("http://[::ffff:127.0.0.1]"),
       false, insecureTransport, QString()},
      {"http-ipv6-expanded", QStringLiteral("http://[0:0:0:0:0:0:0:1]"), false,
       insecureTransport, QString()},
      {"http-ipv6-alternate-shortened", QStringLiteral("http://[::0:1]"), false,
       insecureTransport, QString()},
      {"http-ipv6-zone-id", QStringLiteral("http://[::1%25eth0]"), false,
       insecureTransport, QString()},

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
      //    / mixed lookalikes of "localhost". All of these reach
      //    validateCustomUrl()'s loopback check as syntactically valid
      //    QUrls (QUrl::StrictMode accepts non-ASCII IRI-style hosts) --
      //    except the percent-encoded row, which QUrl itself rejects as an
      //    invalid hostname before the loopback check is ever reached --
      //    so this table asserts the precise, empirically-confirmed
      //    UrlErrorCode for each, not merely "rejected somehow".
      {"http-localhost-long-s-unicode-casefold",
       QString::fromUtf16(u"http://localho\u017Ft"), false, insecureTransport,
       QString()}, // U+017F LATIN SMALL LETTER LONG S case-folds to
                   // ASCII 's' under Qt::CaseInsensitive.
      {"http-localhost-cyrillic-o-homoglyph",
       QString::fromUtf16(u"http://l\u043Ecalhost"), false, insecureTransport,
       QString()}, // U+043E CYRILLIC SMALL LETTER O looks identical
                   // to Latin 'o'.
      {"http-localhost-fullwidth-idna",
       QString::fromUtf16(
           u"http://\uFF4C\uFF4F\uFF43\uFF41\uFF4C\uFF48\uFF4F\uFF53\uFF54"),
       false, insecureTransport,
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
                   // the host as an invalid hostname, before the loopback
                   // check is ever reached.
      {"http-localhost-mixed-homoglyph-and-casefold",
       QString::fromUtf16(u"http://l\u043Ecalho\u017Ft"), false,
       insecureTransport,
       QString()}, // Cyrillic 'о' + long-s combined in one host.
      {"http-localhost-explicit-punycode-ace",
       QStringLiteral("http://xn--lcalhost-nbh"), false, insecureTransport,
       QString()},
      // The literal ASCII-Compatible-Encoding (punycode) form QUrl itself
      // produces for the Cyrillic-o row above -- proves the raw-lexical
      // check also fails closed on an already-ACE-encoded lookalike, not
      // only on the pre-encoding Unicode spelling.

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
      //    InvalidUrl) before this function's raw-authority port check is
      //    ever reached; the remaining two forms QUrl treats as
      //    syntactically valid -- an empty port ("host:") and port 0 --
      //    are only caught by this function's own stricter, in-range
      //    (1..65535), non-empty port rule (UrlErrorCode::
      //    InsecureTransport, since the raw-text loopback policy check is
      //    what actually rejects them). Both groups are asserted here so a
      //    regression in either QUrl's parsing assumptions or this
      //    function's own port rule is caught.
      {"http-localhost-empty-port", QStringLiteral("http://localhost:"), false,
       insecureTransport, QString()},
      {"http-localhost-zero-port", QStringLiteral("http://localhost:0"), false,
       insecureTransport, QString()},
      {"http-bracketed-::1-empty-port", QStringLiteral("http://[::1]:"), false,
       insecureTransport, QString()},
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
