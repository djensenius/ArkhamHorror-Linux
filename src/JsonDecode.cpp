#include "JsonDecode.h"

#include <QJsonDocument>
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

namespace {
// Shared by every obj+key required-field wrapper below: reports a missing
// key with the same "missing required field" phrasing requireRawField/
// requireNullable* already use, rather than letting it fall through to the
// bare value decoder and surface as a less specific "expected <type>, got
// missing". A present value (of any type, including the wrong one) is
// still forwarded to `valueDecoder` unchanged.
template <typename T, typename ValueDecoder>
ValueOrError<T> requireFieldOr(const QJsonObject &obj, QLatin1StringView key,
                               QStringView path, ValueDecoder valueDecoder) {
  if (fieldPresence(obj, key) == FieldPresence::Absent)
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  return valueDecoder(obj.value(key), path);
}
} // namespace

ValueOrError<QString> requireString(const QJsonObject &obj,
                                    QLatin1StringView key, QStringView path) {
  return requireFieldOr<QString>(obj, key, path, requireStringValue);
}

ValueOrError<qint64> requireInt(const QJsonObject &obj, QLatin1StringView key,
                                QStringView path) {
  return requireFieldOr<qint64>(obj, key, path, requireIntValue);
}

ValueOrError<bool> requireBool(const QJsonObject &obj, QLatin1StringView key,
                               QStringView path) {
  return requireFieldOr<bool>(obj, key, path, requireBoolValue);
}

ValueOrError<QJsonObject> requireObjectField(const QJsonObject &obj,
                                             QLatin1StringView key,
                                             QStringView path) {
  return requireFieldOr<QJsonObject>(obj, key, path, requireObject);
}

ValueOrError<QJsonArray> requireArrayField(const QJsonObject &obj,
                                           QLatin1StringView key,
                                           QStringView path) {
  return requireFieldOr<QJsonArray>(obj, key, path, requireArray);
}

ValueOrError<QJsonValue> requireRawField(const QJsonObject &obj,
                                         QLatin1StringView key,
                                         QStringView path) {
  if (fieldPresence(obj, key) == FieldPresence::Absent)
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  return obj.value(key);
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
  switch (fieldPresence(obj, key)) {
  case FieldPresence::Absent:
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  case FieldPresence::Null:
    return std::optional<QString>{};
  case FieldPresence::Present:
    break;
  }
  const QJsonValue v = obj.value(key);
  if (!v.isString())
    return failure(
        QStringLiteral("%1: expected string, got %2").arg(path, typeName(v)));
  return std::optional<QString>{v.toString()};
}

ValueOrError<std::optional<qint64>> requireNullableInt(const QJsonObject &obj,
                                                       QLatin1StringView key,
                                                       QStringView path) {
  switch (fieldPresence(obj, key)) {
  case FieldPresence::Absent:
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  case FieldPresence::Null:
    return std::optional<qint64>{};
  case FieldPresence::Present:
    break;
  }
  auto result = requireIntValue(obj.value(key), path);
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
  // guaranteed by the isfinite() check above.
  Q_ASSERT(conv.ec == std::errc{});
  const std::string_view text(buf, static_cast<size_t>(conv.ptr - buf));

  const size_t ePos = text.find('e');
  // Guaranteed present: std::to_chars(..., chars_format::scientific) always
  // emits an 'e' exponent marker for a finite value.
  Q_ASSERT(ePos != std::string_view::npos);
  const std::string_view mantissa = text.substr(0, ePos);
  std::string_view expText = text.substr(ePos + 1);
  if (!expText.empty() && expText.front() == '+')
    expText.remove_prefix(1);
  int scientificExponent = 0;
  std::from_chars(expText.data(), expText.data() + expText.size(),
                  scientificExponent);

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
