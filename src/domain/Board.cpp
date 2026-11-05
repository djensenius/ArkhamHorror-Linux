#include "Board.h"

#include "JsonDecode.h"

#include <QSet>
#include <array>
#include <utility>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

constexpr std::array<std::pair<QLatin1StringView, Phase>, 6> kPhaseTable{{
    {"MythosPhase"_L1, Phase::MythosPhase},
    {"InvestigationPhase"_L1, Phase::InvestigationPhase},
    {"EnemyPhase"_L1, Phase::EnemyPhase},
    {"UpkeepPhase"_L1, Phase::UpkeepPhase},
    {"ResolutionPhase"_L1, Phase::ResolutionPhase},
    {"CampaignPhase"_L1, Phase::CampaignPhase},
}};

constexpr std::array<std::pair<QLatin1StringView, PhaseStep::Kind>, 4>
    kPhaseStepKindTable{{
        {"MythosPhaseStep"_L1, PhaseStep::Kind::MythosPhaseStep},
        {"InvestigationPhaseStep"_L1, PhaseStep::Kind::InvestigationPhaseStep},
        {"EnemyPhaseStep"_L1, PhaseStep::Kind::EnemyPhaseStep},
        {"UpkeepPhaseStep"_L1, PhaseStep::Kind::UpkeepPhaseStep},
    }};

// One table per PhaseStep::Kind, so a sub-step is only accepted under its
// own tag (phase-step.schema.json's per-tag `contents` enums).
constexpr std::array<std::pair<QLatin1StringView, PhaseStep::Step>, 6>
    kMythosStepTable{{
        {"MythosPhaseBeginsStep"_L1, PhaseStep::Step::MythosPhaseBeginsStep},
        {"PlaceDoomOnAgendaStep"_L1, PhaseStep::Step::PlaceDoomOnAgendaStep},
        {"CheckDoomThresholdStep"_L1, PhaseStep::Step::CheckDoomThresholdStep},
        {"EachInvestigatorDrawsEncounterCardStep"_L1,
         PhaseStep::Step::EachInvestigatorDrawsEncounterCardStep},
        {"MythosPhaseWindow"_L1, PhaseStep::Step::MythosPhaseWindow},
        {"MythosPhaseEndsStep"_L1, PhaseStep::Step::MythosPhaseEndsStep},
    }};

constexpr std::array<std::pair<QLatin1StringView, PhaseStep::Step>, 7>
    kInvestigationStepTable{{
        {"InvestigationPhaseBeginsStep"_L1,
         PhaseStep::Step::InvestigationPhaseBeginsStep},
        {"InvestigationPhaseBeginsWindow"_L1,
         PhaseStep::Step::InvestigationPhaseBeginsWindow},
        {"NextInvestigatorsTurnBeginsStep"_L1,
         PhaseStep::Step::NextInvestigatorsTurnBeginsStep},
        {"NextInvestigatorsTurnBeginsWindow"_L1,
         PhaseStep::Step::NextInvestigatorsTurnBeginsWindow},
        {"InvestigatorTakesActionStep"_L1,
         PhaseStep::Step::InvestigatorTakesActionStep},
        {"InvestigatorsTurnEndsStep"_L1,
         PhaseStep::Step::InvestigatorsTurnEndsStep},
        {"InvestigationPhaseEndsStep"_L1,
         PhaseStep::Step::InvestigationPhaseEndsStep},
    }};

constexpr std::array<std::pair<QLatin1StringView, PhaseStep::Step>, 6>
    kEnemyStepTable{{
        {"EnemyPhaseBeginsStep"_L1, PhaseStep::Step::EnemyPhaseBeginsStep},
        {"HunterEnemiesMoveStep"_L1, PhaseStep::Step::HunterEnemiesMoveStep},
        {"ResolveAttacksWindow"_L1, PhaseStep::Step::ResolveAttacksWindow},
        {"ResolveAttacksStep"_L1, PhaseStep::Step::ResolveAttacksStep},
        {"AfterResolveAttacksWindow"_L1,
         PhaseStep::Step::AfterResolveAttacksWindow},
        {"EnemyPhaseEndsStep"_L1, PhaseStep::Step::EnemyPhaseEndsStep},
    }};

constexpr std::array<std::pair<QLatin1StringView, PhaseStep::Step>, 7>
    kUpkeepStepTable{{
        {"UpkeepPhaseBeginsStep"_L1, PhaseStep::Step::UpkeepPhaseBeginsStep},
        {"UpkeepPhaseBeginsWindow"_L1,
         PhaseStep::Step::UpkeepPhaseBeginsWindow},
        {"ResetActionsStep"_L1, PhaseStep::Step::ResetActionsStep},
        {"ReadyExhaustedStep"_L1, PhaseStep::Step::ReadyExhaustedStep},
        {"DrawCardAndGainResourceStep"_L1,
         PhaseStep::Step::DrawCardAndGainResourceStep},
        {"CheckHandSizeStep"_L1, PhaseStep::Step::CheckHandSizeStep},
        {"UpkeepPhaseEndsStep"_L1, PhaseStep::Step::UpkeepPhaseEndsStep},
    }};

constexpr std::array<std::pair<QLatin1StringView, ActSide>, 8> kActSideTable{{
    {"A"_L1, ActSide::A},
    {"B"_L1, ActSide::B},
    {"C"_L1, ActSide::C},
    {"D"_L1, ActSide::D},
    {"E"_L1, ActSide::E},
    {"F"_L1, ActSide::F},
    {"G"_L1, ActSide::G},
    {"H"_L1, ActSide::H},
}};

constexpr std::array<std::pair<QLatin1StringView, AgendaSide>, 4>
    kAgendaSideTable{{
        {"A"_L1, AgendaSide::A},
        {"B"_L1, AgendaSide::B},
        {"C"_L1, AgendaSide::C},
        {"D"_L1, AgendaSide::D},
    }};

constexpr std::array<std::pair<QLatin1StringView, LocationSymbol>, 16>
    kLocationSymbolTable{{
        {"Circle"_L1, LocationSymbol::Circle},
        {"Square"_L1, LocationSymbol::Square},
        {"Triangle"_L1, LocationSymbol::Triangle},
        {"Plus"_L1, LocationSymbol::Plus},
        {"Diamond"_L1, LocationSymbol::Diamond},
        {"Squiggle"_L1, LocationSymbol::Squiggle},
        {"Moon"_L1, LocationSymbol::Moon},
        {"Hourglass"_L1, LocationSymbol::Hourglass},
        {"T"_L1, LocationSymbol::T},
        {"Equals"_L1, LocationSymbol::Equals},
        {"Heart"_L1, LocationSymbol::Heart},
        {"Star"_L1, LocationSymbol::Star},
        {"Droplet"_L1, LocationSymbol::Droplet},
        {"Trefoil"_L1, LocationSymbol::Trefoil},
        {"Spade"_L1, LocationSymbol::Spade},
        {"NoSymbol"_L1, LocationSymbol::NoSymbol},
    }};

// investigator.schema.json's `class` enum (Arkham.ClassSymbol).
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

// Arkham.Token core names the board reads counts for.
constexpr auto kClueToken = u"Clue";
constexpr auto kDoomToken = u"Doom";
constexpr auto kDamageToken = u"Damage";
constexpr auto kHorrorToken = u"Horror";
constexpr auto kResourceToken = u"Resource";

// The all-zero UUID, the only spelling the backend's UUID grammar gives it.
constexpr auto kNilUuid = "00000000-0000-0000-0000-000000000000"_L1;

ValueOrError<PhaseStep::Step> decodeStepForKind(PhaseStep::Kind kind,
                                                const Json::Value &v,
                                                QStringView path) {
  switch (kind) {
  case PhaseStep::Kind::MythosPhaseStep:
    return Json::decodeClosedEnum(v, path, kMythosStepTable);
  case PhaseStep::Kind::InvestigationPhaseStep:
    return Json::decodeClosedEnum(v, path, kInvestigationStepTable);
  case PhaseStep::Kind::EnemyPhaseStep:
    return Json::decodeClosedEnum(v, path, kEnemyStepTable);
  case PhaseStep::Kind::UpkeepPhaseStep:
    return Json::decodeClosedEnum(v, path, kUpkeepStepTable);
  }
  return failure(QStringLiteral("%1: unsupported phase step kind").arg(path));
}

// act.schema.json/agenda.schema.json `id` and chaos-token.schema.json
// `chaosTokenRevealedBy` narrow a card code to `^c[0-9a-z:._-]+$`, stricter
// than CardCode's catalog grammar (`^c.+$`), which stays unchanged for the
// catalog. Decodes through CardCode::fromJson() (string type, 'c' prefix,
// non-empty tail) and then checks the board character set: ASCII digits,
// lowercase ASCII letters and ':', '.', '_', '-'.
ValueOrError<CardCode> decodeBoardCardCode(const Json::Value &v,
                                           QStringView path) {
  auto code = CardCode::fromJson(v, path);
  if (!code)
    return failure(code.error());
  const QString &text = code->value();
  for (qsizetype i = 1; i < text.size(); ++i) {
    const char16_t c = text.at(i).unicode();
    const bool allowed = (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z') ||
                         c == u':' || c == u'.' || c == u'_' || c == u'-';
    if (!allowed)
      return failure(
          QStringLiteral("%1: card code must match ^c[0-9a-z:._-]+$: \"%2\"")
              .arg(path, text));
  }
  return code;
}

ValueOrError<CardCode> requireBoardCardCode(const Json::Value &obj,
                                            QLatin1StringView key,
                                            QStringView path) {
  return Json::requireField(obj, key, path, decodeBoardCardCode);
}

ValueOrError<CardId> requireCardId(const Json::Value &obj,
                                   QLatin1StringView key, QStringView path) {
  return Json::requireField(obj, key, path,
                            [](const Json::Value &v, QStringView p) {
                              return CardId::fromJson(v, p);
                            });
}

// "Mutated" followed by one or more ASCII digits, e.g. "Mutated21".
bool isMutationName(const QString &text) {
  constexpr QLatin1StringView kPrefix = "Mutated"_L1;
  if (!text.startsWith(kPrefix) || text.size() == kPrefix.size())
    return false;
  for (qsizetype i = kPrefix.size(); i < text.size(); ++i) {
    const char16_t c = text.at(i).unicode();
    if (c < u'0' || c > u'9')
      return false;
  }
  return true;
}

// Decodes a required array of chaos tokens, in wire order.
ValueOrError<QList<ChaosToken>> requireChaosTokens(const Json::Value &obj,
                                                   QLatin1StringView key,
                                                   QStringView path) {
  auto arr = Json::requireArrayField(obj, key, path);
  if (!arr)
    return failure(arr.error());
  QList<ChaosToken> tokens;
  tokens.reserve(arr->size());
  for (qsizetype i = 0; i < arr->size(); ++i) {
    auto token = ChaosToken::fromRawJson((*arr)[i], Json::indexPath(path, i));
    if (!token)
      return failure(token.error());
    tokens.append(*token);
  }
  return tokens;
}

// A required JSON array of TypedId UUIDs (e.g. LocationId), in wire order.
template <typename Id>
ValueOrError<QList<Id>> requireIdArray(const Json::Value &obj,
                                       QLatin1StringView key,
                                       QStringView path) {
  auto arr = Json::requireArrayField(obj, key, path);
  if (!arr)
    return failure(arr.error());
  QList<Id> ids;
  ids.reserve(arr->size());
  for (qsizetype i = 0; i < arr->size(); ++i) {
    auto id = Id::fromJson((*arr)[i], Json::indexPath(path, i));
    if (!id)
      return failure(id.error());
    ids.append(*id);
  }
  return ids;
}

// A required JSON array of board card codes, in wire order.
ValueOrError<QList<CardCode>> requireBoardCardCodeArray(const Json::Value &obj,
                                                        QLatin1StringView key,
                                                        QStringView path) {
  auto arr = Json::requireArrayField(obj, key, path);
  if (!arr)
    return failure(arr.error());
  QList<CardCode> codes;
  codes.reserve(arr->size());
  for (qsizetype i = 0; i < arr->size(); ++i) {
    auto code = decodeBoardCardCode((*arr)[i], Json::indexPath(path, i));
    if (!code)
      return failure(code.error());
    codes.append(*code);
  }
  return codes;
}

// A required key whose value is a GameValue or JSON null.
ValueOrError<std::optional<GameValue>>
requireNullableGameValue(const Json::Value &obj, QLatin1StringView key,
                         QStringView path) {
  return Json::requireField(obj, key, path,
                            [](const Json::Value &value, QStringView p)
                                -> ValueOrError<std::optional<GameValue>> {
                              if (value.isNull())
                                return std::optional<GameValue>{};
                              auto gameValue = GameValue::fromRawJson(value, p);
                              if (!gameValue)
                                return failure(gameValue.error());
                              return std::optional<GameValue>{*gameValue};
                            });
}

ValueOrError<GameValue> requireGameValue(const Json::Value &obj,
                                         QLatin1StringView key,
                                         QStringView path) {
  return Json::requireField(obj, key, path,
                            [](const Json::Value &value, QStringView p) {
                              return GameValue::fromRawJson(value, p);
                            });
}

ValueOrError<LocationSymbol> requireLocationSymbol(const Json::Value &obj,
                                                   QLatin1StringView key,
                                                   QStringView path) {
  return Json::requireField(
      obj, key, path, [](const Json::Value &value, QStringView p) {
        return Json::decodeClosedEnum(value, p, kLocationSymbolTable);
      });
}

ValueOrError<TokenCounts> requireTokenCounts(const Json::Value &obj,
                                             QLatin1StringView key,
                                             QStringView path) {
  return Json::requireField(obj, key, path,
                            [](const Json::Value &value, QStringView p) {
                              return TokenCounts::fromRawJson(value, p);
                            });
}

ValueOrError<qint64> requireNonNegativeInt(const Json::Value &obj,
                                           QLatin1StringView key,
                                           QStringView path) {
  auto value = Json::requireInt(obj, key, path);
  if (!value)
    return failure(value.error());
  if (*value < 0)
    return failure(QStringLiteral("%1: must not be negative, got %2")
                       .arg(path)
                       .arg(*value));
  return *value;
}

// A required JSON object mapping each entity's identity (the member name,
// decoded by `decodeKey`) to the entity itself (Entity::fromRawJson). Every
// decoded key must be unique, so two spellings of one UUID are rejected,
// and must equal the entity's own id (`idOf`). Returns the entities in wire
// order.
template <typename Entity, typename KeyDecoder, typename IdOf>
ValueOrError<QList<Entity>>
requireEntityMap(const Json::Value &obj, QLatin1StringView key,
                 QStringView path, KeyDecoder decodeKey, IdOf idOf) {
  auto map = Json::requireObjectField(obj, key, path);
  if (!map)
    return failure(map.error());
  const auto &members = map->members();
  QList<Entity> entities;
  entities.reserve(members.size());
  // Decoded keys seen so far, for an amortized O(1) duplicate check.
  QSet<QString> seen;
  seen.reserve(members.size());
  for (const auto &[name, value] : members) {
    const QString entryPath = Json::joinPath(path, name);
    auto id = decodeKey(Json::Value::makeString(name), entryPath);
    if (!id)
      return failure(id.error());
    if (seen.contains(id->value()))
      return failure(QStringLiteral("%1: duplicate key \"%2\"")
                         .arg(entryPath, id->value()));
    seen.insert(id->value());
    auto entity = Entity::fromRawJson(value, entryPath);
    if (!entity)
      return failure(entity.error());
    if (!(idOf(*entity) == *id))
      return failure(
          QStringLiteral("%1: map key \"%2\" does not match the entry's id "
                         "\"%3\"")
              .arg(entryPath, name, idOf(*entity).value()));
    entities.append(*entity);
  }
  return entities;
}

// Linear lookup by id; the board maps are small (a scenario's locations).
template <typename Entity, typename Id>
const Entity *findById(const QList<Entity> &entities, const Id &id) noexcept {
  for (const Entity &entity : entities) {
    if (entity.id() == id)
      return &entity;
  }
  return nullptr;
}

} // namespace

ChaosToken::ChaosToken(ChaosTokenId id, QString face,
                       std::optional<CardCode> revealedBy, bool cancelled,
                       bool sealed)
    : m_id(std::move(id)), m_face(std::move(face)),
      m_revealedBy(std::move(revealedBy)), m_cancelled(cancelled),
      m_sealed(sealed) {}

ValueOrError<ChaosToken> ChaosToken::fromRawJson(const Json::Value &v,
                                                 QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto id = Json::requireField(obj, "chaosTokenId"_L1,
                               Json::joinPath(path, u"chaosTokenId"),
                               [](const Json::Value &value, QStringView p) {
                                 return ChaosTokenId::fromJson(value, p);
                               });
  if (!id)
    return failure(id.error());

  auto face = Json::requireString(obj, "chaosTokenFace"_L1,
                                  Json::joinPath(path, u"chaosTokenFace"));
  if (!face)
    return failure(face.error());

  auto revealedBy = Json::requireField(
      obj, "chaosTokenRevealedBy"_L1,
      Json::joinPath(path, u"chaosTokenRevealedBy"),
      [](const Json::Value &value,
         QStringView p) -> ValueOrError<std::optional<CardCode>> {
        if (value.isNull())
          return std::optional<CardCode>{};
        auto code = decodeBoardCardCode(value, p);
        if (!code)
          return failure(code.error());
        return std::optional<CardCode>{*code};
      });
  if (!revealedBy)
    return failure(revealedBy.error());

  auto cancelled =
      Json::requireBool(obj, "chaosTokenCancelled"_L1,
                        Json::joinPath(path, u"chaosTokenCancelled"));
  if (!cancelled)
    return failure(cancelled.error());

  auto sealed = Json::requireBool(obj, "chaosTokenSealed"_L1,
                                  Json::joinPath(path, u"chaosTokenSealed"));
  if (!sealed)
    return failure(sealed.error());

  return ChaosToken(*id, *face, *revealedBy, *cancelled, *sealed);
}

ValueOrError<ChaosToken> ChaosToken::fromRawBytes(QByteArrayView bytes,
                                                  QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

ValueOrError<ChaosBag> ChaosBag::fromRawJson(const Json::Value &v,
                                             QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto chaosTokens = requireChaosTokens(obj, "chaosTokens"_L1,
                                        Json::joinPath(path, u"chaosTokens"));
  if (!chaosTokens)
    return failure(chaosTokens.error());

  auto revealed =
      requireChaosTokens(obj, "revealedChaosTokens"_L1,
                         Json::joinPath(path, u"revealedChaosTokens"));
  if (!revealed)
    return failure(revealed.error());

  ChaosBag result;
  result.m_chaosTokens = *chaosTokens;
  result.m_revealedChaosTokens = *revealed;
  return result;
}

ValueOrError<ChaosBag> ChaosBag::fromRawBytes(QByteArrayView bytes,
                                              QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

ValueOrError<Phase> decodePhase(const Json::Value &v, QStringView path) {
  return Json::decodeClosedEnum(v, path, kPhaseTable);
}

ValueOrError<PhaseStep> PhaseStep::fromRawJson(const Json::Value &v,
                                               QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto kind = Json::requireField(obj, "tag"_L1, Json::joinPath(path, u"tag"),
                                 [](const Json::Value &value, QStringView p) {
                                   return Json::decodeClosedEnum(
                                       value, p, kPhaseStepKindTable);
                                 });
  if (!kind)
    return failure(kind.error());

  const Kind decodedKind = *kind;
  auto step = Json::requireField(
      obj, "contents"_L1, Json::joinPath(path, u"contents"),
      [decodedKind](const Json::Value &value, QStringView p) {
        return decodeStepForKind(decodedKind, value, p);
      });
  if (!step)
    return failure(step.error());

  return PhaseStep(decodedKind, *step);
}

ValueOrError<PhaseStep> PhaseStep::fromRawBytes(QByteArrayView bytes,
                                                QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

Act::Act(CardCode id, CardId cardId, qint64 sequenceStep, ActSide sequenceSide,
         qint64 deckId, bool flipped, Json::Value advanceCost)
    : m_id(std::move(id)), m_cardId(std::move(cardId)),
      m_sequenceStep(sequenceStep), m_sequenceSide(sequenceSide),
      m_deckId(deckId), m_flipped(flipped),
      m_advanceCost(std::move(advanceCost)) {}

ValueOrError<Act> Act::fromRawJson(const Json::Value &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto id = requireBoardCardCode(obj, "id"_L1, Json::joinPath(path, u"id"));
  if (!id)
    return failure(id.error());

  auto cardId =
      requireCardId(obj, "cardId"_L1, Json::joinPath(path, u"cardId"));
  if (!cardId)
    return failure(cardId.error());

  // [step, side]: exactly two elements.
  const QString sequencePath = Json::joinPath(path, u"sequence");
  auto sequence = Json::requireArrayField(obj, "sequence"_L1, sequencePath);
  if (!sequence)
    return failure(sequence.error());
  if (sequence->size() != 2)
    return failure(QStringLiteral("%1: expected a 2-element array, got %2 "
                                  "elements")
                       .arg(sequencePath)
                       .arg(sequence->size()));
  auto sequenceStep =
      Json::requireIntValue((*sequence)[0], Json::indexPath(sequencePath, 0));
  if (!sequenceStep)
    return failure(sequenceStep.error());
  auto sequenceSide = Json::decodeClosedEnum(
      (*sequence)[1], Json::indexPath(sequencePath, 1), kActSideTable);
  if (!sequenceSide)
    return failure(sequenceSide.error());

  auto deckId =
      Json::requireInt(obj, "deckId"_L1, Json::joinPath(path, u"deckId"));
  if (!deckId)
    return failure(deckId.error());

  auto flipped =
      Json::requireBool(obj, "flipped"_L1, Json::joinPath(path, u"flipped"));
  if (!flipped)
    return failure(flipped.error());

  const QString advanceCostPath = Json::joinPath(path, u"advanceCost");
  auto advanceCost =
      Json::requireRawField(obj, "advanceCost"_L1, advanceCostPath);
  if (!advanceCost)
    return failure(advanceCost.error());
  if (!advanceCost->isObject() && !advanceCost->isNull())
    return failure(QStringLiteral("%1: expected object or null, got %2")
                       .arg(advanceCostPath, Json::typeName(*advanceCost)));

  return Act(*id, *cardId, *sequenceStep, *sequenceSide, *deckId, *flipped,
             *advanceCost);
}

ValueOrError<Act> Act::fromRawBytes(QByteArrayView bytes, QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

Agenda::Agenda(CardCode id, CardId cardId, qint64 sequenceStep,
               AgendaSide sequenceSide, qint64 deckId, qint64 doom,
               std::optional<GameValue> doomThreshold, bool flipped)
    : m_id(std::move(id)), m_cardId(std::move(cardId)),
      m_sequenceStep(sequenceStep), m_sequenceSide(sequenceSide),
      m_deckId(deckId), m_doom(doom), m_doomThreshold(std::move(doomThreshold)),
      m_flipped(flipped) {}

ValueOrError<Agenda> Agenda::fromRawJson(const Json::Value &v,
                                         QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto id = requireBoardCardCode(obj, "id"_L1, Json::joinPath(path, u"id"));
  if (!id)
    return failure(id.error());

  auto cardId =
      requireCardId(obj, "cardId"_L1, Json::joinPath(path, u"cardId"));
  if (!cardId)
    return failure(cardId.error());

  const QString sequencePath = Json::joinPath(path, u"sequence");
  auto sequence = Json::requireObjectField(obj, "sequence"_L1, sequencePath);
  if (!sequence)
    return failure(sequence.error());
  auto sequenceSide = Json::requireField(
      *sequence, "agendaSequenceSide"_L1,
      Json::joinPath(sequencePath, u"agendaSequenceSide"),
      [](const Json::Value &value, QStringView p) {
        return Json::decodeClosedEnum(value, p, kAgendaSideTable);
      });
  if (!sequenceSide)
    return failure(sequenceSide.error());
  auto sequenceStep =
      Json::requireInt(*sequence, "agendaSequenceStep"_L1,
                       Json::joinPath(sequencePath, u"agendaSequenceStep"));
  if (!sequenceStep)
    return failure(sequenceStep.error());

  auto deckId =
      Json::requireInt(obj, "deckId"_L1, Json::joinPath(path, u"deckId"));
  if (!deckId)
    return failure(deckId.error());

  const QString doomPath = Json::joinPath(path, u"doom");
  auto doom = Json::requireInt(obj, "doom"_L1, doomPath);
  if (!doom)
    return failure(doom.error());
  if (*doom < 0)
    return failure(QStringLiteral("%1: must not be negative, got %2")
                       .arg(doomPath)
                       .arg(*doom));

  auto doomThreshold = Json::requireField(
      obj, "doomThreshold"_L1, Json::joinPath(path, u"doomThreshold"),
      [](const Json::Value &value,
         QStringView p) -> ValueOrError<std::optional<GameValue>> {
        if (value.isNull())
          return std::optional<GameValue>{};
        auto gameValue = GameValue::fromRawJson(value, p);
        if (!gameValue)
          return failure(gameValue.error());
        return std::optional<GameValue>{*gameValue};
      });
  if (!doomThreshold)
    return failure(doomThreshold.error());

  auto flipped =
      Json::requireBool(obj, "flipped"_L1, Json::joinPath(path, u"flipped"));
  if (!flipped)
    return failure(flipped.error());

  return Agenda(*id, *cardId, *sequenceStep, *sequenceSide, *deckId, *doom,
                *doomThreshold, *flipped);
}

ValueOrError<Agenda> Agenda::fromRawBytes(QByteArrayView bytes,
                                          QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

ValueOrError<TokenCounts> TokenCounts::fromRawJson(const Json::Value &v,
                                                   QStringView path) {
  auto arr = Json::requireArray(v, path);
  if (!arr)
    return failure(arr.error());

  TokenCounts result;
  result.m_entries.reserve(arr->size());
  // Names seen so far, so the duplicate check is amortized O(1) per entry
  // instead of a rescan of m_entries (O(n^2) up to the parser's 20,000
  // array elements); m_entries itself keeps wire order.
  QSet<QString> seen;
  seen.reserve(arr->size());
  for (qsizetype i = 0; i < arr->size(); ++i) {
    const QString pairPath = Json::indexPath(path, i);
    auto pair = Json::requireArray((*arr)[i], pairPath);
    if (!pair)
      return failure(pair.error());
    if (pair->size() != 2)
      return failure(QStringLiteral("%1: expected a 2-element array, got %2 "
                                    "elements")
                         .arg(pairPath)
                         .arg(pair->size()));
    auto name =
        Json::requireStringValue((*pair)[0], Json::indexPath(pairPath, 0));
    if (!name)
      return failure(name.error());
    const QString countPath = Json::indexPath(pairPath, 1);
    auto count = Json::requireIntValue((*pair)[1], countPath);
    if (!count)
      return failure(count.error());
    if (*count < 0)
      return failure(QStringLiteral("%1: must not be negative, got %2")
                         .arg(countPath)
                         .arg(*count));
    if (seen.contains(*name))
      return failure(QStringLiteral("%1: duplicate token \"%2\"")
                         .arg(Json::indexPath(pairPath, 0), *name));
    seen.insert(*name);
    result.m_entries.append({*name, *count});
  }
  return result;
}

ValueOrError<TokenCounts> TokenCounts::fromRawBytes(QByteArrayView bytes,
                                                    QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

qint64 TokenCounts::count(QStringView name) const noexcept {
  for (const auto &[entryName, entryCount] : m_entries) {
    if (entryName == name)
      return entryCount;
  }
  return 0;
}

Location::Location(LocationId id, CardCode cardCode, CardId cardId,
                   TokenCounts tokens)
    : m_id(std::move(id)), m_cardCode(std::move(cardCode)),
      m_cardId(std::move(cardId)), m_tokens(std::move(tokens)) {}

qint64 Location::clues() const noexcept { return m_tokens.count(kClueToken); }
qint64 Location::doom() const noexcept { return m_tokens.count(kDoomToken); }
qint64 Location::damage() const noexcept {
  return m_tokens.count(kDamageToken);
}
qint64 Location::horror() const noexcept {
  return m_tokens.count(kHorrorToken);
}
qint64 Location::resources() const noexcept {
  return m_tokens.count(kResourceToken);
}

ValueOrError<Location> Location::fromRawJson(const Json::Value &v,
                                             QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  // The enemy-location branch's literal discriminator: absent for an
  // ordinary location, `true` for an enemy location.
  const QString enemyLocationPath = Json::joinPath(path, u"enemyLocation");
  auto enemyLocation =
      Json::optionalNonNullBool(obj, "enemyLocation"_L1, enemyLocationPath);
  if (!enemyLocation)
    return failure(enemyLocation.error());
  if (enemyLocation->has_value() && !**enemyLocation)
    return failure(QStringLiteral("%1: expected true or absent, got false")
                       .arg(enemyLocationPath));
  const Kind kind =
      enemyLocation->has_value() ? Kind::EnemyLocation : Kind::Ordinary;

  auto id = Json::requireField(obj, "id"_L1, Json::joinPath(path, u"id"),
                               [](const Json::Value &value, QStringView p) {
                                 return LocationId::fromJson(value, p);
                               });
  if (!id)
    return failure(id.error());

  auto cardCode = requireBoardCardCode(obj, "cardCode"_L1,
                                       Json::joinPath(path, u"cardCode"));
  if (!cardCode)
    return failure(cardCode.error());

  auto cardId =
      requireCardId(obj, "cardId"_L1, Json::joinPath(path, u"cardId"));
  if (!cardId)
    return failure(cardId.error());

  auto label =
      Json::requireString(obj, "label"_L1, Json::joinPath(path, u"label"));
  if (!label)
    return failure(label.error());

  auto revealed =
      Json::requireBool(obj, "revealed"_L1, Json::joinPath(path, u"revealed"));
  if (!revealed)
    return failure(revealed.error());

  auto tokens =
      requireTokenCounts(obj, "tokens"_L1, Json::joinPath(path, u"tokens"));
  if (!tokens)
    return failure(tokens.error());

  auto shroud = requireNullableGameValue(obj, "shroud"_L1,
                                         Json::joinPath(path, u"shroud"));
  if (!shroud)
    return failure(shroud.error());

  auto connectedLocations =
      requireIdArray<LocationId>(obj, "connectedLocations"_L1,
                                 Json::joinPath(path, u"connectedLocations"));
  if (!connectedLocations)
    return failure(connectedLocations.error());

  auto investigators = requireBoardCardCodeArray(
      obj, "investigators"_L1, Json::joinPath(path, u"investigators"));
  if (!investigators)
    return failure(investigators.error());

  Location result(*id, *cardCode, *cardId, *tokens);
  result.m_kind = kind;
  result.m_label = *label;
  result.m_revealed = *revealed;
  result.m_shroud = *shroud;
  result.m_connectedLocations = *connectedLocations;
  result.m_investigators = *investigators;

  if (kind == Kind::EnemyLocation) {
    auto exhausted = Json::requireBool(obj, "exhausted"_L1,
                                       Json::joinPath(path, u"exhausted"));
    if (!exhausted)
      return failure(exhausted.error());
    result.m_exhausted = *exhausted;
    return result;
  }

  auto revealClues = requireGameValue(obj, "revealClues"_L1,
                                      Json::joinPath(path, u"revealClues"));
  if (!revealClues)
    return failure(revealClues.error());
  result.m_revealClues = *revealClues;

  auto symbol =
      requireLocationSymbol(obj, "symbol"_L1, Json::joinPath(path, u"symbol"));
  if (!symbol)
    return failure(symbol.error());
  result.m_symbol = *symbol;

  auto revealedSymbol = requireLocationSymbol(
      obj, "revealedSymbol"_L1, Json::joinPath(path, u"revealedSymbol"));
  if (!revealedSymbol)
    return failure(revealedSymbol.error());
  result.m_revealedSymbol = *revealedSymbol;

  return result;
}

ValueOrError<Location> Location::fromRawBytes(QByteArrayView bytes,
                                              QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

Investigator::Investigator(CardCode id, CardCode cardCode, TokenCounts tokens)
    : m_id(std::move(id)), m_cardCode(std::move(cardCode)),
      m_tokens(std::move(tokens)) {}

qint64 Investigator::damage() const noexcept {
  return m_tokens.count(kDamageToken);
}
qint64 Investigator::horror() const noexcept {
  return m_tokens.count(kHorrorToken);
}
qint64 Investigator::resources() const noexcept {
  return m_tokens.count(kResourceToken);
}
qint64 Investigator::clues() const noexcept {
  return m_tokens.count(kClueToken);
}
qint64 Investigator::doom() const noexcept {
  return m_tokens.count(kDoomToken);
}

ValueOrError<Investigator> Investigator::fromRawJson(const Json::Value &v,
                                                     QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto id = requireBoardCardCode(obj, "id"_L1, Json::joinPath(path, u"id"));
  if (!id)
    return failure(id.error());

  auto cardCode = requireBoardCardCode(obj, "cardCode"_L1,
                                       Json::joinPath(path, u"cardCode"));
  if (!cardCode)
    return failure(cardCode.error());

  // The all-zero UUID is the backend's deliberate "no player" marker here
  // (PlayerId nil), so it decodes to std::nullopt; the backend's UUID
  // grammar spells it only this way. Anything else must be a valid
  // non-null UUID.
  auto playerId = Json::requireField(
      obj, "playerId"_L1, Json::joinPath(path, u"playerId"),
      [](const Json::Value &value,
         QStringView p) -> ValueOrError<std::optional<PlayerId>> {
        if (value.isString() && value.toString() == kNilUuid)
          return std::optional<PlayerId>{};
        auto id = PlayerId::fromJson(value, p);
        if (!id)
          return failure(id.error());
        return std::optional<PlayerId>{*id};
      });
  if (!playerId)
    return failure(playerId.error());

  auto name = Json::requireField(obj, "name"_L1, Json::joinPath(path, u"name"),
                                 [](const Json::Value &value, QStringView p) {
                                   return CardName::fromJson(value, p);
                                 });
  if (!name)
    return failure(name.error());

  auto classSymbol = Json::requireField(
      obj, "class"_L1, Json::joinPath(path, u"class"),
      [](const Json::Value &value, QStringView p) {
        return Json::decodeClosedEnum(value, p, kClassSymbolTable);
      });
  if (!classSymbol)
    return failure(classSymbol.error());

  auto health =
      requireNonNegativeInt(obj, "health"_L1, Json::joinPath(path, u"health"));
  if (!health)
    return failure(health.error());

  auto sanity =
      requireNonNegativeInt(obj, "sanity"_L1, Json::joinPath(path, u"sanity"));
  if (!sanity)
    return failure(sanity.error());

  auto willpower =
      Json::requireInt(obj, "willpower"_L1, Json::joinPath(path, u"willpower"));
  if (!willpower)
    return failure(willpower.error());

  auto intellect =
      Json::requireInt(obj, "intellect"_L1, Json::joinPath(path, u"intellect"));
  if (!intellect)
    return failure(intellect.error());

  auto combat =
      Json::requireInt(obj, "combat"_L1, Json::joinPath(path, u"combat"));
  if (!combat)
    return failure(combat.error());

  auto agility =
      Json::requireInt(obj, "agility"_L1, Json::joinPath(path, u"agility"));
  if (!agility)
    return failure(agility.error());

  auto tokens =
      requireTokenCounts(obj, "tokens"_L1, Json::joinPath(path, u"tokens"));
  if (!tokens)
    return failure(tokens.error());

  auto remainingActions = Json::requireInt(
      obj, "remainingActions"_L1, Json::joinPath(path, u"remainingActions"));
  if (!remainingActions)
    return failure(remainingActions.error());

  auto unhealedHorror =
      Json::requireInt(obj, "unhealedHorrorThisRound"_L1,
                       Json::joinPath(path, u"unhealedHorrorThisRound"));
  if (!unhealedHorror)
    return failure(unhealedHorror.error());

  auto defeated =
      Json::requireBool(obj, "defeated"_L1, Json::joinPath(path, u"defeated"));
  if (!defeated)
    return failure(defeated.error());

  auto resigned =
      Json::requireBool(obj, "resigned"_L1, Json::joinPath(path, u"resigned"));
  if (!resigned)
    return failure(resigned.error());

  auto eliminated = Json::requireBool(obj, "eliminated"_L1,
                                      Json::joinPath(path, u"eliminated"));
  if (!eliminated)
    return failure(eliminated.error());

  const QString placementPath = Json::joinPath(path, u"placement");
  auto placement = Json::requireObjectField(obj, "placement"_L1, placementPath);
  if (!placement)
    return failure(placement.error());
  auto placementTag = Json::requireString(
      *placement, "tag"_L1, Json::joinPath(placementPath, u"tag"));
  if (!placementTag)
    return failure(placementTag.error());
  std::optional<LocationId> placementLocation;
  if (*placementTag == "AtLocation"_L1) {
    auto location = Json::requireField(
        *placement, "contents"_L1, Json::joinPath(placementPath, u"contents"),
        [](const Json::Value &value, QStringView p) {
          return LocationId::fromJson(value, p);
        });
    if (!location)
      return failure(location.error());
    placementLocation = *location;
  }

  auto connectedLocations =
      requireIdArray<LocationId>(obj, "connectedLocations"_L1,
                                 Json::joinPath(path, u"connectedLocations"));
  if (!connectedLocations)
    return failure(connectedLocations.error());

  // Card art is optional presentation data, decoded tolerantly (never fails
  // the investigator): `art` falls back to the investigator id when absent,
  // null, non-string or empty. It may be a homebrew ":campaign:code" id, so
  // it is kept verbatim and validated only when an asset URL is built.
  QString art = id->value();
  if (const auto artValue = obj.find("art"_L1);
      artValue && artValue->isString() && !artValue->toString().isEmpty())
    art = artValue->toString();
  // `mutated` (Maybe Text, e.g. "Mutated21") is kept only when it matches
  // Mutated<N>; anything else (null, absent, malformed, non-string) means
  // the plain art is shown.
  std::optional<QString> mutatedArt;
  if (const auto mutatedValue = obj.find("mutated"_L1);
      mutatedValue && mutatedValue->isString() &&
      isMutationName(mutatedValue->toString()))
    mutatedArt = mutatedValue->toString();

  Investigator result(*id, *cardCode, *tokens);
  result.m_art = std::move(art);
  result.m_mutated = std::move(mutatedArt);
  result.m_playerId = *playerId;
  result.m_name = *name;
  result.m_classSymbol = *classSymbol;
  result.m_health = *health;
  result.m_sanity = *sanity;
  result.m_willpower = *willpower;
  result.m_intellect = *intellect;
  result.m_combat = *combat;
  result.m_agility = *agility;
  result.m_remainingActions = *remainingActions;
  result.m_unhealedHorrorThisRound = *unhealedHorror;
  result.m_defeated = *defeated;
  result.m_resigned = *resigned;
  result.m_eliminated = *eliminated;
  result.m_placement = *placement;
  result.m_placementLocation = placementLocation;
  result.m_connectedLocations = *connectedLocations;
  return result;
}

ValueOrError<Investigator> Investigator::fromRawBytes(QByteArrayView bytes,
                                                      QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

ScenarioSnapshot::ScenarioSnapshot(CardCode id, qint64 turn, ChaosBag chaosBag)
    : m_id(std::move(id)), m_turn(turn), m_chaosBag(std::move(chaosBag)) {}

ValueOrError<ScenarioSnapshot>
ScenarioSnapshot::fromRawJson(const Json::Value &v, QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  auto id = requireBoardCardCode(obj, "id"_L1, Json::joinPath(path, u"id"));
  if (!id)
    return failure(id.error());

  auto turn =
      requireNonNegativeInt(obj, "turn"_L1, Json::joinPath(path, u"turn"));
  if (!turn)
    return failure(turn.error());

  auto chaosBag =
      Json::requireField(obj, "chaosBag"_L1, Json::joinPath(path, u"chaosBag"),
                         [](const Json::Value &value, QStringView p) {
                           return ChaosBag::fromRawJson(value, p);
                         });
  if (!chaosBag)
    return failure(chaosBag.error());

  return ScenarioSnapshot(*id, *turn, *chaosBag);
}

ValueOrError<ScenarioSnapshot>
ScenarioSnapshot::fromRawBytes(QByteArrayView bytes, QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

ValueOrError<GameMode> GameMode::fromRawJson(const Json::Value &v,
                                             QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  const auto campaign = obj.find("This"_L1);
  const auto scenario = obj.find("That"_L1);
  if (!campaign && !scenario)
    return failure(QStringLiteral("%1: unsupported mode: expected a \"This\" "
                                  "and/or \"That\" key")
                       .arg(path));

  GameMode result;
  if (campaign) {
    auto campaignObj =
        Json::requireObject(*campaign, Json::joinPath(path, u"This"));
    if (!campaignObj)
      return failure(campaignObj.error());
    result.m_campaign = *campaignObj;
  }
  if (scenario) {
    auto decoded =
        ScenarioSnapshot::fromRawJson(*scenario, Json::joinPath(path, u"That"));
    if (!decoded)
      return failure(decoded.error());
    result.m_scenario = *decoded;
  }
  if (campaign && scenario)
    result.m_kind = Kind::CampaignAndScenario;
  else if (campaign)
    result.m_kind = Kind::CampaignOnly;
  else
    result.m_kind = Kind::ScenarioOnly;
  return result;
}

ValueOrError<GameMode> GameMode::fromRawBytes(QByteArrayView bytes,
                                              QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

PublicGame::PublicGame(GameId id, GameMode mode)
    : m_id(std::move(id)), m_mode(std::move(mode)) {}

const ChaosBag *PublicGame::chaosBag() const noexcept {
  return m_mode.scenario() ? &m_mode.scenario()->chaosBag() : nullptr;
}

const Location *PublicGame::location(const LocationId &id) const noexcept {
  return findById(m_locations, id);
}

const Investigator *
PublicGame::investigator(const CardCode &id) const noexcept {
  return findById(m_investigators, id);
}

const Act *PublicGame::act(const CardCode &id) const noexcept {
  return findById(m_acts, id);
}

const Agenda *PublicGame::agenda(const CardCode &id) const noexcept {
  return findById(m_agendas, id);
}

ValueOrError<PublicGame> PublicGame::fromRawJson(const Json::Value &v,
                                                 QStringView path) {
  auto objResult = Json::requireObject(v, path);
  if (!objResult)
    return failure(objResult.error());
  const auto &obj = *objResult;

  const QString tagPath = Json::joinPath(path, u"tag");
  auto tag = Json::requireString(obj, "tag"_L1, tagPath);
  if (!tag)
    return failure(tag.error());
  if (*tag != "PublicGame"_L1)
    return failure(QStringLiteral("%1: expected \"PublicGame\", got \"%2\"")
                       .arg(tagPath, *tag));

  auto id = Json::requireField(obj, "id"_L1, Json::joinPath(path, u"id"),
                               [](const Json::Value &value, QStringView p) {
                                 return GameId::fromJson(value, p);
                               });
  if (!id)
    return failure(id.error());

  auto name =
      Json::requireString(obj, "name"_L1, Json::joinPath(path, u"name"));
  if (!name)
    return failure(name.error());

  auto mode = Json::requireField(obj, "mode"_L1, Json::joinPath(path, u"mode"),
                                 [](const Json::Value &value, QStringView p) {
                                   return GameMode::fromRawJson(value, p);
                                 });
  if (!mode)
    return failure(mode.error());

  auto encounterDeckSize = requireNonNegativeInt(
      obj, "encounterDeckSize"_L1, Json::joinPath(path, u"encounterDeckSize"));
  if (!encounterDeckSize)
    return failure(encounterDeckSize.error());

  auto phase = Json::requireField(obj, "phase"_L1,
                                  Json::joinPath(path, u"phase"), decodePhase);
  if (!phase)
    return failure(phase.error());

  auto phaseStep = Json::requireField(
      obj, "phaseStep"_L1, Json::joinPath(path, u"phaseStep"),
      [](const Json::Value &value,
         QStringView p) -> ValueOrError<std::optional<PhaseStep>> {
        if (value.isNull())
          return std::optional<PhaseStep>{};
        auto step = PhaseStep::fromRawJson(value, p);
        if (!step)
          return failure(step.error());
        return std::optional<PhaseStep>{*step};
      });
  if (!phaseStep)
    return failure(phaseStep.error());

  const auto decodeLocationId = [](const Json::Value &value, QStringView p) {
    return LocationId::fromJson(value, p);
  };
  const auto idOf = [](const auto &entity) { return entity.id(); };

  auto locations = requireEntityMap<Location>(
      obj, "locations"_L1, Json::joinPath(path, u"locations"), decodeLocationId,
      idOf);
  if (!locations)
    return failure(locations.error());

  auto investigators = requireEntityMap<Investigator>(
      obj, "investigators"_L1, Json::joinPath(path, u"investigators"),
      decodeBoardCardCode, idOf);
  if (!investigators)
    return failure(investigators.error());

  auto acts = requireEntityMap<Act>(
      obj, "acts"_L1, Json::joinPath(path, u"acts"), decodeBoardCardCode, idOf);
  if (!acts)
    return failure(acts.error());

  auto agendas = requireEntityMap<Agenda>(obj, "agendas"_L1,
                                          Json::joinPath(path, u"agendas"),
                                          decodeBoardCardCode, idOf);
  if (!agendas)
    return failure(agendas.error());

  PublicGame result(*id, *mode);
  result.m_name = *name;
  result.m_encounterDeckSize = *encounterDeckSize;
  result.m_phase = *phase;
  result.m_phaseStep = *phaseStep;
  result.m_locations = *locations;
  result.m_investigators = *investigators;
  result.m_acts = *acts;
  result.m_agendas = *agendas;
  return result;
}

ValueOrError<PublicGame> PublicGame::fromRawBytes(QByteArrayView bytes,
                                                  QStringView path) {
  auto parsed = Json::Value::parse(bytes, path);
  if (!parsed)
    return failure(parsed.error());
  return fromRawJson(*parsed, path);
}

} // namespace Arkham
