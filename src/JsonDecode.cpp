#include "JsonDecode.h"

#include <QLatin1StringView>
#include <charconv>
#include <cmath>
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
  return static_cast<int>(d);
}

ValueOrError<bool> requireBoolValue(const QJsonValue &v, QStringView path) {
  if (!v.isBool())
    return failure(
        QStringLiteral("%1: expected bool, got %2").arg(path, typeName(v)));
  return v.toBool();
}

ValueOrError<QString> requireString(const QJsonObject &obj,
                                    QLatin1StringView key, QStringView path) {
  return requireStringValue(obj.value(key), path);
}

ValueOrError<int> requireInt(const QJsonObject &obj, QLatin1StringView key,
                             QStringView path) {
  return requireIntValue(obj.value(key), path);
}

ValueOrError<bool> requireBool(const QJsonObject &obj, QLatin1StringView key,
                               QStringView path) {
  return requireBoolValue(obj.value(key), path);
}

ValueOrError<QJsonObject> requireObjectField(const QJsonObject &obj,
                                             QLatin1StringView key,
                                             QStringView path) {
  return requireObject(obj.value(key), path);
}

ValueOrError<QJsonArray> requireArrayField(const QJsonObject &obj,
                                           QLatin1StringView key,
                                           QStringView path) {
  return requireArray(obj.value(key), path);
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
  if (!v.isDouble())
    return failure(
        QStringLiteral("%1: expected integer, got %2").arg(path, typeName(v)));
  const double d = v.toDouble();
  if (std::trunc(d) != d)
    return failure(
        QStringLiteral("%1: expected integer, got non-integral number")
            .arg(path));
  return std::optional<int>{static_cast<int>(d)};
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
  const QJsonValue v = obj.value(key);
  if (!v.isDouble())
    return failure(
        QStringLiteral("%1: expected integer, got %2").arg(path, typeName(v)));
  const double d = v.toDouble();
  if (std::trunc(d) != d)
    return failure(
        QStringLiteral("%1: expected integer, got non-integral number")
            .arg(path));
  return std::optional<int>{static_cast<int>(d)};
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

QString scientificShow(double value) {
  // See Data.Scientific's Show instance: fixed-point with a forced ".0" when
  // 0.1 <= |x| < 1e7, scientific notation ("d.ddde<exp>", no zero padding)
  // otherwise. `e` below follows that module's own convention: the decimal
  // exponent such that |value| == 0.<digits> * 10^e.
  const bool negative = value < 0.0;
  const double absValue = std::fabs(value);

  char buf[64];
  const auto conv = std::to_chars(buf, buf + sizeof(buf), absValue,
                                  std::chars_format::scientific);
  Q_ASSERT(conv.ec == std::errc{});
  const std::string_view text(buf, static_cast<size_t>(conv.ptr - buf));

  const size_t ePos = text.find('e');
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
