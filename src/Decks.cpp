#include "Decks.h"

#include "JsonDecode.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// See CardCatalog.cpp's identically-named using-declaration for the full
// rationale: brought in unqualified since it is used below, and its
// Json::Value overload (but not its QJsonValue one) is also reachable via
// ADL.
using Json::toLosslessRaw;

// Decodes a `cardQuantityMapInput`: an object whose property names need
// only be non-empty, with integer values. Used for DeckListInput.slots.
// Templatized over Obj (QJsonObject from the fromJson()/QJsonValue
// convenience family, or Json::Value from the canonical fromRawJson()/
// fromRawBytes() family -- see RawJson.h) so both share one implementation;
// Json::objectMembers() below provides uniform (key, value) iteration for
// either.
template <typename Obj>
ValueOrError<QMap<QString, qint64>>
decodeCardQuantityMapInput(const Obj &obj, QLatin1StringView key,
                           QStringView path) {
  auto objResult = Json::requireObjectField(obj, key, path);
  if (!objResult)
    return failure(objResult.error());
  QMap<QString, qint64> result;
  for (const auto &[entryKey, value] : Json::objectMembers(*objResult)) {
    const QString entryPath = Json::joinPath(path, entryKey);
    if (entryKey.isEmpty())
      return failure(
          QStringLiteral("%1: quantity map key must not be empty").arg(path));
    auto amount = Json::requireIntValue(value, entryPath);
    if (!amount)
      return failure(amount.error());
    result.insert(entryKey, *amount);
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

// Dispatch-shim overload pair (mirrors CardCatalog.cpp's identically-named
// pattern): picks InvestigatorRef::fromJson()'s QJsonValue-taking body or a
// hand-written Json::Value-taking equivalent to match a templated
// decoder's deduced value-family parameter. InvestigatorRef is a plain
// non-empty string wrapper (see Identifiers.h) with no numeric-precision
// concern, so the Json::Value overload need not itself live in
// Identifiers.h -- it only ever needs requireStringValue()/parse(), both
// already dual-overloaded/family-agnostic.
ValueOrError<InvestigatorRef> decodeInvestigatorRefValue(const QJsonValue &v,
                                                         QStringView path) {
  return InvestigatorRef::fromJson(v, path);
}
ValueOrError<InvestigatorRef> decodeInvestigatorRefValue(const Json::Value &v,
                                                         QStringView path) {
  auto str = Json::requireStringValue(v, path);
  if (!str)
    return failure(str.error());
  auto parsed = InvestigatorRef::parse(*str);
  if (!parsed)
    return failure(QStringLiteral("%1: %2").arg(path, parsed.error()));
  return *parsed;
}

// Dispatch-shim pair for ExternalDeckId's two differently-named existing
// factories (fromObject()/fromRawObject()), so the templated decode body
// below can call one uniform name regardless of which value family the
// deduced Obj parameter is.
ValueOrError<ExternalDeckId> decodeExternalDeckId(const QJsonObject &obj,
                                                  QStringView path) {
  return ExternalDeckId::fromObject(obj, path);
}
ValueOrError<ExternalDeckId> decodeExternalDeckId(const Json::Value &obj,
                                                  QStringView path) {
  return ExternalDeckId::fromRawObject(obj, path);
}

// Shared decode body for DeckListInput::fromJson()/fromRawJson(): Obj is
// QJsonObject (via requireObject(QJsonValue)) or Json::Value (via
// requireObject(Json::Value)) depending on which public entry point
// called in. Critically, the Json::Value instantiation never touches
// QJsonValue/QJsonDocument at any point -- sideSlots (and, via
// decodeExternalDeckId, a numeric id) is read directly off the parsed
// AST, so a number nested inside sideSlots at any depth, or an id outside
// qint64/double-exact range, survives byte-exact. toLosslessRaw() (see
// JsonDecode.h) is the sideSlots conversion's only QJsonValue-to-
// Json::Value crossing point, and it is a no-op passthrough for the
// Json::Value instantiation.
template <typename Obj>
ValueOrError<DeckListInput> decodeDeckListInput(const Obj &v,
                                                QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto cardSlots = decodeCardQuantityMapInput(obj, "slots"_L1,
                                              Json::joinPath(path, u"slots"));
  if (!cardSlots)
    return failure(cardSlots.error());

  auto investigatorCode = Json::requireField(
      obj, "investigator_code"_L1, Json::joinPath(path, u"investigator_code"),
      [](const auto &v, QStringView p) {
        return decodeInvestigatorRefValue(v, p);
      });
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
  auto id = decodeExternalDeckId(obj, path);
  if (!id)
    return failure(id.error());
  auto name =
      Json::optionalString(obj, "name"_L1, Json::joinPath(path, u"name"));
  if (!name)
    return failure(name.error());
  auto sideSlots = toLosslessRaw(obj.value("sideSlots"_L1));
  if (!sideSlots)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"sideSlots"), sideSlots.error()));

  return DeckListInput{
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

// Shared decode body for CreateDeckRequest::fromJson()/fromRawJson():
// decodes deckList by calling decodeDeckListInput<V> directly with the
// still-native `obj.value("deckList"_L1)` value (V is deduced identically
// to Obj, since both come from the same requireObject() instantiation) --
// never re-serializing/reparsing it, unlike this method's previous
// implementation, which round-tripped deckList through
// Json::Value::toJsonBytes() and back just to reuse
// DeckListInput::fromRawBytes()'s byte-only entry point.
template <typename Obj>
ValueOrError<CreateDeckRequest> decodeCreateDeckRequest(const Obj &v,
                                                        QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

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
  auto deckList = decodeDeckListInput(obj.value("deckList"_L1),
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
    auto converted = Json::Value::fromQJson(v);
    if (!converted)
      return failure(QStringLiteral("%1: %2").arg(idPath, converted.error()));
    if (!converted->isNumber())
      return failure(
          QStringLiteral("%1: internal error converting number").arg(idPath));
    return ExternalDeckId::number(converted->toRawNumber());
  }
  return failure(QStringLiteral("%1: expected string, number, or null, got %2")
                     .arg(idPath, Json::typeName(v)));
}

ValueOrError<ExternalDeckId>
ExternalDeckId::fromRawObject(const Json::Value &obj, QStringView path) {
  if (!obj.isObject())
    return failure(QStringLiteral("%1: expected an object, got %2")
                       .arg(path, Json::typeName(obj)));
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
                     .arg(idPath, Json::typeName(v)));
}

QJsonValue ExternalDeckId::toJson() const {
  switch (m_kind) {
  case Kind::Absent:
    return QJsonValue(QJsonValue::Undefined);
  case Kind::Null:
    // QJsonValue()'s default constructor is QJsonValue::Null, not
    // Undefined -- spelled out explicitly here (rather than relying on
    // the default constructor) to make the distinction from the Absent
    // case above unambiguous: an explicit JSON null still emits an
    // "id" key when composed into an enclosing object, unlike Absent's
    // Undefined which omits the key entirely.
    return QJsonValue(QJsonValue::Null);
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
  return decodeDeckListInput(v, path);
}

ValueOrError<DeckListInput> DeckListInput::fromRawJson(const Json::Value &v,
                                                       QStringView path) {
  return decodeDeckListInput(v, path);
}

ValueOrError<DeckListInput> DeckListInput::fromRawBytes(QByteArrayView bytes,
                                                        QStringView path) {
  auto raw = Json::Value::parse(bytes, path);
  if (!raw)
    return failure(raw.error());
  return fromRawJson(*raw, path);
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
  auto investigatorCode = Json::requireField(
      obj, "investigator_code"_L1, Json::joinPath(path, u"investigator_code"),
      [](const QJsonValue &v, QStringView p) {
        return CardCode::fromJson(v, p);
      });
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
  // Every ternary below uses QJsonValue(QJsonValue::Null) rather than the
  // bare default-constructed QJsonValue() for the unset case. Both
  // produce an identical Null-kind value (QJsonValue's default
  // constructor is QJsonValue::Null, not Undefined -- QJsonObject only
  // drops a key for an explicit Undefined value), but spelling it out
  // avoids any ambiguity for a reader about which Qt JSON kind is
  // intended: decks.schema.json requires each of these keys to be
  // present, just nullable, so the key must never be omitted here.
  return QJsonObject{
      {QStringLiteral("slots"), encodeCardQuantityMap(cardSlots)},
      {QStringLiteral("sideSlots"), encodeCardQuantityMap(sideSlots)},
      {QStringLiteral("investigator_code"), investigatorCode.toJson()},
      {QStringLiteral("investigator_name"), investigatorName},
      {QStringLiteral("meta"),
       meta ? QJsonValue(*meta) : QJsonValue(QJsonValue::Null)},
      {QStringLiteral("taboo_id"),
       tabooId ? QJsonValue(*tabooId) : QJsonValue(QJsonValue::Null)},
      {QStringLiteral("url"),
       url ? QJsonValue(*url) : QJsonValue(QJsonValue::Null)},
      {QStringLiteral("id"),
       id ? QJsonValue(*id) : QJsonValue(QJsonValue::Null)},
      {QStringLiteral("name"),
       name ? QJsonValue(*name) : QJsonValue(QJsonValue::Null)},
  };
}

ValueOrError<Deck> Deck::fromJson(const QJsonValue &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  auto id = Json::requireField(obj, "id"_L1, Json::joinPath(path, u"id"),
                               [](const QJsonValue &v, QStringView p) {
                                 return DeckId::fromJson(v, p);
                               });
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
  auto list = Json::requireField(obj, "list"_L1, Json::joinPath(path, u"list"),
                                 [](const QJsonValue &v, QStringView p) {
                                   return DeckList::fromJson(v, p);
                                 });
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
      // See DeckList::toJson()'s comment above: QJsonValue(Null) rather
      // than the bare default constructor, to make explicit that this
      // key must remain present (decks.schema.json requires "url",
      // nullable) rather than being dropped like an Undefined value.
      {QStringLiteral("url"),
       url ? QJsonValue(*url) : QJsonValue(QJsonValue::Null)},
      {QStringLiteral("name"), name},
      {QStringLiteral("investigatorName"), investigatorName},
      {QStringLiteral("list"), list.toJson()},
  };
}

ValueOrError<CreateDeckRequest> CreateDeckRequest::fromJson(const QJsonValue &v,
                                                            QStringView path) {
  return decodeCreateDeckRequest(v, path);
}

ValueOrError<CreateDeckRequest>
CreateDeckRequest::fromRawJson(const Json::Value &v, QStringView path) {
  return decodeCreateDeckRequest(v, path);
}

ValueOrError<CreateDeckRequest>
CreateDeckRequest::fromRawBytes(QByteArrayView bytes, QStringView path) {
  auto raw = Json::Value::parse(bytes, path);
  if (!raw)
    return failure(raw.error());
  return fromRawJson(*raw, path);
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
  auto cardCode =
      Json::requireField(obj, "contents"_L1, Json::joinPath(path, u"contents"),
                         [](const QJsonValue &v, QStringView p) {
                           return CardCode::fromJson(v, p);
                         });
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
