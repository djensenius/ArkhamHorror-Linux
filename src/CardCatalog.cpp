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

// Decodes an optional array-of-closed-enum field: an absent key decodes to
// an empty list; a present non-array value, or an unrecognized element,
// fails.
template <typename Enum, std::size_t N>
ValueOrError<QList<Enum>> decodeEnumArray(
    const QJsonObject &obj, QLatin1StringView key, QStringView path,
    const std::array<std::pair<QLatin1StringView, Enum>, N> &table) {
  const QJsonValue v = obj.value(key);
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

// Decodes an optional `stringSet` field (cardTraits/revealedCardTraits/
// tags): an absent key decodes to an empty list.
ValueOrError<QStringList> decodeStringSet(const QJsonObject &obj,
                                          QLatin1StringView key,
                                          QStringView path) {
  const QJsonValue v = obj.value(key);
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
    result.append(*item);
  }
  return result;
}

ValueOrError<QList<CardCode>> decodeCardCodeArray(const QJsonObject &obj,
                                                  QLatin1StringView key,
                                                  QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined())
    return QList<CardCode>{};
  auto arrResult = Json::requireArray(v, path);
  if (!arrResult)
    return failure(arrResult.error());
  QList<CardCode> result;
  result.reserve(arrResult->size());
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    auto item = CardCode::fromJson((*arrResult)[i], Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    result.append(*item);
  }
  return result;
}

ValueOrError<QList<SkillIcon>> decodeSkillIconArray(const QJsonObject &obj,
                                                    QLatin1StringView key,
                                                    QStringView path) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined())
    return QList<SkillIcon>{};
  auto arrResult = Json::requireArray(v, path);
  if (!arrResult)
    return failure(arrResult.error());
  QList<SkillIcon> result;
  result.reserve(arrResult->size());
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    auto item = SkillIcon::fromJson((*arrResult)[i], Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    result.append(*item);
  }
  return result;
}

ValueOrError<QList<std::pair<int, CardCode>>>
decodeBondedWith(const QJsonObject &obj, QStringView path) {
  const QJsonValue v = obj.value("bondedWith"_L1);
  if (v.isUndefined())
    return QList<std::pair<int, CardCode>>{};
  auto arrResult = Json::requireArray(v, path);
  if (!arrResult)
    return failure(arrResult.error());
  QList<std::pair<int, CardCode>> result;
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
        CardCode::fromJson((*pairResult)[1], Json::indexPath(itemPath, 1));
    if (!codeResult)
      return failure(codeResult.error());
    result.append({*countResult, *codeResult});
  }
  return result;
}

ValueOrError<QMap<QString, QList<SkillIcon>>>
decodeAlternateSkills(const QJsonObject &obj, QStringView path) {
  const QJsonValue v = obj.value("alternateSkills"_L1);
  if (v.isUndefined())
    return QMap<QString, QList<SkillIcon>>{};
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  QMap<QString, QList<SkillIcon>> result;
  for (auto it = objResult->constBegin(); it != objResult->constEnd(); ++it) {
    const QString entryPath = Json::joinPath(path, it.key());
    auto arrResult = Json::requireArray(it.value(), entryPath);
    if (!arrResult)
      return failure(arrResult.error());
    QList<SkillIcon> icons;
    icons.reserve(arrResult->size());
    for (qsizetype i = 0; i < arrResult->size(); ++i) {
      auto icon =
          SkillIcon::fromJson((*arrResult)[i], Json::indexPath(entryPath, i));
      if (!icon)
        return failure(icon.error());
      icons.append(*icon);
    }
    result.insert(it.key(), icons);
  }
  return result;
}

ValueOrError<QMap<QString, QString>>
decodeAlternateErrata(const QJsonObject &obj, QStringView path) {
  const QJsonValue v = obj.value("alternateErrata"_L1);
  if (v.isUndefined())
    return QMap<QString, QString>{};
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  QMap<QString, QString> result;
  for (auto it = objResult->constBegin(); it != objResult->constEnd(); ++it) {
    auto strResult =
        Json::requireStringValue(it.value(), Json::joinPath(path, it.key()));
    if (!strResult)
      return failure(strResult.error());
    result.insert(it.key(), *strResult);
  }
  return result;
}

} // namespace

ValueOrError<SkillIcon> SkillIcon::fromJson(const QJsonValue &v,
                                            QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  auto tagResult =
      Json::requireString(obj, "tag"_L1, Json::joinPath(path, u"tag"));
  if (!tagResult)
    return failure(tagResult.error());

  if (*tagResult == "SkillIcon"_L1) {
    auto skillResult = Json::decodeClosedEnum<SkillType>(
        obj.value("contents"_L1), Json::joinPath(path, u"contents"),
        kSkillTypeTable);
    if (!skillResult)
      return failure(skillResult.error());
    return SkillIcon{.tag = SkillIconTag::SkillIcon, .skill = *skillResult};
  }
  if (*tagResult == "WildIcon"_L1)
    return SkillIcon{.tag = SkillIconTag::WildIcon, .skill = std::nullopt};
  if (*tagResult == "WildMinusIcon"_L1)
    return SkillIcon{.tag = SkillIconTag::WildMinusIcon, .skill = std::nullopt};
  return failure(QStringLiteral("%1.tag: unrecognized value \"%2\"")
                     .arg(path, *tagResult));
}

QJsonObject SkillIcon::toJson() const {
  switch (tag) {
  case SkillIconTag::SkillIcon: {
    // skill is documented as always populated when tag == SkillIcon, but
    // it is a public std::optional field with no constructor enforcing
    // that invariant. Q_ASSERT alone is not enough (it compiles out in
    // release/NDEBUG builds, leaving a bare optional dereference below --
    // UB), and substituting JSON null would produce a schema-invalid
    // "contents" (an enum string is always required here) while masking
    // the bug. qFatal() is never compiled out and halts with a clear
    // diagnostic instead of doing either.
    if (!skill)
      qFatal("SkillIcon::toJson: tag == SkillIcon but skill is unset; this "
             "is a construction bug, not a decode failure");
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("SkillIcon")},
        {QStringLiteral("contents"),
         Json::encodeClosedEnum(*skill, kSkillTypeTable)},
    };
  }
  case SkillIconTag::WildIcon:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("WildIcon")}};
  case SkillIconTag::WildMinusIcon:
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("WildMinusIcon")}};
  }
  Q_UNREACHABLE_RETURN(QJsonObject{});
}

ValueOrError<CardCost> CardCost::fromJson(const QJsonValue &v,
                                          QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

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
    return CardCost{.tag = CardCostTag::StaticCost, .staticAmount = *amount};
  }
  if (tag == "DynamicCost"_L1)
    return CardCost{.tag = CardCostTag::DynamicCost};
  if (tag == "DiscardAmountCost"_L1)
    return CardCost{.tag = CardCostTag::DiscardAmountCost};
  if (tag == "DeferredCost"_L1)
    return CardCost{.tag = CardCostTag::DeferredCost};
  if (tag == "MaxDynamicCost"_L1) {
    auto contents = Json::requireRawField(obj, "contents"_L1,
                                          Json::joinPath(path, u"contents"));
    if (!contents)
      return failure(contents.error());
    return CardCost{.tag = CardCostTag::MaxDynamicCost,
                    .rawContents = *contents};
  }
  if (tag == "AnyMatchingCardCost"_L1) {
    auto contents = Json::requireRawField(obj, "contents"_L1,
                                          Json::joinPath(path, u"contents"));
    if (!contents)
      return failure(contents.error());
    return CardCost{.tag = CardCostTag::AnyMatchingCardCost,
                    .rawContents = *contents};
  }
  if (tag == "MatchingEnemyFieldCost"_L1) {
    auto contents = Json::requireRawField(obj, "contents"_L1,
                                          Json::joinPath(path, u"contents"));
    if (!contents)
      return failure(contents.error());
    return CardCost{.tag = CardCostTag::MatchingEnemyFieldCost,
                    .rawContents = *contents};
  }
  return failure(
      QStringLiteral("%1.tag: unrecognized value \"%2\"").arg(path, tag));
}

QJsonObject CardCost::toJson() const {
  switch (tag) {
  case CardCostTag::StaticCost:
    // staticAmount is documented as always populated when tag ==
    // StaticCost, but it is a public std::optional field with no
    // constructor enforcing that invariant. Q_ASSERT alone is not enough
    // (it compiles out in release/NDEBUG builds, leaving a bare optional
    // dereference below -- UB), and substituting JSON null would produce a
    // schema-invalid "contents" (an integer is always required here) while
    // masking the bug. qFatal() is never compiled out and halts with a
    // clear diagnostic instead of doing either.
    if (!staticAmount)
      qFatal("CardCost::toJson: tag == StaticCost but staticAmount is "
             "unset; this is a construction bug, not a decode failure");
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("StaticCost")},
                       {QStringLiteral("contents"), *staticAmount}};
  case CardCostTag::DynamicCost:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("DynamicCost")}};
  case CardCostTag::DiscardAmountCost:
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("DiscardAmountCost")}};
  case CardCostTag::DeferredCost:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("DeferredCost")}};
  case CardCostTag::MaxDynamicCost:
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("MaxDynamicCost")},
        {QStringLiteral("contents"), rawContents}};
  case CardCostTag::AnyMatchingCardCost:
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("AnyMatchingCardCost")},
        {QStringLiteral("contents"), rawContents}};
  case CardCostTag::MatchingEnemyFieldCost:
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("MatchingEnemyFieldCost")},
        {QStringLiteral("contents"), rawContents}};
  }
  Q_UNREACHABLE_RETURN(QJsonObject{});
}

ValueOrError<GameValue> GameValue::fromJson(const QJsonValue &v,
                                            QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

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
    return GameValue{.tag = tag == "Static"_L1 ? GameValueTag::Static
                                               : GameValueTag::PerPlayer,
                     .singleAmount = *amount};
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
    QList<int> contents;
    contents.reserve(expected);
    for (qsizetype i = 0; i < arrResult->size(); ++i) {
      auto item = Json::requireIntValue((*arrResult)[i],
                                        Json::indexPath(contentsPath, i));
      if (!item)
        return failure(item.error());
      contents.append(*item);
    }
    return GameValue{.tag = tag == "StaticWithPerPlayer"_L1
                                ? GameValueTag::StaticWithPerPlayer
                                : GameValueTag::ByPlayerCount,
                     .contents = contents};
  }
  if (tag == "ValueX"_L1)
    return GameValue{.tag = GameValueTag::ValueX};
  if (tag == "ValueStar"_L1)
    return GameValue{.tag = GameValueTag::ValueStar};
  if (tag == "ValueUnknown"_L1)
    return GameValue{.tag = GameValueTag::ValueUnknown};
  return failure(
      QStringLiteral("%1.tag: unrecognized value \"%2\"").arg(path, tag));
}

QJsonObject GameValue::toJson() const {
  auto withContents = [this](QLatin1StringView wireTag,
                             const QJsonValue &contentsVal) {
    return QJsonObject{{QStringLiteral("tag"), QString(wireTag)},
                       {QStringLiteral("contents"), contentsVal}};
  };
  QJsonArray arr;
  for (const int n : contents)
    arr.append(n);
  switch (tag) {
  case GameValueTag::Static:
    // singleAmount is documented as always populated for Static/PerPlayer,
    // but it is a public std::optional field with no constructor enforcing
    // that invariant. Q_ASSERT alone is not enough (it compiles out in
    // release/NDEBUG builds, leaving a bare optional dereference below --
    // UB), and substituting JSON null would produce a schema-invalid
    // "contents" (an integer is always required here) while masking the
    // bug. qFatal() is never compiled out and halts with a clear
    // diagnostic instead of doing either.
    if (!singleAmount)
      qFatal("GameValue::toJson: tag == Static but singleAmount is unset; "
             "this is a construction bug, not a decode failure");
    return withContents("Static"_L1, *singleAmount);
  case GameValueTag::PerPlayer:
    if (!singleAmount)
      qFatal("GameValue::toJson: tag == PerPlayer but singleAmount is "
             "unset; this is a construction bug, not a decode failure");
    return withContents("PerPlayer"_L1, *singleAmount);
  case GameValueTag::StaticWithPerPlayer:
    return withContents("StaticWithPerPlayer"_L1, arr);
  case GameValueTag::ByPlayerCount:
    return withContents("ByPlayerCount"_L1, arr);
  case GameValueTag::ValueX:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("ValueX")}};
  case GameValueTag::ValueStar:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("ValueStar")}};
  case GameValueTag::ValueUnknown:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("ValueUnknown")}};
  }
  Q_UNREACHABLE_RETURN(QJsonObject{});
}

ValueOrError<CardDef> CardDef::fromJson(const QJsonValue &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  auto cardCode = CardCode::fromJson(obj.value("cardCode"_L1),
                                     Json::joinPath(path, u"cardCode"));
  if (!cardCode)
    return failure(cardCode.error());
  auto name =
      CardName::fromJson(obj.value("name"_L1), Json::joinPath(path, u"name"));
  if (!name)
    return failure(name.error());
  auto cardType = Json::decodeClosedEnum<CardType>(
      obj.value("cardType"_L1), Json::joinPath(path, u"cardType"),
      kCardTypeTable);
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
    auto r = CardName::fromJson(obj.value("revealedName"_L1),
                                Json::joinPath(path, u"revealedName"));
    if (!r)
      return failure(r.error());
    revealedName = *r;
  }

  std::optional<CardCost> cost;
  if (Json::fieldPresence(obj, "cost"_L1) != Json::FieldPresence::Absent) {
    auto r =
        CardCost::fromJson(obj.value("cost"_L1), Json::joinPath(path, u"cost"));
    if (!r)
      return failure(r.error());
    cost = *r;
  }

  auto level =
      Json::optionalInt(obj, "level"_L1, Json::joinPath(path, u"level"));
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

  auto classSymbols =
      decodeEnumArray(obj, "classSymbols"_L1,
                      Json::joinPath(path, u"classSymbols"), kClassSymbolTable);
  if (!classSymbols)
    return failure(classSymbols.error());

  auto skills =
      decodeSkillIconArray(obj, "skills"_L1, Json::joinPath(path, u"skills"));
  if (!skills)
    return failure(skills.error());

  auto cardTraits = decodeStringSet(obj, "cardTraits"_L1,
                                    Json::joinPath(path, u"cardTraits"));
  if (!cardTraits)
    return failure(cardTraits.error());

  auto revealedCardTraits =
      decodeStringSet(obj, "revealedCardTraits"_L1,
                      Json::joinPath(path, u"revealedCardTraits"));
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

  auto victoryPoints = Json::optionalInt(
      obj, "victoryPoints"_L1, Json::joinPath(path, u"victoryPoints"));
  if (!victoryPoints)
    return failure(victoryPoints.error());
  auto vengeancePoints = Json::optionalInt(
      obj, "vengeancePoints"_L1, Json::joinPath(path, u"vengeancePoints"));
  if (!vengeancePoints)
    return failure(vengeancePoints.error());
  auto overrideActionPlayableIfCriteriaMet = Json::optionalBool(
      obj, "overrideActionPlayableIfCriteriaMet"_L1,
      Json::joinPath(path, u"overrideActionPlayableIfCriteriaMet"));
  if (!overrideActionPlayableIfCriteriaMet)
    return failure(overrideActionPlayableIfCriteriaMet.error());
  auto permanent = Json::optionalBool(obj, "permanent"_L1,
                                      Json::joinPath(path, u"permanent"));
  if (!permanent)
    return failure(permanent.error());
  auto encounterSet = Json::optionalString(
      obj, "encounterSet"_L1, Json::joinPath(path, u"encounterSet"));
  if (!encounterSet)
    return failure(encounterSet.error());
  auto encounterSetQuantity =
      Json::optionalInt(obj, "encounterSetQuantity"_L1,
                        Json::joinPath(path, u"encounterSetQuantity"));
  if (!encounterSetQuantity)
    return failure(encounterSetQuantity.error());
  auto unique =
      Json::optionalBool(obj, "unique"_L1, Json::joinPath(path, u"unique"));
  if (!unique)
    return failure(unique.error());
  auto doubleSided = Json::optionalBool(obj, "doubleSided"_L1,
                                        Json::joinPath(path, u"doubleSided"));
  if (!doubleSided)
    return failure(doubleSided.error());
  auto exceptional = Json::optionalBool(obj, "exceptional"_L1,
                                        Json::joinPath(path, u"exceptional"));
  if (!exceptional)
    return failure(exceptional.error());
  auto playableFromDiscard =
      Json::optionalBool(obj, "playableFromDiscard"_L1,
                         Json::joinPath(path, u"playableFromDiscard"));
  if (!playableFromDiscard)
    return failure(playableFromDiscard.error());
  auto stage =
      Json::optionalInt(obj, "stage"_L1, Json::joinPath(path, u"stage"));
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

  auto grantedXp = Json::optionalInt(obj, "grantedXp"_L1,
                                     Json::joinPath(path, u"grantedXp"));
  if (!grantedXp)
    return failure(grantedXp.error());
  auto canReplace = Json::optionalBool(obj, "canReplace"_L1,
                                       Json::joinPath(path, u"canReplace"));
  if (!canReplace)
    return failure(canReplace.error());

  auto bondedWith = decodeBondedWith(obj, Json::joinPath(path, u"bondedWith"));
  if (!bondedWith)
    return failure(bondedWith.error());

  auto skipPlayWindows = Json::optionalBool(
      obj, "skipPlayWindows"_L1, Json::joinPath(path, u"skipPlayWindows"));
  if (!skipPlayWindows)
    return failure(skipPlayWindows.error());
  auto beforeEffect = Json::optionalBool(obj, "beforeEffect"_L1,
                                         Json::joinPath(path, u"beforeEffect"));
  if (!beforeEffect)
    return failure(beforeEffect.error());

  std::optional<CardCode> otherSide;
  if (Json::fieldPresence(obj, "otherSide"_L1) != Json::FieldPresence::Absent) {
    auto r = CardCode::fromJson(obj.value("otherSide"_L1),
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
      Json::optionalBool(obj, "canCommitWhenNoIcons"_L1,
                         Json::joinPath(path, u"canCommitWhenNoIcons"));
  if (!canCommitWhenNoIcons)
    return failure(canCommitWhenNoIcons.error());
  auto commitTrigger = Json::optionalBool(
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
    auto r = GameValue::fromJson(obj.value("health"_L1),
                                 Json::joinPath(path, u"health"));
    if (!r)
      return failure(r.error());
    health = *r;
  }
  std::optional<GameValue> fight;
  if (Json::fieldPresence(obj, "fight"_L1) != Json::FieldPresence::Absent) {
    auto r = GameValue::fromJson(obj.value("fight"_L1),
                                 Json::joinPath(path, u"fight"));
    if (!r)
      return failure(r.error());
    fight = *r;
  }
  std::optional<GameValue> evade;
  if (Json::fieldPresence(obj, "evade"_L1) != Json::FieldPresence::Absent) {
    auto r = GameValue::fromJson(obj.value("evade"_L1),
                                 Json::joinPath(path, u"evade"));
    if (!r)
      return failure(r.error());
    evade = *r;
  }
  std::optional<GameValue> healthDamage;
  if (Json::fieldPresence(obj, "healthDamage"_L1) !=
      Json::FieldPresence::Absent) {
    auto r = GameValue::fromJson(obj.value("healthDamage"_L1),
                                 Json::joinPath(path, u"healthDamage"));
    if (!r)
      return failure(r.error());
    healthDamage = *r;
  }
  std::optional<GameValue> sanityDamage;
  if (Json::fieldPresence(obj, "sanityDamage"_L1) !=
      Json::FieldPresence::Absent) {
    auto r = GameValue::fromJson(obj.value("sanityDamage"_L1),
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
  auto errata =
      Json::optionalString(obj, "errata"_L1, Json::joinPath(path, u"errata"));
  if (!errata)
    return failure(errata.error());

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
      .additionalCost = obj.value("additionalCost"_L1),
      .keywords = obj.value("keywords"_L1),
      .fastWindow = obj.value("fastWindow"_L1),
      .actions = obj.value("actions"_L1),
      .criteria = obj.value("criteria"_L1),
      .commitRestrictions = obj.value("commitRestrictions"_L1),
      .attackOfOpportunityModifiers =
          obj.value("attackOfOpportunityModifiers"_L1),
      .limits = obj.value("limits"_L1),
      .uses = obj.value("uses"_L1),
      .locationSymbol = obj.value("locationSymbol"_L1),
      .locationRevealedSymbol = obj.value("locationRevealedSymbol"_L1),
      .locationConnections = obj.value("locationConnections"_L1),
      .locationRevealedConnections =
          obj.value("locationRevealedConnections"_L1),
      .purchaseTrauma = obj.value("purchaseTrauma"_L1),
      .deckRestrictions = obj.value("deckRestrictions"_L1),
      .customizations = obj.value("customizations"_L1),
      .meta = obj.value("meta"_L1),
  };
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

  const auto insertRaw = [&obj](QLatin1StringView key, const QJsonValue &raw) {
    if (!raw.isUndefined())
      obj.insert(key, raw);
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
