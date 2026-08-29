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

ValueOrError<int> requireIntValue(const QJsonValue &v, QStringView path) {
  // QJsonValue::isDouble() covers every JSON number; reject non-numbers
  // outright rather than silently truncating e.g. a string or bool.
  if (!v.isDouble())
    return failure(
        QStringLiteral("%1: expected integer, got %2").arg(path, typeName(v)));
  const double d = v.toDouble();
  if (std::trunc(d) != d)
    return failure(
        QStringLiteral("%1: expected integer, got non-integral number")
            .arg(path));
  // A value that is integral in IEEE-754 double terms can still fall
  // outside int's representable range (e.g. 1e300); casting such a value
  // via static_cast<int> is undefined behavior, so reject it explicitly
  // rather than truncating/wrapping.
  if (d < static_cast<double>(std::numeric_limits<int>::min()) ||
      d > static_cast<double>(std::numeric_limits<int>::max()))
    return failure(
        QStringLiteral("%1: integer %2 is out of range for a 32-bit int")
            .arg(path)
            .arg(d, 0, 'f', 0));
  return static_cast<int>(d);
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

ValueOrError<int> requireInt(const QJsonObject &obj, QLatin1StringView key,
                             QStringView path) {
  return requireFieldOr<int>(obj, key, path, requireIntValue);
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

ValueOrError<std::optional<int>>
optionalInt(const QJsonObject &obj, QLatin1StringView key, QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined() || v.isNull())
    return std::optional<int>{};
  auto result = requireIntValue(v, path);
  if (!result)
    return failure(result.error());
  return std::optional<int>{*result};
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

ValueOrError<std::optional<int>> optionalNonNullInt(const QJsonObject &obj,
                                                    QLatin1StringView key,
                                                    QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined())
    return std::optional<int>{};
  auto result = requireIntValue(v, path);
  if (!result)
    return failure(result.error());
  return std::optional<int>{*result};
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

ValueOrError<std::optional<int>> requireNullableInt(const QJsonObject &obj,
                                                    QLatin1StringView key,
                                                    QStringView path) {
  switch (fieldPresence(obj, key)) {
  case FieldPresence::Absent:
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  case FieldPresence::Null:
    return std::optional<int>{};
  case FieldPresence::Present:
    break;
  }
  auto result = requireIntValue(obj.value(key), path);
  if (!result)
    return failure(result.error());
  return std::optional<int>{*result};
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

namespace {
// JSON string-escapes `key` for splicing as an object member name.
// QLatin1StringView's bytes are Latin-1 code units, so every byte value is
// numerically identical to its Unicode code point (U+0000-U+00FF); any
// byte outside printable, non-quote/backslash ASCII is therefore safe (and
// exact) to emit as a `\u00XX` escape rather than a raw byte, which keeps
// the result valid UTF-8 JSON regardless of what the caller's key
// contains.
QByteArray escapeJsonKeyBytes(QLatin1StringView key) {
  static constexpr char kHex[] = "0123456789abcdef";
  QByteArray out;
  out.reserve(key.size());
  for (const char rawChar : key) {
    const auto ch = static_cast<unsigned char>(rawChar);
    switch (ch) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (ch < 0x20 || ch >= 0x7F) {
        out += "\\u00";
        out += kHex[(ch >> 4) & 0xF];
        out += kHex[ch & 0xF];
      } else {
        out += static_cast<char>(ch);
      }
    }
  }
  return out;
}
} // namespace

QByteArray spliceRawJsonMember(const QJsonObject &obj, QLatin1StringView key,
                               QByteArrayView rawValueBytes) {
  QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  if (!bytes.endsWith('}'))
    return bytes; // defensive-only: QJsonDocument::toJson always ends in '}'.
  bytes.chop(1);
  if (!obj.isEmpty())
    bytes += ',';
  bytes += '"';
  bytes += escapeJsonKeyBytes(key);
  bytes += "\":";
  bytes += rawValueBytes;
  bytes += '}';
  return bytes;
}

} // namespace Arkham::Json
