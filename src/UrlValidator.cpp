#include "UrlValidator.h"

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// Returns true if |path| contains "/api/v1" as a complete path-segment
// boundary (i.e. followed by '/' or end of string).  This catches:
//   /api/v1          — exact
//   /api/v1/whoami   — prefix
//   /proxy/api/v1    — infix at end
//   /proxy/api/v1/x  — infix with trailing segment
// But not:
//   /api/v10         — different segment ("v10" != "v1")
//   /api/v1foo       — no boundary after "v1"
bool containsApiV1Segment(const QString &path) {
  constexpr QLatin1StringView needle{"/api/v1"};
  qsizetype pos = 0;
  while (pos < path.size()) {
    const qsizetype idx = path.indexOf(needle, pos);
    if (idx == -1) {
      break;
    }
    const qsizetype after = idx + needle.size();
    if (after == path.size() || path[after] == QLatin1Char('/')) {
      return true;
    }
    pos = idx + 1;
  }
  return false;
}

} // namespace

UrlValidationResult validateCustomUrl(const QString &input) {
  if (input.trimmed().isEmpty()) {
    return UrlValidationError{
        UrlErrorCode::InvalidUrl,
        QStringLiteral("URL must not be empty"),
    };
  }

  const QUrl url(input, QUrl::StrictMode);
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

  // Fragments are rejected.
  if (!url.fragment().isEmpty()) {
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

  // Reject paths that contain /api/v1 as a complete segment boundary to
  // prevent duplication when apiUrl() appends "/api/v1" later.  The check
  // covers /api/v1 at the start, in the middle (/proxy/api/v1), and as a
  // prefix (/api/v1/foo), while leaving /api/v10 valid.
  const QString path = url.path();
  if (containsApiV1Segment(path)) {
    return UrlValidationError{
        UrlErrorCode::DuplicateApiPath,
        QStringLiteral("URL path already contains /api/v1; provide the base "
                       "URL without the API suffix"),
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
