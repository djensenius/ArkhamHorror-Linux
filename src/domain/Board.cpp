#include "Board.h"

#include "JsonDecode.h"

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

ValueOrError<CardCode> requireCardCode(const Json::Value &obj,
                                       QLatin1StringView key,
                                       QStringView path) {
  return Json::requireField(obj, key, path,
                            [](const Json::Value &v, QStringView p) {
                              return CardCode::fromJson(v, p);
                            });
}

ValueOrError<CardId> requireCardId(const Json::Value &obj,
                                   QLatin1StringView key, QStringView path) {
  return Json::requireField(obj, key, path,
                            [](const Json::Value &v, QStringView p) {
                              return CardId::fromJson(v, p);
                            });
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
        auto code = CardCode::fromJson(value, p);
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

  auto id = requireCardCode(obj, "id"_L1, Json::joinPath(path, u"id"));
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

  auto id = requireCardCode(obj, "id"_L1, Json::joinPath(path, u"id"));
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

} // namespace Arkham
