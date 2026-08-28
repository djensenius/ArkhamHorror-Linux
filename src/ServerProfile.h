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
  // Construct from a raw QUrl, stripping credentials, path, query, and
  // fragment.  Retained for existing unit tests only; prefer the factory
  // methods for new code.  Profiles created this way have no stable ID and
  // cannot be persisted.
  explicit ServerProfile(QUrl baseUrl = {});

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

  // Stable, never-empty identifier.  Deterministic for HostedDefault;
  // a UUID-based string for Custom profiles.
  [[nodiscard]] const QString &profileId() const;

  [[nodiscard]] ServerProfileKind kind() const;
  [[nodiscard]] const QString &displayName() const;
  [[nodiscard]] const QUrl &baseUrl() const;

  // Returns the full URL for a REST endpoint at |path| relative to the API
  // root.  Any stored path prefix (set by custom()) is prepended before
  // the API base path from currentPin().
  [[nodiscard]] QUrl apiUrl(QStringView path) const;
  [[nodiscard]] QUrl websocketUrl(QStringView path) const;
  [[nodiscard]] bool isValid() const;

private:
  ServerProfileKind m_kind{ServerProfileKind::Custom};
  QString m_id;
  QString m_displayName;
  QUrl m_baseUrl;
};

} // namespace Arkham
