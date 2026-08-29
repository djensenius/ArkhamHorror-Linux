#include "Identifiers.h"

using namespace Qt::StringLiterals;

namespace Arkham {

ValueOrError<CardCode> CardCode::parse(const QString &text) {
  // Matches contracts/schemas/catalog.schema.json's `cardCode` pattern
  // `^c.+$`: a literal 'c' followed by at least one more character.
  if (text.size() < 2 || text.at(0) != u'c')
    return failure(QStringLiteral("card code must start with 'c' and have at "
                                  "least one more character: \"%1\"")
                       .arg(text));
  return CardCode(text);
}

ValueOrError<CardCode> CardCode::fromJson(const QJsonValue &v,
                                          QStringView path) {
  if (!v.isString())
    return failure(QStringLiteral("%1: expected string, got %2")
                       .arg(path, Json::typeName(v)));
  auto result = parse(v.toString());
  if (!result)
    return failure(QStringLiteral("%1: %2").arg(path, result.error()));
  return *result;
}

ValueOrError<CardName> CardName::fromJson(const QJsonValue &v,
                                          QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

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

QJsonObject CardName::toJson() const {
  return QJsonObject{
      {QStringLiteral("title"), title},
      {QStringLiteral("subtitle"),
       subtitle ? QJsonValue(*subtitle) : QJsonValue()},
  };
}

} // namespace Arkham
