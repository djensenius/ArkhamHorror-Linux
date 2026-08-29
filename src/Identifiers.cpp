#include "Identifiers.h"

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

template <typename V>
ValueOrError<CardCode> cardCodeFromValueImpl(const V &v, QStringView path) {
  if (!v.isString())
    return failure(QStringLiteral("%1: expected string, got %2")
                       .arg(path, Json::typeName(v)));
  auto result = CardCode::parse(v.toString());
  if (!result)
    return failure(QStringLiteral("%1: %2").arg(path, result.error()));
  return *result;
}

template <typename V>
ValueOrError<CardName> cardNameFromValueImpl(const V &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto titleResult =
      Json::requireString(obj, "title"_L1, Json::joinPath(path, u"title"));
  if (!titleResult)
    return failure(titleResult.error());

  auto subtitleResult = Json::requireNullableString(
      obj, "subtitle"_L1, Json::joinPath(path, u"subtitle"));
  if (!subtitleResult)
    return failure(subtitleResult.error());

  return CardName{.title = *titleResult, .subtitle = *subtitleResult};
}

} // namespace

ValueOrError<CardCode> CardCode::parse(const QString &text) {
  // Matches contracts/schemas/catalog.schema.json's `cardCode` pattern
  // `^c.+$` under plain ECMA-262 regex semantics (no schema-level regex
  // flags): `^`/`$` anchor to the whole-string boundaries only (no
  // multiline collapsing of a trailing line terminator), comparisons are
  // done per UTF-16 code unit without any Unicode normalization (so this
  // never NFC/NFD-folds the input), and `.` matches any code unit except
  // the four ECMA-262 line terminators (LF, CR, U+2028 LINE SEPARATOR,
  // U+2029 PARAGRAPH SEPARATOR) -- including ones embedded mid-string, not
  // just a trailing one. A supplementary-plane character contributes two
  // UTF-16 code units, each independently satisfying `.` (never a line
  // terminator), so no special surrogate-pair handling is needed beyond
  // this per-code-unit exclusion.
  if (text.size() < 2 || text.at(0) != u'c')
    return failure(QStringLiteral("card code must start with 'c' and have at "
                                  "least one more character: \"%1\"")
                       .arg(text));
  for (qsizetype i = 1; i < text.size(); ++i) {
    const QChar c = text.at(i);
    if (c == u'\n' || c == u'\r' || c == QChar(0x2028) || c == QChar(0x2029))
      return failure(
          QStringLiteral("card code must not contain a line terminator: \"%1\"")
              .arg(text));
  }
  return CardCode(text);
}

ValueOrError<CardCode> CardCode::fromJson(const QJsonValue &v,
                                          QStringView path) {
  return cardCodeFromValueImpl(v, path);
}

ValueOrError<CardCode> CardCode::fromJson(const Json::Value &v,
                                          QStringView path) {
  return cardCodeFromValueImpl(v, path);
}

ValueOrError<CardName> CardName::fromJson(const QJsonValue &v,
                                          QStringView path) {
  return cardNameFromValueImpl(v, path);
}

ValueOrError<CardName> CardName::fromJson(const Json::Value &v,
                                          QStringView path) {
  return cardNameFromValueImpl(v, path);
}

QJsonObject CardName::toJson() const {
  return QJsonObject{
      {QStringLiteral("title"), title},
      // QJsonValue()'s default constructor is QJsonValue::Null, not
      // Undefined -- QJsonObject::insert() only drops a key for an
      // explicit Undefined value, so this key is preserved with an
      // explicit JSON null. Spelled out explicitly (rather than relying
      // on the default constructor's less obvious Null default) since
      // catalog.schema.json requires "subtitle" to be present, just
      // nullable.
      {QStringLiteral("subtitle"),
       subtitle ? QJsonValue(*subtitle) : QJsonValue(QJsonValue::Null)},
  };
}

Json::Value CardName::toRawJson() const {
  QList<std::pair<QString, Json::Value>> members;
  members.append({QStringLiteral("title"), Json::Value::makeString(title)});
  members.append(
      {QStringLiteral("subtitle"), subtitle ? Json::Value::makeString(*subtitle)
                                            : Json::Value::makeNull()});
  return Json::Value::makeObject(std::move(members));
}

} // namespace Arkham
