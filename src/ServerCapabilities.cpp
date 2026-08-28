#include "ServerCapabilities.h"

#include <QJsonArray>
#include <QJsonValue>
#include <algorithm>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// Returns a human-readable name for a QJsonValue's type.
QString jsonTypeName(const QJsonValue &v) {
  switch (v.type()) {
  case QJsonValue::Null:
    return QStringLiteral("null");
  case QJsonValue::Bool:
    return QStringLiteral("bool");
  case QJsonValue::Double:
    return QStringLiteral("number");
  case QJsonValue::String:
    return QStringLiteral("string");
  case QJsonValue::Array:
    return QStringLiteral("array");
  case QJsonValue::Object:
    return QStringLiteral("object");
  case QJsonValue::Undefined:
    return QStringLiteral("missing");
  }
  Q_UNREACHABLE_RETURN(QStringLiteral("unknown"));
}

// Helper: require a string field; return parse error on any mismatch.
ValueOrError<QString> requireString(const QJsonObject &obj,
                                    QLatin1StringView key) {
  const QJsonValue v = obj.value(key);
  if (!v.isString()) {
    return failure(QStringLiteral("%1: expected string, got %2")
                       .arg(key, jsonTypeName(v)));
  }
  return v.toString();
}

} // namespace

ValueOrError<ServerCapabilities>
ServerCapabilities::fromJson(const QJsonObject &obj) {
  ServerCapabilities caps;

  // schemaRevision — required, must parse as ContractRevision.
  auto schemaRevStr = requireString(obj, "schemaRevision"_L1);
  if (!schemaRevStr) {
    return failure(schemaRevStr.error());
  }
  auto schemaRev = ContractRevision::parse(*schemaRevStr);
  if (!schemaRev) {
    return failure(QStringLiteral("schemaRevision: %1").arg(schemaRev.error()));
  }
  caps.schemaRevision = *schemaRev;

  // status — required open string; any string value is valid.
  auto statusStr = requireString(obj, "status"_L1);
  if (!statusStr) {
    return failure(statusStr.error());
  }
  caps.status = *statusStr;

  // apiBasePath — required string.
  auto apiStr = requireString(obj, "apiBasePath"_L1);
  if (!apiStr) {
    return failure(apiStr.error());
  }
  caps.apiBasePath = *apiStr;

  // nativeClientMinimumRevision — required, must parse as ContractRevision.
  auto nativeRevStr = requireString(obj, "nativeClientMinimumRevision"_L1);
  if (!nativeRevStr) {
    return failure(nativeRevStr.error());
  }
  auto nativeRev = ContractRevision::parse(*nativeRevStr);
  if (!nativeRev) {
    return failure(QStringLiteral("nativeClientMinimumRevision: %1")
                       .arg(nativeRev.error()));
  }
  caps.nativeClientMinimumRevision = *nativeRev;

  // capabilities — required array of strings; duplicates are preserved.
  const QJsonValue capsVal = obj.value("capabilities"_L1);
  if (!capsVal.isArray()) {
    return failure(QStringLiteral("capabilities: expected array, got %1")
                       .arg(jsonTypeName(capsVal)));
  }
  const QJsonArray capsArr = capsVal.toArray();
  caps.capabilities.reserve(static_cast<qsizetype>(capsArr.size()));
  for (qsizetype i = 0; i < capsArr.size(); ++i) {
    const QJsonValue elem = capsArr[i];
    if (!elem.isString()) {
      return failure(QStringLiteral("capabilities[%1]: expected string, got %2")
                         .arg(i)
                         .arg(jsonTypeName(elem)));
    }
    caps.capabilities.append(elem.toString());
  }

  return caps;
}

ServerCapabilities ServerCapabilities::legacyFallback() {
  // Conservative fallback for a pre-capabilities server (404 on endpoint).
  // isLegacyFallback=true causes CompatibilityEvaluator to return the
  // explicit LegacyFallback outcome instead of running version checks against
  // artificially low revision numbers.
  // Callers must not infer any modern feature support from this value.
  return ServerCapabilities{
      .schemaRevision = {0, 1, 0},
      .status = QStringLiteral("legacy"),
      .apiBasePath = QStringLiteral("/api/v1"),
      .nativeClientMinimumRevision = {0, 1, 0},
      .capabilities = {},
      .isLegacyFallback = true,
  };
}

bool ServerCapabilities::hasCapability(const QStringView name) const {
  return std::any_of(capabilities.cbegin(), capabilities.cend(),
                     [name](const QString &capability) {
                       return QStringView(capability) == name;
                     });
}

} // namespace Arkham
