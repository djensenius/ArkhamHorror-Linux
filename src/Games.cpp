#include "Games.h"

#include "JsonDecode.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <array>
#include <utility>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// Brings Json::toLosslessRaw into unqualified lookup for both overloads:
// ADL alone finds the Json::Value overload (its associated namespace is
// Arkham::Json) but not the QJsonValue one (QJsonValue's associated
// namespace is Qt's), so an unqualified call inside a template shared
// between both value families needs this using-declaration -- see
// CardCatalog.cpp/Decks.cpp for the identical rationale.
using Json::toLosslessRaw;

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
// Generic over Obj (QJsonObject or Json::Value, see RawJson.h): both
// support fieldPresence()/value() identically, and decodeClosedEnum is
// itself already generic, so one body serves both the fromJson() and
// fromRawJson() decode families.
template <typename Obj, typename Enum, std::size_t N>
ValueOrError<std::optional<Enum>> requireNullableClosedEnum(
    const Obj &obj, QLatin1StringView key, QStringView path,
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
// JSON null both collapse to unset: createGameRequest.asIfRuling. Generic
// over Obj for the same reason as requireNullableClosedEnum above.
template <typename Obj, typename Enum, std::size_t N>
ValueOrError<std::optional<Enum>> optionalClosedEnum(
    const Obj &obj, QLatin1StringView key, QStringView path,
    const std::array<std::pair<QLatin1StringView, Enum>, N> &table) {
  const auto v = obj.value(key);
  if (v.isUndefined() || v.isNull())
    return std::optional<Enum>{};
  auto result = Json::decodeClosedEnum(v, path, table);
  if (!result)
    return failure(result.error());
  return std::optional<Enum>(*result);
}

template <typename Enum, std::size_t N>
ValueOrError<QJsonArray> encodeEnumArray(
    const QList<Enum> &values,
    const std::array<std::pair<QLatin1StringView, Enum>, N> &table) {
  QJsonArray result;
  for (qsizetype i = 0; i < values.size(); ++i) {
    auto encoded = Json::encodeClosedEnum(values.at(i), table);
    if (!encoded)
      return failure(QStringLiteral("encodeEnumArray[%1]: %2")
                         .arg(i)
                         .arg(encoded.error()));
    result.append(*encoded);
  }
  return result;
}

// Arr is QJsonArray (from Json::requireArrayField(QJsonObject...)) or
// QList<Json::Value> (from Json::requireArrayField(Json::Value...)): both
// support .size()/operator[] identically, and Json::decodeUuid is itself
// overloaded for QJsonValue/Json::Value, so this one body serves both
// GameState::fromJson (QJsonValue path) and GameState::fromRawJson
// (Json::Value path, see Games.h) without duplication.
template <typename Arr>
ValueOrError<QList<QUuid>> decodeUuidArray(const Arr &arr, QStringView path) {
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

// Same-name overload pair so a generic template body (e.g.
// GameListRow::fromValueImpl<V> below) can decode a nested "gameState"
// field with one spelling despite GameState's differently-named fromJson()/
// fromRawJson() public entry points.
ValueOrError<GameState> decodeGameStateValue(const QJsonValue &v,
                                             QStringView path) {
  return GameState::fromJson(v, path);
}
ValueOrError<GameState> decodeGameStateValue(const Json::Value &v,
                                             QStringView path) {
  return GameState::fromRawJson(v, path);
}

QJsonArray encodeUuidArray(const QList<QUuid> &ids) {
  QJsonArray result;
  for (const QUuid &id : ids)
    result.append(id.toString(QUuid::WithoutBraces));
  return result;
}

// Raw-AST counterpart of encodeUuidArray() above, for GameState::toRawJson().
Json::Value encodeUuidArrayRaw(const QList<QUuid> &ids) {
  QList<Json::Value> result;
  for (const QUuid &id : ids)
    result.append(Json::Value::makeString(id.toString(QUuid::WithoutBraces)));
  return Json::Value::makeArray(result);
}

// Decodes a required array of InvestigatorSummary (gameDetails.investigators/
// otherInvestigators): unlike catalog.schema.json's optional arrays, the
// list-schema.json key itself is always required, so an absent key fails
// rather than defaulting to empty. Generic over Obj (QJsonObject or
// Json::Value, see RawJson.h): Json::requireArrayField is itself already
// generic, and InvestigatorSummary::fromJson is overloaded for both
// element types below, so one body serves both the fromJson() and
// fromRawJson() decode families.
template <typename Obj>
ValueOrError<QList<InvestigatorSummary>>
decodeInvestigatorArray(const Obj &obj, QLatin1StringView key,
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

namespace {

template <typename V>
ValueOrError<InvestigatorSummary>
investigatorSummaryFromValueImpl(const V &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  // The pinned schema's `investigator` def is additionalProperties:false;
  // unlike CardDef's deliberate forward-compat leniency, this closed,
  // fixed-shape summary object enforces its exact key set (round-9 item 5).
  auto exactKeys =
      Json::requireExactKeys(obj, {"id"_L1, "classSymbol"_L1}, path);
  if (!exactKeys)
    return failure(exactKeys.error());

  auto id = Json::requireField(
      obj, "id"_L1, Json::joinPath(path, u"id"),
      [](const auto &v, QStringView p) { return CardCode::fromJson(v, p); });
  if (!id)
    return failure(id.error());

  auto classSymbol = Json::requireField(
      obj, "classSymbol"_L1, Json::joinPath(path, u"classSymbol"),
      [](const auto &v, QStringView p) {
        return Json::decodeClosedEnum(v, p, kClassSymbolTable);
      });
  if (!classSymbol)
    return failure(classSymbol.error());

  return InvestigatorSummary{.id = *id, .classSymbol = *classSymbol};
}

template <typename V>
ValueOrError<ScenarioSummary> scenarioSummaryFromValueImpl(const V &v,
                                                           QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  // Closed, fixed-shape summary object (additionalProperties:false in the
  // pinned schema) -- enforce its exact key set (round-9 item 5).
  auto exactKeys = Json::requireExactKeys(
      obj, {"id"_L1, "difficulty"_L1, "name"_L1, "variant"_L1}, path);
  if (!exactKeys)
    return failure(exactKeys.error());

  auto id = Json::requireField(
      obj, "id"_L1, Json::joinPath(path, u"id"),
      [](const auto &v, QStringView p) { return CardCode::fromJson(v, p); });
  if (!id)
    return failure(id.error());

  auto difficulty = Json::requireField(
      obj, "difficulty"_L1, Json::joinPath(path, u"difficulty"),
      [](const auto &v, QStringView p) {
        return Json::decodeClosedEnum(v, p, kDifficultyTable);
      });
  if (!difficulty)
    return failure(difficulty.error());

  auto name = Json::requireField(
      obj, "name"_L1, Json::joinPath(path, u"name"),
      [](const auto &v, QStringView p) { return CardName::fromJson(v, p); });
  if (!name)
    return failure(name.error());

  auto variant = Json::requireNullableString(obj, "variant"_L1,
                                             Json::joinPath(path, u"variant"));
  if (!variant)
    return failure(variant.error());

  return ScenarioSummary{
      .id = *id, .difficulty = *difficulty, .name = *name, .variant = *variant};
}

template <typename V>
ValueOrError<CampaignSummary> campaignSummaryFromValueImpl(const V &v,
                                                           QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  // Closed, fixed-shape summary object (additionalProperties:false in the
  // pinned schema) -- enforce its exact key set (round-9 item 5).
  auto exactKeys = Json::requireExactKeys(
      obj, {"id"_L1, "difficulty"_L1, "currentCampaignMode"_L1}, path);
  if (!exactKeys)
    return failure(exactKeys.error());

  auto id = Json::requireField(
      obj, "id"_L1, Json::joinPath(path, u"id"),
      [](const auto &v, QStringView p) { return CampaignId::fromJson(v, p); });
  if (!id)
    return failure(id.error());

  auto difficulty = Json::requireField(
      obj, "difficulty"_L1, Json::joinPath(path, u"difficulty"),
      [](const auto &v, QStringView p) {
        return Json::decodeClosedEnum(v, p, kDifficultyTable);
      });
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

} // namespace

ValueOrError<InvestigatorSummary>
InvestigatorSummary::fromJson(const QJsonValue &v, QStringView path) {
  return investigatorSummaryFromValueImpl(v, path);
}

ValueOrError<InvestigatorSummary>
InvestigatorSummary::fromJson(const Json::Value &v, QStringView path) {
  return investigatorSummaryFromValueImpl(v, path);
}

ValueOrError<QJsonObject> InvestigatorSummary::toJson() const {
  auto classSymbolEncoded =
      Json::encodeClosedEnum(classSymbol, kClassSymbolTable);
  if (!classSymbolEncoded)
    return failure(
        QStringLiteral("classSymbol: %1").arg(classSymbolEncoded.error()));
  auto idEncoded = id.toJson();
  if (!idEncoded)
    return failure(QStringLiteral("id: %1").arg(idEncoded.error()));
  return QJsonObject{
      {QStringLiteral("id"), *idEncoded},
      {QStringLiteral("classSymbol"), *classSymbolEncoded},
  };
}

ValueOrError<ScenarioSummary> ScenarioSummary::fromJson(const QJsonValue &v,
                                                        QStringView path) {
  return scenarioSummaryFromValueImpl(v, path);
}

ValueOrError<ScenarioSummary> ScenarioSummary::fromJson(const Json::Value &v,
                                                        QStringView path) {
  return scenarioSummaryFromValueImpl(v, path);
}

ValueOrError<QJsonObject> ScenarioSummary::toJson() const {
  auto difficultyEncoded = Json::encodeClosedEnum(difficulty, kDifficultyTable);
  if (!difficultyEncoded)
    return failure(
        QStringLiteral("difficulty: %1").arg(difficultyEncoded.error()));
  auto idEncoded = id.toJson();
  if (!idEncoded)
    return failure(QStringLiteral("id: %1").arg(idEncoded.error()));
  auto nameEncoded = name.toJson();
  if (!nameEncoded)
    return failure(QStringLiteral("name: %1").arg(nameEncoded.error()));
  return QJsonObject{
      {QStringLiteral("id"), *idEncoded},
      {QStringLiteral("difficulty"), *difficultyEncoded},
      {QStringLiteral("name"), *nameEncoded},
      {QStringLiteral("variant"),
       variant ? QJsonValue(*variant) : QJsonValue(QJsonValue::Null)},
  };
}

ValueOrError<CampaignSummary> CampaignSummary::fromJson(const QJsonValue &v,
                                                        QStringView path) {
  return campaignSummaryFromValueImpl(v, path);
}

ValueOrError<CampaignSummary> CampaignSummary::fromJson(const Json::Value &v,
                                                        QStringView path) {
  return campaignSummaryFromValueImpl(v, path);
}

ValueOrError<QJsonObject> CampaignSummary::toJson() const {
  auto difficultyEncoded = Json::encodeClosedEnum(difficulty, kDifficultyTable);
  if (!difficultyEncoded)
    return failure(
        QStringLiteral("difficulty: %1").arg(difficultyEncoded.error()));
  QJsonValue currentCampaignModeValue = QJsonValue(QJsonValue::Null);
  if (currentCampaignMode) {
    auto currentCampaignModeEncoded =
        Json::encodeClosedEnum(*currentCampaignMode, kCampaignPartTable);
    if (!currentCampaignModeEncoded)
      return failure(QStringLiteral("currentCampaignMode: %1")
                         .arg(currentCampaignModeEncoded.error()));
    currentCampaignModeValue = QJsonValue(*currentCampaignModeEncoded);
  }
  auto idEncoded = id.toJson();
  if (!idEncoded)
    return failure(QStringLiteral("id: %1").arg(idEncoded.error()));
  return QJsonObject{
      {QStringLiteral("id"), *idEncoded},
      {QStringLiteral("difficulty"), *difficultyEncoded},
      {QStringLiteral("currentCampaignMode"), currentCampaignModeValue},
  };
}

ValueOrError<GameState> GameState::pending(QList<QUuid> playerIds) {
  for (const QUuid &id : playerIds)
    if (id.isNull())
      return failure(QStringLiteral(
          "GameState::pending: playerIds must not contain a null uuid"));
  GameState result;
  result.m_kind = Kind::Pending;
  result.m_playerIds = std::move(playerIds);
  return result;
}

ValueOrError<GameState> GameState::chooseDecks(QList<QUuid> playerIds) {
  for (const QUuid &id : playerIds)
    if (id.isNull())
      return failure(QStringLiteral(
          "GameState::chooseDecks: playerIds must not contain a null uuid"));
  GameState result;
  result.m_kind = Kind::ChooseDecks;
  result.m_playerIds = std::move(playerIds);
  return result;
}

GameState GameState::active() {
  GameState result;
  result.m_kind = Kind::Active;
  return result;
}

GameState GameState::over() {
  GameState result;
  result.m_kind = Kind::Over;
  return result;
}

ValueOrError<GameState> GameState::fromJson(const QJsonValue &v,
                                            QStringView path) {
  return fromValueImpl(v, path);
}

ValueOrError<GameState> GameState::fromRawJson(const Json::Value &v,
                                               QStringView path) {
  return fromValueImpl(v, path);
}

ValueOrError<GameState> GameState::fromRawBytes(QByteArrayView bytes,
                                                QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

template <typename V>
ValueOrError<GameState> GameState::fromValueImpl(const V &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto tagResult =
      Json::requireString(obj, "tag"_L1, Json::joinPath(path, u"tag"));
  if (!tagResult)
    return failure(tagResult.error());
  const QString &tag = *tagResult;

  if (tag == "IsPending"_L1 || tag == "IsChooseDecks"_L1) {
    auto keysResult =
        Json::requireExactKeys(obj, {"tag"_L1, "contents"_L1}, path);
    if (!keysResult)
      return failure(keysResult.error());
    const QString contentsPath = Json::joinPath(path, u"contents");
    auto arrResult = Json::requireArrayField(obj, "contents"_L1, contentsPath);
    if (!arrResult)
      return failure(arrResult.error());
    auto ids = decodeUuidArray(*arrResult, contentsPath);
    if (!ids)
      return failure(ids.error());
    // decodeUuidArray already rejects a null uuid per element (see
    // Json::decodeUuid), so pending()/chooseDecks()'s own validation below
    // can never actually fail here -- but routing through the same
    // validated factory used by hand-constructed callers keeps the
    // invariant defined in exactly one place rather than duplicated.
    auto state = tag == "IsPending"_L1 ? GameState::pending(*ids)
                                       : GameState::chooseDecks(*ids);
    if (!state)
      return failure(QStringLiteral("%1: %2").arg(path, state.error()));
    return state;
  }
  if (tag == "IsActive"_L1 || tag == "IsOver"_L1) {
    // Known nullary tags reject any "contents" presence (or any other
    // key) -- even an explicit null -- rather than silently ignoring an
    // unexpected payload.
    auto keysResult = Json::requireExactKeys(obj, {"tag"_L1}, path);
    if (!keysResult)
      return failure(keysResult.error());
    return tag == "IsActive"_L1 ? GameState::active() : GameState::over();
  }

  // An unrecognized tag is preserved verbatim rather than rejected: this is
  // a live state machine a future backend release can extend. The
  // *complete* decoded object (not merely "contents") is captured -- any
  // additive sibling key a future backend adds alongside "tag"/"contents"
  // survives too, and toJson() below re-emits it byte-for-byte rather than
  // reconstructing only the two keys this client currently knows about.
  auto rawResult = toLosslessRaw(v);
  if (!rawResult)
    return failure(QStringLiteral("%1: %2").arg(path, rawResult.error()));
  GameState result;
  result.m_kind = Kind::Unknown;
  result.m_unknownTag = tag;
  result.m_unknownRaw = *rawResult;
  return result;
}

QJsonObject GameState::toJson() const {
  return toRawJson().toQJson().toObject();
}

Json::Value GameState::toRawJson() const {
  switch (m_kind) {
  case Kind::Pending:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("IsPending"))},
         {QStringLiteral("contents"), encodeUuidArrayRaw(m_playerIds)}});
  case Kind::ChooseDecks:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("IsChooseDecks"))},
         {QStringLiteral("contents"), encodeUuidArrayRaw(m_playerIds)}});
  case Kind::Active:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("IsActive"))}});
  case Kind::Over:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("IsOver"))}});
  case Kind::Unknown:
    return m_unknownRaw;
  }
  // See CardCost::toRawJson()/GameValue::toRawJson() in CardCatalog.cpp
  // for why a plain trailing return -- rather than Q_UNREACHABLE_RETURN
  // -- is used here: an out-of-range m_kind encodes as Undefined (which
  // Value::toJsonBytes() already rejects) instead of aborting/UB.
  return Json::Value{};
}

ValueOrError<QByteArray> GameState::toJsonBytes() const {
  return toRawJson().toJsonBytes();
}

GameListRow GameListRow::success(GameId id,
                                 std::optional<ScenarioSummary> scenario,
                                 std::optional<CampaignSummary> campaign,
                                 GameState gameState, QString name,
                                 QList<InvestigatorSummary> investigators,
                                 QList<InvestigatorSummary> otherInvestigators,
                                 MultiplayerVariant multiplayerVariant,
                                 bool hasOpenSeats) {
  GameListRow row;
  row.m_kind = Kind::Success;
  row.m_id = std::move(id);
  row.m_scenario = std::move(scenario);
  row.m_campaign = std::move(campaign);
  row.m_gameState = std::move(gameState);
  row.m_name = std::move(name);
  row.m_investigators = std::move(investigators);
  row.m_otherInvestigators = std::move(otherInvestigators);
  row.m_multiplayerVariant = multiplayerVariant;
  row.m_hasOpenSeats = hasOpenSeats;
  return row;
}

GameListRow GameListRow::failed(QString error) {
  GameListRow row;
  row.m_kind = Kind::Failure;
  row.m_error = std::move(error);
  return row;
}

ValueOrError<GameListRow> GameListRow::fromJson(const QJsonValue &v,
                                                QStringView path) {
  return fromValueImpl(v, path);
}

ValueOrError<GameListRow> GameListRow::fromRawJson(const Json::Value &v,
                                                   QStringView path) {
  return fromValueImpl(v, path);
}

ValueOrError<GameListRow> GameListRow::fromRawBytes(QByteArrayView bytes,
                                                    QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

template <typename V>
ValueOrError<GameListRow> GameListRow::fromValueImpl(const V &v,
                                                     QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  // Disambiguated by shape, not an explicit tag: the backend's hand-written
  // GameDetailsEntry ToJSON encodes a failure as a bare `{"error": message}`
  // object and a success as the raw gameDetails object. Per the schema,
  // failedGameDetails is `additionalProperties: false` with "error" as its
  // *only* allowed key, so an object carrying "error" alongside any other
  // key (a success key or otherwise) matches neither def -- it is a
  // malformed row, not an automatic failure that happens to have "extra"
  // data attached. Propagate that as a decode error instead of silently
  // discarding the other keys and declaring victory as Kind::Failure.
  if (obj.contains("error"_L1)) {
    if (Json::objectMembers(obj).size() != 1)
      return failure(
          QStringLiteral("%1: a failure row's \"error\" must be its only "
                         "key (found %2 keys)")
              .arg(path)
              .arg(Json::objectMembers(obj).size()));
    auto error =
        Json::requireString(obj, "error"_L1, Json::joinPath(path, u"error"));
    if (!error)
      return failure(error.error());
    return GameListRow::failed(*error);
  }

  // The pinned schema's `gameDetails` (success) def is
  // additionalProperties:false. Unlike CardDef's deliberate forward-compat
  // additive-field leniency (a documented policy choice for evolving card
  // data), this is a fixed, closed contract shape used to positively
  // disambiguate a success row from a failure row -- an additive/unknown
  // top-level field here is a contract violation, not a forward-compat
  // signal to tolerate (round-9 item 5; supersedes this file's prior
  // additive-tolerant behavior for this specific shape).
  auto exactKeys = Json::requireExactKeys(
      obj,
      {"id"_L1, "scenario"_L1, "campaign"_L1, "gameState"_L1, "name"_L1,
       "investigators"_L1, "otherInvestigators"_L1, "multiplayerVariant"_L1,
       "hasOpenSeats"_L1},
      path);
  if (!exactKeys)
    return failure(exactKeys.error());

  auto id = Json::requireField(
      obj, "id"_L1, Json::joinPath(path, u"id"),
      [](const auto &v, QStringView p) { return GameId::fromJson(v, p); });
  if (!id)
    return failure(id.error());

  std::optional<ScenarioSummary> scenario;
  {
    const QString fieldPath = Json::joinPath(path, u"scenario");
    if (!obj.contains("scenario"_L1))
      return failure(QStringLiteral("%1: missing required field \"scenario\"")
                         .arg(fieldPath));
    const auto sv = obj.value("scenario"_L1);
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
    const auto cv = obj.value("campaign"_L1);
    if (!cv.isNull()) {
      auto result = CampaignSummary::fromJson(cv, fieldPath);
      if (!result)
        return failure(result.error());
      campaign = *result;
    }
  }

  auto gameState = Json::requireField(
      obj, "gameState"_L1, Json::joinPath(path, u"gameState"),
      [](const auto &v, QStringView p) { return decodeGameStateValue(v, p); });
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

  auto multiplayerVariant = Json::requireField(
      obj, "multiplayerVariant"_L1, Json::joinPath(path, u"multiplayerVariant"),
      [](const auto &v, QStringView p) {
        return Json::decodeClosedEnum(v, p, kMultiplayerVariantTable);
      });
  if (!multiplayerVariant)
    return failure(multiplayerVariant.error());

  auto hasOpenSeats = Json::requireBool(obj, "hasOpenSeats"_L1,
                                        Json::joinPath(path, u"hasOpenSeats"));
  if (!hasOpenSeats)
    return failure(hasOpenSeats.error());

  return GameListRow::success(*id, std::move(scenario), std::move(campaign),
                              *gameState, *name, std::move(*investigators),
                              std::move(*otherInvestigators),
                              *multiplayerVariant, *hasOpenSeats);
}

ValueOrError<Json::Value> GameListRow::toRawJson() const {
  if (m_kind == Kind::Failure)
    return Json::Value::makeObject(
        {{QStringLiteral("error"), Json::Value::makeString(m_error)}});

  // No invariant check is needed here: success() is the only way to build
  // a Kind::Success instance, and it always populates id/gameState/
  // multiplayerVariant together, so they are guaranteed present by
  // construction -- there is no qFatal()/Q_ASSERT()-guarded fallback
  // anywhere in this file; every tagged/sum-type toJson() in this
  // codebase is unconditionally safe by construction instead.
  QList<std::pair<QString, Json::Value>> members;
  const auto insert = [&members](QLatin1StringView key, Json::Value value) {
    members.append({QString(key), std::move(value)});
  };

  auto idRaw = Json::Value::fromQJson(m_id->toJson());
  if (!idRaw)
    return failure(QStringLiteral("id: %1").arg(idRaw.error()));
  insert("id"_L1, *idRaw);

  if (m_scenario) {
    auto scenarioEncoded = m_scenario->toJson();
    if (!scenarioEncoded)
      return failure(
          QStringLiteral("scenario: %1").arg(scenarioEncoded.error()));
    auto scenarioRaw = Json::Value::fromQJson(*scenarioEncoded);
    if (!scenarioRaw)
      return failure(QStringLiteral("scenario: %1").arg(scenarioRaw.error()));
    insert("scenario"_L1, *scenarioRaw);
  } else {
    insert("scenario"_L1, Json::Value::makeNull());
  }
  if (m_campaign) {
    auto campaignEncoded = m_campaign->toJson();
    if (!campaignEncoded)
      return failure(
          QStringLiteral("campaign: %1").arg(campaignEncoded.error()));
    auto campaignRaw = Json::Value::fromQJson(*campaignEncoded);
    if (!campaignRaw)
      return failure(QStringLiteral("campaign: %1").arg(campaignRaw.error()));
    insert("campaign"_L1, *campaignRaw);
  } else {
    insert("campaign"_L1, Json::Value::makeNull());
  }
  // The reason this method exists distinct from toJson(): route gameState
  // through GameState::toRawJson() (lossless Json::Value AST), not its
  // QJsonObject-typed toJson() (double-backed) -- so an Unknown tag's
  // numeric literal outside IEEE-754 double's exact-integer range
  // survives an encode-then-reparse round trip through this row, not
  // merely through GameState in isolation.
  insert("gameState"_L1, m_gameState->toRawJson());
  insert("name"_L1, Json::Value::makeString(m_name));

  QList<Json::Value> investigatorsArr;
  for (qsizetype i = 0; i < m_investigators.size(); ++i) {
    auto encoded = m_investigators.at(i).toJson();
    if (!encoded)
      return failure(
          QStringLiteral("investigators[%1]: %2").arg(i).arg(encoded.error()));
    auto raw = Json::Value::fromQJson(*encoded);
    if (!raw)
      return failure(
          QStringLiteral("investigators[%1]: %2").arg(i).arg(raw.error()));
    investigatorsArr.append(*raw);
  }
  insert("investigators"_L1, Json::Value::makeArray(investigatorsArr));

  QList<Json::Value> otherArr;
  for (qsizetype i = 0; i < m_otherInvestigators.size(); ++i) {
    auto encoded = m_otherInvestigators.at(i).toJson();
    if (!encoded)
      return failure(QStringLiteral("otherInvestigators[%1]: %2")
                         .arg(i)
                         .arg(encoded.error()));
    auto raw = Json::Value::fromQJson(*encoded);
    if (!raw)
      return failure(
          QStringLiteral("otherInvestigators[%1]: %2").arg(i).arg(raw.error()));
    otherArr.append(*raw);
  }
  insert("otherInvestigators"_L1, Json::Value::makeArray(otherArr));

  auto multiplayerVariantEncoded =
      Json::encodeClosedEnum(*m_multiplayerVariant, kMultiplayerVariantTable);
  if (!multiplayerVariantEncoded)
    return failure(QStringLiteral("multiplayerVariant: %1")
                       .arg(multiplayerVariantEncoded.error()));
  insert("multiplayerVariant"_L1,
         Json::Value::makeString(*multiplayerVariantEncoded));
  insert("hasOpenSeats"_L1, Json::Value::makeBool(m_hasOpenSeats));

  return Json::Value::makeObject(members);
}

ValueOrError<QJsonObject> GameListRow::toJson() const {
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toQJson().toObject();
}

ValueOrError<QByteArray> GameListRow::toJsonBytes() const {
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toJsonBytes();
}

namespace {

// Same-name overload pair so decodeGameListImpl<V> below can call one
// spelling generically despite GameListRow's differently-named fromJson()/
// fromRawJson() public entry points (see this codebase's established
// idiom in Decks.cpp's decodeExternalDeckId/decodeInvestigatorRefValue).
ValueOrError<GameListRow> decodeGameListRow(const QJsonValue &v,
                                            QStringView path) {
  return GameListRow::fromJson(v, path);
}
ValueOrError<GameListRow> decodeGameListRow(const Json::Value &v,
                                            QStringView path) {
  return GameListRow::fromRawJson(v, path);
}

template <typename V>
ValueOrError<QList<GameListRow>> decodeGameListImpl(const V &v,
                                                    QStringView path) {
  auto arrResult = Json::requireArray(v, path);
  if (!arrResult)
    return failure(arrResult.error());
  QList<GameListRow> result;
  result.reserve(arrResult->size());
  for (qsizetype i = 0; i < arrResult->size(); ++i) {
    auto item = decodeGameListRow((*arrResult)[i], Json::indexPath(path, i));
    if (!item)
      return failure(item.error());
    result.append(*item);
  }
  return result;
}

} // namespace

ValueOrError<QList<GameListRow>> decodeGameList(const QJsonValue &v,
                                                QStringView path) {
  return decodeGameListImpl(v, path);
}

ValueOrError<QList<GameListRow>> decodeGameListFromRawJson(const Json::Value &v,
                                                           QStringView path) {
  return decodeGameListImpl(v, path);
}

ValueOrError<QList<GameListRow>>
decodeGameListFromRawBytes(QByteArrayView bytes, QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return decodeGameListFromRawJson(*parsed, path);
}

ValueOrError<QJsonArray> encodeGameList(const QList<GameListRow> &rows) {
  QJsonArray result;
  for (qsizetype i = 0; i < rows.size(); ++i) {
    auto encoded = rows.at(i).toJson();
    if (!encoded)
      return failure(
          QStringLiteral("rows[%1]: %2").arg(i).arg(encoded.error()));
    result.append(*encoded);
  }
  return result;
}

ValueOrError<Json::Value>
encodeGameListToRawJson(const QList<GameListRow> &rows) {
  QList<Json::Value> result;
  result.reserve(rows.size());
  for (qsizetype i = 0; i < rows.size(); ++i) {
    auto encoded = rows.at(i).toRawJson();
    if (!encoded)
      return failure(
          QStringLiteral("rows[%1]: %2").arg(i).arg(encoded.error()));
    result.append(*encoded);
  }
  return Json::Value::makeArray(result);
}

ValueOrError<QByteArray>
encodeGameListToJsonBytes(const QList<GameListRow> &rows) {
  auto raw = encodeGameListToRawJson(rows);
  if (!raw)
    return failure(raw.error());
  return raw->toJsonBytes();
}

ValueOrError<CampaignOption>
CampaignOption::knownOption(KnownCampaignOption option) {
  auto encoded = Json::encodeClosedEnum(option, kKnownCampaignOptionTable);
  if (!encoded)
    return failure(
        QStringLiteral("CampaignOption::knownOption: %1").arg(encoded.error()));
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
  return fromValueImpl(v, path);
}

ValueOrError<CampaignOption> CampaignOption::fromRawJson(const Json::Value &v,
                                                         QStringView path) {
  return fromValueImpl(v, path);
}

ValueOrError<CampaignOption> CampaignOption::fromRawBytes(QByteArrayView bytes,
                                                          QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

template <typename V>
ValueOrError<CampaignOption> CampaignOption::fromValueImpl(const V &v,
                                                           QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto tagResult =
      Json::requireString(obj, "tag"_L1, Json::joinPath(path, u"tag"));
  if (!tagResult)
    return failure(tagResult.error());
  const QString &tag = *tagResult;

  // Captured once and preserved for EVERY kind below, not merely
  // Kind::Unknown: round 6's review found a known tag's response object
  // can carry additive/nullable fields (e.g. an explicit "contents": null
  // beside a nullary option, or a future sibling key) this client does
  // not model just as easily as an unrecognized tag can, and those must
  // survive too rather than being silently discarded at decode time.
  // toRequestOption() below inspects this to refuse narrowing past any
  // such loss instead of quietly resubmitting a "cleaner" request than
  // what was actually decoded.
  auto rawResult = toLosslessRaw(v);
  if (!rawResult)
    return failure(QStringLiteral("%1: %2").arg(path, rawResult.error()));

  if (tag == "CampaignVariant"_L1) {
    auto contents = Json::requireString(obj, "contents"_L1,
                                        Json::joinPath(path, u"contents"));
    if (!contents)
      return failure(contents.error());
    CampaignOption result;
    result.m_kind = Kind::Variant;
    result.m_text = *contents;
    result.m_raw = *rawResult;
    return result;
  }

  for (const auto &[wire, option] : kKnownCampaignOptionTable) {
    if (tag == wire) {
      CampaignOption result;
      result.m_kind = Kind::Known;
      result.m_known = option;
      result.m_raw = *rawResult;
      return result;
    }
  }

  // An unrecognized tag is preserved verbatim rather than rejected. The
  // *complete* decoded object (not merely "contents") is captured -- any
  // additive sibling key a future backend adds alongside "tag"/"contents"
  // survives too, and toJson() below re-emits it byte-for-byte rather than
  // reconstructing only the two keys this client currently knows about.
  CampaignOption result;
  result.m_kind = Kind::Unknown;
  result.m_text = tag;
  result.m_raw = *rawResult;
  return result;
}

ValueOrError<QJsonObject> CampaignOption::toJson() const {
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toQJson().toObject();
}

ValueOrError<Json::Value> CampaignOption::toRawJson() const {
  // A decoded instance (m_raw populated for Known/Variant/Unknown alike,
  // see fromValueImpl above) re-emits its complete original raw object
  // verbatim, so an additive sibling key or an explicit "contents": null
  // beside a known nullary tag survives a full round trip -- not merely
  // the "tag"/m_known this client itself derived from it. Only an
  // instance built via knownOption()/variantOption() (m_raw left
  // Undefined; nothing decoded to preserve) falls through to
  // reconstructing the minimal shape below.
  if (!m_raw.isUndefined())
    return m_raw;
  switch (m_kind) {
  case Kind::Known: {
    auto encoded = Json::encodeClosedEnum(*m_known, kKnownCampaignOptionTable);
    if (!encoded)
      return failure(
          QStringLiteral("CampaignOption::toRawJson: %1").arg(encoded.error()));
    return Json::Value::makeObject(
        {{QStringLiteral("tag"), Json::Value::makeString(*encoded)}});
  }
  case Kind::Variant:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("CampaignVariant"))},
         {QStringLiteral("contents"), Json::Value::makeString(m_text)}});
  case Kind::Unknown:
    // Unreachable: Kind::Unknown is only ever produced by fromValueImpl
    // above, which always populates m_raw and is therefore already
    // handled by the isUndefined() check -- there is no public factory
    // that can construct an Unknown instance with m_raw left Undefined.
    return failure(
        QStringLiteral("CampaignOption::toRawJson: unknown option with no "
                       "raw object to encode"));
  }
  // A typed failure -- rather than Q_UNREACHABLE_RETURN -- for an
  // out-of-range m_kind, matching every other case in this switch.
  return failure(
      QStringLiteral("CampaignOption::toRawJson: unhandled tag value"));
}

ValueOrError<QByteArray> CampaignOption::toJsonBytes() const {
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toJsonBytes();
}

ValueOrError<CampaignOptionRequest>
CampaignOption::toRequestOption(QStringView path) const {
  switch (m_kind) {
  case Kind::Known: {
    if (m_raw.isObject()) {
      auto exact = Json::requireExactKeys(m_raw, {"tag"_L1}, path);
      if (!exact) {
        QString knownName = QStringLiteral("<unrepresentable>");
        if (auto encoded =
                Json::encodeClosedEnum(*m_known, kKnownCampaignOptionTable))
          knownName = *encoded;
        return failure(
            QStringLiteral(
                "%1: known campaign option \"%2\" carries additive/unexpected "
                "field(s) beyond \"tag\" and cannot be narrowed to a request "
                "without silently discarding them (%3)")
                .arg(path, knownName, exact.error()));
      }
    }
    return CampaignOptionRequest::knownOption(*m_known);
  }
  case Kind::Variant: {
    if (m_raw.isObject()) {
      auto exact =
          Json::requireExactKeys(m_raw, {"tag"_L1, "contents"_L1}, path);
      if (!exact)
        return failure(
            QStringLiteral(
                "%1: campaign variant option carries additive/unexpected "
                "field(s) beyond \"tag\"/\"contents\" and cannot be narrowed "
                "to a request without silently discarding them (%2)")
                .arg(path, exact.error()));
    }
    return CampaignOptionRequest::variantOption(m_text);
  }
  case Kind::Unknown:
    return failure(QStringLiteral("%1: cannot submit unrecognized campaign "
                                  "option \"%2\" in a request")
                       .arg(path, m_text));
  }
  // A typed failure -- rather than Q_UNREACHABLE_RETURN -- for an
  // out-of-range m_kind, matching every other case in this switch.
  return failure(
      QStringLiteral("%1: CampaignOption::toRequestOption: unhandled tag value")
          .arg(path));
}

ValueOrError<CampaignOptionRequest>
CampaignOptionRequest::knownOption(KnownCampaignOption option) {
  auto encoded = Json::encodeClosedEnum(option, kKnownCampaignOptionTable);
  if (!encoded)
    return failure(QStringLiteral("CampaignOptionRequest::knownOption: %1")
                       .arg(encoded.error()));
  CampaignOptionRequest result;
  result.m_kind = Kind::Known;
  result.m_known = option;
  return result;
}

CampaignOptionRequest CampaignOptionRequest::variantOption(QString contents) {
  CampaignOptionRequest result;
  result.m_kind = Kind::Variant;
  result.m_text = std::move(contents);
  return result;
}

ValueOrError<CampaignOptionRequest>
CampaignOptionRequest::fromJson(const QJsonValue &v, QStringView path) {
  return fromValueImpl(v, path);
}

ValueOrError<CampaignOptionRequest>
CampaignOptionRequest::fromJson(const Json::Value &v, QStringView path) {
  return fromValueImpl(v, path);
}

template <typename V>
ValueOrError<CampaignOptionRequest>
CampaignOptionRequest::fromValueImpl(const V &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto tagResult =
      Json::requireString(obj, "tag"_L1, Json::joinPath(path, u"tag"));
  if (!tagResult)
    return failure(tagResult.error());
  const QString &tag = *tagResult;

  if (tag == "CampaignVariant"_L1) {
    // Direct request decode enforces the exact closed shape too: a
    // request-bound value has no additive-field tolerance, forward-
    // compatible or otherwise.
    auto exact = Json::requireExactKeys(obj, {"tag"_L1, "contents"_L1}, path);
    if (!exact)
      return failure(exact.error());
    auto contents = Json::requireString(obj, "contents"_L1,
                                        Json::joinPath(path, u"contents"));
    if (!contents)
      return failure(contents.error());
    return CampaignOptionRequest::variantOption(*contents);
  }

  for (const auto &[wire, option] : kKnownCampaignOptionTable) {
    if (tag == wire) {
      auto exact = Json::requireExactKeys(obj, {"tag"_L1}, path);
      if (!exact)
        return failure(exact.error());
      return CampaignOptionRequest::knownOption(option);
    }
  }

  // Unlike CampaignOption, a request-bound value has no forward-compatible
  // fallback: an unrecognized tag is a hard decode failure here.
  return failure(QStringLiteral("%1: unrecognized campaign option tag \"%2\"")
                     .arg(path, tag));
}

ValueOrError<QJsonObject> CampaignOptionRequest::toJson() const {
  // Composes toRawJson() below and its own bounded exact QJsonObject
  // conversion (see Value::toExactQJsonObject() in RawJson.h) rather than
  // hand-building a QJsonObject: the previous implementation embedded
  // Kind::Variant's `m_text` (an unconstrained caller-supplied string)
  // via a raw QJsonValue(QString) construction with zero validation, so a
  // lone/mismatched UTF-16 surrogate there would have silently produced a
  // normal-looking-but-invalid QJsonObject even though toJsonBytes()
  // correctly rejected the identical input. toRawJson()'s own error
  // already identifies itself, so it is propagated verbatim rather than
  // re-wrapped.
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toExactQJsonObject();
}

ValueOrError<Json::Value> CampaignOptionRequest::toRawJson() const {
  switch (m_kind) {
  case Kind::Known: {
    auto encoded = Json::encodeClosedEnum(*m_known, kKnownCampaignOptionTable);
    if (!encoded)
      return failure(QStringLiteral("CampaignOptionRequest::toRawJson: %1")
                         .arg(encoded.error()));
    return Json::Value::makeObject(
        {{QStringLiteral("tag"), Json::Value::makeString(*encoded)}});
  }
  case Kind::Variant:
    return Json::Value::makeObject(
        {{QStringLiteral("tag"),
          Json::Value::makeString(QStringLiteral("CampaignVariant"))},
         {QStringLiteral("contents"), Json::Value::makeString(m_text)}});
  }
  // A typed failure -- rather than Q_UNREACHABLE_RETURN -- for an
  // out-of-range m_kind, matching every other case in this switch.
  return failure(
      QStringLiteral("CampaignOptionRequest::toRawJson: unhandled tag value"));
}

ValueOrError<QByteArray> CampaignOptionRequest::toJsonBytes() const {
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toJsonBytes();
}

CampaignOrScenario CampaignOrScenario::campaign(CampaignId id) {
  CampaignOrScenario result;
  result.m_campaignId = std::move(id);
  return result;
}

CampaignOrScenario
CampaignOrScenario::campaignWithStartingScenario(CampaignId campaignId,
                                                 ScenarioId scenarioId) {
  CampaignOrScenario result;
  result.m_campaignId = std::move(campaignId);
  result.m_scenarioId = std::move(scenarioId);
  return result;
}

CampaignOrScenario CampaignOrScenario::scenario(ScenarioId id) {
  CampaignOrScenario result;
  result.m_scenarioId = std::move(id);
  return result;
}

ValueOrError<CampaignOrScenario>
CampaignOrScenario::fromJson(const QJsonObject &requestObj, QStringView path) {
  return fromValueImpl(requestObj, path);
}

ValueOrError<CampaignOrScenario>
CampaignOrScenario::fromJson(const Json::Value &requestObj, QStringView path) {
  return fromValueImpl(requestObj, path);
}

template <typename Obj>
ValueOrError<CampaignOrScenario>
CampaignOrScenario::fromValueImpl(const Obj &requestObj, QStringView path) {
  // Both keys collapse absent-and-null identically, matching CreateGamePost's
  // hand-written `.:?` parse for each.
  std::optional<CampaignId> campaignId;
  const auto campaignV = requestObj.value("campaignId"_L1);
  if (!campaignV.isUndefined() && !campaignV.isNull()) {
    auto result =
        CampaignId::fromJson(campaignV, Json::joinPath(path, u"campaignId"));
    if (!result)
      return failure(result.error());
    campaignId = *result;
  }

  std::optional<ScenarioId> scenarioId;
  const auto scenarioV = requestObj.value("scenarioId"_L1);
  if (!scenarioV.isUndefined() && !scenarioV.isNull()) {
    auto result =
        ScenarioId::fromJson(scenarioV, Json::joinPath(path, u"scenarioId"));
    if (!result)
      return failure(result.error());
    scenarioId = *result;
  }

  // The backend's only invalid combination is neither set (a campaign may
  // freely carry a starting scenario, and a standalone scenario carries no
  // campaign) -- see this class's doc comment.
  if (!campaignId.has_value() && !scenarioId.has_value())
    return failure(
        QStringLiteral("%1: at least one of \"campaignId\"/\"scenarioId\" "
                       "must be set")
            .arg(path));

  CampaignOrScenario result;
  result.m_campaignId = std::move(campaignId);
  result.m_scenarioId = std::move(scenarioId);
  return result;
}

void CampaignOrScenario::insertRawInto(
    QList<std::pair<QString, Json::Value>> &members) const {
  members.append({QStringLiteral("campaignId"),
                  m_campaignId ? Json::Value::makeString(m_campaignId->value())
                               : Json::Value::makeNull()});
  members.append({QStringLiteral("scenarioId"),
                  m_scenarioId ? Json::Value::makeString(m_scenarioId->value())
                               : Json::Value::makeNull()});
}

template <typename V>
ValueOrError<CreateGameRequest>
createGameRequestFromValueImpl(const V &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

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

  auto difficulty = Json::requireField(
      obj, "difficulty"_L1, Json::joinPath(path, u"difficulty"),
      [](const auto &v, QStringView p) {
        return Json::decodeClosedEnum(v, p, kDifficultyTable);
      });
  if (!difficulty)
    return failure(difficulty.error());

  auto campaignName = Json::requireString(
      obj, "campaignName"_L1, Json::joinPath(path, u"campaignName"));
  if (!campaignName)
    return failure(campaignName.error());

  auto multiplayerVariant = Json::requireField(
      obj, "multiplayerVariant"_L1, Json::joinPath(path, u"multiplayerVariant"),
      [](const auto &v, QStringView p) {
        return Json::decodeClosedEnum(v, p, kMultiplayerVariantTable);
      });
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
  QList<CampaignOptionRequest> options;
  options.reserve(optionsArr->size());
  for (qsizetype i = 0; i < optionsArr->size(); ++i) {
    auto item = CampaignOptionRequest::fromJson(
        (*optionsArr)[i], Json::indexPath(optionsPath, i));
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
  const auto ultimatumsV = obj.value("ultimatumsAndBoons"_L1);
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
  const auto achievementsV = obj.value("achievementsEnabled"_L1);
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

ValueOrError<CreateGameRequest> CreateGameRequest::fromJson(const QJsonValue &v,
                                                            QStringView path) {
  return createGameRequestFromValueImpl(v, path);
}

ValueOrError<CreateGameRequest>
CreateGameRequest::fromRawJson(const Json::Value &v, QStringView path) {
  return createGameRequestFromValueImpl(v, path);
}

ValueOrError<CreateGameRequest>
CreateGameRequest::fromRawBytes(QByteArrayView bytes, QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

ValueOrError<QJsonObject> CreateGameRequest::toJson() const {
  // Composes toRawJson() below and its own bounded exact QJsonObject
  // conversion (see Value::toExactQJsonObject() in RawJson.h) rather than
  // hand-building a QJsonObject: the previous implementation embedded
  // campaignName via a raw QJsonValue(QString) construction with zero
  // validation (and, transitively via a since-removed
  // CampaignOrScenario::insertInto() test-only fragment method --
  // round-16-cumulative-review item 1 -- campaignId/scenarioId the same
  // way), so a lone/mismatched UTF-16 surrogate there would have
  // silently produced a normal-looking-but-invalid QJsonObject even
  // though toJsonBytes() correctly rejected the identical input.
  // toRawJson() already validates deckIds' null-uuid invariant, so that
  // check is not duplicated here.
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toExactQJsonObject();
}

ValueOrError<Json::Value> CreateGameRequest::toRawJson() const {
  QList<Json::Value> deckIdsArr;
  deckIdsArr.reserve(deckIds.size());
  for (qsizetype i = 0; i < deckIds.size(); ++i) {
    const auto &id = deckIds.at(i);
    if (id && id->isNull())
      return failure(
          QStringLiteral("deckIds[%1]: must not be the null uuid").arg(i));
    deckIdsArr.append(
        id ? Json::Value::makeString(id->toString(QUuid::WithoutBraces))
           : Json::Value::makeNull());
  }
  QList<std::pair<QString, Json::Value>> members{
      {QStringLiteral("deckIds"), Json::Value::makeArray(deckIdsArr)},
      {QStringLiteral("playerCount"),
       Json::Value::makeNumber(Json::RawNumber::fromInt64(playerCount))},
  };
  campaignOrScenario.insertRawInto(members);
  auto difficultyEncoded = Json::encodeClosedEnum(difficulty, kDifficultyTable);
  if (!difficultyEncoded)
    return failure(
        QStringLiteral("difficulty: %1").arg(difficultyEncoded.error()));
  members.append({QStringLiteral("difficulty"),
                  Json::Value::makeString(*difficultyEncoded)});
  members.append(
      {QStringLiteral("campaignName"), Json::Value::makeString(campaignName)});
  auto multiplayerVariantEncoded =
      Json::encodeClosedEnum(multiplayerVariant, kMultiplayerVariantTable);
  if (!multiplayerVariantEncoded)
    return failure(QStringLiteral("multiplayerVariant: %1")
                       .arg(multiplayerVariantEncoded.error()));
  members.append({QStringLiteral("multiplayerVariant"),
                  Json::Value::makeString(*multiplayerVariantEncoded)});
  members.append({QStringLiteral("includeTarotReadings"),
                  Json::Value::makeBool(includeTarotReadings)});
  QList<Json::Value> optionsArr;
  optionsArr.reserve(options.size());
  for (qsizetype i = 0; i < options.size(); ++i) {
    auto encoded = options.at(i).toRawJson();
    if (!encoded)
      return failure(
          QStringLiteral("options[%1]: %2").arg(i).arg(encoded.error()));
    optionsArr.append(*encoded);
  }
  members.append(
      {QStringLiteral("options"), Json::Value::makeArray(optionsArr)});
  if (strictAsIfAt)
    members.append(
        {QStringLiteral("strictAsIfAt"), Json::Value::makeBool(*strictAsIfAt)});
  if (asIfRuling) {
    auto asIfRulingEncoded =
        Json::encodeClosedEnum(*asIfRuling, kAsIfRulingTable);
    if (!asIfRulingEncoded)
      return failure(
          QStringLiteral("asIfRuling: %1").arg(asIfRulingEncoded.error()));
    members.append({QStringLiteral("asIfRuling"),
                    Json::Value::makeString(*asIfRulingEncoded)});
  }
  QList<Json::Value> ultimatumsAndBoonsArr;
  ultimatumsAndBoonsArr.reserve(ultimatumsAndBoons.size());
  for (qsizetype i = 0; i < ultimatumsAndBoons.size(); ++i) {
    auto encoded =
        Json::encodeClosedEnum(ultimatumsAndBoons.at(i), kUltimatumOrBoonTable);
    if (!encoded)
      return failure(QStringLiteral("ultimatumsAndBoons[%1]: %2")
                         .arg(i)
                         .arg(encoded.error()));
    ultimatumsAndBoonsArr.append(Json::Value::makeString(*encoded));
  }
  members.append({QStringLiteral("ultimatumsAndBoons"),
                  Json::Value::makeArray(ultimatumsAndBoonsArr)});
  members.append({QStringLiteral("achievementsEnabled"),
                  Json::Value::makeBool(achievementsEnabled)});
  return Json::Value::makeObject(std::move(members));
}

ValueOrError<QByteArray> CreateGameRequest::toJsonBytes() const {
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toJsonBytes();
}

// Dispatch-shim pair for InvestigatorRef (see CardCatalog.cpp/Decks.cpp for
// the same pattern's rationale): picks InvestigatorRef::fromJson()'s
// QJsonValue-taking body, or a hand-written Json::Value equivalent, based
// on the deduced value-family template parameter in
// decodeChooseDeckRequest<Obj> below.
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

// Dispatch-shim pair for DeckListInput: picks fromJson()'s QJsonValue-
// taking body, or the precision-preserving fromRawJson() overload.
ValueOrError<DeckListInput> decodeDeckListInputValue(const QJsonValue &v,
                                                     QStringView path) {
  return DeckListInput::fromJson(v, path);
}
ValueOrError<DeckListInput> decodeDeckListInputValue(const Json::Value &v,
                                                     QStringView path) {
  return DeckListInput::fromRawJson(v, path);
}

// Shared decode body for ChooseDeckRequest::fromJson()/fromRawBytes(): Obj
// is QJsonObject or Json::Value. `deckList` (when present) decodes through
// decodeDeckListInputValue's Json::Value overload for the fromRawBytes()
// path, so a numeric literal nested inside its sideSlots survives exactly
// rather than only as closely as QJsonValue's double-backed storage
// allows -- no toQJson()-then-reparse round trip is needed, unlike the
// collapse-then-patch pattern this replaces.
template <typename Obj>
ValueOrError<ChooseDeckRequest> decodeChooseDeckRequest(const Obj &obj,
                                                        QStringView path) {
  auto investigatorId = Json::requireField(
      obj, "investigatorId"_L1, Json::joinPath(path, u"investigatorId"),
      [](const auto &v, QStringView p) {
        return decodeInvestigatorRefValue(v, p);
      });
  if (!investigatorId)
    return failure(investigatorId.error());

  auto deckUrl =
      Json::optionalString(obj, "deckUrl"_L1, Json::joinPath(path, u"deckUrl"));
  if (!deckUrl)
    return failure(deckUrl.error());

  std::optional<DeckListInput> deckList;
  if (Json::fieldPresence(obj, "deckList"_L1) == Json::FieldPresence::Present) {
    auto result = decodeDeckListInputValue(obj.value("deckList"_L1),
                                           Json::joinPath(path, u"deckList"));
    if (!result)
      return failure(result.error());
    deckList = *result;
  }

  return ChooseDeckRequest{.investigatorId = *investigatorId,
                           .deckUrl = *deckUrl,
                           .deckList = std::move(deckList)};
}

ValueOrError<ChooseDeckRequest> ChooseDeckRequest::fromJson(const QJsonValue &v,
                                                            QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  return decodeChooseDeckRequest(*objResult, path);
}

ValueOrError<Json::Value> ChooseDeckRequest::toRawJson() const {
  QList<std::pair<QString, Json::Value>> members{
      {QStringLiteral("investigatorId"),
       Json::Value::makeString(investigatorId.value())},
  };
  if (deckUrl)
    members.append(
        {QStringLiteral("deckUrl"), Json::Value::makeString(*deckUrl)});
  if (deckList) {
    auto deckListRaw = deckList->toRawJson();
    if (!deckListRaw)
      return failure(deckListRaw.error());
    members.append({QStringLiteral("deckList"), *deckListRaw});
  }
  return Json::Value::makeObject(std::move(members));
}

ValueOrError<QJsonObject> ChooseDeckRequest::toJson() const {
  // Composes toRawJson() above and its own bounded exact QJsonObject
  // conversion (see Value::toExactQJsonObject() in RawJson.h) rather than
  // hand-building a QJsonObject: the previous implementation embedded
  // investigatorId/deckUrl via raw, unvalidated QJsonValue construction
  // with zero validation, so a lone/mismatched UTF-16 surrogate there
  // would have silently produced a normal-looking-but-invalid QJsonObject
  // even though toJsonBytes() correctly rejected the identical input.
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toExactQJsonObject();
}

ValueOrError<ChooseDeckRequest>
ChooseDeckRequest::fromRawJson(const Json::Value &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  return decodeChooseDeckRequest(*objResult, path);
}

ValueOrError<ChooseDeckRequest>
ChooseDeckRequest::fromRawBytes(QByteArrayView bytes, QStringView path) {
  auto raw = Json::Value::parse(bytes, path);
  if (!raw)
    return failure(raw.error());
  return fromRawJson(*raw, path);
}

ValueOrError<QByteArray> ChooseDeckRequest::toJsonBytes() const {
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toJsonBytes();
}

// Shared decode body for ClaimSeatRequest::fromJson()/fromRawJson(): Obj is
// QJsonObject or Json::Value, matching decodeChooseDeckRequest's pattern
// above and reusing the same decodeInvestigatorRefValue dispatch-shim
// pair.
template <typename Obj>
ValueOrError<ClaimSeatRequest> decodeClaimSeatRequest(const Obj &obj,
                                                      QStringView path) {
  auto investigatorId = Json::requireField(
      obj, "investigatorId"_L1, Json::joinPath(path, u"investigatorId"),
      [](const auto &v, QStringView p) {
        return decodeInvestigatorRefValue(v, p);
      });
  if (!investigatorId)
    return failure(investigatorId.error());
  return ClaimSeatRequest{.investigatorId = *investigatorId};
}

ValueOrError<ClaimSeatRequest> ClaimSeatRequest::fromJson(const QJsonValue &v,
                                                          QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  return decodeClaimSeatRequest(*objResult, path);
}

ValueOrError<QJsonObject> ClaimSeatRequest::toJson() const {
  // Composes toRawJson() below and its own bounded exact QJsonObject
  // conversion (see Value::toExactQJsonObject() in RawJson.h) rather than
  // embedding investigatorId via a raw, unvalidated QJsonValue(QString)
  // construction, so a lone/mismatched UTF-16 surrogate is a typed
  // failure here too, matching toJsonBytes().
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toExactQJsonObject();
}

ValueOrError<ClaimSeatRequest>
ClaimSeatRequest::fromRawJson(const Json::Value &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  return decodeClaimSeatRequest(*objResult, path);
}

ValueOrError<ClaimSeatRequest>
ClaimSeatRequest::fromRawBytes(QByteArrayView bytes, QStringView path) {
  auto raw = Json::Value::parse(bytes, path);
  if (!raw)
    return failure(raw.error());
  return fromRawJson(*raw, path);
}

ValueOrError<Json::Value> ClaimSeatRequest::toRawJson() const {
  return Json::Value::makeObject(
      {{QStringLiteral("investigatorId"),
        Json::Value::makeString(investigatorId.value())}});
}

ValueOrError<QByteArray> ClaimSeatRequest::toJsonBytes() const {
  auto raw = toRawJson();
  if (!raw)
    return failure(raw.error());
  return raw->toJsonBytes();
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

ValueOrError<QJsonArray> encodeOpenSeats(const QList<CardCode> &seats) {
  QJsonArray result;
  for (qsizetype i = 0; i < seats.size(); ++i) {
    auto encoded = seats.at(i).toJson();
    if (!encoded)
      return failure(QStringLiteral("[%1]: %2").arg(i).arg(encoded.error()));
    result.append(*encoded);
  }
  return result;
}

} // namespace Arkham
