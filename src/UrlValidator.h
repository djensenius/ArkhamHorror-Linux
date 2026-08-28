#pragma once

#include "ValueOrError.h"

#include <QUrl>
#include <optional>

namespace Arkham {

// Discriminated error codes returned by validateCustomUrl().
// The matching message in the returned UrlValidationResult provides
// human-readable detail suitable for display or logging.
enum class UrlErrorCode {
  InvalidUrl,         ///< Input is empty or QUrl cannot parse it.
  UnsupportedScheme,  ///< Scheme is not "https" or "http".
  MissingHost,        ///< URL contains no host component.
  CredentialsPresent, ///< User-info (username or password) is present.
  FragmentPresent,    ///< A fragment component (#...) is present.
  QueryPresent,       ///< A query string (?...) is present.
  DuplicateApiPath,   ///< Path already starts with /api/v1.
};

// Typed error returned by validateCustomUrl() on validation failure.
struct UrlValidationError {
  UrlErrorCode code;
  QString message;
};

// Result of validateCustomUrl(): a clean base QUrl on success, or a typed
// UrlValidationError on any validation failure.
class UrlValidationResult {
public:
  UrlValidationResult(QUrl url) : m_url(std::move(url)) {} // NOLINT
  UrlValidationResult(UrlValidationError error)            // NOLINT
      : m_error(std::move(error)) {}

  [[nodiscard]] bool has_value() const noexcept { return m_url.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] const QUrl &operator*() const { return *m_url; }
  [[nodiscard]] const QUrl *operator->() const { return &*m_url; }
  [[nodiscard]] const UrlValidationError &error() const { return *m_error; }

private:
  std::optional<QUrl> m_url;
  std::optional<UrlValidationError> m_error;
};

// Validate and normalise a custom server base URL string.
//
// Accepted: https:// and http:// URLs with a non-empty host.
//           Non-default ports (e.g. :3000) are preserved.
//           Non-empty, non-root path prefixes (e.g. /prefix) are kept.
// Rejected: empty input, unparseable input, non-http/https scheme,
//           missing host, credentials, fragments, query strings, and
//           paths that already contain /api/v1 (prevents duplication).
//
// On success returns a normalised QUrl (scheme + host + optional port +
// optional clean path prefix, no trailing slash, no other components).
[[nodiscard]] UrlValidationResult validateCustomUrl(const QString &input);

} // namespace Arkham
