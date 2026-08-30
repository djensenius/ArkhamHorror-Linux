#include "UrlValidator.h"

#include "AuthTransportSecurity.h"
#include "ContractPin.h"

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// Finds the pin's API base path at a complete segment boundary, including
// behind a reverse-proxy prefix, while rejecting partial-segment matches.
bool containsApiBasePath(const QString &path, const QString &apiBasePath) {
  qsizetype pos = 0;
  while (pos < path.size()) {
    const qsizetype idx = path.indexOf(apiBasePath, pos);
    if (idx == -1) {
      break;
    }
    const qsizetype after = idx + apiBasePath.size();
    if (after == path.size() || path[after] == QLatin1Char('/')) {
      return true;
    }
    pos = idx + 1;
  }
  return false;
}

// Returns true iff |text| contains a Unicode "control" character (category
// Cc: the C0 controls U+0000-U+001F including tab/newline/CR, plus DELETE
// U+007F and the C1 controls U+0080-U+009F). Deliberately narrower than
// QChar::isSpace(), which also matches ordinary space and various Unicode
// space separators that are fine to trim and are not a policy concern
// here.
bool containsControlCharacter(const QString &text) {
  for (const QChar c : text) {
    if (c.category() == QChar::Other_Control) {
      return true;
    }
  }
  return false;
}

} // namespace

UrlValidationResult validateCustomUrl(const QString &input) {
  // Checked against the ORIGINAL input, before trimming: QString::trimmed()
  // strips any character for which QChar::isSpace() is true, which
  // includes tab, newline, and carriage return -- so a trailing control
  // character in the caller-supplied text could otherwise be silently
  // stripped away before this function ever saw it, letting an invalid
  // original masquerade as a clean, valid URL. Rejecting it here, before
  // any trimming happens, closes that laundering path.
  if (containsControlCharacter(input)) {
    return UrlValidationError{
        UrlErrorCode::ControlCharacterPresent,
        QStringLiteral("URL must not contain control characters"),
    };
  }

  const QString trimmedInput = input.trimmed();
  if (trimmedInput.isEmpty()) {
    return UrlValidationError{
        UrlErrorCode::InvalidUrl,
        QStringLiteral("URL must not be empty"),
    };
  }

  const QUrl url(trimmedInput, QUrl::StrictMode);
  if (!url.isValid()) {
    return UrlValidationError{
        UrlErrorCode::InvalidUrl,
        QStringLiteral("invalid URL: %1").arg(url.errorString()),
    };
  }

  // Scheme must be https or http.
  const QString scheme = url.scheme();
  if (scheme != "https"_L1 && scheme != "http"_L1) {
    return UrlValidationError{
        UrlErrorCode::UnsupportedScheme,
        QStringLiteral("unsupported scheme \"%1\": must be https or http")
            .arg(scheme),
    };
  }

  // Host must be non-empty.
  if (url.host().isEmpty()) {
    return UrlValidationError{
        UrlErrorCode::MissingHost,
        QStringLiteral("URL must include a host"),
    };
  }

  // Credentials are rejected — never store user or password in a profile.
  if (!url.userInfo().isEmpty()) {
    return UrlValidationError{
        UrlErrorCode::CredentialsPresent,
        QStringLiteral("URL must not contain credentials"),
    };
  }

  // Fragments are rejected. Round-9+ review (MEDIUM): checked via
  // hasFragment(), never fragment().isEmpty() -- QUrl distinguishes "no
  // fragment delimiter at all" from "an explicit `#` delimiter followed
  // by an empty fragment" (e.g. a trailing bare "#", or "?#" combining an
  // empty query and an empty fragment); the latter still carries the
  // delimiter itself in the original URL text and must be rejected just
  // as decisively as a non-empty fragment.
  if (url.hasFragment()) {
    return UrlValidationError{
        UrlErrorCode::FragmentPresent,
        QStringLiteral("URL must not contain a fragment (#...)"),
    };
  }

  // Query strings are rejected.
  if (url.hasQuery()) {
    return UrlValidationError{
        UrlErrorCode::QueryPresent,
        QStringLiteral("URL must not contain a query string (?...)"),
    };
  }

  // Cumulative-review finding (PR #18): the raw, pre-QUrl authority text
  // must be unambiguous and well-formed for EVERY scheme (not merely
  // gated behind the http-loopback exception below) -- QUrl silently
  // normalises percent-escaped hosts, folds certain Unicode look-alike
  // characters, and canonicalises alternate numeric IP spellings at parse
  // time, and an https URL is just as capable of carrying one of these
  // ambiguities as an http one. See
  // rawAuthorityHostIsUnambiguousAndWellFormed()'s doc comment in
  // AuthTransportSecurity.h for the exact policy.
  if (!rawAuthorityHostIsUnambiguousAndWellFormed(trimmedInput)) {
    return UrlValidationError{
        UrlErrorCode::AmbiguousAuthority,
        QStringLiteral("URL authority is malformed or ambiguous (percent-"
                       "escaped, non-ASCII, backslash/control characters, "
                       "or an alternate numeric IP spelling are not "
                       "permitted in the host)"),
    };
  }

  // http is only permitted to an exact canonical loopback spelling. This
  // check MUST run against the raw, not-yet-normalised trimmedInput text --
  // not url.host() -- because QUrl itself silently canonicalises ambiguous
  // numeric loopback encodings (e.g. "127.1", octal/hex octets, a bare
  // 32-bit integer) into "127.0.0.1" at parse time, before any later check
  // could tell the difference. See AuthTransportSecurity.h for details.
  //
  // Placed after credentials/fragment/query so those more fundamental
  // input-hygiene failures are reported first when they also apply.
  if (!isCleartextAuthAllowedForRawInput(scheme, trimmedInput)) {
    return UrlValidationError{
        UrlErrorCode::InsecureTransport,
        QStringLiteral("http is only permitted to an exact loopback host "
                       "(\"localhost\", canonical dotted-decimal 127.x.y.z, or "
                       "\"::1\"); use https for any other host"),
    };
  }

  const QString path = url.path();
  const QString apiBasePath = currentPin().expectedApiBasePath;
  if (containsApiBasePath(path, apiBasePath)) {
    return UrlValidationError{
        UrlErrorCode::DuplicateApiPath,
        QStringLiteral("URL path already contains %1; provide the base URL "
                       "without the API suffix")
            .arg(apiBasePath),
    };
  }

  // Build the normalised base URL.
  QUrl normalised;
  normalised.setScheme(scheme);
  normalised.setHost(url.host());
  if (url.port() != -1) {
    normalised.setPort(url.port());
  }

  // Preserve a non-trivial path prefix, stripping any trailing slashes.
  QString cleanPath = path;
  while (cleanPath.size() > 1 && cleanPath.endsWith(QLatin1Char('/'))) {
    cleanPath.chop(1);
  }
  if (!cleanPath.isEmpty() && cleanPath != QLatin1String("/")) {
    normalised.setPath(cleanPath);
  }

  return normalised;
}

} // namespace Arkham
