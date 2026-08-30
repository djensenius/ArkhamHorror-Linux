#include "CardCatalog.h"

#include "JsonDecode.h"

#include <QJsonArray>
#include <QSet>
#include <array>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

constexpr std::array<std::pair<QLatin1StringView, CardType>, 17> kCardTypeTable{
    {
        {"AssetType"_L1, CardType::AssetType},
        {"EventType"_L1, CardType::EventType},
        {"SkillType"_L1, CardType::SkillType},
        {"PlayerTreacheryType"_L1, CardType::PlayerTreacheryType},
        {"PlayerEnemyType"_L1, CardType::PlayerEnemyType},
        {"TreacheryType"_L1, CardType::TreacheryType},
        {"EnemyType"_L1, CardType::EnemyType},
        {"LocationType"_L1, CardType::LocationType},
        {"EnemyLocationCardType"_L1, CardType::EnemyLocationCardType},
        {"EncounterAssetType"_L1, CardType::EncounterAssetType},
        {"EncounterEventType"_L1, CardType::EncounterEventType},
        {"ActType"_L1, CardType::ActType},
        {"AgendaType"_L1, CardType::AgendaType},
        {"StoryType"_L1, CardType::StoryType},
        {"InvestigatorType"_L1, CardType::InvestigatorType},
        {"ScenarioType"_L1, CardType::ScenarioType},
        {"KeyType"_L1, CardType::KeyType},
    }};

constexpr std::array<std::pair<QLatin1StringView, CardSubType>, 2>
    kCardSubTypeTable{{
        {"Weakness"_L1, CardSubType::Weakness},
        {"BasicWeakness"_L1, CardSubType::BasicWeakness},
    }};

constexpr std::array<std::pair<QLatin1StringView, ClassSymbol>, 7>
    kClassSymbolTable{{
        {"Guardian"_L1, ClassSymbol::Guardian},
        {"Seeker"_L1, ClassSymbol::Seeker},
        {"Survivor"_L1, ClassSymbol::Survivor},
        {"Rogue"_L1, ClassSymbol::Rogue},
        {"Mystic"_L1, ClassSymbol::Mystic},
        {"Neutral"_L1, ClassSymbol::Neutral},
        {"Mythos"_L1, ClassSymbol::Mythos},
    }};

constexpr std::array<std::pair<QLatin1StringView, Revelation>, 3>
    kRevelationTable{{
        {"NoRevelation"_L1, Revelation::NoRevelation},
        {"IsRevelation"_L1, Revelation::IsRevelation},
        {"CannotBeCanceledRevelation"_L1,
         Revelation::CannotBeCanceledRevelation},
    }};

constexpr std::array<std::pair<QLatin1StringView, SlotType>, 7> kSlotTypeTable{{
    {"HandSlot"_L1, SlotType::HandSlot},
    {"BodySlot"_L1, SlotType::BodySlot},
    {"AllySlot"_L1, SlotType::AllySlot},
    {"AccessorySlot"_L1, SlotType::AccessorySlot},
    {"ArcaneSlot"_L1, SlotType::ArcaneSlot},
    {"TarotSlot"_L1, SlotType::TarotSlot},
    {"HeadSlot"_L1, SlotType::HeadSlot},
}};

constexpr std::array<std::pair<QLatin1StringView, WhenDiscarded>, 3>
    kWhenDiscardedTable{{
        {"ToDiscard"_L1, WhenDiscarded::ToDiscard},
        {"ToBonded"_L1, WhenDiscarded::ToBonded},
        {"ToSetAside"_L1, WhenDiscarded::ToSetAside},
    }};

constexpr std::array<std::pair<QLatin1StringView, OutOfPlayEffect>, 4>
    kOutOfPlayEffectTable{{
        {"InHandEffect"_L1, OutOfPlayEffect::InHandEffect},
        {"InDiscardEffect"_L1, OutOfPlayEffect::InDiscardEffect},
        {"InSearchEffect"_L1, OutOfPlayEffect::InSearchEffect},
        {"OnTopOfDeckEffect"_L1, OutOfPlayEffect::OnTopOfDeckEffect},
    }};

constexpr std::array<std::pair<QLatin1StringView, SkillType>, 4>
    kSkillTypeTable{{
        {"SkillWillpower"_L1, SkillType::SkillWillpower},
        {"SkillIntellect"_L1, SkillType::SkillIntellect},
        {"SkillCombat"_L1, SkillType::SkillCombat},
        {"SkillAgility"_L1, SkillType::SkillAgility},
    }};

// Dispatch shims: a templated decode body calls these by unqualified name
// so ADL/overload resolution -- not an explicit if-constexpr branch --
// picks the QJsonValue-taking convenience overload or the Json::Value-
// taking canonical overload to match the surrounding template's deduced
// value-family parameter, mirroring the dual-overload pattern JsonDecode.h
// itself already uses throughout.
ValueOrError<CardCode> decodeCardCodeValue(const QJsonValue &v,
                                           QStringView path) {
  return CardCode::fromJson(v, path);
}
ValueOrError<CardCode> decodeCardCodeValue(const Json::Value &v,
                                           QStringView path) {
  auto str = Json::requireStringValue(v, path);
  if (!str)
    return failure(str.error());
  auto parsed = CardCode::parse(*str);
  if (!parsed)
    return failure(QStringLiteral("%1: %2").arg(path, parsed.error()));
  return *parsed;
}

ValueOrError<CardName> decodeCardNameValue(const QJsonValue &v,
                                           QStringView path) {
  return CardName::fromJson(v, path);
}
ValueOrError<CardName> decodeCardNameValue(const Json::Value &v,
                                           QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const Json::Value &obj = *objResult;
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

ValueOrError<SkillIcon> decodeSkillIconValue(const QJsonValue &v,
                                             QStringView path) {
  return SkillIcon::fromJson(v, path);
}
ValueOrError<SkillIcon> decodeSkillIconValue(const Json::Value &v,
                                             QStringView path) {
  return SkillIcon::fromRawJson(v, path);
}

ValueOrError<CardCost> decodeCardCostValue(const QJsonValue &v,
                                           QStringView path) {
  return CardCost::fromJson(v, path);
}
ValueOrError<CardCost> decodeCardCostValue(const Json::Value &v,
                                           QStringView path) {
  return CardCost::fromRawJson(v, path);
}

ValueOrError<GameValue> decodeGameValueValue(const QJsonValue &v,
                                             QStringView path) {
  return GameValue::fromJson(v, path);
}
ValueOrError<GameValue> decodeGameValueValue(const Json::Value &v,
                                             QStringView path) {
  return GameValue::fromRawJson(v, path);
}

// Converts an already-extracted field value to the lossless AST type every
// scoped contract domain type now stores its schema-unconstrained fields
// as (see CardDef's class comment in CardCatalog.h) -- see
// Json::toLosslessRaw()'s own doc comment in JsonDecode.h for the full
// contract; brought in unqualified here since it is used pervasively
// below and its Json::Value overload is also reachable via ADL, but its
// QJsonValue overload is not (QJsonValue's associated namespace is Qt's,
// not Arkham::Json).
using Json::toLosslessRaw;

// Decodes an optional array-of-closed-enum field: an absent key decodes to
// an empty list; a present non-array value, or an unrecognized element,
// fails. When requireUnique is true (classSymbols' inline
// "uniqueItems":true), a repeated decoded value fails rather than silently
// collapsing -- duplicates are compared by decoded value, not raw JSON
// text, so e.g. two differently-cased spellings of the same enum literal
// would already have failed decodeClosedEnum for one of them.
//
// Templated over Obj (QJsonObject or Json::Value) so the identical
// validation logic backs both SkillIcon/CardCost/GameValue/CardDef's
// QJsonValue-based fromJson() convenience entry points and their
// Json::Value-based fromRawJson() canonical entry points -- see
// CardDef::fromRawJson()'s doc comment in CardCatalog.h for why the
// canonical path must never convert through QJsonValue first.
template <typename Enum, std::size_t N, typename Obj>
ValueOrError<QList<Enum>>
decodeEnumArray(const Obj &obj, QLatin1StringView key, QStringView path,
                const std::array<std::pair<QLatin1StringView, Enum>, N> &table,
                bool requireUnique = false) {
  const auto v = obj.value(key);
  if (v.isUndefined())
    return QList<Enum>{};
  auto arrResult = Json::requireArray(v, path);
  if (!arrResult)
    return failure(arrResult.error());
  QList<Enum> result;
  result.reserve(arrResult->size());
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    auto item = Json::decodeClosedEnum<Enum>((*arrResult)[i],
                                             Json::indexPath(path, i), table);
    if (!item)
      return failure(item.error());
    if (requireUnique && result.contains(*item))
      return failure(
          QStringLiteral("%1: duplicate value at index %2").arg(path).arg(i));
    result.append(*item);
  }
  return result;
}

// Builds a raw-AST array of closed-enum values, for CardDef::toRawJson().
// Fails (rather than crashing via
// Json::encodeClosedEnum()'s former Q_UNREACHABLE) if any element is a
// closed-enum value fabricated via static_cast from outside its real
// range -- see Json::encodeClosedEnum()'s doc comment in JsonDecode.h.
template <typename Enum, std::size_t N>
ValueOrError<Json::Value> encodeEnumArrayRaw(
    const QList<Enum> &values,
    const std::array<std::pair<QLatin1StringView, Enum>, N> &table) {
  QList<Json::Value> result;
  for (qsizetype i = 0; i < values.size(); ++i) {
    auto encoded = Json::encodeClosedEnum(values.at(i), table);
    if (!encoded)
      return failure(QStringLiteral("[%1]: %2").arg(i).arg(encoded.error()));
    result.append(Json::Value::makeString(*encoded));
  }
  return Json::Value::makeArray(result);
}

// Decodes an optional `stringSet`-shaped field (cardTraits/
// revealedCardTraits/tags): an absent key decodes to an empty list. Only
// cardTraits/revealedCardTraits are schema-typed with "uniqueItems":true
// (via the shared `stringSet` $def); `tags` is a plain string array with
// no uniqueness constraint, so requireUnique defaults to false and callers
// opt in explicitly.
template <typename Obj>
ValueOrError<QStringList> decodeStringSet(const Obj &obj, QLatin1StringView key,
                                          QStringView path,
                                          bool requireUnique = false) {
  const auto v = obj.value(key);
  if (v.isUndefined())
    return QStringList{};
  auto arrResult = Json::requireArray(v, path);
  if (!arrResult)
    return failure(arrResult.error());
  QStringList result;
  result.reserve(arrResult->size());
  // Membership is tracked in a QSet (amortized O(1) lookup/insert) rather
  // than repeated QStringList::contains() scans (which would make this
  // function O(n^2) in the accepted array's length -- up to
  // ParseLimits::maxArrayElements, 20,000 by default): `result` itself
  // stays an ordered QStringList so callers still see first-seen order,
  // and QSet<QString>'s hashing (Qt's own qHash(QString), seeded per
  // process against hash-flooding) keeps a single duplicate-tag string
  // repeated many times just as cheap as many distinct ones.
  QSet<QString> seen;
  seen.reserve(arrResult->size());
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    auto item =
        Json::requireStringValue((*arrResult)[i], Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    if (requireUnique) {
      if (seen.contains(*item))
        return failure(
            QStringLiteral("%1: duplicate value at index %2").arg(path).arg(i));
      seen.insert(*item);
    }
    result.append(*item);
  }
  return result;
}

template <typename Obj>
ValueOrError<QList<CardCode>>
decodeCardCodeArray(const Obj &obj, QLatin1StringView key, QStringView path) {
  const auto v = obj.value(key);
  if (v.isUndefined())
    return QList<CardCode>{};
  auto arrResult = Json::requireArray(v, path);
  if (!arrResult)
    return failure(arrResult.error());
  QList<CardCode> result;
  result.reserve(arrResult->size());
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    auto item = decodeCardCodeValue((*arrResult)[i], Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    result.append(*item);
  }
  return result;
}

template <typename Obj>
ValueOrError<QList<SkillIcon>>
decodeSkillIconArray(const Obj &obj, QLatin1StringView key, QStringView path) {
  const auto v = obj.value(key);
  if (v.isUndefined())
    return QList<SkillIcon>{};
  auto arrResult = Json::requireArray(v, path);
  if (!arrResult)
    return failure(arrResult.error());
  QList<SkillIcon> result;
  result.reserve(arrResult->size());
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    auto item = decodeSkillIconValue((*arrResult)[i], Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    result.append(*item);
  }
  return result;
}

template <typename Obj>
ValueOrError<QList<std::pair<qint64, CardCode>>>
decodeBondedWith(const Obj &obj, QStringView path) {
  const auto v = obj.value("bondedWith"_L1);
  if (v.isUndefined())
    return QList<std::pair<qint64, CardCode>>{};
  auto arrResult = Json::requireArray(v, path);
  if (!arrResult)
    return failure(arrResult.error());
  QList<std::pair<qint64, CardCode>> result;
  result.reserve(arrResult->size());
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    const QString itemPath = Json::indexPath(path, i);
    auto pairResult = Json::requireArray((*arrResult)[i], itemPath);
    if (!pairResult)
      return failure(pairResult.error());
    if (pairResult->size() != 2)
      return failure(QStringLiteral("%1: expected a 2-element array, got %2 "
                                    "elements")
                         .arg(itemPath)
                         .arg(pairResult->size()));
    auto countResult =
        Json::requireIntValue((*pairResult)[0], Json::indexPath(itemPath, 0));
    if (!countResult)
      return failure(countResult.error());
    auto codeResult =
        decodeCardCodeValue((*pairResult)[1], Json::indexPath(itemPath, 1));
    if (!codeResult)
      return failure(codeResult.error());
    result.append({*countResult, *codeResult});
  }
  return result;
}

template <typename Obj>
ValueOrError<QMap<QString, QList<SkillIcon>>>
decodeAlternateSkills(const Obj &obj, QStringView path) {
  const auto v = obj.value("alternateSkills"_L1);
  if (v.isUndefined())
    return QMap<QString, QList<SkillIcon>>{};
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  QMap<QString, QList<SkillIcon>> result;
  for (const auto &[key, value] : Json::objectMembers(*objResult)) {
    const QString entryPath = Json::joinPath(path, key);
    auto arrResult = Json::requireArray(value, entryPath);
    if (!arrResult)
      return failure(arrResult.error());
    QList<SkillIcon> icons;
    icons.reserve(arrResult->size());
    for (qsizetype i = 0; i < arrResult->size(); ++i) {
      auto icon =
          decodeSkillIconValue((*arrResult)[i], Json::indexPath(entryPath, i));
      if (!icon)
        return failure(icon.error());
      icons.append(*icon);
    }
    result.insert(key, icons);
  }
  return result;
}

template <typename Obj>
ValueOrError<QMap<QString, QString>> decodeAlternateErrata(const Obj &obj,
                                                           QStringView path) {
  const auto v = obj.value("alternateErrata"_L1);
  if (v.isUndefined())
    return QMap<QString, QString>{};
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  QMap<QString, QString> result;
  for (const auto &[key, value] : Json::objectMembers(*objResult)) {
    auto strResult = Json::requireStringValue(value, Json::joinPath(path, key));
    if (!strResult)
      return failure(strResult.error());
    result.insert(key, *strResult);
  }
  return result;
}

} // namespace

ValueOrError<SkillIcon> SkillIcon::skillType(SkillType type) {
  auto encoded = Json::encodeClosedEnum(type, kSkillTypeTable);
  if (!encoded)
    return failure(
        QStringLiteral("SkillIcon::skillType: %1").arg(encoded.error()));
  SkillIcon result;
  result.m_tag = SkillIconTag::SkillIcon;
  result.m_skill = type;
  return result;
}

SkillIcon SkillIcon::wild() {
  SkillIcon result;
  result.m_tag = SkillIconTag::WildIcon;
  return result;
}

SkillIcon SkillIcon::wildMinus() {
  SkillIcon result;
  result.m_tag = SkillIconTag::WildMinusIcon;
  return result;
}

ValueOrError<SkillIcon> SkillIcon::fromJson(const QJsonValue &v,
                                            QStringView path) {
  return fromValueImpl(v, path);
}

ValueOrError<SkillIcon> SkillIcon::fromRawJson(const Json::Value &v,
                                               QStringView path) {
  return fromValueImpl(v, path);
}

template <typename V>
ValueOrError<SkillIcon> SkillIcon::fromValueImpl(const V &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto tagResult =
      Json::requireString(obj, "tag"_L1, Json::joinPath(path, u"tag"));
  if (!tagResult)
    return failure(tagResult.error());

  if (*tagResult == "SkillIcon"_L1) {
    // additionalProperties: false on this branch's exact shape -- an
    // extra key beside "tag"/"contents" is malformed, not a forward-
    // compat additive field (see requireExactKeys's doc comment).
    auto keysResult =
        Json::requireExactKeys(obj, {"tag"_L1, "contents"_L1}, path);
    if (!keysResult)
      return failure(keysResult.error());
    auto skillResult = Json::requireField(
        obj, "contents"_L1, Json::joinPath(path, u"contents"),
        [](const auto &cv, QStringView cp) {
          return Json::decodeClosedEnum<SkillType>(cv, cp, kSkillTypeTable);
        });
    if (!skillResult)
      return failure(skillResult.error());
    return SkillIcon::skillType(*skillResult);
  }
  // WildIcon/WildMinusIcon are documented nullary tags: the schema allows
  // no "contents" key at all (or any other key), so an explicit contents
  // value -- even an explicit JSON null -- is malformed input, not a
  // value to silently discard.
  if (*tagResult == "WildIcon"_L1 || *tagResult == "WildMinusIcon"_L1) {
    auto keysResult = Json::requireExactKeys(obj, {"tag"_L1}, path);
    if (!keysResult)
      return failure(keysResult.error());
    return *tagResult == "WildIcon"_L1 ? SkillIcon::wild()
                                       : SkillIcon::wildMinus();
  }
  // An unrecognized tag preserves the complete raw decoded object (its
  // "tag" and, if present, "contents") verbatim rather than failing --
  // this client cannot interpret the tag, but the backend fixture/response
  // it came from is otherwise well-formed, and forward-compat additive
  // tags must decode safely.
  auto rawResult = toLosslessRaw(v);
  if (!rawResult)
    return failure(QStringLiteral("%1: %2").arg(path, rawResult.error()));
  SkillIcon result;
  result.m_tag = SkillIconTag::Unknown;
  result.m_unknownRaw = *rawResult;
  return result;
}

ValueOrError<Json::Value> SkillIcon::toRawJson() const {
  switch (m_tag) {
  case SkillIconTag::SkillIcon: {
    // m_skill is guaranteed populated here: the only way to construct a
    // SkillIcon with tag == SkillIcon is the skillType() factory, which
    // always sets it, and the private constructor/fromJson never leave it
    // unset for this tag -- no runtime guard is needed or appropriate.
    auto encoded = Json::encodeClosedEnum(*m_skill, kSkillTypeTable);
    if (!encoded)
      return failure(
          QStringLiteral("SkillIcon::toRawJson: %1").arg(encoded.error()));
    return Json::Value::makeObject({
        {QStringLiteral("tag"),
         Json::Value::makeString(QStringLiteral("SkillIcon"))},
        {QStringLiteral("contents"), Json::Value::makeString(*encoded)},
    });
  }
  case SkillIconTag::WildIcon:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("WildIcon"))}});
  case SkillIconTag::WildMinusIcon:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("WildMinusIcon"))}});
  case SkillIconTag::Unknown:
    return m_unknownRaw;
  }
  // No case above can actually be missed for any m_tag this type's
  // private constructor/factories can produce, but a typed failure --
  // rather than Q_UNREACHABLE_RETURN -- means an out-of-range m_tag
  // fails cleanly instead of aborting/UB.
  return failure(
      QStringLiteral("SkillIcon::toRawJson: unhandled SkillIcon tag value"));
}

ValueOrError<QByteArray> SkillIcon::toJsonBytes() const {
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toJsonBytes();
}

CardCost CardCost::staticCost(qint64 amount) {
  CardCost result;
  result.m_tag = CardCostTag::StaticCost;
  result.m_staticAmount = amount;
  return result;
}

CardCost CardCost::dynamicCost() {
  CardCost result;
  result.m_tag = CardCostTag::DynamicCost;
  return result;
}

CardCost CardCost::discardAmountCost() {
  CardCost result;
  result.m_tag = CardCostTag::DiscardAmountCost;
  return result;
}

CardCost CardCost::deferredCost() {
  CardCost result;
  result.m_tag = CardCostTag::DeferredCost;
  return result;
}

ValueOrError<CardCost> CardCost::maxDynamicCost(Json::Value contents) {
  // The schema's payload is genuinely unconstrained ("{}"), but
  // "unconstrained" still means "some JSON value is present" -- an
  // Undefined contents is not a value at all, and a public factory that
  // silently accepted one anyway would let calling code build a CardCost
  // whose toJson() then has no "contents" to encode for a tag the schema
  // requires one for.
  if (contents.isUndefined())
    return failure(QStringLiteral(
        "CardCost::maxDynamicCost: contents must not be undefined"));
  CardCost result;
  result.m_tag = CardCostTag::MaxDynamicCost;
  result.m_rawContents = std::move(contents);
  return result;
}

ValueOrError<CardCost> CardCost::anyMatchingCardCost(Json::Value contents) {
  if (contents.isUndefined())
    return failure(QStringLiteral(
        "CardCost::anyMatchingCardCost: contents must not be undefined"));
  CardCost result;
  result.m_tag = CardCostTag::AnyMatchingCardCost;
  result.m_rawContents = std::move(contents);
  return result;
}

ValueOrError<CardCost> CardCost::matchingEnemyFieldCost(Json::Value contents) {
  if (contents.isUndefined())
    return failure(QStringLiteral(
        "CardCost::matchingEnemyFieldCost: contents must not be undefined"));
  // See this factory's header doc comment: the pinned backend's
  // MatchingEnemyFieldCost is a genuine two-argument constructor, so
  // "contents" must be a JSON array of exactly two elements -- not
  // arbitrary shape -- despite the schema's conservative "{}".
  if (!contents.isArray() || contents.toArray().size() != 2)
    return failure(QStringLiteral(
        "CardCost::matchingEnemyFieldCost: contents must be a JSON array "
        "of exactly two elements"));
  CardCost result;
  result.m_tag = CardCostTag::MatchingEnemyFieldCost;
  result.m_rawContents = std::move(contents);
  return result;
}

ValueOrError<CardCost> CardCost::fromJson(const QJsonValue &v,
                                          QStringView path) {
  return fromValueImpl(v, path);
}

ValueOrError<CardCost> CardCost::fromRawJson(const Json::Value &v,
                                             QStringView path) {
  return fromValueImpl(v, path);
}

template <typename V>
ValueOrError<CardCost> CardCost::fromValueImpl(const V &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto tagResult =
      Json::requireString(obj, "tag"_L1, Json::joinPath(path, u"tag"));
  if (!tagResult)
    return failure(tagResult.error());
  const QString &tag = *tagResult;

  if (tag == "StaticCost"_L1) {
    auto keysResult =
        Json::requireExactKeys(obj, {"tag"_L1, "contents"_L1}, path);
    if (!keysResult)
      return failure(keysResult.error());
    auto amount =
        Json::requireInt(obj, "contents"_L1, Json::joinPath(path, u"contents"));
    if (!amount)
      return failure(amount.error());
    return CardCost::staticCost(*amount);
  }
  // DynamicCost/DiscardAmountCost/DeferredCost are documented nullary
  // tags: the schema allows no "contents" key (or any other key), so an
  // explicit contents value -- even an explicit JSON null -- is malformed
  // input.
  if (tag == "DynamicCost"_L1 || tag == "DiscardAmountCost"_L1 ||
      tag == "DeferredCost"_L1) {
    auto keysResult = Json::requireExactKeys(obj, {"tag"_L1}, path);
    if (!keysResult)
      return failure(keysResult.error());
    if (tag == "DynamicCost"_L1)
      return CardCost::dynamicCost();
    if (tag == "DiscardAmountCost"_L1)
      return CardCost::discardAmountCost();
    return CardCost::deferredCost();
  }
  if (tag == "MaxDynamicCost"_L1 || tag == "AnyMatchingCardCost"_L1 ||
      tag == "MatchingEnemyFieldCost"_L1) {
    auto keysResult =
        Json::requireExactKeys(obj, {"tag"_L1, "contents"_L1}, path);
    if (!keysResult)
      return failure(keysResult.error());
    auto contents = Json::requireRawField(obj, "contents"_L1,
                                          Json::joinPath(path, u"contents"));
    if (!contents)
      return failure(contents.error());
    auto lossless = toLosslessRaw(*contents);
    if (!lossless)
      return failure(QStringLiteral("%1: %2").arg(path, lossless.error()));
    if (tag == "MaxDynamicCost"_L1) {
      auto result = CardCost::maxDynamicCost(*lossless);
      if (!result)
        return failure(QStringLiteral("%1: %2").arg(path, result.error()));
      return *result;
    }
    if (tag == "AnyMatchingCardCost"_L1) {
      auto result = CardCost::anyMatchingCardCost(*lossless);
      if (!result)
        return failure(QStringLiteral("%1: %2").arg(path, result.error()));
      return *result;
    }
    auto result = CardCost::matchingEnemyFieldCost(*lossless);
    if (!result)
      return failure(QStringLiteral("%1: %2").arg(path, result.error()));
    return *result;
  }
  // An unrecognized tag preserves the complete raw decoded object
  // verbatim; see SkillIcon::fromJson's Unknown branch for the rationale.
  auto rawResult = toLosslessRaw(v);
  if (!rawResult)
    return failure(QStringLiteral("%1: %2").arg(path, rawResult.error()));
  CardCost result;
  result.m_tag = CardCostTag::Unknown;
  result.m_unknownRaw = *rawResult;
  return result;
}

Json::Value CardCost::toRawJson() const {
  const auto tagged = [](QLatin1StringView tagName,
                         const Json::Value &contents) {
    return Json::Value::makeObject(
        {{QStringLiteral("tag"), Json::Value::makeString(QString(tagName))},
         {QStringLiteral("contents"), contents}});
  };
  switch (m_tag) {
  case CardCostTag::StaticCost:
    // m_staticAmount is guaranteed populated for tag == StaticCost: the
    // only way to construct one is the staticCost() factory, which always
    // sets it.
    return tagged(
        "StaticCost"_L1,
        Json::Value::makeNumber(Json::RawNumber::fromInt64(*m_staticAmount)));
  case CardCostTag::DynamicCost:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("DynamicCost"))}});
  case CardCostTag::DiscardAmountCost:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("DiscardAmountCost"))}});
  case CardCostTag::DeferredCost:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("DeferredCost"))}});
  case CardCostTag::MaxDynamicCost:
    // m_rawContents is guaranteed populated (non-Undefined) for the three
    // raw-payload tags below: their only factories (maxDynamicCost() etc.)
    // always set it, and fromJson requires "contents" to be present.
    return tagged("MaxDynamicCost"_L1, m_rawContents);
  case CardCostTag::AnyMatchingCardCost:
    return tagged("AnyMatchingCardCost"_L1, m_rawContents);
  case CardCostTag::MatchingEnemyFieldCost:
    return tagged("MatchingEnemyFieldCost"_L1, m_rawContents);
  case CardCostTag::Unknown:
    return m_unknownRaw;
  }
  // No case above can actually be missed for any m_tag this type's
  // private constructor/factories can produce (see the class invariant
  // comments above), but this trailing return -- rather than
  // Q_UNREACHABLE_RETURN -- means an out-of-range m_tag (e.g. future enum
  // extension not handled here) fails to encode as Undefined instead of
  // aborting/UB; Value::toJsonBytes() already rejects an Undefined value
  // outright, so misuse still fails loudly, just not catastrophically.
  return Json::Value{};
}

ValueOrError<QByteArray> CardCost::toJsonBytes() const {
  return toRawJson().toJsonBytes();
}

GameValue GameValue::staticValue(qint64 amount) {
  GameValue result;
  result.m_tag = GameValueTag::Static;
  result.m_singleAmount = amount;
  return result;
}

GameValue GameValue::perPlayer(qint64 amount) {
  GameValue result;
  result.m_tag = GameValueTag::PerPlayer;
  result.m_singleAmount = amount;
  return result;
}

GameValue GameValue::staticWithPerPlayer(qint64 staticAmount,
                                         qint64 perPlayerAmount) {
  GameValue result;
  result.m_tag = GameValueTag::StaticWithPerPlayer;
  result.m_contents = {staticAmount, perPlayerAmount};
  return result;
}

GameValue GameValue::byPlayerCount(qint64 onePlayer, qint64 twoPlayers,
                                   qint64 threePlayers, qint64 fourPlayers) {
  GameValue result;
  result.m_tag = GameValueTag::ByPlayerCount;
  result.m_contents = {onePlayer, twoPlayers, threePlayers, fourPlayers};
  return result;
}

GameValue GameValue::valueX() {
  GameValue result;
  result.m_tag = GameValueTag::ValueX;
  return result;
}

GameValue GameValue::valueStar() {
  GameValue result;
  result.m_tag = GameValueTag::ValueStar;
  return result;
}

GameValue GameValue::valueUnknown() {
  GameValue result;
  result.m_tag = GameValueTag::ValueUnknown;
  return result;
}

ValueOrError<GameValue> GameValue::fromJson(const QJsonValue &v,
                                            QStringView path) {
  return fromValueImpl(v, path);
}

ValueOrError<GameValue> GameValue::fromRawJson(const Json::Value &v,
                                               QStringView path) {
  return fromValueImpl(v, path);
}

template <typename V>
ValueOrError<GameValue> GameValue::fromValueImpl(const V &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto tagResult =
      Json::requireString(obj, "tag"_L1, Json::joinPath(path, u"tag"));
  if (!tagResult)
    return failure(tagResult.error());
  const QString &tag = *tagResult;
  const QString contentsPath = Json::joinPath(path, u"contents");

  if (tag == "Static"_L1 || tag == "PerPlayer"_L1) {
    auto keysResult =
        Json::requireExactKeys(obj, {"tag"_L1, "contents"_L1}, path);
    if (!keysResult)
      return failure(keysResult.error());
    auto amount = Json::requireInt(obj, "contents"_L1, contentsPath);
    if (!amount)
      return failure(amount.error());
    return tag == "Static"_L1 ? GameValue::staticValue(*amount)
                              : GameValue::perPlayer(*amount);
  }
  if (tag == "StaticWithPerPlayer"_L1 || tag == "ByPlayerCount"_L1) {
    auto keysResult =
        Json::requireExactKeys(obj, {"tag"_L1, "contents"_L1}, path);
    if (!keysResult)
      return failure(keysResult.error());
    const qsizetype expected = tag == "StaticWithPerPlayer"_L1 ? 2 : 4;
    auto arrResult = Json::requireArrayField(obj, "contents"_L1, contentsPath);
    if (!arrResult)
      return failure(arrResult.error());
    if (arrResult->size() != expected)
      return failure(QStringLiteral("%1: expected a %2-element array, got "
                                    "%3 elements")
                         .arg(contentsPath)
                         .arg(expected)
                         .arg(arrResult->size()));
    QList<qint64> contents;
    contents.reserve(expected);
    for (qsizetype i = 0; i < arrResult->size(); ++i) {
      auto item = Json::requireIntValue((*arrResult)[i],
                                        Json::indexPath(contentsPath, i));
      if (!item)
        return failure(item.error());
      contents.append(*item);
    }
    if (tag == "StaticWithPerPlayer"_L1)
      return GameValue::staticWithPerPlayer(contents[0], contents[1]);
    return GameValue::byPlayerCount(contents[0], contents[1], contents[2],
                                    contents[3]);
  }
  // ValueX/ValueStar/ValueUnknown are documented nullary tags: the schema
  // allows no "contents" key (or any other key), so an explicit contents
  // value -- even an explicit JSON null -- is malformed input.
  if (tag == "ValueX"_L1 || tag == "ValueStar"_L1 || tag == "ValueUnknown"_L1) {
    auto keysResult = Json::requireExactKeys(obj, {"tag"_L1}, path);
    if (!keysResult)
      return failure(keysResult.error());
    if (tag == "ValueX"_L1)
      return GameValue::valueX();
    if (tag == "ValueStar"_L1)
      return GameValue::valueStar();
    return GameValue::valueUnknown();
  }
  // An unrecognized tag preserves the complete raw decoded object
  // verbatim; see SkillIcon::fromJson's Unknown branch for the rationale.
  auto rawResult = toLosslessRaw(v);
  if (!rawResult)
    return failure(QStringLiteral("%1: %2").arg(path, rawResult.error()));
  GameValue result;
  result.m_tag = GameValueTag::Unknown;
  result.m_unknownRaw = *rawResult;
  return result;
}

Json::Value GameValue::toRawJson() const {
  const auto withContents = [](QLatin1StringView wireTag,
                               const Json::Value &contentsVal) {
    return Json::Value::makeObject(
        {{QStringLiteral("tag"), Json::Value::makeString(QString(wireTag))},
         {QStringLiteral("contents"), contentsVal}});
  };
  switch (m_tag) {
  case GameValueTag::Static:
    // m_singleAmount is guaranteed populated for Static/PerPlayer: the
    // only way to construct one is staticValue()/perPlayer(), which
    // always set it.
    return withContents(
        "Static"_L1,
        Json::Value::makeNumber(Json::RawNumber::fromInt64(*m_singleAmount)));
  case GameValueTag::PerPlayer:
    return withContents(
        "PerPlayer"_L1,
        Json::Value::makeNumber(Json::RawNumber::fromInt64(*m_singleAmount)));
  case GameValueTag::StaticWithPerPlayer: {
    // m_contents is guaranteed to hold exactly 2 elements here: the only
    // way to construct a StaticWithPerPlayer GameValue is
    // staticWithPerPlayer(qint64, qint64), which always sets exactly 2.
    QList<Json::Value> arr;
    for (const qint64 n : m_contents)
      arr.append(Json::Value::makeNumber(Json::RawNumber::fromInt64(n)));
    return withContents("StaticWithPerPlayer"_L1, Json::Value::makeArray(arr));
  }
  case GameValueTag::ByPlayerCount: {
    // Likewise guaranteed to hold exactly 4 elements via byPlayerCount().
    QList<Json::Value> arr;
    for (const qint64 n : m_contents)
      arr.append(Json::Value::makeNumber(Json::RawNumber::fromInt64(n)));
    return withContents("ByPlayerCount"_L1, Json::Value::makeArray(arr));
  }
  case GameValueTag::ValueX:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("ValueX"))}});
  case GameValueTag::ValueStar:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("ValueStar"))}});
  case GameValueTag::ValueUnknown:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("ValueUnknown"))}});
  case GameValueTag::Unknown:
    return m_unknownRaw;
  }
  // See CardCost::toRawJson() above for why a plain trailing return
  // (rather than Q_UNREACHABLE_RETURN) is used here: an out-of-range
  // m_tag encodes as Undefined -- which Value::toJsonBytes() already
  // rejects -- instead of aborting/UB.
  return Json::Value{};
}

ValueOrError<QByteArray> GameValue::toJsonBytes() const {
  return toRawJson().toJsonBytes();
}

namespace {

// Shared decode body for CardDef::fromJson()/fromRawJson(): V is
// QJsonValue or Json::Value. A free function (not a CardDef member
// template) is sufficient here, unlike SkillIcon/CardCost/GameValue's
// private fromValueImpl, since CardDef is a plain aggregate with no
// private constructor to guard.
template <typename V>
ValueOrError<CardDef> decodeCardDef(const V &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  // catalog.schema.json's `cardDef` is additionalProperties:false with
  // exactly these 62 properties (round-10-cumulative-review item 4:
  // reversing this client's earlier policy of silently ignoring an
  // unrecognized top-level key here -- this pinned contract slice treats
  // CardDef as fully closed, matching the schema's explicit `false`,
  // rather than pre-emptively tolerating a hypothetical future field this
  // client has not yet modeled; see extraTopLevelFieldOnCardDefRejected in
  // CardCatalogTests.cpp).
  auto exactKeys =
      Json::requireExactKeys(obj,
                             {"cardCode"_L1,
                              "name"_L1,
                              "revealedName"_L1,
                              "cost"_L1,
                              "additionalCost"_L1,
                              "level"_L1,
                              "cardType"_L1,
                              "cardSubType"_L1,
                              "classSymbols"_L1,
                              "skills"_L1,
                              "cardTraits"_L1,
                              "revealedCardTraits"_L1,
                              "keywords"_L1,
                              "fastWindow"_L1,
                              "actions"_L1,
                              "revelation"_L1,
                              "victoryPoints"_L1,
                              "vengeancePoints"_L1,
                              "criteria"_L1,
                              "overrideActionPlayableIfCriteriaMet"_L1,
                              "commitRestrictions"_L1,
                              "attackOfOpportunityModifiers"_L1,
                              "permanent"_L1,
                              "encounterSet"_L1,
                              "encounterSetQuantity"_L1,
                              "unique"_L1,
                              "doubleSided"_L1,
                              "limits"_L1,
                              "exceptional"_L1,
                              "uses"_L1,
                              "playableFromDiscard"_L1,
                              "stage"_L1,
                              "slots"_L1,
                              "alternateCardCodes"_L1,
                              "art"_L1,
                              "locationSymbol"_L1,
                              "locationRevealedSymbol"_L1,
                              "locationConnections"_L1,
                              "locationRevealedConnections"_L1,
                              "purchaseTrauma"_L1,
                              "grantedXp"_L1,
                              "canReplace"_L1,
                              "deckRestrictions"_L1,
                              "bondedWith"_L1,
                              "skipPlayWindows"_L1,
                              "beforeEffect"_L1,
                              "customizations"_L1,
                              "otherSide"_L1,
                              "whenDiscarded"_L1,
                              "canCommitWhenNoIcons"_L1,
                              "commitTrigger"_L1,
                              "meta"_L1,
                              "tags"_L1,
                              "outOfPlayEffects"_L1,
                              "health"_L1,
                              "fight"_L1,
                              "evade"_L1,
                              "healthDamage"_L1,
                              "sanityDamage"_L1,
                              "alternateSkills"_L1,
                              "alternateErrata"_L1,
                              "errata"_L1},
                             path);
  if (!exactKeys)
    return failure(exactKeys.error());

  auto cardCode = Json::requireField(
      obj, "cardCode"_L1, Json::joinPath(path, u"cardCode"),
      [](const auto &v, QStringView p) { return decodeCardCodeValue(v, p); });
  if (!cardCode)
    return failure(cardCode.error());
  auto name = Json::requireField(
      obj, "name"_L1, Json::joinPath(path, u"name"),
      [](const auto &v, QStringView p) { return decodeCardNameValue(v, p); });
  if (!name)
    return failure(name.error());
  auto cardType = Json::requireField(
      obj, "cardType"_L1, Json::joinPath(path, u"cardType"),
      [](const auto &v, QStringView p) {
        return Json::decodeClosedEnum<CardType>(v, p, kCardTypeTable);
      });
  if (!cardType)
    return failure(cardType.error());
  auto art = Json::requireString(obj, "art"_L1, Json::joinPath(path, u"art"));
  if (!art)
    return failure(art.error());
  if (art->isEmpty())
    return failure(QStringLiteral("%1: must not be empty")
                       .arg(Json::joinPath(path, u"art")));

  std::optional<CardName> revealedName;
  if (Json::fieldPresence(obj, "revealedName"_L1) !=
      Json::FieldPresence::Absent) {
    auto r = decodeCardNameValue(obj.value("revealedName"_L1),
                                 Json::joinPath(path, u"revealedName"));
    if (!r)
      return failure(r.error());
    revealedName = *r;
  }

  std::optional<CardCost> cost;
  if (Json::fieldPresence(obj, "cost"_L1) != Json::FieldPresence::Absent) {
    auto r = decodeCardCostValue(obj.value("cost"_L1),
                                 Json::joinPath(path, u"cost"));
    if (!r)
      return failure(r.error());
    cost = *r;
  }

  auto level =
      Json::optionalNonNullInt(obj, "level"_L1, Json::joinPath(path, u"level"));
  if (!level)
    return failure(level.error());

  std::optional<CardSubType> cardSubType;
  if (Json::fieldPresence(obj, "cardSubType"_L1) !=
      Json::FieldPresence::Absent) {
    auto r = Json::decodeClosedEnum<CardSubType>(
        obj.value("cardSubType"_L1), Json::joinPath(path, u"cardSubType"),
        kCardSubTypeTable);
    if (!r)
      return failure(r.error());
    cardSubType = *r;
  }

  auto classSymbols = decodeEnumArray(
      obj, "classSymbols"_L1, Json::joinPath(path, u"classSymbols"),
      kClassSymbolTable, /*requireUnique=*/true);
  if (!classSymbols)
    return failure(classSymbols.error());

  auto skills =
      decodeSkillIconArray(obj, "skills"_L1, Json::joinPath(path, u"skills"));
  if (!skills)
    return failure(skills.error());

  auto cardTraits =
      decodeStringSet(obj, "cardTraits"_L1, Json::joinPath(path, u"cardTraits"),
                      /*requireUnique=*/true);
  if (!cardTraits)
    return failure(cardTraits.error());

  auto revealedCardTraits = decodeStringSet(
      obj, "revealedCardTraits"_L1, Json::joinPath(path, u"revealedCardTraits"),
      /*requireUnique=*/true);
  if (!revealedCardTraits)
    return failure(revealedCardTraits.error());

  std::optional<Revelation> revelation;
  if (Json::fieldPresence(obj, "revelation"_L1) !=
      Json::FieldPresence::Absent) {
    auto r = Json::decodeClosedEnum<Revelation>(
        obj.value("revelation"_L1), Json::joinPath(path, u"revelation"),
        kRevelationTable);
    if (!r)
      return failure(r.error());
    revelation = *r;
  }

  auto victoryPoints = Json::optionalNonNullInt(
      obj, "victoryPoints"_L1, Json::joinPath(path, u"victoryPoints"));
  if (!victoryPoints)
    return failure(victoryPoints.error());
  auto vengeancePoints = Json::optionalNonNullInt(
      obj, "vengeancePoints"_L1, Json::joinPath(path, u"vengeancePoints"));
  if (!vengeancePoints)
    return failure(vengeancePoints.error());
  auto overrideActionPlayableIfCriteriaMet = Json::optionalNonNullBool(
      obj, "overrideActionPlayableIfCriteriaMet"_L1,
      Json::joinPath(path, u"overrideActionPlayableIfCriteriaMet"));
  if (!overrideActionPlayableIfCriteriaMet)
    return failure(overrideActionPlayableIfCriteriaMet.error());
  auto permanent = Json::optionalNonNullBool(
      obj, "permanent"_L1, Json::joinPath(path, u"permanent"));
  if (!permanent)
    return failure(permanent.error());
  auto encounterSet = Json::optionalNonNullString(
      obj, "encounterSet"_L1, Json::joinPath(path, u"encounterSet"));
  if (!encounterSet)
    return failure(encounterSet.error());
  auto encounterSetQuantity =
      Json::optionalNonNullInt(obj, "encounterSetQuantity"_L1,
                               Json::joinPath(path, u"encounterSetQuantity"));
  if (!encounterSetQuantity)
    return failure(encounterSetQuantity.error());
  auto unique = Json::optionalNonNullBool(obj, "unique"_L1,
                                          Json::joinPath(path, u"unique"));
  if (!unique)
    return failure(unique.error());
  auto doubleSided = Json::optionalNonNullBool(
      obj, "doubleSided"_L1, Json::joinPath(path, u"doubleSided"));
  if (!doubleSided)
    return failure(doubleSided.error());
  auto exceptional = Json::optionalNonNullBool(
      obj, "exceptional"_L1, Json::joinPath(path, u"exceptional"));
  if (!exceptional)
    return failure(exceptional.error());
  auto playableFromDiscard =
      Json::optionalNonNullBool(obj, "playableFromDiscard"_L1,
                                Json::joinPath(path, u"playableFromDiscard"));
  if (!playableFromDiscard)
    return failure(playableFromDiscard.error());
  auto stage =
      Json::optionalNonNullInt(obj, "stage"_L1, Json::joinPath(path, u"stage"));
  if (!stage)
    return failure(stage.error());

  auto cardSlots = decodeEnumArray(
      obj, "slots"_L1, Json::joinPath(path, u"slots"), kSlotTypeTable);
  if (!cardSlots)
    return failure(cardSlots.error());

  auto alternateCardCodes =
      decodeCardCodeArray(obj, "alternateCardCodes"_L1,
                          Json::joinPath(path, u"alternateCardCodes"));
  if (!alternateCardCodes)
    return failure(alternateCardCodes.error());

  auto grantedXp = Json::optionalNonNullInt(obj, "grantedXp"_L1,
                                            Json::joinPath(path, u"grantedXp"));
  if (!grantedXp)
    return failure(grantedXp.error());
  auto canReplace = Json::optionalNonNullBool(
      obj, "canReplace"_L1, Json::joinPath(path, u"canReplace"));
  if (!canReplace)
    return failure(canReplace.error());

  auto bondedWith = decodeBondedWith(obj, Json::joinPath(path, u"bondedWith"));
  if (!bondedWith)
    return failure(bondedWith.error());

  auto skipPlayWindows = Json::optionalNonNullBool(
      obj, "skipPlayWindows"_L1, Json::joinPath(path, u"skipPlayWindows"));
  if (!skipPlayWindows)
    return failure(skipPlayWindows.error());
  auto beforeEffect = Json::optionalNonNullBool(
      obj, "beforeEffect"_L1, Json::joinPath(path, u"beforeEffect"));
  if (!beforeEffect)
    return failure(beforeEffect.error());

  std::optional<CardCode> otherSide;
  if (Json::fieldPresence(obj, "otherSide"_L1) != Json::FieldPresence::Absent) {
    auto r = decodeCardCodeValue(obj.value("otherSide"_L1),
                                 Json::joinPath(path, u"otherSide"));
    if (!r)
      return failure(r.error());
    otherSide = *r;
  }

  std::optional<WhenDiscarded> whenDiscarded;
  if (Json::fieldPresence(obj, "whenDiscarded"_L1) !=
      Json::FieldPresence::Absent) {
    auto r = Json::decodeClosedEnum<WhenDiscarded>(
        obj.value("whenDiscarded"_L1), Json::joinPath(path, u"whenDiscarded"),
        kWhenDiscardedTable);
    if (!r)
      return failure(r.error());
    whenDiscarded = *r;
  }

  auto canCommitWhenNoIcons =
      Json::optionalNonNullBool(obj, "canCommitWhenNoIcons"_L1,
                                Json::joinPath(path, u"canCommitWhenNoIcons"));
  if (!canCommitWhenNoIcons)
    return failure(canCommitWhenNoIcons.error());
  auto commitTrigger = Json::optionalNonNullBool(
      obj, "commitTrigger"_L1, Json::joinPath(path, u"commitTrigger"));
  if (!commitTrigger)
    return failure(commitTrigger.error());

  auto tags = decodeStringSet(obj, "tags"_L1, Json::joinPath(path, u"tags"));
  if (!tags)
    return failure(tags.error());

  auto outOfPlayEffects = decodeEnumArray(
      obj, "outOfPlayEffects"_L1, Json::joinPath(path, u"outOfPlayEffects"),
      kOutOfPlayEffectTable);
  if (!outOfPlayEffects)
    return failure(outOfPlayEffects.error());

  std::optional<GameValue> health;
  if (Json::fieldPresence(obj, "health"_L1) != Json::FieldPresence::Absent) {
    auto r = decodeGameValueValue(obj.value("health"_L1),
                                  Json::joinPath(path, u"health"));
    if (!r)
      return failure(r.error());
    health = *r;
  }
  std::optional<GameValue> fight;
  if (Json::fieldPresence(obj, "fight"_L1) != Json::FieldPresence::Absent) {
    auto r = decodeGameValueValue(obj.value("fight"_L1),
                                  Json::joinPath(path, u"fight"));
    if (!r)
      return failure(r.error());
    fight = *r;
  }
  std::optional<GameValue> evade;
  if (Json::fieldPresence(obj, "evade"_L1) != Json::FieldPresence::Absent) {
    auto r = decodeGameValueValue(obj.value("evade"_L1),
                                  Json::joinPath(path, u"evade"));
    if (!r)
      return failure(r.error());
    evade = *r;
  }
  std::optional<GameValue> healthDamage;
  if (Json::fieldPresence(obj, "healthDamage"_L1) !=
      Json::FieldPresence::Absent) {
    auto r = decodeGameValueValue(obj.value("healthDamage"_L1),
                                  Json::joinPath(path, u"healthDamage"));
    if (!r)
      return failure(r.error());
    healthDamage = *r;
  }
  std::optional<GameValue> sanityDamage;
  if (Json::fieldPresence(obj, "sanityDamage"_L1) !=
      Json::FieldPresence::Absent) {
    auto r = decodeGameValueValue(obj.value("sanityDamage"_L1),
                                  Json::joinPath(path, u"sanityDamage"));
    if (!r)
      return failure(r.error());
    sanityDamage = *r;
  }

  auto alternateSkills =
      decodeAlternateSkills(obj, Json::joinPath(path, u"alternateSkills"));
  if (!alternateSkills)
    return failure(alternateSkills.error());
  auto alternateErrata =
      decodeAlternateErrata(obj, Json::joinPath(path, u"alternateErrata"));
  if (!alternateErrata)
    return failure(alternateErrata.error());
  auto errata = Json::optionalNonNullString(obj, "errata"_L1,
                                            Json::joinPath(path, u"errata"));
  if (!errata)
    return failure(errata.error());

  // Schema-unconstrained fields ("{}"): each is converted to the lossless
  // Json::Value AST individually (see toLosslessRaw()'s doc comment
  // above) rather than the whole `obj` being converted at once, so a
  // malformed/unrepresentable numeric subtree in exactly one field fails
  // with that field's own path rather than corrupting or discarding its
  // siblings.
  auto additionalCost = toLosslessRaw(obj.value("additionalCost"_L1));
  if (!additionalCost)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"additionalCost"), additionalCost.error()));
  auto fastWindow = toLosslessRaw(obj.value("fastWindow"_L1));
  if (!fastWindow)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"fastWindow"), fastWindow.error()));
  auto actions = toLosslessRaw(obj.value("actions"_L1));
  if (!actions)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"actions"), actions.error()));
  auto criteria = toLosslessRaw(obj.value("criteria"_L1));
  if (!criteria)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"criteria"), criteria.error()));
  auto uses = toLosslessRaw(obj.value("uses"_L1));
  if (!uses)
    return failure(QStringLiteral("%1: %2").arg(Json::joinPath(path, u"uses"),
                                                uses.error()));
  auto locationSymbol = toLosslessRaw(obj.value("locationSymbol"_L1));
  if (!locationSymbol)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"locationSymbol"), locationSymbol.error()));
  auto locationRevealedSymbol =
      toLosslessRaw(obj.value("locationRevealedSymbol"_L1));
  if (!locationRevealedSymbol)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"locationRevealedSymbol"),
        locationRevealedSymbol.error()));
  auto purchaseTrauma = toLosslessRaw(obj.value("purchaseTrauma"_L1));
  if (!purchaseTrauma)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"purchaseTrauma"), purchaseTrauma.error()));
  auto customizations = toLosslessRaw(obj.value("customizations"_L1));
  if (!customizations)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"customizations"), customizations.error()));

  // These 7 fields are schema-typed "array" (not fully unconstrained
  // "{}"): their outer JSON shape is validated -- rejecting a present
  // non-array value, including an explicit null, which matches neither --
  // but their element contents are otherwise unconstrained and preserved
  // verbatim as a lossless Json::Value.
  auto keywords = Json::optionalRawArrayField(
      obj, "keywords"_L1, Json::joinPath(path, u"keywords"));
  if (!keywords)
    return failure(keywords.error());
  auto keywordsRaw = toLosslessRaw(*keywords);
  if (!keywordsRaw)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"keywords"), keywordsRaw.error()));
  auto commitRestrictions =
      Json::optionalRawArrayField(obj, "commitRestrictions"_L1,
                                  Json::joinPath(path, u"commitRestrictions"));
  if (!commitRestrictions)
    return failure(commitRestrictions.error());
  auto commitRestrictionsRaw = toLosslessRaw(*commitRestrictions);
  if (!commitRestrictionsRaw)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"commitRestrictions"),
        commitRestrictionsRaw.error()));
  auto attackOfOpportunityModifiers = Json::optionalRawArrayField(
      obj, "attackOfOpportunityModifiers"_L1,
      Json::joinPath(path, u"attackOfOpportunityModifiers"));
  if (!attackOfOpportunityModifiers)
    return failure(attackOfOpportunityModifiers.error());
  auto attackOfOpportunityModifiersRaw =
      toLosslessRaw(*attackOfOpportunityModifiers);
  if (!attackOfOpportunityModifiersRaw)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"attackOfOpportunityModifiers"),
        attackOfOpportunityModifiersRaw.error()));
  auto limits = Json::optionalRawArrayField(obj, "limits"_L1,
                                            Json::joinPath(path, u"limits"));
  if (!limits)
    return failure(limits.error());
  auto limitsRaw = toLosslessRaw(*limits);
  if (!limitsRaw)
    return failure(QStringLiteral("%1: %2").arg(Json::joinPath(path, u"limits"),
                                                limitsRaw.error()));
  auto locationConnections =
      Json::optionalRawArrayField(obj, "locationConnections"_L1,
                                  Json::joinPath(path, u"locationConnections"));
  if (!locationConnections)
    return failure(locationConnections.error());
  auto locationConnectionsRaw = toLosslessRaw(*locationConnections);
  if (!locationConnectionsRaw)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"locationConnections"),
        locationConnectionsRaw.error()));
  auto locationRevealedConnections = Json::optionalRawArrayField(
      obj, "locationRevealedConnections"_L1,
      Json::joinPath(path, u"locationRevealedConnections"));
  if (!locationRevealedConnections)
    return failure(locationRevealedConnections.error());
  auto locationRevealedConnectionsRaw =
      toLosslessRaw(*locationRevealedConnections);
  if (!locationRevealedConnectionsRaw)
    return failure(QStringLiteral("%1: %2").arg(
        Json::joinPath(path, u"locationRevealedConnections"),
        locationRevealedConnectionsRaw.error()));
  auto deckRestrictions = Json::optionalRawArrayField(
      obj, "deckRestrictions"_L1, Json::joinPath(path, u"deckRestrictions"));
  if (!deckRestrictions)
    return failure(deckRestrictions.error());
  auto deckRestrictionsRaw = toLosslessRaw(*deckRestrictions);
  if (!deckRestrictionsRaw)
    return failure(
        QStringLiteral("%1: %2").arg(Json::joinPath(path, u"deckRestrictions"),
                                     deckRestrictionsRaw.error()));
  // meta is schema-typed "object": same outer-shape validation, applied to
  // an object rather than an array.
  auto meta = Json::optionalRawObjectField(obj, "meta"_L1,
                                           Json::joinPath(path, u"meta"));
  if (!meta)
    return failure(meta.error());
  auto metaRaw = toLosslessRaw(*meta);
  if (!metaRaw)
    return failure(QStringLiteral("%1: %2").arg(Json::joinPath(path, u"meta"),
                                                metaRaw.error()));

  return CardDef{
      .cardCode = *cardCode,
      .name = *name,
      .cardType = *cardType,
      .art = *art,
      .revealedName = revealedName,
      .cost = cost,
      .level = *level,
      .cardSubType = cardSubType,
      .classSymbols = *classSymbols,
      .skills = *skills,
      .cardTraits = *cardTraits,
      .revealedCardTraits = *revealedCardTraits,
      .revelation = revelation,
      .victoryPoints = *victoryPoints,
      .vengeancePoints = *vengeancePoints,
      .overrideActionPlayableIfCriteriaMet =
          *overrideActionPlayableIfCriteriaMet,
      .permanent = *permanent,
      .encounterSet = *encounterSet,
      .encounterSetQuantity = *encounterSetQuantity,
      .unique = *unique,
      .doubleSided = *doubleSided,
      .exceptional = *exceptional,
      .playableFromDiscard = *playableFromDiscard,
      .stage = *stage,
      .cardSlots = *cardSlots,
      .alternateCardCodes = *alternateCardCodes,
      .grantedXp = *grantedXp,
      .canReplace = *canReplace,
      .bondedWith = *bondedWith,
      .skipPlayWindows = *skipPlayWindows,
      .beforeEffect = *beforeEffect,
      .otherSide = otherSide,
      .whenDiscarded = whenDiscarded,
      .canCommitWhenNoIcons = *canCommitWhenNoIcons,
      .commitTrigger = *commitTrigger,
      .tags = *tags,
      .outOfPlayEffects = *outOfPlayEffects,
      .health = health,
      .fight = fight,
      .evade = evade,
      .healthDamage = healthDamage,
      .sanityDamage = sanityDamage,
      .alternateSkills = *alternateSkills,
      .alternateErrata = *alternateErrata,
      .errata = *errata,
      .additionalCost = *additionalCost,
      .fastWindow = *fastWindow,
      .actions = *actions,
      .criteria = *criteria,
      .uses = *uses,
      .locationSymbol = *locationSymbol,
      .locationRevealedSymbol = *locationRevealedSymbol,
      .purchaseTrauma = *purchaseTrauma,
      .customizations = *customizations,
      .keywords = *keywordsRaw,
      .commitRestrictions = *commitRestrictionsRaw,
      .attackOfOpportunityModifiers = *attackOfOpportunityModifiersRaw,
      .limits = *limitsRaw,
      .locationConnections = *locationConnectionsRaw,
      .locationRevealedConnections = *locationRevealedConnectionsRaw,
      .deckRestrictions = *deckRestrictionsRaw,
      .meta = *metaRaw,
  };
}

} // namespace

ValueOrError<CardDef> CardDef::fromJson(const QJsonValue &v, QStringView path) {
  return decodeCardDef(v, path);
}

ValueOrError<CardDef> CardDef::fromRawJson(const Json::Value &v,
                                           QStringView path) {
  return decodeCardDef(v, path);
}

ValueOrError<CardDef> CardDef::fromRawBytes(QByteArrayView bytes,
                                            QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return decodeCardDef(*parsed, path);
}

ValueOrError<QList<CardDef>> decodeCatalogFromRawBytes(QByteArrayView bytes,
                                                       QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  if (!parsed->isArray())
    return failure(QStringLiteral("%1: expected array, got %2")
                       .arg(path, Json::typeName(*parsed)));
  const QList<Json::Value> &elements = parsed->toArray();
  QList<CardDef> result;
  result.reserve(elements.size());
  for (qsizetype i = 0; i < elements.size(); ++i) {
    auto item = decodeCardDef(elements[i], Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    result.append(*item);
  }
  return result;
}

ValueOrError<Json::Value> CardDef::toRawJson() const {
  QList<std::pair<QString, Json::Value>> members;
  const auto insert = [&members](QLatin1StringView key, Json::Value value) {
    members.append({QString(key), std::move(value)});
  };
  insert("cardCode"_L1, Json::Value::makeString(cardCode.value()));
  insert("name"_L1, name.toRawJson());
  auto cardTypeEncoded = Json::encodeClosedEnum(cardType, kCardTypeTable);
  if (!cardTypeEncoded)
    return failure(QStringLiteral("cardType: %1").arg(cardTypeEncoded.error()));
  insert("cardType"_L1, Json::Value::makeString(*cardTypeEncoded));
  insert("art"_L1, Json::Value::makeString(art));

  if (revealedName)
    insert("revealedName"_L1, revealedName->toRawJson());
  if (cost)
    insert("cost"_L1, cost->toRawJson());
  if (level)
    insert("level"_L1,
           Json::Value::makeNumber(Json::RawNumber::fromInt64(*level)));
  if (cardSubType) {
    auto encoded = Json::encodeClosedEnum(*cardSubType, kCardSubTypeTable);
    if (!encoded)
      return failure(QStringLiteral("cardSubType: %1").arg(encoded.error()));
    insert("cardSubType"_L1, Json::Value::makeString(*encoded));
  }
  if (!classSymbols.isEmpty()) {
    auto encoded = encodeEnumArrayRaw(classSymbols, kClassSymbolTable);
    if (!encoded)
      return failure(QStringLiteral("classSymbols: %1").arg(encoded.error()));
    insert("classSymbols"_L1, *encoded);
  }
  if (!skills.isEmpty()) {
    QList<Json::Value> arr;
    for (qsizetype i = 0; i < skills.size(); ++i) {
      auto encoded = skills.at(i).toRawJson();
      if (!encoded)
        return failure(
            QStringLiteral("skills[%1]: %2").arg(i).arg(encoded.error()));
      arr.append(*encoded);
    }
    insert("skills"_L1, Json::Value::makeArray(arr));
  }
  if (!cardTraits.isEmpty()) {
    QList<Json::Value> arr;
    for (const QString &trait : cardTraits)
      arr.append(Json::Value::makeString(trait));
    insert("cardTraits"_L1, Json::Value::makeArray(arr));
  }
  if (!revealedCardTraits.isEmpty()) {
    QList<Json::Value> arr;
    for (const QString &trait : revealedCardTraits)
      arr.append(Json::Value::makeString(trait));
    insert("revealedCardTraits"_L1, Json::Value::makeArray(arr));
  }
  if (revelation) {
    auto encoded = Json::encodeClosedEnum(*revelation, kRevelationTable);
    if (!encoded)
      return failure(QStringLiteral("revelation: %1").arg(encoded.error()));
    insert("revelation"_L1, Json::Value::makeString(*encoded));
  }
  if (victoryPoints)
    insert("victoryPoints"_L1,
           Json::Value::makeNumber(Json::RawNumber::fromInt64(*victoryPoints)));
  if (vengeancePoints)
    insert(
        "vengeancePoints"_L1,
        Json::Value::makeNumber(Json::RawNumber::fromInt64(*vengeancePoints)));
  if (overrideActionPlayableIfCriteriaMet)
    insert("overrideActionPlayableIfCriteriaMet"_L1,
           Json::Value::makeBool(*overrideActionPlayableIfCriteriaMet));
  if (permanent)
    insert("permanent"_L1, Json::Value::makeBool(*permanent));
  if (encounterSet)
    insert("encounterSet"_L1, Json::Value::makeString(*encounterSet));
  if (encounterSetQuantity)
    insert("encounterSetQuantity"_L1,
           Json::Value::makeNumber(
               Json::RawNumber::fromInt64(*encounterSetQuantity)));
  if (unique)
    insert("unique"_L1, Json::Value::makeBool(*unique));
  if (doubleSided)
    insert("doubleSided"_L1, Json::Value::makeBool(*doubleSided));
  if (exceptional)
    insert("exceptional"_L1, Json::Value::makeBool(*exceptional));
  if (playableFromDiscard)
    insert("playableFromDiscard"_L1,
           Json::Value::makeBool(*playableFromDiscard));
  if (stage)
    insert("stage"_L1,
           Json::Value::makeNumber(Json::RawNumber::fromInt64(*stage)));
  if (!cardSlots.isEmpty()) {
    auto encoded = encodeEnumArrayRaw(cardSlots, kSlotTypeTable);
    if (!encoded)
      return failure(QStringLiteral("slots: %1").arg(encoded.error()));
    insert("slots"_L1, *encoded);
  }
  if (!alternateCardCodes.isEmpty()) {
    QList<Json::Value> arr;
    for (const CardCode &code : alternateCardCodes)
      arr.append(Json::Value::makeString(code.value()));
    insert("alternateCardCodes"_L1, Json::Value::makeArray(arr));
  }
  if (grantedXp)
    insert("grantedXp"_L1,
           Json::Value::makeNumber(Json::RawNumber::fromInt64(*grantedXp)));
  if (canReplace)
    insert("canReplace"_L1, Json::Value::makeBool(*canReplace));
  if (!bondedWith.isEmpty()) {
    QList<Json::Value> arr;
    for (const auto &[count, code] : bondedWith)
      arr.append(Json::Value::makeArray(
          {Json::Value::makeNumber(Json::RawNumber::fromInt64(count)),
           Json::Value::makeString(code.value())}));
    insert("bondedWith"_L1, Json::Value::makeArray(arr));
  }
  if (skipPlayWindows)
    insert("skipPlayWindows"_L1, Json::Value::makeBool(*skipPlayWindows));
  if (beforeEffect)
    insert("beforeEffect"_L1, Json::Value::makeBool(*beforeEffect));
  if (otherSide)
    insert("otherSide"_L1, Json::Value::makeString(otherSide->value()));
  if (whenDiscarded) {
    auto encoded = Json::encodeClosedEnum(*whenDiscarded, kWhenDiscardedTable);
    if (!encoded)
      return failure(QStringLiteral("whenDiscarded: %1").arg(encoded.error()));
    insert("whenDiscarded"_L1, Json::Value::makeString(*encoded));
  }
  if (canCommitWhenNoIcons)
    insert("canCommitWhenNoIcons"_L1,
           Json::Value::makeBool(*canCommitWhenNoIcons));
  if (commitTrigger)
    insert("commitTrigger"_L1, Json::Value::makeBool(*commitTrigger));
  if (!tags.isEmpty()) {
    QList<Json::Value> arr;
    for (const QString &tag : tags)
      arr.append(Json::Value::makeString(tag));
    insert("tags"_L1, Json::Value::makeArray(arr));
  }
  if (!outOfPlayEffects.isEmpty()) {
    auto encoded = encodeEnumArrayRaw(outOfPlayEffects, kOutOfPlayEffectTable);
    if (!encoded)
      return failure(
          QStringLiteral("outOfPlayEffects: %1").arg(encoded.error()));
    insert("outOfPlayEffects"_L1, *encoded);
  }
  if (health)
    insert("health"_L1, health->toRawJson());
  if (fight)
    insert("fight"_L1, fight->toRawJson());
  if (evade)
    insert("evade"_L1, evade->toRawJson());
  if (healthDamage)
    insert("healthDamage"_L1, healthDamage->toRawJson());
  if (sanityDamage)
    insert("sanityDamage"_L1, sanityDamage->toRawJson());
  if (!alternateSkills.isEmpty()) {
    QList<std::pair<QString, Json::Value>> alternateSkillsMembers;
    for (auto it = alternateSkills.constBegin();
         it != alternateSkills.constEnd(); ++it) {
      QList<Json::Value> arr;
      for (qsizetype i = 0; i < it.value().size(); ++i) {
        auto encoded = it.value().at(i).toRawJson();
        if (!encoded)
          return failure(QStringLiteral("alternateSkills[%1][%2]: %3")
                             .arg(it.key())
                             .arg(i)
                             .arg(encoded.error()));
        arr.append(*encoded);
      }
      alternateSkillsMembers.append({it.key(), Json::Value::makeArray(arr)});
    }
    insert("alternateSkills"_L1,
           Json::Value::makeObject(alternateSkillsMembers));
  }
  if (!alternateErrata.isEmpty()) {
    QList<std::pair<QString, Json::Value>> alternateErrataMembers;
    for (auto it = alternateErrata.constBegin();
         it != alternateErrata.constEnd(); ++it)
      alternateErrataMembers.append(
          {it.key(), Json::Value::makeString(it.value())});
    insert("alternateErrata"_L1,
           Json::Value::makeObject(alternateErrataMembers));
  }
  if (errata)
    insert("errata"_L1, Json::Value::makeString(*errata));

  const auto insertRaw = [&insert](QLatin1StringView key,
                                   const Json::Value &raw) {
    if (!raw.isUndefined())
      insert(key, raw);
  };
  insertRaw("additionalCost"_L1, additionalCost);
  insertRaw("keywords"_L1, keywords);
  insertRaw("fastWindow"_L1, fastWindow);
  insertRaw("actions"_L1, actions);
  insertRaw("criteria"_L1, criteria);
  insertRaw("commitRestrictions"_L1, commitRestrictions);
  insertRaw("attackOfOpportunityModifiers"_L1, attackOfOpportunityModifiers);
  insertRaw("limits"_L1, limits);
  insertRaw("uses"_L1, uses);
  insertRaw("locationSymbol"_L1, locationSymbol);
  insertRaw("locationRevealedSymbol"_L1, locationRevealedSymbol);
  insertRaw("locationConnections"_L1, locationConnections);
  insertRaw("locationRevealedConnections"_L1, locationRevealedConnections);
  insertRaw("purchaseTrauma"_L1, purchaseTrauma);
  insertRaw("deckRestrictions"_L1, deckRestrictions);
  insertRaw("customizations"_L1, customizations);
  insertRaw("meta"_L1, meta);

  return Json::Value::makeObject(std::move(members));
}

ValueOrError<QByteArray> CardDef::toJsonBytes() const {
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toJsonBytes();
}

} // namespace Arkham
