#include "Decks.h"

#include "JsonDecode.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// Decodes a `cardQuantityMapInput`: an object whose property names need
// only be non-empty, with integer values. Used for DeckListInput.slots.
ValueOrError<QMap<QString, qint64>>
decodeCardQuantityMapInput(const QJsonObject &obj, QLatin1StringView key,
                           QStringView path) {
  auto objResult = Json::requireObjectField(obj, key, path);
  if (!objResult)
    return failure(objResult.error());
  QMap<QString, qint64> result;
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
ValueOrError<QMap<CardCode, qint64>>
decodeCardQuantityMap(const QJsonObject &obj, QLatin1StringView key,
                      QStringView path) {
  auto objResult = Json::requireObjectField(obj, key, path);
  if (!objResult)
    return failure(objResult.error());
  QMap<CardCode, qint64> result;
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

QJsonObject encodeCardQuantityMapInput(const QMap<QString, qint64> &map) {
  QJsonObject obj;
  for (auto it = map.constBegin(); it != map.constEnd(); ++it)
    obj.insert(it.key(), it.value());
  return obj;
}

QJsonObject encodeCardQuantityMap(const QMap<CardCode, qint64> &map) {
  QJsonObject obj;
  for (auto it = map.constBegin(); it != map.constEnd(); ++it)
    obj.insert(it.key().value(), it.value());
  return obj;
}

// Lossless equivalents of the two encoders above, for use by
// DeckListInput::toJsonBytes()'s Json::Value AST build (see RawJson.h).
Json::Value rawEncodeCardQuantityMapInput(const QMap<QString, qint64> &map) {
  QList<std::pair<QString, Json::Value>> members;
  members.reserve(map.size());
  for (auto it = map.constBegin(); it != map.constEnd(); ++it)
    members.append({it.key(), Json::Value::makeNumber(
                                  Json::RawNumber::fromInt64(it.value()))});
  return Json::Value::makeObject(std::move(members));
}

} // namespace

ExternalDeckId ExternalDeckId::absent() {
  ExternalDeckId result;
  result.m_kind = Kind::Absent;
  return result;
}

ExternalDeckId ExternalDeckId::null() {
  ExternalDeckId result;
  result.m_kind = Kind::Null;
  return result;
}

ExternalDeckId ExternalDeckId::text(QString value) {
  ExternalDeckId result;
  result.m_kind = Kind::Text;
  result.m_text = std::move(value);
  return result;
}

ExternalDeckId ExternalDeckId::number(Json::RawNumber value) {
  ExternalDeckId result;
  result.m_kind = Kind::Number;
  result.m_number = std::move(value);
  return result;
}

ValueOrError<ExternalDeckId> ExternalDeckId::fromObject(const QJsonObject &obj,
                                                        QStringView path) {
  switch (Json::fieldPresence(obj, "id"_L1)) {
  case Json::FieldPresence::Absent:
    return ExternalDeckId::absent();
  case Json::FieldPresence::Null:
    return ExternalDeckId::null();
  case Json::FieldPresence::Present:
    break;
  }
  const QJsonValue v = obj.value("id"_L1);
  const QString idPath = Json::joinPath(path, u"id");
  if (v.isString())
    return ExternalDeckId::text(v.toString());
  if (v.isDouble()) {
    if (!std::isfinite(v.toDouble()))
      return failure(
          QStringLiteral("%1: number is too large to represent").arg(idPath));
    // Recover as much precision as the source QJsonValue itself carries
    // (exact for any qint64-range integer -- see Json::Value::fromQJson()'s
    // doc comment -- best-effort IEEE-754 double otherwise) rather than
    // ever constructing a RawNumber from unchecked/re-parsed text.
    const Json::Value converted = Json::Value::fromQJson(v);
    if (!converted.isNumber())
      return failure(
          QStringLiteral("%1: internal error converting number").arg(idPath));
    return ExternalDeckId::number(converted.toRawNumber());
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
    return ExternalDeckId::absent();
  const Json::Value v = obj.value("id"_L1);
  const QString idPath = Json::joinPath(path, u"id");
  if (v.isNull())
    return ExternalDeckId::null();
  if (v.isString())
    return ExternalDeckId::text(v.toString());
  if (v.isNumber())
    return ExternalDeckId::number(v.toRawNumber());
  return failure(QStringLiteral("%1: expected string, number, or null, got %2")
                     .arg(idPath, Json::typeName(v.toQJson())));
}

QJsonValue ExternalDeckId::toJson() const {
  switch (m_kind) {
  case Kind::Absent:
    return QJsonValue(QJsonValue::Undefined);
  case Kind::Null:
    return QJsonValue();
  case Kind::Text:
    return QJsonValue(m_text);
  case Kind::Number:
    // Display/debug-only: see this method's header doc comment. Uses
    // RawNumber::toDouble() (best-effort IEEE-754), never toRawJson()'s
    // exact literal.
    return QJsonValue(m_number.toDouble());
  }
  Q_UNREACHABLE_RETURN(QJsonValue());
}

Json::Value ExternalDeckId::toRawJson() const {
  switch (m_kind) {
  case Kind::Absent:
  case Kind::Null:
    return Json::Value::makeNull();
  case Kind::Text:
    return Json::Value::makeString(m_text);
  case Kind::Number:
    return Json::Value::makeNumber(m_number);
  }
  Q_UNREACHABLE_RETURN(Json::Value::makeNull());
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
      .sideSlots = Json::Value::fromQJson(obj.value("sideSlots"_L1)),
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
    auto id = ExternalDeckId::fromRawObject(*raw, path);
    if (!id)
      return failure(id.error());
    decoded->id = *id;
    // raw->value() on a missing key returns Kind::Undefined, matching
    // this field's documented "absent" representation exactly -- no
    // separate presence check needed here.
    decoded->sideSlots = raw->value("sideSlots"_L1);
  }
  return *decoded;
}

QJsonObject DeckListInput::toJson() const {
  QJsonObject obj;
  obj.insert(QStringLiteral("slots"), encodeCardQuantityMapInput(cardSlots));
  if (!sideSlots.isUndefined())
    obj.insert(QStringLiteral("sideSlots"), sideSlots.toQJson());
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

Json::Value DeckListInput::toRawJson() const {
  QList<std::pair<QString, Json::Value>> members;
  members.append(
      {QStringLiteral("slots"), rawEncodeCardQuantityMapInput(cardSlots)});
  if (!sideSlots.isUndefined())
    members.append({QStringLiteral("sideSlots"), sideSlots});
  members.append({QStringLiteral("investigator_code"),
                  Json::Value::makeString(investigatorCode.value())});
  if (investigatorName)
    members.append({QStringLiteral("investigator_name"),
                    Json::Value::makeString(*investigatorName)});
  if (meta)
    members.append({QStringLiteral("meta"), Json::Value::makeString(*meta)});
  if (tabooId)
    members.append(
        {QStringLiteral("taboo_id"),
         Json::Value::makeNumber(Json::RawNumber::fromInt64(*tabooId))});
  if (url)
    members.append({QStringLiteral("url"), Json::Value::makeString(*url)});
  if (id.kind() != ExternalDeckId::Kind::Absent)
    members.append({QStringLiteral("id"), id.toRawJson()});
  if (name)
    members.append({QStringLiteral("name"), Json::Value::makeString(*name)});
  return Json::Value::makeObject(std::move(members));
}

ValueOrError<QByteArray> DeckListInput::toJsonBytes() const {
  return toRawJson().toJsonBytes();
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
      auto deckListBytes = deckListRaw.toJsonBytes();
      if (!deckListBytes)
        return failure(deckListBytes.error());
      auto preciseDeckList = DeckListInput::fromRawBytes(
          *deckListBytes, Json::joinPath(path, u"deckList"));
      if (!preciseDeckList)
        return failure(preciseDeckList.error());
      decoded->deckList = *preciseDeckList;
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

ValueOrError<QByteArray> CreateDeckRequest::toJsonBytes() const {
  QList<std::pair<QString, Json::Value>> members{
      {QStringLiteral("deckId"), Json::Value::makeString(deckId)},
      {QStringLiteral("deckName"), Json::Value::makeString(deckName)},
      {QStringLiteral("deckList"), deckList.toRawJson()},
  };
  if (deckUrl)
    members.append(
        {QStringLiteral("deckUrl"), Json::Value::makeString(*deckUrl)});
  return Json::Value::makeObject(std::move(members)).toJsonBytes();
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
