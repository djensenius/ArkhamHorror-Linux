#include "UrlValidator.h"

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

} // namespace

UrlValidationResult validateCustomUrl(const QString &input) {
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
