#include "JsonDecode.h"

#include <QLatin1StringView>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

using namespace Qt::StringLiterals;

namespace Arkham::Json {

QString typeName(const QJsonValue &v) {
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

QString joinPath(QStringView parent, QStringView field) {
  if (parent.isEmpty())
    return field.toString();
  return QStringLiteral("%1.%2").arg(parent, field);
}

QString indexPath(QStringView parent, qsizetype index) {
  return QStringLiteral("%1[%2]").arg(parent).arg(index);
}

FieldPresence fieldPresence(const QJsonObject &obj, QLatin1StringView key) {
  const auto it = obj.constFind(key);
  if (it == obj.constEnd())
    return FieldPresence::Absent;
  return it->isNull() ? FieldPresence::Null : FieldPresence::Present;
}

ValueOrError<QJsonObject> requireObject(const QJsonValue &v, QStringView path) {
  if (!v.isObject())
    return failure(
        QStringLiteral("%1: expected object, got %2").arg(path, typeName(v)));
  return v.toObject();
}

ValueOrError<QJsonArray> requireArray(const QJsonValue &v, QStringView path) {
  if (!v.isArray())
    return failure(
        QStringLiteral("%1: expected array, got %2").arg(path, typeName(v)));
  return v.toArray();
}

ValueOrError<QString> requireStringValue(const QJsonValue &v,
                                         QStringView path) {
  if (!v.isString())
    return failure(
        QStringLiteral("%1: expected string, got %2").arg(path, typeName(v)));
  return v.toString();
}

ValueOrError<qint64> requireIntValue(const QJsonValue &v, QStringView path) {
  // QJsonValue::isDouble() covers every JSON number; reject non-numbers
  // outright rather than silently truncating e.g. a string or bool.
  if (!v.isDouble())
    return failure(
        QStringLiteral("%1: expected integer, got %2").arg(path, typeName(v)));
  const double d = v.toDouble();
  if (!std::isfinite(d) || std::trunc(d) != d)
    return failure(
        QStringLiteral("%1: expected integer, got non-integral number")
            .arg(path));
  // Deliberately NOT range-checked by comparing `d` against a qint64
  // boundary: `d` is a double approximation of whatever this QJsonValue's
  // underlying storage actually is, and for a value at/near the qint64
  // boundary that approximation can itself already be wrong. Concretely,
  // qint64::max() (9223372036854775807) is not exactly representable as a
  // double and rounds UP to 2^63 -- so a naive `d >= 2^63` check rejects a
  // perfectly valid boundary value.
  //
  // QJsonValue::toInteger() is documented to read the value's underlying
  // storage directly and return the *exact* qint64 when the value fits
  // (this holds both for a QJsonValue built via the qint64 constructor --
  // see RawJson.h's Value::toQJson() -- and for one parsed by Qt's own
  // QJsonDocument::fromJson() from a bare integer literal), and to return
  // the default value (0) when it does not fit. Re-widening that result
  // back to double and comparing against `d` -- rather than trusting the
  // sentinel 0 outright, which could theoretically collide with a
  // genuine zero -- distinguishes "exact value recovered" from "value
  // didn't fit": a faithfully-recovered integer always re-widens to the
  // same double Qt already computed for `d` (by the same round-to-nearest
  // rule both conversions follow), while the out-of-range sentinel does
  // not (unless `d` is genuinely 0, which trivially matches).
  const qint64 candidate = v.toInteger();
  if (static_cast<double>(candidate) != d)
    return failure(
        QStringLiteral("%1: integer %2 is out of range for a 64-bit integer")
            .arg(path)
            .arg(d, 0, 'f', 0));
  return candidate;
}

ValueOrError<bool> requireBoolValue(const QJsonValue &v, QStringView path) {
  if (!v.isBool())
    return failure(
        QStringLiteral("%1: expected bool, got %2").arg(path, typeName(v)));
  return v.toBool();
}

QString typeName(const Json::Value &v) {
  switch (v.kind()) {
  case Json::Value::Kind::Null:
    return QStringLiteral("null");
  case Json::Value::Kind::Bool:
    return QStringLiteral("bool");
  case Json::Value::Kind::Number:
    return QStringLiteral("number");
  case Json::Value::Kind::String:
    return QStringLiteral("string");
  case Json::Value::Kind::Array:
    return QStringLiteral("array");
  case Json::Value::Kind::Object:
    return QStringLiteral("object");
  case Json::Value::Kind::Undefined:
    return QStringLiteral("missing");
  }
  Q_UNREACHABLE_RETURN(QStringLiteral("unknown"));
}

FieldPresence fieldPresence(const Json::Value &obj, QLatin1StringView key) {
  // A single Value::find() lookup -- rather than a separate contains()
  // check followed by a second value() lookup, and never Value::value()
  // alone (see its own doc comment in RawJson.h: unlike find(), it cannot
  // distinguish "no such key" from "key present with a directly-
  // constructed Kind::Undefined value", since both return an identical
  // default-constructed Undefined Value). find()'s std::optional engaged/
  // disengaged state alone already answers "is this key present" for
  // every case, including that one -- present-with-Undefined classifies
  // as Present below, not Absent, since the key genuinely exists in the
  // object.
  const std::optional<Json::Value> found = obj.find(key);
  if (!found)
    return FieldPresence::Absent;
  return found->isNull() ? FieldPresence::Null : FieldPresence::Present;
}

ValueOrError<Json::Value> requireObject(const Json::Value &v,
                                        QStringView path) {
  if (!v.isObject())
    return failure(
        QStringLiteral("%1: expected object, got %2").arg(path, typeName(v)));
  return v;
}

ValueOrError<QList<Json::Value>> requireArray(const Json::Value &v,
                                              QStringView path) {
  if (!v.isArray())
    return failure(
        QStringLiteral("%1: expected array, got %2").arg(path, typeName(v)));
  return v.toArray();
}

ValueOrError<QString> requireStringValue(const Json::Value &v,
                                         QStringView path) {
  if (!v.isString())
    return failure(
        QStringLiteral("%1: expected string, got %2").arg(path, typeName(v)));
  return v.toString();
}

ValueOrError<qint64> requireIntValue(const Json::Value &v, QStringView path) {
  if (!v.isNumber())
    return failure(
        QStringLiteral("%1: expected integer, got %2").arg(path, typeName(v)));
  // RawNumber::toExactInt64() reads the literal's exact digits directly
  // (no double involved anywhere), so -- unlike the QJsonValue overload
  // above -- there is no boundary/precision case to reconcile: every
  // qint64-range mathematically-integral literal (see that method's own
  // doc comment for the exact set of Aeson-compatible spellings accepted)
  // decodes exactly, and everything else is rejected outright.
  auto exact = v.toRawNumber().toExactInt64();
  if (!exact)
    return failure(QStringLiteral("%1: expected integer, got a fractional "
                                  "or out-of-range number")
                       .arg(path));
  return *exact;
}

ValueOrError<bool> requireBoolValue(const Json::Value &v, QStringView path) {
  if (!v.isBool())
    return failure(
        QStringLiteral("%1: expected bool, got %2").arg(path, typeName(v)));
  return v.toBool();
}

ValueOrError<QString> requireString(const QJsonObject &obj,
                                    QLatin1StringView key, QStringView path) {
  return requireField(obj, key, path, [](const QJsonValue &v, QStringView p) {
    return requireStringValue(v, p);
  });
}

ValueOrError<qint64> requireInt(const QJsonObject &obj, QLatin1StringView key,
                                QStringView path) {
  return requireField(obj, key, path, [](const QJsonValue &v, QStringView p) {
    return requireIntValue(v, p);
  });
}

ValueOrError<bool> requireBool(const QJsonObject &obj, QLatin1StringView key,
                               QStringView path) {
  return requireField(obj, key, path, [](const QJsonValue &v, QStringView p) {
    return requireBoolValue(v, p);
  });
}

ValueOrError<QJsonObject> requireObjectField(const QJsonObject &obj,
                                             QLatin1StringView key,
                                             QStringView path) {
  return requireField(obj, key, path, [](const QJsonValue &v, QStringView p) {
    return requireObject(v, p);
  });
}

ValueOrError<QJsonArray> requireArrayField(const QJsonObject &obj,
                                           QLatin1StringView key,
                                           QStringView path) {
  return requireField(obj, key, path, [](const QJsonValue &v, QStringView p) {
    return requireArray(v, p);
  });
}

ValueOrError<QString> requireString(const Json::Value &obj,
                                    QLatin1StringView key, QStringView path) {
  return requireField(obj, key, path, [](const Json::Value &v, QStringView p) {
    return requireStringValue(v, p);
  });
}

ValueOrError<qint64> requireInt(const Json::Value &obj, QLatin1StringView key,
                                QStringView path) {
  return requireField(obj, key, path, [](const Json::Value &v, QStringView p) {
    return requireIntValue(v, p);
  });
}

ValueOrError<bool> requireBool(const Json::Value &obj, QLatin1StringView key,
                               QStringView path) {
  return requireField(obj, key, path, [](const Json::Value &v, QStringView p) {
    return requireBoolValue(v, p);
  });
}

ValueOrError<Json::Value> requireObjectField(const Json::Value &obj,
                                             QLatin1StringView key,
                                             QStringView path) {
  return requireField(obj, key, path, [](const Json::Value &v, QStringView p) {
    return requireObject(v, p);
  });
}

ValueOrError<QList<Json::Value>> requireArrayField(const Json::Value &obj,
                                                   QLatin1StringView key,
                                                   QStringView path) {
  return requireField(obj, key, path, [](const Json::Value &v, QStringView p) {
    return requireArray(v, p);
  });
}

ValueOrError<QJsonValue> requireRawField(const QJsonObject &obj,
                                         QLatin1StringView key,
                                         QStringView path) {
  // detail::findField(), shared with requireField()'s own template above
  // -- see its doc comment for why this is a single lookup and already
  // unambiguous for QJsonObject specifically.
  auto found = detail::findField(obj, key);
  if (!found)
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  return *found;
}

ValueOrError<Json::Value> requireRawField(const Json::Value &obj,
                                          QLatin1StringView key,
                                          QStringView path) {
  auto found = detail::findField(obj, key);
  if (!found)
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  if (found->isUndefined())
    // The key is genuinely present (found engaged) but its stored value
    // is a directly-constructed Kind::Undefined -- reachable only via a
    // programmatically-built Json::Value AST (e.g. makeObject()), never
    // via Value::parse() -- which has no valid JSON spelling at all.
    // Neither "missing" (the key does exist) nor a silent success
    // (handing back an unrepresentable-in-JSON "raw value" for a field
    // whose whole contract is to preserve exactly what was present) is
    // honest here, so this is its own distinct, specific failure.
    return failure(QStringLiteral("%1: field \"%2\" holds no valid JSON value")
                       .arg(path, key));
  return *found;
}

ValueOrError<std::optional<QString>> optionalString(const QJsonObject &obj,
                                                    QLatin1StringView key,
                                                    QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined() || v.isNull())
    return std::optional<QString>{};
  if (!v.isString())
    return failure(
        QStringLiteral("%1: expected string, got %2").arg(path, typeName(v)));
  return std::optional<QString>{v.toString()};
}

ValueOrError<std::optional<qint64>>
optionalInt(const QJsonObject &obj, QLatin1StringView key, QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined() || v.isNull())
    return std::optional<qint64>{};
  auto result = requireIntValue(v, path);
  if (!result)
    return failure(result.error());
  return std::optional<qint64>{*result};
}

ValueOrError<std::optional<bool>>
optionalBool(const QJsonObject &obj, QLatin1StringView key, QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined() || v.isNull())
    return std::optional<bool>{};
  if (!v.isBool())
    return failure(
        QStringLiteral("%1: expected bool, got %2").arg(path, typeName(v)));
  return std::optional<bool>{v.toBool()};
}

ValueOrError<std::optional<QString>>
optionalNonNullString(const QJsonObject &obj, QLatin1StringView key,
                      QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined())
    return std::optional<QString>{};
  auto result = requireStringValue(v, path);
  if (!result)
    return failure(result.error());
  return std::optional<QString>{*result};
}

ValueOrError<std::optional<qint64>> optionalNonNullInt(const QJsonObject &obj,
                                                       QLatin1StringView key,
                                                       QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined())
    return std::optional<qint64>{};
  auto result = requireIntValue(v, path);
  if (!result)
    return failure(result.error());
  return std::optional<qint64>{*result};
}

ValueOrError<std::optional<bool>> optionalNonNullBool(const QJsonObject &obj,
                                                      QLatin1StringView key,
                                                      QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined())
    return std::optional<bool>{};
  auto result = requireBoolValue(v, path);
  if (!result)
    return failure(result.error());
  return std::optional<bool>{*result};
}

ValueOrError<QJsonValue> optionalRawArrayField(const QJsonObject &obj,
                                               QLatin1StringView key,
                                               QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined())
    return v;
  if (!v.isArray())
    return failure(
        QStringLiteral("%1: expected array, got %2").arg(path, typeName(v)));
  return v;
}

ValueOrError<QJsonValue> optionalRawObjectField(const QJsonObject &obj,
                                                QLatin1StringView key,
                                                QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined())
    return v;
  if (!v.isObject())
    return failure(
        QStringLiteral("%1: expected object, got %2").arg(path, typeName(v)));
  return v;
}

ValueOrError<std::optional<QString>>
requireNullableString(const QJsonObject &obj, QLatin1StringView key,
                      QStringView path) {
  // Single obj.value(key) lookup; see requireField()'s doc comment above.
  const QJsonValue v = obj.value(key);
  if (v.isUndefined())
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  if (v.isNull())
    return std::optional<QString>{};
  if (!v.isString())
    return failure(
        QStringLiteral("%1: expected string, got %2").arg(path, typeName(v)));
  return std::optional<QString>{v.toString()};
}

ValueOrError<std::optional<qint64>> requireNullableInt(const QJsonObject &obj,
                                                       QLatin1StringView key,
                                                       QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined())
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  if (v.isNull())
    return std::optional<qint64>{};
  auto result = requireIntValue(v, path);
  if (!result)
    return failure(result.error());
  return std::optional<qint64>{*result};
}

ValueOrError<QUuid> decodeUuid(const QJsonValue &v, QStringView path) {
  if (!v.isString())
    return failure(QStringLiteral("%1: expected uuid string, got %2")
                       .arg(path, typeName(v)));
  const QString text = v.toString();
  const QUuid parsed(text);
  if (parsed.isNull())
    return failure(QStringLiteral("%1: not a valid non-null uuid").arg(path));
  return parsed;
}

ValueOrError<std::optional<QUuid>> decodeNullableUuid(const QJsonValue &v,
                                                      QStringView path) {
  if (v.isNull())
    return std::optional<QUuid>{};
  auto result = decodeUuid(v, path);
  if (!result)
    return failure(result.error());
  return std::optional<QUuid>{*result};
}

namespace {
// Shared single-lookup presence classification for every optional*/
// requireNullable* Json::Value field helper below: distinguishes a key
// genuinely absent from the object (returns an empty success, i.e.
// std::optional<Json::Value>{}) from one present with a directly-
// constructed Kind::Undefined value (a Json::Value AST state reachable
// only via a programmatically-built object, e.g. makeObject({{"k",
// Value{}}}); never via Value::parse() -- see Value::find()'s doc
// comment in RawJson.h), which is neither a value any of these helpers
// could honestly decode nor a state indistinguishable from "omitted" --
// so it is reported as a distinct typed failure instead of silently
// folding into "absent, treat as std::nullopt/default". A genuinely
// present, non-Undefined value (including an explicit JSON null, left to
// each caller's own null-handling) is forwarded unchanged. This is the
// one place all ten Json::Value optional/nullable field helpers share
// that classification, via a single obj.find(key) lookup each.
ValueOrError<std::optional<Json::Value>>
findOptionalField(const Json::Value &obj, QLatin1StringView key,
                  QStringView path) {
  auto found = obj.find(key);
  if (!found)
    return std::optional<Json::Value>{};
  if (found->isUndefined())
    return failure(QStringLiteral("%1: field \"%2\" holds no valid JSON value")
                       .arg(path, key));
  return std::optional<Json::Value>{std::move(*found)};
}
} // namespace

ValueOrError<std::optional<QString>> optionalString(const Json::Value &obj,
                                                    QLatin1StringView key,
                                                    QStringView path) {
  auto found = findOptionalField(obj, key, path);
  if (!found)
    return failure(found.error());
  if (!*found || (*found)->isNull())
    return std::optional<QString>{};
  auto result = requireStringValue(**found, path);
  if (!result)
    return failure(result.error());
  return std::optional<QString>{*result};
}

ValueOrError<std::optional<qint64>>
optionalInt(const Json::Value &obj, QLatin1StringView key, QStringView path) {
  auto found = findOptionalField(obj, key, path);
  if (!found)
    return failure(found.error());
  if (!*found || (*found)->isNull())
    return std::optional<qint64>{};
  auto result = requireIntValue(**found, path);
  if (!result)
    return failure(result.error());
  return std::optional<qint64>{*result};
}

ValueOrError<std::optional<bool>>
optionalBool(const Json::Value &obj, QLatin1StringView key, QStringView path) {
  auto found = findOptionalField(obj, key, path);
  if (!found)
    return failure(found.error());
  if (!*found || (*found)->isNull())
    return std::optional<bool>{};
  auto result = requireBoolValue(**found, path);
  if (!result)
    return failure(result.error());
  return std::optional<bool>{*result};
}

ValueOrError<std::optional<QString>>
optionalNonNullString(const Json::Value &obj, QLatin1StringView key,
                      QStringView path) {
  auto found = findOptionalField(obj, key, path);
  if (!found)
    return failure(found.error());
  if (!*found)
    return std::optional<QString>{};
  auto result = requireStringValue(**found, path);
  if (!result)
    return failure(result.error());
  return std::optional<QString>{*result};
}

ValueOrError<std::optional<qint64>> optionalNonNullInt(const Json::Value &obj,
                                                       QLatin1StringView key,
                                                       QStringView path) {
  auto found = findOptionalField(obj, key, path);
  if (!found)
    return failure(found.error());
  if (!*found)
    return std::optional<qint64>{};
  auto result = requireIntValue(**found, path);
  if (!result)
    return failure(result.error());
  return std::optional<qint64>{*result};
}

ValueOrError<std::optional<bool>> optionalNonNullBool(const Json::Value &obj,
                                                      QLatin1StringView key,
                                                      QStringView path) {
  auto found = findOptionalField(obj, key, path);
  if (!found)
    return failure(found.error());
  if (!*found)
    return std::optional<bool>{};
  auto result = requireBoolValue(**found, path);
  if (!result)
    return failure(result.error());
  return std::optional<bool>{*result};
}

ValueOrError<Json::Value> optionalRawArrayField(const Json::Value &obj,
                                                QLatin1StringView key,
                                                QStringView path) {
  auto found = findOptionalField(obj, key, path);
  if (!found)
    return failure(found.error());
  if (!*found)
    return Json::Value{};
  if (!(*found)->isArray())
    return failure(QStringLiteral("%1: expected array, got %2")
                       .arg(path, typeName(**found)));
  return **found;
}

ValueOrError<Json::Value> optionalRawObjectField(const Json::Value &obj,
                                                 QLatin1StringView key,
                                                 QStringView path) {
  auto found = findOptionalField(obj, key, path);
  if (!found)
    return failure(found.error());
  if (!*found)
    return Json::Value{};
  if (!(*found)->isObject())
    return failure(QStringLiteral("%1: expected object, got %2")
                       .arg(path, typeName(**found)));
  return **found;
}

ValueOrError<std::optional<QString>>
requireNullableString(const Json::Value &obj, QLatin1StringView key,
                      QStringView path) {
  auto found = findOptionalField(obj, key, path);
  if (!found)
    return failure(found.error());
  if (!*found)
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  if ((*found)->isNull())
    return std::optional<QString>{};
  auto result = requireStringValue(**found, path);
  if (!result)
    return failure(result.error());
  return std::optional<QString>{*result};
}

ValueOrError<std::optional<qint64>> requireNullableInt(const Json::Value &obj,
                                                       QLatin1StringView key,
                                                       QStringView path) {
  auto found = findOptionalField(obj, key, path);
  if (!found)
    return failure(found.error());
  if (!*found)
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  if ((*found)->isNull())
    return std::optional<qint64>{};
  auto result = requireIntValue(**found, path);
  if (!result)
    return failure(result.error());
  return std::optional<qint64>{*result};
}

ValueOrError<QUuid> decodeUuid(const Json::Value &v, QStringView path) {
  if (!v.isString())
    return failure(QStringLiteral("%1: expected uuid string, got %2")
                       .arg(path, typeName(v)));
  const QString text = v.toString();
  const QUuid parsed(text);
  if (parsed.isNull())
    return failure(QStringLiteral("%1: not a valid non-null uuid").arg(path));
  return parsed;
}

ValueOrError<std::optional<QUuid>> decodeNullableUuid(const Json::Value &v,
                                                      QStringView path) {
  if (v.isNull())
    return std::optional<QUuid>{};
  auto result = decodeUuid(v, path);
  if (!result)
    return failure(result.error());
  return std::optional<QUuid>{*result};
}

QList<std::pair<QString, QJsonValue>> objectMembers(const QJsonObject &obj) {
  QList<std::pair<QString, QJsonValue>> result;
  result.reserve(obj.size());
  for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
    result.append({it.key(), it.value()});
  return result;
}

const QList<std::pair<QString, Json::Value>> &
objectMembers(const Json::Value &obj) {
  return obj.members();
}

ValueOrError<Json::Value> toLosslessRaw(const QJsonValue &v) {
  return Json::Value::fromQJson(v);
}
ValueOrError<Json::Value> toLosslessRaw(const Json::Value &v) { return v; }

ValueOrError<QString> scientificShow(double value, QStringView path) {
  // A syntactically valid JSON number can still parse to a non-finite
  // double once Qt's JSON parser has already narrowed it (e.g. "1e400"
  // overflows to +Infinity) -- reject explicitly here rather than falling
  // into std::to_chars's non-scientific "inf"/"nan" textual output, which
  // has no Scientific-style digits/exponent to extract.
  if (!std::isfinite(value))
    return failure(
        QStringLiteral("%1: number is too large to represent").arg(path));

  // See Data.Scientific's Show instance: fixed-point with a forced ".0" when
  // 0.1 <= |x| < 1e7, scientific notation ("d.ddde<exp>", no zero padding)
  // otherwise. `e` below follows that module's own convention: the decimal
  // exponent such that |value| == 0.<digits> * 10^e.
  const bool negative = value < 0.0;
  const double absValue = std::fabs(value);

  char buf[64];
  const auto conv = std::to_chars(buf, buf + sizeof(buf), absValue,
                                  std::chars_format::scientific);
  // Not data-dependent: every finite double's shortest round-trip
  // scientific-notation representation fits comfortably within 64 bytes
  // (at most ~1 sign + 17 significant digits + '.' + 'e' + sign + 3
  // exponent digits), so this can never fail for the finite `absValue`
  // guaranteed by the isfinite() check above. Checked explicitly (rather
  // than Q_ASSERT, which compiles out in release builds) so a
  // standard-library-specific quirk this invariant did not anticipate can
  // never silently proceed with an incomplete/garbage buffer and
  // mis-encode a wire number with no indication anything went wrong.
  if (conv.ec != std::errc{})
    return failure(
        QStringLiteral("%1: internal error formatting number").arg(path));
  const std::string_view text(buf, static_cast<size_t>(conv.ptr - buf));

  const size_t ePos = text.find('e');
  // Guaranteed present: std::to_chars(..., chars_format::scientific) always
  // emits an 'e' exponent marker for a finite value. Checked explicitly
  // (rather than Q_ASSERT) for the same release-build-safety reason as
  // above -- substr()-ing on an unexpected position must fail cleanly,
  // not emit incorrect output.
  if (ePos == std::string_view::npos)
    return failure(
        QStringLiteral("%1: internal error formatting number's mantissa")
            .arg(path));
  const std::string_view mantissa = text.substr(0, ePos);
  std::string_view expText = text.substr(ePos + 1);
  if (!expText.empty() && expText.front() == '+')
    expText.remove_prefix(1);
  int scientificExponent = 0;
  const auto expConv = std::from_chars(
      expText.data(), expText.data() + expText.size(), scientificExponent);
  // expText is exactly the exponent substring std::to_chars(...,
  // chars_format::scientific) itself just emitted a few lines above (with
  // only a leading '+' stripped), so this parse can never legitimately
  // fail for the finite `absValue` guaranteed by the isfinite() check
  // above. Checked explicitly (like the two internal-error checks above),
  // since a discarded from_chars failure here would silently leave
  // scientificExponent at its default-initialized 0 and let this
  // function emit an incorrect wire number with no indication anything
  // went wrong -- matching this function's own ValueOrError<QString>
  // contract, so a standard-library-specific quirk this invariant did not
  // anticipate can never masquerade as a valid encoded value.
  if (expConv.ec != std::errc{} ||
      expConv.ptr != expText.data() + expText.size())
    return failure(
        QStringLiteral("%1: internal error formatting number's exponent")
            .arg(path));

  std::string digits;
  digits.reserve(mantissa.size());
  for (const char c : mantissa)
    if (c != '.')
      digits.push_back(c);

  const int e = scientificExponent + 1;
  const auto digitsQ = [&digits](size_t from, size_t count) {
    return QString::fromLatin1(digits.data() + from,
                               static_cast<qsizetype>(count));
  };

  QString out;
  if (e < 0 || e > 7) {
    out += QLatin1Char(digits.front());
    out += u'.';
    out += digits.size() > 1 ? digitsQ(1, digits.size() - 1) : u"0"_s;
    out += u'e';
    out += QString::number(e - 1);
  } else if (e <= 0) {
    out = u"0."_s + QString(-e, u'0') + digitsQ(0, digits.size());
  } else if (static_cast<size_t>(e) >= digits.size()) {
    out = digitsQ(0, digits.size()) +
          QString(e - static_cast<int>(digits.size()), u'0') + u".0"_s;
  } else {
    out =
        digitsQ(0, static_cast<size_t>(e)) + u'.' +
        digitsQ(static_cast<size_t>(e), digits.size() - static_cast<size_t>(e));
  }
  return negative ? u'-' + out : out;
}

} // namespace Arkham::Json
