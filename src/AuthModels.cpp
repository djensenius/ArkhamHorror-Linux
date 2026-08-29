#include "AuthModels.h"

#include "TokenValidation.h"

#include <QJsonValue>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// Returns a human-readable name for a QJsonValue's type. Mirrors the helper
// in ServerCapabilities.cpp; kept local here since it is a tiny,
// self-contained formatting helper and this translation unit has no other
// shared dependency on that file.
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

// Requires a string field. On failure, the error names only the field and
// the JSON type actually found -- never the offending value -- so decode
// failures are safe to log even when the source document contained a
// password or token.
ValueOrError<QString> requireString(const QJsonObject &obj,
                                    QLatin1StringView key) {
  const QJsonValue v = obj.value(key);
  if (!v.isString()) {
    return failure(QStringLiteral("response field \"%1\": expected string, "
                                  "got %2")
                       .arg(QString::fromLatin1(key), jsonTypeName(v)));
  }
  return v.toString();
}

ValueOrError<bool> requireBool(const QJsonObject &obj, QLatin1StringView key) {
  const QJsonValue v = obj.value(key);
  if (!v.isBool()) {
    return failure(
        QStringLiteral("response field \"%1\": expected bool, got %2")
            .arg(QString::fromLatin1(key), jsonTypeName(v)));
  }
  return v.toBool();
}

} // namespace

QJsonObject AuthenticateRequest::toJson() const {
  return QJsonObject{
      {QStringLiteral("email"), email},
      {QStringLiteral("password"), password},
  };
}

QJsonObject RegisterRequest::toJson() const {
  return QJsonObject{
      {QStringLiteral("email"), email},
      {QStringLiteral("username"), username},
      {QStringLiteral("password"), password},
  };
}

ValueOrError<AuthToken> AuthToken::fromJson(const QJsonObject &obj) {
  auto tokenStr = requireString(obj, "token"_L1);
  if (!tokenStr) {
    return failure(tokenStr.error());
  }
  if (!isValidTokenContent(*tokenStr)) {
    // See TokenValidation.h: this is the exact same shared check every
    // other token trust boundary (whoAmI() admission, the coordinator's
    // own handling of this result, secure-store save, and the durable
    // envelope's own reader) enforces. Rejecting it here, at decode time,
    // means a value like " valid-jwt " (which trims to non-blank but
    // still carries leading/trailing whitespace) can never be admitted
    // into an Authorization header or persisted in the first place, only
    // to be classified Malformed and deleted on the very next restore.
    // The error message never echoes the offending value.
    return failure(
        QStringLiteral("response field \"token\" is not a usable token"));
  }
  return AuthToken{*tokenStr};
}

ValueOrError<CurrentUser> CurrentUser::fromJson(const QJsonObject &obj) {
  CurrentUser user;

  auto usernameStr = requireString(obj, "username"_L1);
  if (!usernameStr) {
    return failure(usernameStr.error());
  }
  user.username = *usernameStr;

  auto emailStr = requireString(obj, "email"_L1);
  if (!emailStr) {
    return failure(emailStr.error());
  }
  user.email = *emailStr;

  auto betaBool = requireBool(obj, "beta"_L1);
  if (!betaBool) {
    return failure(betaBool.error());
  }
  user.beta = *betaBool;

  auto adminBool = requireBool(obj, "admin"_L1);
  if (!adminBool) {
    return failure(adminBool.error());
  }
  user.admin = *adminBool;

  return user;
}

} // namespace Arkham
