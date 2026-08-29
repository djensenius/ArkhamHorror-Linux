#include "Games.h"

#include "JsonDecode.h"

#include <QJsonArray>
#include <array>
#include <utility>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

constexpr std::array<std::pair<QLatin1StringView, Difficulty>, 4>
    kDifficultyTable{{
        {"Easy"_L1, Difficulty::Easy},
        {"Standard"_L1, Difficulty::Standard},
        {"Hard"_L1, Difficulty::Hard},
        {"Expert"_L1, Difficulty::Expert},
    }};

constexpr std::array<std::pair<QLatin1StringView, MultiplayerVariant>, 2>
    kMultiplayerVariantTable{{
        {"Solo"_L1, MultiplayerVariant::Solo},
        {"WithFriends"_L1, MultiplayerVariant::WithFriends},
    }};

// Duplicated from CardCatalog.cpp's file-local table (both derive from the
// backend's single `Arkham.ClassSymbol` type): keeping each module's wire
// table private avoids coupling CardCatalog's internal layout to Games.cpp.
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

constexpr std::array<std::pair<QLatin1StringView, CampaignPart>, 2>
    kCampaignPartTable{{
        {"TheDreamQuest"_L1, CampaignPart::TheDreamQuest},
        {"TheWebOfDreams"_L1, CampaignPart::TheWebOfDreams},
    }};

// Two real values, four accepted wire spellings (the backend's hand-written
// FromJSON accepts both the current short form and a legacy PascalCase form
// still emitted by old saved games); the short forms come first so
// encodeClosedEnum's first-match lookup always emits them.
constexpr std::array<std::pair<QLatin1StringView, AsIfRulingValue>, 4>
    kAsIfRulingTable{{
        {"chapter1"_L1, AsIfRulingValue::Chapter1AsIfRuling},
        {"chapter2"_L1, AsIfRulingValue::Chapter2AsIfRuling},
        {"Chapter1AsIfRuling"_L1, AsIfRulingValue::Chapter1AsIfRuling},
        {"Chapter2AsIfRuling"_L1, AsIfRulingValue::Chapter2AsIfRuling},
    }};

constexpr std::array<std::pair<QLatin1StringView, UltimatumOrBoon>, 30>
    kUltimatumOrBoonTable{{
        {"BoonOfTheAncients"_L1, UltimatumOrBoon::BoonOfTheAncients},
        {"BoonOfAthena"_L1, UltimatumOrBoon::BoonOfAthena},
        {"BoonOfDestiny"_L1, UltimatumOrBoon::BoonOfDestiny},
        {"BoonOfHades"_L1, UltimatumOrBoon::BoonOfHades},
        {"BoonOfHermes"_L1, UltimatumOrBoon::BoonOfHermes},
        {"BoonOfThoth"_L1, UltimatumOrBoon::BoonOfThoth},
        {"BoonOfOsiris"_L1, UltimatumOrBoon::BoonOfOsiris},
        {"BoonOfTheMorrigan"_L1, UltimatumOrBoon::BoonOfTheMorrigan},
        {"BoonOfPersephone"_L1, UltimatumOrBoon::BoonOfPersephone},
        {"BoonOfTheExplorer"_L1, UltimatumOrBoon::BoonOfTheExplorer},
        {"BoonOfTheChild"_L1, UltimatumOrBoon::BoonOfTheChild},
        {"UltimatumOfAgony"_L1, UltimatumOrBoon::UltimatumOfAgony},
        {"UltimatumOfBrokenPromises"_L1,
         UltimatumOrBoon::UltimatumOfBrokenPromises},
        {"UltimatumOfTheBrokenVeil"_L1,
         UltimatumOrBoon::UltimatumOfTheBrokenVeil},
        {"UltimatumOfChaos"_L1, UltimatumOrBoon::UltimatumOfChaos},
        {"UltimatumOfDisaster"_L1, UltimatumOrBoon::UltimatumOfDisaster},
        {"UltimatumOfDread"_L1, UltimatumOrBoon::UltimatumOfDread},
        {"UltimatumOfFailure"_L1, UltimatumOrBoon::UltimatumOfFailure},
        {"UltimatumOfFinality"_L1, UltimatumOrBoon::UltimatumOfFinality},
        {"UltimatumOfForbiddenKnowledge"_L1,
         UltimatumOrBoon::UltimatumOfForbiddenKnowledge},
        {"UltimatumOfHardship"_L1, UltimatumOrBoon::UltimatumOfHardship},
        {"UltimatumOfTheHighlander"_L1,
         UltimatumOrBoon::UltimatumOfTheHighlander},
        {"UltimatumOfInduction"_L1, UltimatumOrBoon::UltimatumOfInduction},
        {"UltimatumOfOrthodoxy"_L1, UltimatumOrBoon::UltimatumOfOrthodoxy},
        {"UltimatumOfTheScream"_L1, UltimatumOrBoon::UltimatumOfTheScream},
        {"UltimatumOfSurvival"_L1, UltimatumOrBoon::UltimatumOfSurvival},
        {"UltimatumOfUltimatums"_L1, UltimatumOrBoon::UltimatumOfUltimatums},
        {"UltimatumOfExile"_L1, UltimatumOrBoon::UltimatumOfExile},
        {"UltimatumOfTheSpiral"_L1, UltimatumOrBoon::UltimatumOfTheSpiral},
        {"UltimatumOfMalevolence"_L1, UltimatumOrBoon::UltimatumOfMalevolence},
    }};

constexpr std::array<std::pair<QLatin1StringView, KnownCampaignOption>, 32>
    kKnownCampaignOptionTable{{
        {"PerformIntro"_L1, KnownCampaignOption::PerformIntro},
        {"PlayersDoNotControlStoryAssetClues"_L1,
         KnownCampaignOption::PlayersDoNotControlStoryAssetClues},
        {"AddLitaChantler"_L1, KnownCampaignOption::AddLitaChantler},
        {"Cheated"_L1, KnownCampaignOption::Cheated},
        {"TakeArmitage"_L1, KnownCampaignOption::TakeArmitage},
        {"TakeWarrenRice"_L1, KnownCampaignOption::TakeWarrenRice},
        {"TakeFrancisMorgan"_L1, KnownCampaignOption::TakeFrancisMorgan},
        {"TakeZebulonWhately"_L1, KnownCampaignOption::TakeZebulonWhately},
        {"TakeEarlSawyer"_L1, KnownCampaignOption::TakeEarlSawyer},
        {"TakePowderOfIbnGhazi"_L1, KnownCampaignOption::TakePowderOfIbnGhazi},
        {"TakeTheNecronomicon"_L1, KnownCampaignOption::TakeTheNecronomicon},
        {"AddAcrossSpaceAndTime"_L1,
         KnownCampaignOption::AddAcrossSpaceAndTime},
        {"UseSwarmPlaceholders"_L1, KnownCampaignOption::UseSwarmPlaceholders},
        {"TakeBlackBook"_L1, KnownCampaignOption::TakeBlackBook},
        {"TakePuzzleBox"_L1, KnownCampaignOption::TakePuzzleBox},
        {"ProceedToInterlude3"_L1, KnownCampaignOption::ProceedToInterlude3},
        {"DebugOption"_L1, KnownCampaignOption::DebugOption},
        {"ManuallyPickCamp"_L1, KnownCampaignOption::ManuallyPickCamp},
        {"ManuallyPickKilledInPlaneCrash"_L1,
         KnownCampaignOption::ManuallyPickKilledInPlaneCrash},
        {"AddGreenSoapstone"_L1, KnownCampaignOption::AddGreenSoapstone},
        {"AddWoodenSledge"_L1, KnownCampaignOption::AddWoodenSledge},
        {"AddDynamite"_L1, KnownCampaignOption::AddDynamite},
        {"AddMiasmicCrystal"_L1, KnownCampaignOption::AddMiasmicCrystal},
        {"AddMineralSpecimen"_L1, KnownCampaignOption::AddMineralSpecimen},
        {"AddSmallRadio"_L1, KnownCampaignOption::AddSmallRadio},
        {"AddSpareParts"_L1, KnownCampaignOption::AddSpareParts},
        {"IncludePartners"_L1, KnownCampaignOption::IncludePartners},
        {"FatalMiragePart1"_L1, KnownCampaignOption::FatalMiragePart1},
        {"FatalMiragePart2"_L1, KnownCampaignOption::FatalMiragePart2},
        {"FatalMiragePart3"_L1, KnownCampaignOption::FatalMiragePart3},
        {"PlayAsMiniCampaign"_L1, KnownCampaignOption::PlayAsMiniCampaign},
        {"PlayWithTheBlobThatAteEverythingElse"_L1,
         KnownCampaignOption::PlayWithTheBlobThatAteEverythingElse},
    }};

// Decodes a required-but-nullable closed-enum field (the key itself must be
// present; its value may be JSON null): campaign.currentCampaignMode.
template <typename Enum, std::size_t N>
ValueOrError<std::optional<Enum>> requireNullableClosedEnum(
    const QJsonObject &obj, QLatin1StringView key, QStringView path,
    const std::array<std::pair<QLatin1StringView, Enum>, N> &table) {
  switch (Json::fieldPresence(obj, key)) {
  case Json::FieldPresence::Absent:
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  case Json::FieldPresence::Null:
    return std::optional<Enum>{};
  case Json::FieldPresence::Present:
    break;
  }
  auto result = Json::decodeClosedEnum(obj.value(key), path, table);
  if (!result)
    return failure(result.error());
  return std::optional<Enum>(*result);
}

// Decodes an optional closed-enum field where an absent key and an explicit
// JSON null both collapse to unset: createGameRequest.asIfRuling.
template <typename Enum, std::size_t N>
ValueOrError<std::optional<Enum>> optionalClosedEnum(
    const QJsonObject &obj, QLatin1StringView key, QStringView path,
    const std::array<std::pair<QLatin1StringView, Enum>, N> &table) {
  const QJsonValue v = obj.value(key);
  if (v.isUndefined() || v.isNull())
    return std::optional<Enum>{};
  auto result = Json::decodeClosedEnum(v, path, table);
  if (!result)
    return failure(result.error());
  return std::optional<Enum>(*result);
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

ValueOrError<QList<QUuid>> decodeUuidArray(const QJsonArray &arr,
                                           QStringView path) {
  QList<QUuid> result;
  result.reserve(arr.size());
  for (qsizetype i = 0; i < arr.size(); ++i) {
    auto item = Json::decodeUuid(arr[i], Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    result.append(*item);
  }
  return result;
}

QJsonArray encodeUuidArray(const QList<QUuid> &ids) {
  QJsonArray result;
  for (const QUuid &id : ids)
    result.append(id.toString(QUuid::WithoutBraces));
  return result;
}

// Decodes a required array of InvestigatorSummary (gameDetails.investigators/
// otherInvestigators): unlike catalog.schema.json's optional arrays, the
// list-schema.json key itself is always required, so an absent key fails
// rather than defaulting to empty.
ValueOrError<QList<InvestigatorSummary>>
decodeInvestigatorArray(const QJsonObject &obj, QLatin1StringView key,
                        QStringView path) {
  auto arrResult = Json::requireArrayField(obj, key, path);
  if (!arrResult)
    return failure(arrResult.error());
  QList<InvestigatorSummary> result;
  result.reserve(arrResult->size());
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    auto item = InvestigatorSummary::fromJson((*arrResult)[i],
                                              Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    result.append(*item);
  }
  return result;
}

} // namespace

ValueOrError<InvestigatorSummary>
InvestigatorSummary::fromJson(const QJsonValue &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  auto id = CardCode::fromJson(obj.value("id"_L1), Json::joinPath(path, u"id"));
  if (!id)
    return failure(id.error());

  auto classSymbol = Json::decodeClosedEnum(
      obj.value("classSymbol"_L1), Json::joinPath(path, u"classSymbol"),
      kClassSymbolTable);
  if (!classSymbol)
    return failure(classSymbol.error());

  return InvestigatorSummary{.id = *id, .classSymbol = *classSymbol};
}

QJsonObject InvestigatorSummary::toJson() const {
  return QJsonObject{
      {QStringLiteral("id"), id.toJson()},
      {QStringLiteral("classSymbol"),
       Json::encodeClosedEnum(classSymbol, kClassSymbolTable)},
  };
}

ValueOrError<ScenarioSummary> ScenarioSummary::fromJson(const QJsonValue &v,
                                                        QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  auto id = CardCode::fromJson(obj.value("id"_L1), Json::joinPath(path, u"id"));
  if (!id)
    return failure(id.error());

  auto difficulty = Json::decodeClosedEnum(obj.value("difficulty"_L1),
                                           Json::joinPath(path, u"difficulty"),
                                           kDifficultyTable);
  if (!difficulty)
    return failure(difficulty.error());

  auto name =
      CardName::fromJson(obj.value("name"_L1), Json::joinPath(path, u"name"));
  if (!name)
    return failure(name.error());

  auto variant = Json::requireNullableString(obj, "variant"_L1,
                                             Json::joinPath(path, u"variant"));
  if (!variant)
    return failure(variant.error());

  return ScenarioSummary{
      .id = *id, .difficulty = *difficulty, .name = *name, .variant = *variant};
}

QJsonObject ScenarioSummary::toJson() const {
  return QJsonObject{
      {QStringLiteral("id"), id.toJson()},
      {QStringLiteral("difficulty"),
       Json::encodeClosedEnum(difficulty, kDifficultyTable)},
      {QStringLiteral("name"), name.toJson()},
      {QStringLiteral("variant"),
       variant ? QJsonValue(*variant) : QJsonValue(QJsonValue::Null)},
  };
}

ValueOrError<CampaignSummary> CampaignSummary::fromJson(const QJsonValue &v,
                                                        QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  auto id =
      CampaignId::fromJson(obj.value("id"_L1), Json::joinPath(path, u"id"));
  if (!id)
    return failure(id.error());

  auto difficulty = Json::decodeClosedEnum(obj.value("difficulty"_L1),
                                           Json::joinPath(path, u"difficulty"),
                                           kDifficultyTable);
  if (!difficulty)
    return failure(difficulty.error());

  auto currentCampaignMode = requireNullableClosedEnum(
      obj, "currentCampaignMode"_L1,
      Json::joinPath(path, u"currentCampaignMode"), kCampaignPartTable);
  if (!currentCampaignMode)
    return failure(currentCampaignMode.error());

  return CampaignSummary{.id = *id,
                         .difficulty = *difficulty,
                         .currentCampaignMode = *currentCampaignMode};
}

QJsonObject CampaignSummary::toJson() const {
  return QJsonObject{
      {QStringLiteral("id"), id.toJson()},
      {QStringLiteral("difficulty"),
       Json::encodeClosedEnum(difficulty, kDifficultyTable)},
      {QStringLiteral("currentCampaignMode"),
       currentCampaignMode ? QJsonValue(Json::encodeClosedEnum(
                                 *currentCampaignMode, kCampaignPartTable))
                           : QJsonValue(QJsonValue::Null)},
  };
}

ValueOrError<GameState> GameState::fromJson(const QJsonValue &v,
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

  if (tag == "IsPending"_L1 || tag == "IsChooseDecks"_L1) {
    const QString contentsPath = Json::joinPath(path, u"contents");
    auto arrResult = Json::requireArrayField(obj, "contents"_L1, contentsPath);
    if (!arrResult)
      return failure(arrResult.error());
    auto ids = decodeUuidArray(*arrResult, contentsPath);
    if (!ids)
      return failure(ids.error());
    return GameState{.kind = tag == "IsPending"_L1 ? Kind::Pending
                                                   : Kind::ChooseDecks,
                     .playerIds = *ids};
  }
  if (tag == "IsActive"_L1)
    return GameState{.kind = Kind::Active};
  if (tag == "IsOver"_L1)
    return GameState{.kind = Kind::Over};
  return GameState{.kind = Kind::Unknown,
                   .unknownTag = tag,
                   .unknownContents = obj.value("contents"_L1)};
}

QJsonObject GameState::toJson() const {
  switch (kind) {
  case Kind::Pending:
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("IsPending")},
        {QStringLiteral("contents"), encodeUuidArray(playerIds)}};
  case Kind::ChooseDecks:
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("IsChooseDecks")},
        {QStringLiteral("contents"), encodeUuidArray(playerIds)}};
  case Kind::Active:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("IsActive")}};
  case Kind::Over:
    return QJsonObject{{QStringLiteral("tag"), QStringLiteral("IsOver")}};
  case Kind::Unknown: {
    QJsonObject obj{{QStringLiteral("tag"), unknownTag}};
    if (!unknownContents.isUndefined())
      obj.insert(QStringLiteral("contents"), unknownContents);
    return obj;
  }
  }
  Q_UNREACHABLE_RETURN(QJsonObject{});
}

ValueOrError<GameListRow> GameListRow::fromJson(const QJsonValue &v,
                                                QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  // Disambiguated by shape, not an explicit tag: the backend's hand-written
  // GameDetailsEntry ToJSON encodes a failure as a bare `{"error": message}`
  // object and a success as the raw gameDetails object, so a top-level
  // "error" key is the only reliable discriminator.
  if (obj.contains("error"_L1)) {
    auto error =
        Json::requireString(obj, "error"_L1, Json::joinPath(path, u"error"));
    if (!error)
      return failure(error.error());
    return GameListRow{.kind = Kind::Failure, .error = *error};
  }

  auto id = GameId::fromJson(obj.value("id"_L1), Json::joinPath(path, u"id"));
  if (!id)
    return failure(id.error());

  std::optional<ScenarioSummary> scenario;
  {
    const QString fieldPath = Json::joinPath(path, u"scenario");
    if (!obj.contains("scenario"_L1))
      return failure(QStringLiteral("%1: missing required field \"scenario\"")
                         .arg(fieldPath));
    const QJsonValue sv = obj.value("scenario"_L1);
    if (!sv.isNull()) {
      auto result = ScenarioSummary::fromJson(sv, fieldPath);
      if (!result)
        return failure(result.error());
      scenario = *result;
    }
  }

  std::optional<CampaignSummary> campaign;
  {
    const QString fieldPath = Json::joinPath(path, u"campaign");
    if (!obj.contains("campaign"_L1))
      return failure(QStringLiteral("%1: missing required field \"campaign\"")
                         .arg(fieldPath));
    const QJsonValue cv = obj.value("campaign"_L1);
    if (!cv.isNull()) {
      auto result = CampaignSummary::fromJson(cv, fieldPath);
      if (!result)
        return failure(result.error());
      campaign = *result;
    }
  }

  auto gameState = GameState::fromJson(obj.value("gameState"_L1),
                                       Json::joinPath(path, u"gameState"));
  if (!gameState)
    return failure(gameState.error());

  auto name =
      Json::requireString(obj, "name"_L1, Json::joinPath(path, u"name"));
  if (!name)
    return failure(name.error());

  auto investigators = decodeInvestigatorArray(
      obj, "investigators"_L1, Json::joinPath(path, u"investigators"));
  if (!investigators)
    return failure(investigators.error());

  auto otherInvestigators =
      decodeInvestigatorArray(obj, "otherInvestigators"_L1,
                              Json::joinPath(path, u"otherInvestigators"));
  if (!otherInvestigators)
    return failure(otherInvestigators.error());

  auto multiplayerVariant = Json::decodeClosedEnum(
      obj.value("multiplayerVariant"_L1),
      Json::joinPath(path, u"multiplayerVariant"), kMultiplayerVariantTable);
  if (!multiplayerVariant)
    return failure(multiplayerVariant.error());

  auto hasOpenSeats = Json::requireBool(obj, "hasOpenSeats"_L1,
                                        Json::joinPath(path, u"hasOpenSeats"));
  if (!hasOpenSeats)
    return failure(hasOpenSeats.error());

  return GameListRow{
      .kind = Kind::Success,
      .id = *id,
      .scenario = std::move(scenario),
      .campaign = std::move(campaign),
      .gameState = *gameState,
      .name = *name,
      .investigators = std::move(*investigators),
      .otherInvestigators = std::move(*otherInvestigators),
      .multiplayerVariant = *multiplayerVariant,
      .hasOpenSeats = *hasOpenSeats,
  };
}

QJsonObject GameListRow::toJson() const {
  if (kind == Kind::Failure)
    return QJsonObject{{QStringLiteral("error"), error}};

  // id/gameState/multiplayerVariant are documented as always populated
  // together with Kind::Success, but they are public std::optional fields
  // with no constructor enforcing that invariant. Q_ASSERT alone is not
  // enough: it compiles out entirely in release (NDEBUG) builds, which
  // would leave a bare dereference of an empty optional below -- undefined
  // behavior. Keep the assert for a loud, early failure in debug/test
  // builds, but also branch on has_value() so a release build that somehow
  // hits the invalid state degrades to a JSON null for the offending
  // field(s) instead of crashing.
  Q_ASSERT(id.has_value());
  Q_ASSERT(gameState.has_value());
  Q_ASSERT(multiplayerVariant.has_value());

  QJsonObject obj;
  obj.insert(QStringLiteral("id"),
             id ? id->toJson() : QJsonValue(QJsonValue::Null));
  obj.insert(QStringLiteral("scenario"), scenario
                                             ? QJsonValue(scenario->toJson())
                                             : QJsonValue(QJsonValue::Null));
  obj.insert(QStringLiteral("campaign"), campaign
                                             ? QJsonValue(campaign->toJson())
                                             : QJsonValue(QJsonValue::Null));
  obj.insert(QStringLiteral("gameState"),
             gameState ? gameState->toJson() : QJsonValue(QJsonValue::Null));
  obj.insert(QStringLiteral("name"), name);
  QJsonArray investigatorsArr;
  for (const auto &investigator : investigators)
    investigatorsArr.append(investigator.toJson());
  obj.insert(QStringLiteral("investigators"), investigatorsArr);
  QJsonArray otherArr;
  for (const auto &investigator : otherInvestigators)
    otherArr.append(investigator.toJson());
  obj.insert(QStringLiteral("otherInvestigators"), otherArr);
  obj.insert(QStringLiteral("multiplayerVariant"),
             multiplayerVariant
                 ? QJsonValue(Json::encodeClosedEnum(*multiplayerVariant,
                                                     kMultiplayerVariantTable))
                 : QJsonValue(QJsonValue::Null));
  obj.insert(QStringLiteral("hasOpenSeats"), hasOpenSeats);
  return obj;
}

ValueOrError<QList<GameListRow>> decodeGameList(const QJsonValue &v,
                                                QStringView path) {
  auto arrResult = Json::requireArray(v, path);
  if (!arrResult)
    return failure(arrResult.error());
  QList<GameListRow> result;
  result.reserve(arrResult->size());
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    auto item =
        GameListRow::fromJson((*arrResult)[i], Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    result.append(*item);
  }
  return result;
}

QJsonArray encodeGameList(const QList<GameListRow> &rows) {
  QJsonArray result;
  for (const auto &row : rows)
    result.append(row.toJson());
  return result;
}

CampaignOption CampaignOption::knownOption(KnownCampaignOption option) {
  CampaignOption result;
  result.m_kind = Kind::Known;
  result.m_known = option;
  return result;
}

CampaignOption CampaignOption::variantOption(QString contents) {
  CampaignOption result;
  result.m_kind = Kind::Variant;
  result.m_text = std::move(contents);
  return result;
}

ValueOrError<CampaignOption> CampaignOption::fromJson(const QJsonValue &v,
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

  if (tag == "CampaignVariant"_L1) {
    auto contents = Json::requireString(obj, "contents"_L1,
                                        Json::joinPath(path, u"contents"));
    if (!contents)
      return failure(contents.error());
    CampaignOption result;
    result.m_kind = Kind::Variant;
    result.m_text = *contents;
    return result;
  }

  for (const auto &[wire, option] : kKnownCampaignOptionTable) {
    if (tag == wire) {
      CampaignOption result;
      result.m_kind = Kind::Known;
      result.m_known = option;
      return result;
    }
  }

  CampaignOption result;
  result.m_kind = Kind::Unknown;
  result.m_text = tag;
  result.m_unknownContents = obj.value("contents"_L1);
  return result;
}

QJsonObject CampaignOption::toJson() const {
  switch (m_kind) {
  case Kind::Known:
    return QJsonObject{
        {QStringLiteral("tag"),
         Json::encodeClosedEnum(*m_known, kKnownCampaignOptionTable)}};
  case Kind::Variant:
    return QJsonObject{
        {QStringLiteral("tag"), QStringLiteral("CampaignVariant")},
        {QStringLiteral("contents"), m_text}};
  case Kind::Unknown: {
    QJsonObject obj{{QStringLiteral("tag"), m_text}};
    if (!m_unknownContents.isUndefined())
      obj.insert(QStringLiteral("contents"), m_unknownContents);
    return obj;
  }
  }
  Q_UNREACHABLE_RETURN(QJsonObject{});
}

CampaignOrScenario CampaignOrScenario::campaign(CampaignId id) {
  CampaignOrScenario result;
  result.m_campaignId = std::move(id);
  return result;
}

CampaignOrScenario CampaignOrScenario::scenario(ScenarioId id) {
  CampaignOrScenario result;
  result.m_scenarioId = std::move(id);
  return result;
}

ValueOrError<CampaignOrScenario>
CampaignOrScenario::fromJson(const QJsonObject &requestObj, QStringView path) {
  // Both keys collapse absent-and-null identically, matching CreateGamePost's
  // hand-written `.:?` parse for each.
  std::optional<CampaignId> campaignId;
  const QJsonValue campaignV = requestObj.value("campaignId"_L1);
  if (!campaignV.isUndefined() && !campaignV.isNull()) {
    auto result =
        CampaignId::fromJson(campaignV, Json::joinPath(path, u"campaignId"));
    if (!result)
      return failure(result.error());
    campaignId = *result;
  }

  std::optional<ScenarioId> scenarioId;
  const QJsonValue scenarioV = requestObj.value("scenarioId"_L1);
  if (!scenarioV.isUndefined() && !scenarioV.isNull()) {
    auto result =
        ScenarioId::fromJson(scenarioV, Json::joinPath(path, u"scenarioId"));
    if (!result)
      return failure(result.error());
    scenarioId = *result;
  }

  if (campaignId.has_value() == scenarioId.has_value())
    return failure(
        QStringLiteral(
            "%1: exactly one of \"campaignId\"/\"scenarioId\" must be set, "
            "not %2")
            .arg(path, campaignId.has_value() ? QStringLiteral("both")
                                              : QStringLiteral("neither")));

  CampaignOrScenario result;
  result.m_campaignId = std::move(campaignId);
  result.m_scenarioId = std::move(scenarioId);
  return result;
}

void CampaignOrScenario::insertInto(QJsonObject &obj) const {
  obj.insert(QStringLiteral("campaignId"), m_campaignId
                                               ? m_campaignId->toJson()
                                               : QJsonValue(QJsonValue::Null));
  obj.insert(QStringLiteral("scenarioId"), m_scenarioId
                                               ? m_scenarioId->toJson()
                                               : QJsonValue(QJsonValue::Null));
}

ValueOrError<CreateGameRequest> CreateGameRequest::fromJson(const QJsonValue &v,
                                                            QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  const QString deckIdsPath = Json::joinPath(path, u"deckIds");
  auto deckIdsArr = Json::requireArrayField(obj, "deckIds"_L1, deckIdsPath);
  if (!deckIdsArr)
    return failure(deckIdsArr.error());
  QList<std::optional<QUuid>> deckIds;
  deckIds.reserve(deckIdsArr->size());
  for (qsizetype i = 0; i < deckIdsArr->size(); ++i) {
    auto item = Json::decodeNullableUuid((*deckIdsArr)[i],
                                         Json::indexPath(deckIdsPath, i));
    if (!item)
      return failure(item.error());
    deckIds.append(*item);
  }

  auto playerCount = Json::requireInt(obj, "playerCount"_L1,
                                      Json::joinPath(path, u"playerCount"));
  if (!playerCount)
    return failure(playerCount.error());

  auto campaignOrScenario = CampaignOrScenario::fromJson(obj, path);
  if (!campaignOrScenario)
    return failure(campaignOrScenario.error());

  auto difficulty = Json::decodeClosedEnum(obj.value("difficulty"_L1),
                                           Json::joinPath(path, u"difficulty"),
                                           kDifficultyTable);
  if (!difficulty)
    return failure(difficulty.error());

  auto campaignName = Json::requireString(
      obj, "campaignName"_L1, Json::joinPath(path, u"campaignName"));
  if (!campaignName)
    return failure(campaignName.error());

  auto multiplayerVariant = Json::decodeClosedEnum(
      obj.value("multiplayerVariant"_L1),
      Json::joinPath(path, u"multiplayerVariant"), kMultiplayerVariantTable);
  if (!multiplayerVariant)
    return failure(multiplayerVariant.error());

  auto includeTarotReadings =
      Json::requireBool(obj, "includeTarotReadings"_L1,
                        Json::joinPath(path, u"includeTarotReadings"));
  if (!includeTarotReadings)
    return failure(includeTarotReadings.error());

  const QString optionsPath = Json::joinPath(path, u"options");
  auto optionsArr = Json::requireArrayField(obj, "options"_L1, optionsPath);
  if (!optionsArr)
    return failure(optionsArr.error());
  QList<CampaignOption> options;
  options.reserve(optionsArr->size());
  for (qsizetype i = 0; i < optionsArr->size(); ++i) {
    auto item = CampaignOption::fromJson((*optionsArr)[i],
                                         Json::indexPath(optionsPath, i));
    if (!item)
      return failure(item.error());
    options.append(*item);
  }

  auto strictAsIfAt = Json::optionalBool(obj, "strictAsIfAt"_L1,
                                         Json::joinPath(path, u"strictAsIfAt"));
  if (!strictAsIfAt)
    return failure(strictAsIfAt.error());

  auto asIfRuling =
      optionalClosedEnum(obj, "asIfRuling"_L1,
                         Json::joinPath(path, u"asIfRuling"), kAsIfRulingTable);
  if (!asIfRuling)
    return failure(asIfRuling.error());

  // ultimatumsAndBoons/achievementsEnabled: the backend parses both with
  // `.:? ... .!= <default>`, which -- like plain `.:?` -- folds an absent
  // key and an explicit JSON null to the same resolved default, so there is
  // nothing left here for this client to preserve; only a present non-null
  // value is actually decoded.
  QList<UltimatumOrBoon> ultimatumsAndBoons;
  const QJsonValue ultimatumsV = obj.value("ultimatumsAndBoons"_L1);
  if (!ultimatumsV.isUndefined() && !ultimatumsV.isNull()) {
    const QString ultimatumsPath = Json::joinPath(path, u"ultimatumsAndBoons");
    auto arrResult = Json::requireArray(ultimatumsV, ultimatumsPath);
    if (!arrResult)
      return failure(arrResult.error());
    ultimatumsAndBoons.reserve(arrResult->size());
    for (qsizetype i = 0; i < arrResult->size(); ++i) {
      auto item = Json::decodeClosedEnum((*arrResult)[i],
                                         Json::indexPath(ultimatumsPath, i),
                                         kUltimatumOrBoonTable);
      if (!item)
        return failure(item.error());
      ultimatumsAndBoons.append(*item);
    }
  }

  bool achievementsEnabled = true;
  const QJsonValue achievementsV = obj.value("achievementsEnabled"_L1);
  if (!achievementsV.isUndefined() && !achievementsV.isNull()) {
    auto result = Json::requireBoolValue(
        achievementsV, Json::joinPath(path, u"achievementsEnabled"));
    if (!result)
      return failure(result.error());
    achievementsEnabled = *result;
  }

  return CreateGameRequest{
      .deckIds = std::move(deckIds),
      .playerCount = *playerCount,
      .campaignOrScenario = *campaignOrScenario,
      .difficulty = *difficulty,
      .campaignName = *campaignName,
      .multiplayerVariant = *multiplayerVariant,
      .includeTarotReadings = *includeTarotReadings,
      .options = std::move(options),
      .strictAsIfAt = *strictAsIfAt,
      .asIfRuling = *asIfRuling,
      .ultimatumsAndBoons = std::move(ultimatumsAndBoons),
      .achievementsEnabled = achievementsEnabled,
  };
}

QJsonObject CreateGameRequest::toJson() const {
  QJsonObject obj;
  QJsonArray deckIdsArr;
  for (const auto &id : deckIds)
    deckIdsArr.append(id ? QJsonValue(id->toString(QUuid::WithoutBraces))
                         : QJsonValue(QJsonValue::Null));
  obj.insert(QStringLiteral("deckIds"), deckIdsArr);
  obj.insert(QStringLiteral("playerCount"), playerCount);
  campaignOrScenario.insertInto(obj);
  obj.insert(QStringLiteral("difficulty"),
             Json::encodeClosedEnum(difficulty, kDifficultyTable));
  obj.insert(QStringLiteral("campaignName"), campaignName);
  obj.insert(
      QStringLiteral("multiplayerVariant"),
      Json::encodeClosedEnum(multiplayerVariant, kMultiplayerVariantTable));
  obj.insert(QStringLiteral("includeTarotReadings"), includeTarotReadings);
  QJsonArray optionsArr;
  for (const auto &option : options)
    optionsArr.append(option.toJson());
  obj.insert(QStringLiteral("options"), optionsArr);
  if (strictAsIfAt)
    obj.insert(QStringLiteral("strictAsIfAt"), *strictAsIfAt);
  if (asIfRuling)
    obj.insert(QStringLiteral("asIfRuling"),
               Json::encodeClosedEnum(*asIfRuling, kAsIfRulingTable));
  obj.insert(QStringLiteral("ultimatumsAndBoons"),
             encodeEnumArray(ultimatumsAndBoons, kUltimatumOrBoonTable));
  obj.insert(QStringLiteral("achievementsEnabled"), achievementsEnabled);
  return obj;
}

ValueOrError<ChooseDeckRequest> ChooseDeckRequest::fromJson(const QJsonValue &v,
                                                            QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const QJsonObject &obj = *objResult;

  auto investigatorId = InvestigatorRef::fromJson(
      obj.value("investigatorId"_L1), Json::joinPath(path, u"investigatorId"));
  if (!investigatorId)
    return failure(investigatorId.error());

  auto deckUrl =
      Json::optionalString(obj, "deckUrl"_L1, Json::joinPath(path, u"deckUrl"));
  if (!deckUrl)
    return failure(deckUrl.error());

  std::optional<DeckListInput> deckList;
  const QJsonValue deckListV = obj.value("deckList"_L1);
  if (!deckListV.isUndefined() && !deckListV.isNull()) {
    auto result =
        DeckListInput::fromJson(deckListV, Json::joinPath(path, u"deckList"));
    if (!result)
      return failure(result.error());
    deckList = *result;
  }

  return ChooseDeckRequest{.investigatorId = *investigatorId,
                           .deckUrl = *deckUrl,
                           .deckList = std::move(deckList)};
}

QJsonObject ChooseDeckRequest::toJson() const {
  QJsonObject obj;
  obj.insert(QStringLiteral("investigatorId"), investigatorId.toJson());
  if (deckUrl)
    obj.insert(QStringLiteral("deckUrl"), *deckUrl);
  if (deckList)
    obj.insert(QStringLiteral("deckList"), deckList->toJson());
  return obj;
}

ValueOrError<ClaimSeatRequest> ClaimSeatRequest::fromJson(const QJsonValue &v,
                                                          QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());

  auto investigatorId =
      InvestigatorRef::fromJson(objResult->value("investigatorId"_L1),
                                Json::joinPath(path, u"investigatorId"));
  if (!investigatorId)
    return failure(investigatorId.error());

  return ClaimSeatRequest{.investigatorId = *investigatorId};
}

QJsonObject ClaimSeatRequest::toJson() const {
  return QJsonObject{
      {QStringLiteral("investigatorId"), investigatorId.toJson()}};
}

ValueOrError<QList<CardCode>> decodeOpenSeats(const QJsonValue &v,
                                              QStringView path) {
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

QJsonArray encodeOpenSeats(const QList<CardCode> &seats) {
  QJsonArray result;
  for (const auto &seat : seats)
    result.append(seat.toJson());
  return result;
}

} // namespace Arkham
