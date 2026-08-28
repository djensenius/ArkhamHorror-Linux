#pragma once

#include "ContractRevision.h"
#include "ValueOrError.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace Arkham {

// Capabilities reported by a live server via GET /api/v1/capabilities.
// All string fields use open-string semantics: unknown values are preserved
// without error to stay forward-compatible with future server versions.
struct ServerCapabilities {
  // Semantic version of the API contract schema in use.
  ContractRevision schemaRevision;
  // Server-reported deployment status (open string; e.g.
  // "baseline-incomplete").
  QString status;
  // Base path of the API (expected: "/api/v1").
  QString apiBasePath;
  // Minimum native-client revision the server will accept.
  ContractRevision nativeClientMinimumRevision;
  // Additive feature flags declared by the server (open strings).
  QStringList capabilities;
  // True only when constructed via legacyFallback(); never set by fromJson().
  bool isLegacyFallback{false};

  // Decode from a parsed JSON object.  Returns an actionable error string on
  // any structural or type mismatch so the caller can log or surface it.
  [[nodiscard]] static ValueOrError<ServerCapabilities>
  fromJson(const QJsonObject &obj);

  // Conservative fallback used when the capabilities endpoint returns 404
  // (server predates the endpoint).  Reports no capabilities and the minimum
  // revision so compatibility checks behave conservatively.
  [[nodiscard]] static ServerCapabilities legacyFallback();

  [[nodiscard]] bool hasCapability(QStringView name) const;
};

} // namespace Arkham
