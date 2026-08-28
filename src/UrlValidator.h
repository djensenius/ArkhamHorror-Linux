#pragma once

#include <QString>
#include <QUrl>
#include <optional>
#include <utility>

namespace Arkham {

// Discriminated error codes returned by validateCustomUrl().
// The matching message in the returned UrlValidationResult provides
// human-readable detail suitable for display or logging.
enum class UrlErrorCode {
  InvalidUrl,              ///< Input is empty or QUrl cannot parse it.
  ControlCharacterPresent, ///< Original input contains a control character.
  UnsupportedScheme,       ///< Scheme is not "https" or "http".
  MissingHost,             ///< URL contains no host component.
  CredentialsPresent,      ///< User-info (username or password) is present.
  FragmentPresent,         ///< A fragment component (#...) is present.
  QueryPresent,            ///< A query string (?...) is present.
  DuplicateApiPath,        ///< Path already contains the pinned API base path.
  InsecureTransport, ///< http to a non-canonical-loopback host is rejected.
};

// Typed error returned by validateCustomUrl() on validation failure.
struct UrlValidationError {
  UrlErrorCode code{UrlErrorCode::InvalidUrl};
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
  [[nodiscard]] const UrlValidationError &error() const { return m_error; }

private:
  std::optional<QUrl> m_url;
  UrlValidationError m_error;
};

// Validate and normalise a custom server base URL string.
//
// Accepted: https:// URLs with any non-empty host, and http:// URLs but
//           ONLY to an exact canonical loopback spelling ("localhost",
//           canonical dotted-decimal 127.x.y.z, or "::1") -- never any
//           other http host, LAN or public. See
//           isCleartextAuthAllowedForRawInput() in AuthTransportSecurity
//           for the precise, lexically-strict rule and why it must run
//           against the raw pre-QUrl input text.
//           Non-default ports (e.g. :3000) are preserved.
//           Non-empty, non-root path prefixes (e.g. /prefix) are kept.
// Rejected: empty input, input containing any control character (checked
//           against the ORIGINAL, not-yet-trimmed input -- see below),
//           unparseable input, non-http/https scheme, missing host,
//           credentials, fragments, query strings, http to any
//           non-canonical-loopback host (including a loopback host
//           followed by an empty, non-numeric, out-of-range, or malformed
//           port), and paths that already contain the pinned API base
//           path.
//
// The control-character check runs against the caller-supplied input
// verbatim, before QString::trimmed() is applied: trimmed() strips leading
// and trailing whitespace, including a trailing tab/newline/CR, so
// checking only the trimmed string would let an original input containing
// such a character be silently laundered into what looks like a clean,
// valid URL.
// Ordinary leading/trailing plain spaces (not control characters) are
// still trimmed as before.
//
// On success returns a normalised QUrl (scheme + host + optional port +
// optional clean path prefix, no trailing slash, no other components).
[[nodiscard]] UrlValidationResult validateCustomUrl(const QString &input);

} // namespace Arkham
