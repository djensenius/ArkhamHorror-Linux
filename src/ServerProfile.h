#pragma once

#include "ValueOrError.h"

#include <QString>
#include <QStringView>
#include <QUrl>

namespace Arkham {

// Identifies how a ServerProfile was created.
enum class ServerProfileKind {
  HostedDefault, ///< The canonical hosted service at https://arkhamhorror.app.
  Custom,        ///< A user-supplied self-hosted or local server.
};

class ServerProfile {
public:
  // Default-constructs an empty, permanently-invalid profile (no scheme, no
  // host, no ID, unvalidated provenance). Because this constructor takes no
  // argument, it can never carry a caller-supplied URL, so it cannot be used
  // to smuggle an unvalidated host/scheme past auth's transport-security
  // checks -- unlike the raw-QUrl constructor this class used to expose,
  // which let a caller pass any QUrl (including one QUrl had already
  // silently canonicalised from an ambiguous loopback spelling such as
  // "127.1") directly into a profile without going through
  // UrlValidator::validateCustomUrl(). Use hostedDefault(), custom(), or
  // customWithId() to build a usable profile; every one of those factories
  // validates its input URL against the same rules and marks the result as
  // having validated provenance (see hasValidatedProvenance()).
  ServerProfile() = default;

  // Returns a profile for the canonical hosted service
  // (https://arkhamhorror.app).  Always carries the same deterministic ID.
  [[nodiscard]] static ServerProfile hostedDefault();

  // Construct a custom (self-hosted or local) profile from a display name and
  // URL string.  displayName is trimmed; a blank/whitespace-only name is
  // rejected.  The URL is validated: HTTPS and HTTP accepted; credentials,
  // fragments, query strings, and pinned API-path duplication rejected;
  // non-default ports and path prefixes preserved.  A new UUID is generated
  // for the profile.  Returns a typed error on any validation failure.
  [[nodiscard]] static ValueOrError<ServerProfile>
  custom(QString displayName, const QString &urlString);

  // Reconstruct a custom profile with an existing UUID (used by the
  // persistence layer).  Validates the UUID, display name, and URL using the
  // same rules as custom().  Returns a typed error on any failure.
  [[nodiscard]] static ValueOrError<ServerProfile>
  customWithId(const QString &id, QString displayName,
               const QString &urlString);

  // Stable identifier for factory-created profiles: deterministic for
  // HostedDefault and UUID-based for Custom. Empty for a default-constructed
  // profile.
  [[nodiscard]] const QString &profileId() const;

  [[nodiscard]] ServerProfileKind kind() const;
  [[nodiscard]] const QString &displayName() const;
  [[nodiscard]] const QUrl &baseUrl() const;

  // Returns true iff |other| designates the SAME network endpoint as this
  // profile: identical scheme, identical host, identical EFFECTIVE port
  // (an explicit port, or the scheme's implied default port -- 443 for
  // https, 80 for http -- when none was given, so "https://host" and
  // "https://host:443" compare equal despite QUrl::port() returning -1 for
  // the former and 443 for the latter), and an identical stored path
  // prefix compared CASE-SENSITIVELY (URL paths are case-sensitive, so
  // "/foo" and "/Foo" genuinely select different routes on the server).
  // Scheme and host are compared with plain QString equality because
  // UrlValidator::validateCustomUrl() already normalises both via QUrl's
  // own parsing (which lowercases them) before either is ever stored in
  // m_baseUrl, so no further case-folding is needed -- or wanted, since
  // that would risk silently masking a genuine host difference.
  //
  // Two profiles with the same profileId() but a false result here refer
  // to different endpoints even though they share a stable ID (stable IDs
  // are explicitly supported by customWithId(), e.g. across a persisted
  // reload that changed the underlying URL); any credential previously
  // stored for that profileId() was scoped to the OLD endpoint and must
  // never be used against the new one -- see
  // SessionCoordinator::mutateSelectedProfile().
  [[nodiscard]] bool hasEquivalentEndpoint(const ServerProfile &other) const;

  // Returns the full URL for a REST endpoint at |path| relative to the API
  // root.  Any stored path prefix (set by custom()) is prepended before
  // the API base path from currentPin().
  [[nodiscard]] QUrl apiUrl(QStringView path) const;
  [[nodiscard]] QUrl websocketUrl(QStringView path) const;
  [[nodiscard]] bool isValid() const;

  // True only for a profile produced by hostedDefault(), custom(), or
  // customWithId() -- i.e. one whose baseUrl was validated by
  // UrlValidator::validateCustomUrl() (or is the hardcoded hosted-default
  // URL) before being stored. False for a default-constructed profile.
  // NetworkAuthenticationClient rejects any profile for which this is false
  // in addition to rejecting any profile for which isValid() is false, so
  // that even if some future code path were to construct a profile outside
  // the three sanctioned factories, it could still never reach the network
  // with an unvalidated host/scheme.
  [[nodiscard]] bool hasValidatedProvenance() const;

private:
  // Test-only escape hatch used exclusively by the regression tests that
  // must prove NetworkAuthenticationClient, QSettingsProfileStore, and
  // NetworkCapabilityProbe defensively reject a profile lacking validated
  // provenance, in case such a profile were ever produced by a future code
  // path. No production code may construct a ServerProfile this way: every
  // sanctioned public entry point (hostedDefault(), custom(),
  // customWithId()) always validates its URL and sets
  // m_validatedProvenance = true before returning.
  friend class ServerProfileTestSupport;
  [[nodiscard]] static ServerProfile unvalidatedForTesting(QUrl baseUrl);

  ServerProfileKind m_kind{ServerProfileKind::Custom};
  QString m_id;
  QString m_displayName;
  QUrl m_baseUrl;
  bool m_validatedProvenance{false};
};

} // namespace Arkham
