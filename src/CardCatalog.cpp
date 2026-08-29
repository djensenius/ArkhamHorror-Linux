#include "CardCatalog.h"

#include "JsonDecode.h"

#include <QJsonArray>
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

template <typename Enum, std::size_t N>
QJsonArray encodeEnumArray(
    const QList<Enum> &values,
    const std::array<std::pair<QLatin1StringView, Enum>, N> &table) {
  QJsonArray result;
  for (const Enum value : values)
    result.append(Json::encodeClosedEnum(value, table));
  return result;
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
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    auto item =
        Json::requireStringValue((*arrResult)[i], Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    if (requireUnique && result.contains(*item))
      return failure(
          QStringLiteral("%1: duplicate value at index %2").arg(path).arg(i));
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

SkillIcon SkillIcon::skillType(SkillType type) {
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
  // no "contents" key at all, so an explicit contents value -- even an
  // explicit JSON null -- is malformed input, not a value to silently
  // discard.
  if (*tagResult == "WildIcon"_L1 || *tagResult == "WildMinusIcon"_L1) {
    if (Json::fieldPresence(obj, "contents"_L1) != Json::FieldPresence::Absent)
      return failure(QStringLiteral("%1: tag \"%2\" must not have a "
                                    "\"contents\" field")
                         .arg(path, *tagResult));
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

QJsonObject SkillIcon::toJson() const {
  switch (m_tag) {
  case SkillIconTag::SkillIcon:
    // m_skill is guaranteed populated here: the only way to construct a
    // SkillIcon with tag == SkillIcon is the skillType() factory, which
    // always sets it, and the private constructor/fromJson never leave it
    // unset for this tag -- no runtime guard is needed or appropriate.
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("SkillIcon")},
        {QStringLiteral("contents"),
         Json::encodeClosedEnum(*m_skill, kSkillTypeTable)},
    };
  case SkillIconTag::WildIcon:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("WildIcon")}};
  case SkillIconTag::WildMinusIcon:
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("WildMinusIcon")}};
  case SkillIconTag::Unknown:
    return m_unknownRaw.toQJson().toObject();
  }
  Q_UNREACHABLE_RETURN(QJsonObject{});
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
    auto amount =
        Json::requireInt(obj, "contents"_L1, Json::joinPath(path, u"contents"));
    if (!amount)
      return failure(amount.error());
    return CardCost::staticCost(*amount);
  }
  // DynamicCost/DiscardAmountCost/DeferredCost are documented nullary
  // tags: the schema allows no "contents" key, so an explicit contents
  // value -- even an explicit JSON null -- is malformed input.
  if (tag == "DynamicCost"_L1 || tag == "DiscardAmountCost"_L1 ||
      tag == "DeferredCost"_L1) {
    if (Json::fieldPresence(obj, "contents"_L1) != Json::FieldPresence::Absent)
      return failure(QStringLiteral("%1: tag \"%2\" must not have a "
                                    "\"contents\" field")
                         .arg(path, tag));
    if (tag == "DynamicCost"_L1)
      return CardCost::dynamicCost();
    if (tag == "DiscardAmountCost"_L1)
      return CardCost::discardAmountCost();
    return CardCost::deferredCost();
  }
  if (tag == "MaxDynamicCost"_L1 || tag == "AnyMatchingCardCost"_L1 ||
      tag == "MatchingEnemyFieldCost"_L1) {
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

QJsonObject CardCost::toJson() const {
  switch (m_tag) {
  case CardCostTag::StaticCost:
    // m_staticAmount is guaranteed populated for tag == StaticCost: the
    // only way to construct one is the staticCost() factory, which always
    // sets it.
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("StaticCost")},
                       {QStringLiteral("contents"), *m_staticAmount}};
  case CardCostTag::DynamicCost:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("DynamicCost")}};
  case CardCostTag::DiscardAmountCost:
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("DiscardAmountCost")}};
  case CardCostTag::DeferredCost:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("DeferredCost")}};
  case CardCostTag::MaxDynamicCost:
    // m_rawContents is guaranteed populated (non-Undefined) for the three
    // raw-payload tags below: their only factories (maxDynamicCost() etc.)
    // always set it, and fromJson requires "contents" to be present.
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("MaxDynamicCost")},
        {QStringLiteral("contents"), m_rawContents.toQJson()}};
  case CardCostTag::AnyMatchingCardCost:
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("AnyMatchingCardCost")},
        {QStringLiteral("contents"), m_rawContents.toQJson()}};
  case CardCostTag::MatchingEnemyFieldCost:
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("MatchingEnemyFieldCost")},
        {QStringLiteral("contents"), m_rawContents.toQJson()}};
  case CardCostTag::Unknown:
    return m_unknownRaw.toQJson().toObject();
  }
  Q_UNREACHABLE_RETURN(QJsonObject{});
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

GameValue GameValue::byPlayerCount(qint64 oneOrTwo, qint64 three, qint64 four,
                                   qint64 fiveOrMore) {
  GameValue result;
  result.m_tag = GameValueTag::ByPlayerCount;
  result.m_contents = {oneOrTwo, three, four, fiveOrMore};
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
    auto amount = Json::requireInt(obj, "contents"_L1, contentsPath);
    if (!amount)
      return failure(amount.error());
    return tag == "Static"_L1 ? GameValue::staticValue(*amount)
                              : GameValue::perPlayer(*amount);
  }
  if (tag == "StaticWithPerPlayer"_L1 || tag == "ByPlayerCount"_L1) {
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
  // allows no "contents" key, so an explicit contents value -- even an
  // explicit JSON null -- is malformed input.
  if (tag == "ValueX"_L1 || tag == "ValueStar"_L1 || tag == "ValueUnknown"_L1) {
    if (Json::fieldPresence(obj, "contents"_L1) != Json::FieldPresence::Absent)
      return failure(QStringLiteral("%1: tag \"%2\" must not have a "
                                    "\"contents\" field")
                         .arg(path, tag));
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

QJsonObject GameValue::toJson() const {
  auto withContents = [](QLatin1StringView wireTag,
                         const QJsonValue &contentsVal) {
    return QJsonObject{{QStringLiteral("tag"), QString(wireTag)},
                       {QStringLiteral("contents"), contentsVal}};
  };
  switch (m_tag) {
  case GameValueTag::Static:
    // m_singleAmount is guaranteed populated for Static/PerPlayer: the
    // only way to construct one is staticValue()/perPlayer(), which
    // always set it.
    return withContents("Static"_L1, *m_singleAmount);
  case GameValueTag::PerPlayer:
    return withContents("PerPlayer"_L1, *m_singleAmount);
  case GameValueTag::StaticWithPerPlayer: {
    // m_contents is guaranteed to hold exactly 2 elements here: the only
    // way to construct a StaticWithPerPlayer GameValue is
    // staticWithPerPlayer(qint64, qint64), which always sets exactly 2.
    QJsonArray arr;
    for (const qint64 n : m_contents)
      arr.append(n);
    return withContents("StaticWithPerPlayer"_L1, arr);
  }
  case GameValueTag::ByPlayerCount: {
    // Likewise guaranteed to hold exactly 4 elements via byPlayerCount().
    QJsonArray arr;
    for (const qint64 n : m_contents)
      arr.append(n);
    return withContents("ByPlayerCount"_L1, arr);
  }
  case GameValueTag::ValueX:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("ValueX")}};
  case GameValueTag::ValueStar:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("ValueStar")}};
  case GameValueTag::ValueUnknown:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("ValueUnknown")}};
  case GameValueTag::Unknown:
    return m_unknownRaw.toQJson().toObject();
  }
  Q_UNREACHABLE_RETURN(QJsonObject{});
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

QJsonObject CardDef::toJson() const {
  QJsonObject obj;
  obj.insert(QStringLiteral("cardCode"), cardCode.toJson());
  obj.insert(QStringLiteral("name"), name.toJson());
  obj.insert(QStringLiteral("cardType"),
             Json::encodeClosedEnum(cardType, kCardTypeTable));
  obj.insert(QStringLiteral("art"), art);

  if (revealedName)
    obj.insert(QStringLiteral("revealedName"), revealedName->toJson());
  if (cost)
    obj.insert(QStringLiteral("cost"), cost->toJson());
  if (level)
    obj.insert(QStringLiteral("level"), *level);
  if (cardSubType)
    obj.insert(QStringLiteral("cardSubType"),
               Json::encodeClosedEnum(*cardSubType, kCardSubTypeTable));
  if (!classSymbols.isEmpty())
    obj.insert(QStringLiteral("classSymbols"),
               encodeEnumArray(classSymbols, kClassSymbolTable));
  if (!skills.isEmpty()) {
    QJsonArray arr;
    for (const SkillIcon &icon : skills)
      arr.append(icon.toJson());
    obj.insert(QStringLiteral("skills"), arr);
  }
  if (!cardTraits.isEmpty())
    obj.insert(QStringLiteral("cardTraits"),
               QJsonArray::fromStringList(cardTraits));
  if (!revealedCardTraits.isEmpty())
    obj.insert(QStringLiteral("revealedCardTraits"),
               QJsonArray::fromStringList(revealedCardTraits));
  if (revelation)
    obj.insert(QStringLiteral("revelation"),
               Json::encodeClosedEnum(*revelation, kRevelationTable));
  if (victoryPoints)
    obj.insert(QStringLiteral("victoryPoints"), *victoryPoints);
  if (vengeancePoints)
    obj.insert(QStringLiteral("vengeancePoints"), *vengeancePoints);
  if (overrideActionPlayableIfCriteriaMet)
    obj.insert(QStringLiteral("overrideActionPlayableIfCriteriaMet"),
               *overrideActionPlayableIfCriteriaMet);
  if (permanent)
    obj.insert(QStringLiteral("permanent"), *permanent);
  if (encounterSet)
    obj.insert(QStringLiteral("encounterSet"), *encounterSet);
  if (encounterSetQuantity)
    obj.insert(QStringLiteral("encounterSetQuantity"), *encounterSetQuantity);
  if (unique)
    obj.insert(QStringLiteral("unique"), *unique);
  if (doubleSided)
    obj.insert(QStringLiteral("doubleSided"), *doubleSided);
  if (exceptional)
    obj.insert(QStringLiteral("exceptional"), *exceptional);
  if (playableFromDiscard)
    obj.insert(QStringLiteral("playableFromDiscard"), *playableFromDiscard);
  if (stage)
    obj.insert(QStringLiteral("stage"), *stage);
  if (!cardSlots.isEmpty())
    obj.insert(QStringLiteral("slots"),
               encodeEnumArray(cardSlots, kSlotTypeTable));
  if (!alternateCardCodes.isEmpty()) {
    QJsonArray arr;
    for (const CardCode &code : alternateCardCodes)
      arr.append(code.toJson());
    obj.insert(QStringLiteral("alternateCardCodes"), arr);
  }
  if (grantedXp)
    obj.insert(QStringLiteral("grantedXp"), *grantedXp);
  if (canReplace)
    obj.insert(QStringLiteral("canReplace"), *canReplace);
  if (!bondedWith.isEmpty()) {
    QJsonArray arr;
    for (const auto &[count, code] : bondedWith)
      arr.append(QJsonArray{count, code.toJson()});
    obj.insert(QStringLiteral("bondedWith"), arr);
  }
  if (skipPlayWindows)
    obj.insert(QStringLiteral("skipPlayWindows"), *skipPlayWindows);
  if (beforeEffect)
    obj.insert(QStringLiteral("beforeEffect"), *beforeEffect);
  if (otherSide)
    obj.insert(QStringLiteral("otherSide"), otherSide->toJson());
  if (whenDiscarded)
    obj.insert(QStringLiteral("whenDiscarded"),
               Json::encodeClosedEnum(*whenDiscarded, kWhenDiscardedTable));
  if (canCommitWhenNoIcons)
    obj.insert(QStringLiteral("canCommitWhenNoIcons"), *canCommitWhenNoIcons);
  if (commitTrigger)
    obj.insert(QStringLiteral("commitTrigger"), *commitTrigger);
  if (!tags.isEmpty())
    obj.insert(QStringLiteral("tags"), QJsonArray::fromStringList(tags));
  if (!outOfPlayEffects.isEmpty())
    obj.insert(QStringLiteral("outOfPlayEffects"),
               encodeEnumArray(outOfPlayEffects, kOutOfPlayEffectTable));
  if (health)
    obj.insert(QStringLiteral("health"), health->toJson());
  if (fight)
    obj.insert(QStringLiteral("fight"), fight->toJson());
  if (evade)
    obj.insert(QStringLiteral("evade"), evade->toJson());
  if (healthDamage)
    obj.insert(QStringLiteral("healthDamage"), healthDamage->toJson());
  if (sanityDamage)
    obj.insert(QStringLiteral("sanityDamage"), sanityDamage->toJson());
  if (!alternateSkills.isEmpty()) {
    QJsonObject alternateSkillsObj;
    for (auto it = alternateSkills.constBegin();
         it != alternateSkills.constEnd(); ++it) {
      QJsonArray arr;
      for (const SkillIcon &icon : it.value())
        arr.append(icon.toJson());
      alternateSkillsObj.insert(it.key(), arr);
    }
    obj.insert(QStringLiteral("alternateSkills"), alternateSkillsObj);
  }
  if (!alternateErrata.isEmpty()) {
    QJsonObject alternateErrataObj;
    for (auto it = alternateErrata.constBegin();
         it != alternateErrata.constEnd(); ++it)
      alternateErrataObj.insert(it.key(), it.value());
    obj.insert(QStringLiteral("alternateErrata"), alternateErrataObj);
  }
  if (errata)
    obj.insert(QStringLiteral("errata"), *errata);

  const auto insertRaw = [&obj](QLatin1StringView key, const Json::Value &raw) {
    if (!raw.isUndefined())
      obj.insert(key, raw.toQJson());
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

  return obj;
}

} // namespace Arkham
