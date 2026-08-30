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
// Templatized over Obj like decodeCardQuantityMapInput above, via
// Json::objectMembers() for uniform (key, value) iteration.
template <typename Obj>
ValueOrError<QMap<CardCode, qint64>>
decodeCardQuantityMap(const Obj &obj, QLatin1StringView key, QStringView path) {
  auto objResult = Json::requireObjectField(obj, key, path);
  if (!objResult)
    return failure(objResult.error());
  QMap<CardCode, qint64> result;
  for (const auto &[entryKey, value] : Json::objectMembers(*objResult)) {
    const QString entryPath = Json::joinPath(path, entryKey);
    auto code = CardCode::parse(entryKey);
    if (!code)
      return failure(QStringLiteral("%1: %2").arg(entryPath, code.error()));
    auto amount = Json::requireIntValue(value, entryPath);
    if (!amount)
      return failure(amount.error());
    result.insert(*code, *amount);
  }
  return result;
}

// Shared decode body for DeckList::fromJson()/fromRawJson(): V is
// QJsonValue or Json::Value. Every field decoder called below
// (decodeCardQuantityMap, Json::requireField/requireString/
// requireNullableString/requireNullableInt) is already generic over the
// object-family it receives, and CardCode::fromJson is likewise
// dual-overloaded (see Identifiers.h), so this one body serves both the
// convenience fromJson() family and the canonical fromRawJson()/
// fromRawBytes() family without duplication.
template <typename V>
ValueOrError<DeckList> decodeDeckList(const V &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  // decks.schema.json's `deckList` (the backend-normalized shape) is
  // additionalProperties:false with exactly these nine keys, all
  // `required` (round-10-cumulative-review item 5).
  auto exactKeys = Json::requireExactKeys(
      obj,
      {"slots"_L1, "sideSlots"_L1, "investigator_code"_L1,
       "investigator_name"_L1, "meta"_L1, "taboo_id"_L1, "url"_L1, "id"_L1,
       "name"_L1},
      path);
  if (!exactKeys)
    return failure(exactKeys.error());

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
      [](const auto &v, QStringView p) { return CardCode::fromJson(v, p); });
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

QJsonObject encodeCardQuantityMapInput(const QMap<QString, qint64> &map) {
  QJsonObject obj;
  for (auto it = map.constBegin(); it != map.constEnd(); ++it)
    obj.insert(it.key(), it.value());
  return obj;
}

// Builds the `slots`/`sideSlots` QJsonObject for DeckList::toJson() below.
// A CardCode's own construction-time validation (see CardCode::parse() in
// Identifiers.h) does not rule out an over-length or lone/mismatched
// UTF-16 surrogate key, so -- unlike a value embedded via
// CardCode::toJson() -- a key built via .value() directly bypasses that
// type's own encode-time check entirely. Builds the equivalent Json::Value
// object (mirroring rawEncodeCardQuantityMapInput() below) and routes it
// through Value::toExactQJsonObject()'s single canonical, bounded check
// (string length, lone/mismatched UTF-16 surrogates, duplicate keys)
// rather than hand-duplicating just the lone-surrogate case, so this stays
// in lockstep with every other encoder built the same way.
ValueOrError<QJsonObject>
encodeCardQuantityMap(const QMap<CardCode, qint64> &map) {
  QList<std::pair<QString, Json::Value>> members;
  members.reserve(map.size());
  for (auto it = map.constBegin(); it != map.constEnd(); ++it)
    members.append(
        {it.key().value(),
         Json::Value::makeNumber(Json::RawNumber::fromInt64(it.value()))});
  return Json::Value::makeObject(std::move(members)).toExactQJsonObject();
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

// DeckListInput.cardSlots is a public QMap<QString, qint64> field (for
// ergonomic construction from a permissive external caller), so unlike
// decode (decodeCardQuantityMapInput above always rejects an empty key
// before it ever reaches this type) an encoder must still guard a
// hand-constructed instance against one slipping through: an empty key
// would otherwise silently serialize into a schema-invalid
// cardQuantityMapInput request. Returns an empty string when every key is
// non-empty.
QString firstEmptyCardSlotsKeyError(const QMap<QString, qint64> &map) {
  for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
    if (it.key().isEmpty())
      return QStringLiteral("slots: quantity map key must not be empty");
  }
  return QString();
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

// Dispatch-shim pair for DeckList (mirrors this file's
// decodeInvestigatorRefValue/decodeExternalDeckId pattern): picks
// DeckList::fromJson()'s QJsonValue-taking body, or the precision-
// preserving fromRawJson() overload, to match decodeDeck<Obj>'s deduced
// value-family parameter below.
ValueOrError<DeckList> decodeDeckListValue(const QJsonValue &v,
                                           QStringView path) {
  return DeckList::fromJson(v, path);
}
ValueOrError<DeckList> decodeDeckListValue(const Json::Value &v,
                                           QStringView path) {
  return DeckList::fromRawJson(v, path);
}

// Shared decode body for Deck::fromJson()/fromRawJson(): V is QJsonValue
// or Json::Value. `list` decodes through decodeDeckListValue's Json::Value
// overload for the fromRawJson()/fromRawBytes() path, so a numeric card
// quantity or other nested value survives exactly rather than only as
// closely as QJsonValue's double-backed storage allows.
template <typename V>
ValueOrError<Deck> decodeDeck(const V &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  // decks.schema.json's `deck` is additionalProperties:false with exactly
  // these six required keys (round-10-cumulative-review item 5).
  auto exactKeys =
      Json::requireExactKeys(obj,
                             {"id"_L1, "userId"_L1, "url"_L1, "name"_L1,
                              "investigatorName"_L1, "list"_L1},
                             path);
  if (!exactKeys)
    return failure(exactKeys.error());

  auto id = Json::requireField(
      obj, "id"_L1, Json::joinPath(path, u"id"),
      [](const auto &v, QStringView p) { return DeckId::fromJson(v, p); });
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
  auto list = Json::requireField(
      obj, "list"_L1, Json::joinPath(path, u"list"),
      [](const auto &v, QStringView p) { return decodeDeckListValue(v, p); });
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

ValueOrError<QJsonValue> ExternalDeckId::toJson() const {
  // Builds the identical Json::Value toRawJson() already composes (see
  // its own doc comment in Decks.h) and routes it through
  // Value::toExactQJson()'s single canonical, bounded check, rather than
  // hand-duplicating a subset of it here: the previous implementation
  // checked Kind::Text for a lone surrogate but never against
  // ParseLimits::production().maxStringLength, and called
  // m_number.toExactInt64() directly for Kind::Number without first
  // checking maxNumberDigits the way toExactQJson()'s own Number branch
  // does -- so e.g. a "0." + 65 zero-digit fraction (all-zero, so
  // toExactInt64() trivially returns 0) would have silently encoded as
  // 0 here while toJsonBytes()/toExactQJson() reject the same literal
  // for exceeding the digit budget. Kind::Absent/Kind::Null need no
  // string/number validation of their own; toExactQJson() itself
  // reproduces their exact QJsonValue::Undefined/Null results (see its
  // own Kind::Undefined/Kind::Null cases), so this is a strict
  // behavioral superset of the previous switch, not merely an equivalent
  // rewrite.
  return toRawJson().toExactQJson();
}

Json::Value ExternalDeckId::toRawJson() const {
  switch (m_kind) {
  case Kind::Absent:
    // Mirrors toJson()'s QJsonValue::Undefined above: Undefined (not
    // Null) so a caller composing this directly into an enclosing object
    // omits the "id" key entirely, rather than silently emitting an
    // explicit JSON null for what is semantically an omitted id. Any
    // attempt to serialize this Undefined value on its own fails loudly
    // (Value::toJsonBytes() rejects Kind::Undefined) instead of
    // producing a request payload with a different meaning than
    // intended.
    return Json::Value{};
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

ValueOrError<QJsonObject> DeckListInput::toJson() const {
  // Composes toRawJson() (below) and its own bounded exact QJsonObject
  // conversion (see Value::toExactQJsonObject() in RawJson.h) rather than
  // hand-inserting fields into a QJsonObject: the previous implementation
  // built cardSlots/id/sideSlots this way but embedded
  // investigatorCode/investigatorName/meta/url/name via raw
  // QJsonValue(QString) construction with zero validation, so a
  // lone/mismatched UTF-16 surrogate in any of those fields would have
  // silently produced a normal-looking-but-invalid QJsonObject here even
  // though toJsonBytes() correctly rejected the identical input. Routing
  // through the same toRawJson() AST toJsonBytes() itself serializes, and
  // the same Value::toExactQJson() machinery toJsonBytes() effectively
  // relies on for validation, means this convenience can never again
  // diverge from the canonical encoder's invariants.
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toExactQJsonObject();
}

ValueOrError<Json::Value> DeckListInput::toRawJson() const {
  const QString slotsError = firstEmptyCardSlotsKeyError(cardSlots);
  if (!slotsError.isEmpty())
    return failure(slotsError);
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
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toJsonBytes();
}

ValueOrError<DeckList> DeckList::fromJson(const QJsonValue &v,
                                          QStringView path) {
  return decodeDeckList(v, path);
}

ValueOrError<DeckList> DeckList::fromRawJson(const Json::Value &v,
                                             QStringView path) {
  return decodeDeckList(v, path);
}

ValueOrError<DeckList> DeckList::fromRawBytes(QByteArrayView bytes,
                                              QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

ValueOrError<QJsonObject> DeckList::toJson() const {
  // Every ternary below uses QJsonValue(QJsonValue::Null) rather than the
  // bare default-constructed QJsonValue() for the unset case. Both
  // produce an identical Null-kind value (QJsonValue's default
  // constructor is QJsonValue::Null, not Undefined -- QJsonObject only
  // drops a key for an explicit Undefined value), but spelling it out
  // avoids any ambiguity for a reader about which Qt JSON kind is
  // intended: decks.schema.json requires each of these keys to be
  // present, just nullable, so the key must never be omitted here.
  auto slotsEncoded = encodeCardQuantityMap(cardSlots);
  if (!slotsEncoded)
    return failure(QStringLiteral("slots: %1").arg(slotsEncoded.error()));
  auto sideSlotsEncoded = encodeCardQuantityMap(sideSlots);
  if (!sideSlotsEncoded)
    return failure(
        QStringLiteral("sideSlots: %1").arg(sideSlotsEncoded.error()));
  auto investigatorCodeEncoded = investigatorCode.toJson();
  if (!investigatorCodeEncoded)
    return failure(QStringLiteral("investigator_code: %1")
                       .arg(investigatorCodeEncoded.error()));
  return QJsonObject{
      {QStringLiteral("slots"), *slotsEncoded},
      {QStringLiteral("sideSlots"), *sideSlotsEncoded},
      {QStringLiteral("investigator_code"), *investigatorCodeEncoded},
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
  return decodeDeck(v, path);
}

ValueOrError<Deck> Deck::fromRawJson(const Json::Value &v, QStringView path) {
  return decodeDeck(v, path);
}

ValueOrError<Deck> Deck::fromRawBytes(QByteArrayView bytes, QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

ValueOrError<QJsonObject> Deck::toJson() const {
  auto listEncoded = list.toJson();
  if (!listEncoded)
    return failure(QStringLiteral("list: %1").arg(listEncoded.error()));
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
      {QStringLiteral("list"), *listEncoded},
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

ValueOrError<Json::Value> CreateDeckRequest::toRawJson() const {
  auto deckListRaw = deckList.toRawJson();
  if (!deckListRaw)
    return failure(deckListRaw.error());
  QList<std::pair<QString, Json::Value>> members{
      {QStringLiteral("deckId"), Json::Value::makeString(deckId)},
      {QStringLiteral("deckName"), Json::Value::makeString(deckName)},
      {QStringLiteral("deckList"), *deckListRaw},
  };
  if (deckUrl)
    members.append(
        {QStringLiteral("deckUrl"), Json::Value::makeString(*deckUrl)});
  return Json::Value::makeObject(std::move(members));
}

ValueOrError<QJsonObject> CreateDeckRequest::toJson() const {
  // Composes toRawJson() above and its own bounded exact QJsonObject
  // conversion (see Value::toExactQJsonObject() in RawJson.h) rather than
  // embedding deckId/deckName/deckUrl via raw, unvalidated
  // QJsonValue(QString) construction, so a lone/mismatched UTF-16
  // surrogate anywhere in this request is a typed failure here too, not
  // only at toJsonBytes().
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toExactQJsonObject();
}

ValueOrError<QByteArray> CreateDeckRequest::toJsonBytes() const {
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toJsonBytes();
}

// Shared decode body for FetchDeckRequest::fromJson()/fromRawJson(): V is
// QJsonValue or Json::Value.
template <typename V>
ValueOrError<FetchDeckRequest> decodeFetchDeckRequest(const V &v,
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

ValueOrError<FetchDeckRequest> FetchDeckRequest::fromJson(const QJsonValue &v,
                                                          QStringView path) {
  return decodeFetchDeckRequest(v, path);
}

ValueOrError<FetchDeckRequest>
FetchDeckRequest::fromRawJson(const Json::Value &v, QStringView path) {
  return decodeFetchDeckRequest(v, path);
}

ValueOrError<FetchDeckRequest>
FetchDeckRequest::fromRawBytes(QByteArrayView bytes, QStringView path) {
  auto raw = Json::Value::parse(bytes, path);
  if (!raw)
    return failure(raw.error());
  return fromRawJson(*raw, path);
}

ValueOrError<QJsonObject> FetchDeckRequest::toJson() const {
  // Composes toRawJson() below and its own bounded exact QJsonObject
  // conversion (see Value::toExactQJsonObject() in RawJson.h), rather
  // than embedding `url` via a raw, unvalidated QJsonValue(QString)
  // construction: a lone/mismatched UTF-16 surrogate in `url` is now a
  // typed failure here too, matching toJsonBytes() rather than silently
  // emitting an invalid QJsonObject the byte encoder would separately
  // reject.
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toExactQJsonObject();
}

ValueOrError<Json::Value> FetchDeckRequest::toRawJson() const {
  return Json::Value::makeObject(
      {{QStringLiteral("url"), Json::Value::makeString(url)}});
}

ValueOrError<QByteArray> FetchDeckRequest::toJsonBytes() const {
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toJsonBytes();
}

// Shared decode body for DeckValidationError::fromJson()/fromRawJson(): V
// is QJsonValue or Json::Value.
template <typename V>
ValueOrError<DeckValidationError> decodeDeckValidationError(const V &v,
                                                            QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  // decks.schema.json's deckValidationError is additionalProperties:false
  // with exactly {"tag","contents"} (round-10-cumulative-review item 5).
  auto exactKeys = Json::requireExactKeys(obj, {"tag"_L1, "contents"_L1}, path);
  if (!exactKeys)
    return failure(exactKeys.error());

  auto tag = Json::requireString(obj, "tag"_L1, Json::joinPath(path, u"tag"));
  if (!tag)
    return failure(tag.error());
  if (*tag != "UnimplementedCard"_L1)
    return failure(
        QStringLiteral("%1.tag: unrecognized value \"%2\"").arg(path, *tag));
  auto cardCode = Json::requireField(
      obj, "contents"_L1, Json::joinPath(path, u"contents"),
      [](const auto &v, QStringView p) { return CardCode::fromJson(v, p); });
  if (!cardCode)
    return failure(cardCode.error());
  return DeckValidationError{.cardCode = *cardCode};
}

ValueOrError<DeckValidationError>
DeckValidationError::fromJson(const QJsonValue &v, QStringView path) {
  return decodeDeckValidationError(v, path);
}

ValueOrError<DeckValidationError>
DeckValidationError::fromRawJson(const Json::Value &v, QStringView path) {
  return decodeDeckValidationError(v, path);
}

ValueOrError<DeckValidationError>
DeckValidationError::fromRawBytes(QByteArrayView bytes, QStringView path) {
  auto raw = Json::Value::parse(bytes, path);
  if (!raw)
    return failure(raw.error());
  return fromRawJson(*raw, path);
}

ValueOrError<QJsonObject> DeckValidationError::toJson() const {
  auto cardCodeEncoded = cardCode.toJson();
  if (!cardCodeEncoded)
    return failure(QStringLiteral("contents: %1").arg(cardCodeEncoded.error()));
  return QJsonObject{
      {QStringLiteral("tag"), QStringLiteral("UnimplementedCard")},
      {QStringLiteral("contents"), *cardCodeEncoded},
  };
}

// Shared decode body for DeckValidationResult::fromJson()/fromRawJson():
// V is QJsonValue or Json::Value. Each element decodes through
// decodeDeckValidationError<V>, so a duplicate/extra key nested inside any
// one entry is caught on the canonical byte-level path without ever
// collapsing the whole array to QJsonValue first.
template <typename V>
ValueOrError<QList<DeckValidationError>>
decodeDeckValidationResultItems(const V &v, QStringView path) {
  auto arrResult = Json::requireArray(v, path);
  if (!arrResult)
    return failure(arrResult.error());
  QList<DeckValidationError> result;
  result.reserve(arrResult->size());
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    auto item =
        decodeDeckValidationError((*arrResult)[i], Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    result.append(*item);
  }
  return result;
}

template <typename V>
ValueOrError<DeckValidationResult>
decodeDeckValidationResult(const V &v, QStringView path) {
  auto items = decodeDeckValidationResultItems(v, path);
  if (!items)
    return failure(items.error());
  if (items->isEmpty())
    return DeckValidationResult::success();
  return DeckValidationResult::errors(*items);
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
  return decodeDeckValidationResult(v, path);
}

ValueOrError<DeckValidationResult>
DeckValidationResult::fromRawJson(const Json::Value &v, QStringView path) {
  return decodeDeckValidationResult(v, path);
}

ValueOrError<DeckValidationResult>
DeckValidationResult::fromRawBytes(QByteArrayView bytes, QStringView path) {
  auto raw = Json::Value::parse(bytes, path);
  if (!raw)
    return failure(raw.error());
  return fromRawJson(*raw, path);
}

ValueOrError<QJsonArray> DeckValidationResult::toJson() const {
  QJsonArray arr;
  for (qsizetype i = 0; i < m_errors.size(); ++i) {
    auto encoded = m_errors.at(i).toJson();
    if (!encoded)
      return failure(QStringLiteral("[%1]: %2").arg(i).arg(encoded.error()));
    arr.append(*encoded);
  }
  return arr;
}

// Shared decode body for DeckOperationError::fromJson()/fromRawJson(): V
// is QJsonValue or Json::Value.
template <typename V>
ValueOrError<DeckOperationError> decodeDeckOperationError(const V &v,
                                                          QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  // decks.schema.json's deckOperationError is additionalProperties:false
  // with exactly {"errorMsg"} (round-10-cumulative-review item 5).
  auto exactKeys = Json::requireExactKeys(obj, {"errorMsg"_L1}, path);
  if (!exactKeys)
    return failure(exactKeys.error());

  auto errorMsg = Json::requireString(obj, "errorMsg"_L1,
                                      Json::joinPath(path, u"errorMsg"));
  if (!errorMsg)
    return failure(errorMsg.error());
  return DeckOperationError{.errorMsg = *errorMsg};
}

ValueOrError<DeckOperationError>
DeckOperationError::fromJson(const QJsonValue &v, QStringView path) {
  return decodeDeckOperationError(v, path);
}

ValueOrError<DeckOperationError>
DeckOperationError::fromRawJson(const Json::Value &v, QStringView path) {
  return decodeDeckOperationError(v, path);
}

ValueOrError<DeckOperationError>
DeckOperationError::fromRawBytes(QByteArrayView bytes, QStringView path) {
  auto raw = Json::Value::parse(bytes, path);
  if (!raw)
    return failure(raw.error());
  return fromRawJson(*raw, path);
}

QJsonObject DeckOperationError::toJson() const {
  return QJsonObject{{QStringLiteral("errorMsg"), errorMsg}};
}

} // namespace Arkham
