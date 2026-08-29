#include "Decks.h"

#include "JsonDecode.h"

#include <QJsonArray>
#include <QJsonDocument>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// Decodes a `cardQuantityMapInput`: an object whose property names need
// only be non-empty, with integer values. Used for DeckListInput.slots.
ValueOrError<QMap<QString, int>>
decodeCardQuantityMapInput(const QJsonObject &obj, QLatin1StringView key,
                           QStringView path) {
  auto objResult = Json::requireObjectField(obj, key, path);
  if (!objResult)
    return failure(objResult.error());
  QMap<QString, int> result;
  for (auto it = objResult->constBegin(); it != objResult->constEnd(); ++it) {
    const QString entryPath = Json::joinPath(path, it.key());
    if (it.key().isEmpty())
      return failure(
          QStringLiteral("%1: card code key must not be empty").arg(path));
    auto amount = Json::requireIntValue(it.value(), entryPath);
    if (!amount)
      return failure(amount.error());
    result.insert(it.key(), *amount);
  }
  return result;
}

// Decodes a `cardQuantityMap`: an object whose property names must be valid
// CardCodes, with integer values. Used for DeckList.slots/sideSlots.
ValueOrError<QMap<CardCode, int>> decodeCardQuantityMap(const QJsonObject &obj,
                                                        QLatin1StringView key,
                                                        QStringView path) {
  auto objResult = Json::requireObjectField(obj, key, path);
  if (!objResult)
    return failure(objResult.error());
  QMap<CardCode, int> result;
  for (auto it = objResult->constBegin(); it != objResult->constEnd(); ++it) {
    const QString entryPath = Json::joinPath(path, it.key());
    auto code = CardCode::parse(it.key());
    if (!code)
      return failure(QStringLiteral("%1: %2").arg(entryPath, code.error()));
    auto amount = Json::requireIntValue(it.value(), entryPath);
    if (!amount)
      return failure(amount.error());
    result.insert(*code, *amount);
  }
  return result;
}

QJsonObject encodeCardQuantityMapInput(const QMap<QString, int> &map) {
  QJsonObject obj;
  for (auto it = map.constBegin(); it != map.constEnd(); ++it)
    obj.insert(it.key(), it.value());
  return obj;
}

QJsonObject encodeCardQuantityMap(const QMap<CardCode, int> &map) {
  QJsonObject obj;
  for (auto it = map.constBegin(); it != map.constEnd(); ++it)
    obj.insert(it.key().value(), it.value());
  return obj;
}

} // namespace

ValueOrError<ExternalDeckId> ExternalDeckId::fromObject(const QJsonObject &obj,
                                                        QStringView path) {
  switch (Json::fieldPresence(obj, "id"_L1)) {
  case Json::FieldPresence::Absent:
    return ExternalDeckId{.tag = ExternalDeckIdTag::Absent};
  case Json::FieldPresence::Null:
    return ExternalDeckId{.tag = ExternalDeckIdTag::Null};
  case Json::FieldPresence::Present:
    break;
  }
  const QJsonValue v = obj.value("id"_L1);
  const QString idPath = Json::joinPath(path, u"id");
  if (v.isString())
    return ExternalDeckId{.tag = ExternalDeckIdTag::Text, .text = v.toString()};
  if (v.isDouble()) {
    auto literal = Json::scientificShow(v.toDouble(), idPath);
    if (!literal)
      return failure(literal.error());
    return ExternalDeckId{.tag = ExternalDeckIdTag::Number,
                          .numberLiteral = *literal};
  }
  return failure(QStringLiteral("%1: expected string, number, or null, got %2")
                     .arg(idPath, Json::typeName(v)));
}

ValueOrError<ExternalDeckId>
ExternalDeckId::fromRawObject(const Json::Value &obj, QStringView path) {
  if (!obj.isObject())
    return failure(QStringLiteral("%1: expected an object, got %2")
                       .arg(path, Json::typeName(obj.toQJson())));
  if (!obj.contains("id"_L1))
    return ExternalDeckId{.tag = ExternalDeckIdTag::Absent};
  const Json::Value v = obj.value("id"_L1);
  const QString idPath = Json::joinPath(path, u"id");
  if (v.isNull())
    return ExternalDeckId{.tag = ExternalDeckIdTag::Null};
  if (v.isString())
    return ExternalDeckId{.tag = ExternalDeckIdTag::Text, .text = v.toString()};
  if (v.isNumber())
    return ExternalDeckId{.tag = ExternalDeckIdTag::Number,
                          .numberLiteral = v.toRawNumber().literal()};
  return failure(QStringLiteral("%1: expected string, number, or null, got %2")
                     .arg(idPath, Json::typeName(v.toQJson())));
}

QJsonValue ExternalDeckId::toJson() const {
  switch (tag) {
  case ExternalDeckIdTag::Absent:
    return QJsonValue(QJsonValue::Undefined);
  case ExternalDeckIdTag::Null:
    return QJsonValue();
  case ExternalDeckIdTag::Text:
    return QJsonValue(text);
  case ExternalDeckIdTag::Number:
    return QJsonValue(numberLiteral.toDouble());
  }
  Q_UNREACHABLE_RETURN(QJsonValue());
}

ValueOrError<DeckListInput> DeckListInput::fromJson(const QJsonValue &v,
                                                    QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  auto cardSlots = decodeCardQuantityMapInput(obj, "slots"_L1,
                                              Json::joinPath(path, u"slots"));
  if (!cardSlots)
    return failure(cardSlots.error());

  auto investigatorCode =
      InvestigatorRef::fromJson(obj.value("investigator_code"_L1),
                                Json::joinPath(path, u"investigator_code"));
  if (!investigatorCode)
    return failure(investigatorCode.error());

  auto investigatorName = Json::optionalString(
      obj, "investigator_name"_L1, Json::joinPath(path, u"investigator_name"));
  if (!investigatorName)
    return failure(investigatorName.error());
  auto meta =
      Json::optionalString(obj, "meta"_L1, Json::joinPath(path, u"meta"));
  if (!meta)
    return failure(meta.error());
  auto tabooId =
      Json::optionalInt(obj, "taboo_id"_L1, Json::joinPath(path, u"taboo_id"));
  if (!tabooId)
    return failure(tabooId.error());
  auto url = Json::optionalString(obj, "url"_L1, Json::joinPath(path, u"url"));
  if (!url)
    return failure(url.error());
  auto id = ExternalDeckId::fromObject(obj, path);
  if (!id)
    return failure(id.error());
  auto name =
      Json::optionalString(obj, "name"_L1, Json::joinPath(path, u"name"));
  if (!name)
    return failure(name.error());

  return DeckListInput{
      .cardSlots = *cardSlots,
      .sideSlots = obj.value("sideSlots"_L1),
      .investigatorCode = *investigatorCode,
      .investigatorName = *investigatorName,
      .meta = *meta,
      .tabooId = *tabooId,
      .url = *url,
      .id = *id,
      .name = *name,
  };
}

ValueOrError<DeckListInput> DeckListInput::fromRawBytes(QByteArrayView bytes,
                                                        QStringView path) {
  auto raw = Json::Value::parse(bytes, path);
  if (!raw)
    return failure(raw.error());
  auto decoded = fromJson(raw->toQJson(), path);
  if (!decoded)
    return failure(decoded.error());
  if (raw->isObject()) {
    const Json::Value idValue = raw->value("id"_L1);
    if (idValue.isNumber())
      decoded->id.numberLiteral = idValue.toRawNumber().literal();
  }
  return *decoded;
}

QJsonObject DeckListInput::toJson() const {
  QJsonObject obj;
  obj.insert(QStringLiteral("slots"), encodeCardQuantityMapInput(cardSlots));
  if (!sideSlots.isUndefined())
    obj.insert(QStringLiteral("sideSlots"), sideSlots);
  obj.insert(QStringLiteral("investigator_code"), investigatorCode.value());
  if (investigatorName)
    obj.insert(QStringLiteral("investigator_name"), *investigatorName);
  if (meta)
    obj.insert(QStringLiteral("meta"), *meta);
  if (tabooId)
    obj.insert(QStringLiteral("taboo_id"), *tabooId);
  if (url)
    obj.insert(QStringLiteral("url"), *url);
  const QJsonValue idJson = id.toJson();
  if (!idJson.isUndefined())
    obj.insert(QStringLiteral("id"), idJson);
  if (name)
    obj.insert(QStringLiteral("name"), *name);
  return obj;
}

QByteArray DeckListInput::toJsonBytes() const {
  QJsonObject obj = toJson();
  if (id.tag != ExternalDeckIdTag::Number)
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  // ExternalDeckId::numberLiteral is a public field, so nothing prevents a
  // caller from setting it to something other than a valid JSON number
  // literal (accidentally or otherwise) before reaching here. Splicing
  // that text into the output bytes unvalidated could produce malformed
  // JSON or, worse, inject extra tokens/keys past the intended numeric
  // value. Validate it against the same strict RFC 8259 grammar used to
  // parse incoming numbers before splicing; if it fails (or somehow
  // parses as something other than a bare number), fall back to the
  // ordinary QJsonDocument-based encoding above -- lossy for this one
  // field, but always syntactically valid -- rather than emitting bytes
  // built from unchecked, potentially attacker-controlled text.
  const auto parsedLiteral =
      Json::Value::parse(id.toJsonLiteral().toUtf8(), u"id");
  if (!parsedLiteral || !parsedLiteral->isNumber())
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  // toJson() above already inserted a lossy double-based "id"; replace it
  // with the exact literal via byte-level splicing (see
  // ExternalDeckId::numberLiteral's doc comment).
  obj.remove(QStringLiteral("id"));
  return Json::spliceRawJsonMember(obj, "id"_L1, id.toJsonLiteral().toUtf8());
}

ValueOrError<DeckList> DeckList::fromJson(const QJsonValue &v,
                                          QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  auto cardSlots =
      decodeCardQuantityMap(obj, "slots"_L1, Json::joinPath(path, u"slots"));
  if (!cardSlots)
    return failure(cardSlots.error());
  auto sideSlots = decodeCardQuantityMap(obj, "sideSlots"_L1,
                                         Json::joinPath(path, u"sideSlots"));
  if (!sideSlots)
    return failure(sideSlots.error());
  auto investigatorCode =
      CardCode::fromJson(obj.value("investigator_code"_L1),
                         Json::joinPath(path, u"investigator_code"));
  if (!investigatorCode)
    return failure(investigatorCode.error());
  auto investigatorName = Json::requireString(
      obj, "investigator_name"_L1, Json::joinPath(path, u"investigator_name"));
  if (!investigatorName)
    return failure(investigatorName.error());
  auto meta = Json::requireNullableString(obj, "meta"_L1,
                                          Json::joinPath(path, u"meta"));
  if (!meta)
    return failure(meta.error());
  auto tabooId = Json::requireNullableInt(obj, "taboo_id"_L1,
                                          Json::joinPath(path, u"taboo_id"));
  if (!tabooId)
    return failure(tabooId.error());
  auto url =
      Json::requireNullableString(obj, "url"_L1, Json::joinPath(path, u"url"));
  if (!url)
    return failure(url.error());
  auto id =
      Json::requireNullableString(obj, "id"_L1, Json::joinPath(path, u"id"));
  if (!id)
    return failure(id.error());
  auto name = Json::requireNullableString(obj, "name"_L1,
                                          Json::joinPath(path, u"name"));
  if (!name)
    return failure(name.error());

  return DeckList{
      .cardSlots = *cardSlots,
      .sideSlots = *sideSlots,
      .investigatorCode = *investigatorCode,
      .investigatorName = *investigatorName,
      .meta = *meta,
      .tabooId = *tabooId,
      .url = *url,
      .id = *id,
      .name = *name,
  };
}

QJsonObject DeckList::toJson() const {
  return QJsonObject{
      {QStringLiteral("slots"), encodeCardQuantityMap(cardSlots)},
      {QStringLiteral("sideSlots"), encodeCardQuantityMap(sideSlots)},
      {QStringLiteral("investigator_code"), investigatorCode.toJson()},
      {QStringLiteral("investigator_name"), investigatorName},
      {QStringLiteral("meta"), meta ? QJsonValue(*meta) : QJsonValue()},
      {QStringLiteral("taboo_id"),
       tabooId ? QJsonValue(*tabooId) : QJsonValue()},
      {QStringLiteral("url"), url ? QJsonValue(*url) : QJsonValue()},
      {QStringLiteral("id"), id ? QJsonValue(*id) : QJsonValue()},
      {QStringLiteral("name"), name ? QJsonValue(*name) : QJsonValue()},
  };
}

ValueOrError<Deck> Deck::fromJson(const QJsonValue &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  auto id = DeckId::fromJson(obj.value("id"_L1), Json::joinPath(path, u"id"));
  if (!id)
    return failure(id.error());
  auto userId =
      Json::requireInt(obj, "userId"_L1, Json::joinPath(path, u"userId"));
  if (!userId)
    return failure(userId.error());
  auto url =
      Json::requireNullableString(obj, "url"_L1, Json::joinPath(path, u"url"));
  if (!url)
    return failure(url.error());
  auto name =
      Json::requireString(obj, "name"_L1, Json::joinPath(path, u"name"));
  if (!name)
    return failure(name.error());
  auto investigatorName = Json::requireString(
      obj, "investigatorName"_L1, Json::joinPath(path, u"investigatorName"));
  if (!investigatorName)
    return failure(investigatorName.error());
  auto list =
      DeckList::fromJson(obj.value("list"_L1), Json::joinPath(path, u"list"));
  if (!list)
    return failure(list.error());

  return Deck{
      .id = *id,
      .userId = *userId,
      .url = *url,
      .name = *name,
      .investigatorName = *investigatorName,
      .list = *list,
  };
}

QJsonObject Deck::toJson() const {
  return QJsonObject{
      {QStringLiteral("id"), id.toJson()},
      {QStringLiteral("userId"), userId},
      {QStringLiteral("url"), url ? QJsonValue(*url) : QJsonValue()},
      {QStringLiteral("name"), name},
      {QStringLiteral("investigatorName"), investigatorName},
      {QStringLiteral("list"), list.toJson()},
  };
}

ValueOrError<CreateDeckRequest> CreateDeckRequest::fromJson(const QJsonValue &v,
                                                            QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  auto deckId =
      Json::requireString(obj, "deckId"_L1, Json::joinPath(path, u"deckId"));
  if (!deckId)
    return failure(deckId.error());
  auto deckName = Json::requireString(obj, "deckName"_L1,
                                      Json::joinPath(path, u"deckName"));
  if (!deckName)
    return failure(deckName.error());
  auto deckUrl =
      Json::optionalString(obj, "deckUrl"_L1, Json::joinPath(path, u"deckUrl"));
  if (!deckUrl)
    return failure(deckUrl.error());
  auto deckList = DeckListInput::fromJson(obj.value("deckList"_L1),
                                          Json::joinPath(path, u"deckList"));
  if (!deckList)
    return failure(deckList.error());

  return CreateDeckRequest{
      .deckId = *deckId,
      .deckName = *deckName,
      .deckUrl = *deckUrl,
      .deckList = *deckList,
  };
}

ValueOrError<CreateDeckRequest>
CreateDeckRequest::fromRawBytes(QByteArrayView bytes, QStringView path) {
  auto raw = Json::Value::parse(bytes, path);
  if (!raw)
    return failure(raw.error());
  auto decoded = fromJson(raw->toQJson(), path);
  if (!decoded)
    return failure(decoded.error());
  if (raw->isObject()) {
    const Json::Value deckListRaw = raw->value("deckList"_L1);
    if (deckListRaw.isObject()) {
      const Json::Value idValue = deckListRaw.value("id"_L1);
      if (idValue.isNumber())
        decoded->deckList.id.numberLiteral = idValue.toRawNumber().literal();
    }
  }
  return *decoded;
}

QJsonObject CreateDeckRequest::toJson() const {
  QJsonObject obj{
      {QStringLiteral("deckId"), deckId},
      {QStringLiteral("deckName"), deckName},
      {QStringLiteral("deckList"), deckList.toJson()},
  };
  if (deckUrl)
    obj.insert(QStringLiteral("deckUrl"), *deckUrl);
  return obj;
}

QByteArray CreateDeckRequest::toJsonBytes() const {
  QJsonObject obj = toJson();
  if (deckList.id.tag != ExternalDeckIdTag::Number)
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  obj.remove(QStringLiteral("deckList"));
  return Json::spliceRawJsonMember(obj, "deckList"_L1, deckList.toJsonBytes());
}

ValueOrError<FetchDeckRequest> FetchDeckRequest::fromJson(const QJsonValue &v,
                                                          QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  auto url =
      Json::requireString(*objResult, "url"_L1, Json::joinPath(path, u"url"));
  if (!url)
    return failure(url.error());
  return FetchDeckRequest{.url = *url};
}

QJsonObject FetchDeckRequest::toJson() const {
  return QJsonObject{{QStringLiteral("url"), url}};
}

ValueOrError<DeckValidationError>
DeckValidationError::fromJson(const QJsonValue &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  auto tag = Json::requireString(obj, "tag"_L1, Json::joinPath(path, u"tag"));
  if (!tag)
    return failure(tag.error());
  if (*tag != "UnimplementedCard"_L1)
    return failure(
        QStringLiteral("%1.tag: unrecognized value \"%2\"").arg(path, *tag));
  auto cardCode = CardCode::fromJson(obj.value("contents"_L1),
                                     Json::joinPath(path, u"contents"));
  if (!cardCode)
    return failure(cardCode.error());
  return DeckValidationError{.cardCode = *cardCode};
}

QJsonObject DeckValidationError::toJson() const {
  return QJsonObject{
      {QStringLiteral("tag"), QStringLiteral("UnimplementedCard")},
      {QStringLiteral("contents"), cardCode.toJson()},
  };
}

static ValueOrError<QList<DeckValidationError>>
decodeDeckValidationResultItems(const QJsonValue &v, QStringView path) {
  auto arrResult = Json::requireArray(v, path);
  if (!arrResult)
    return failure(arrResult.error());
  QList<DeckValidationError> result;
  result.reserve(arrResult->size());
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    auto item = DeckValidationError::fromJson((*arrResult)[i],
                                              Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    result.append(*item);
  }
  return result;
}

DeckValidationResult DeckValidationResult::success() {
  DeckValidationResult result;
  result.m_kind = Kind::Success;
  return result;
}

ValueOrError<DeckValidationResult>
DeckValidationResult::errors(QList<DeckValidationError> errors) {
  if (errors.isEmpty())
    return failure(QStringLiteral(
        "DeckValidationResult::errors: errors must not be empty -- an "
        "empty list is deckValidationSuccess, not deckValidationErrors"));
  DeckValidationResult result;
  result.m_kind = Kind::Errors;
  result.m_errors = std::move(errors);
  return result;
}

ValueOrError<DeckValidationResult>
DeckValidationResult::fromJson(const QJsonValue &v, QStringView path) {
  auto items = decodeDeckValidationResultItems(v, path);
  if (!items)
    return failure(items.error());
  if (items->isEmpty())
    return DeckValidationResult::success();
  return DeckValidationResult::errors(*items);
}

QJsonArray DeckValidationResult::toJson() const {
  QJsonArray arr;
  for (const DeckValidationError &error : m_errors)
    arr.append(error.toJson());
  return arr;
}

ValueOrError<DeckOperationError>
DeckOperationError::fromJson(const QJsonValue &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  auto errorMsg = Json::requireString(*objResult, "errorMsg"_L1,
                                      Json::joinPath(path, u"errorMsg"));
  if (!errorMsg)
    return failure(errorMsg.error());
  return DeckOperationError{.errorMsg = *errorMsg};
}

QJsonObject DeckOperationError::toJson() const {
  return QJsonObject{{QStringLiteral("errorMsg"), errorMsg}};
}

} // namespace Arkham
