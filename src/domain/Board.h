#pragma once

#include "CardCatalog.h"
#include "Identifiers.h"
#include "RawJson.h"
#include "ValueOrError.h"

#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringView>
#include <optional>

// Leaf models of the board snapshot that GET /games/:id
// (get-game.schema.json) and the GameUpdate WebSocket message
// (server-message.schema.json) both carry inside PublicGame: the chaos bag
// and its tokens, the current phase and phase step, and the act and agenda
// entities. These are response-only, read-only types. Each one exposes only
// the canonical raw entry points (fromRawJson()/fromRawBytes(), or
// decodePhase() for the bare Phase enum); there are no QJsonValue
// overloads. Locations, investigators, PublicGame itself and the
// GetGame/GameUpdate envelopes are decoded by later stages.
//
// Response decoding policy (see JsonDecode.h): object keys these types do
// not model are ignored, so additive server fields never break decoding.
// Modeled fields stay strict: required keys, JSON types, closed enums,
// UUIDs, card codes and value ranges are all validated. Broad fields the
// board does not need yet are not parsed at all. Board card codes (act and
// agenda `id`, a non-null chaosTokenRevealedBy) use the board schemas'
// `^c[0-9a-z:._-]+$`, which is narrower than CardCode's catalog grammar.

namespace Arkham {

// chaos-token.schema.json: one physical chaos token.
class ChaosToken {
public:
  [[nodiscard]] static ValueOrError<ChaosToken>
  fromRawJson(const Json::Value &v, QStringView path);
  [[nodiscard]] static ValueOrError<ChaosToken>
  fromRawBytes(QByteArrayView bytes, QStringView path);

  [[nodiscard]] const ChaosTokenId &id() const noexcept { return m_id; }
  // chaosTokenFace is an open string set: the base faces (PlusOne, Zero,
  // MinusOne .. MinusEight, Skull, Cultist, Tablet, ElderThing, AutoFail,
  // ElderSign, CurseToken, BlessToken, FrostToken, BloodToken) plus
  // homebrew slugs. It is kept verbatim so an unknown face still decodes.
  [[nodiscard]] const QString &face() const noexcept { return m_face; }
  // The investigator the token was drawn for. Required key; JSON null
  // decodes to std::nullopt.
  [[nodiscard]] const std::optional<CardCode> &revealedBy() const noexcept {
    return m_revealedBy;
  }
  [[nodiscard]] bool cancelled() const noexcept { return m_cancelled; }
  [[nodiscard]] bool sealed() const noexcept { return m_sealed; }

  friend bool operator==(const ChaosToken &, const ChaosToken &) = default;

private:
  ChaosToken(ChaosTokenId id, QString face, std::optional<CardCode> revealedBy,
             bool cancelled, bool sealed);

  ChaosTokenId m_id;
  QString m_face;
  std::optional<CardCode> m_revealedBy;
  bool m_cancelled = false;
  bool m_sealed = false;
};

// chaos-bag.schema.json: the scenario's chaos bag (PublicGame
// mode.That.chaosBag). Only the tokens in the bag and the tokens currently
// revealed are modeled; the draw-choice state, token pool, forced draws and
// pending requests are broad and not parsed.
class ChaosBag {
public:
  [[nodiscard]] static ValueOrError<ChaosBag> fromRawJson(const Json::Value &v,
                                                          QStringView path);
  [[nodiscard]] static ValueOrError<ChaosBag> fromRawBytes(QByteArrayView bytes,
                                                           QStringView path);

  // In wire order.
  [[nodiscard]] const QList<ChaosToken> &chaosTokens() const noexcept {
    return m_chaosTokens;
  }
  // In wire order.
  [[nodiscard]] const QList<ChaosToken> &revealedChaosTokens() const noexcept {
    return m_revealedChaosTokens;
  }

  friend bool operator==(const ChaosBag &, const ChaosBag &) = default;

private:
  ChaosBag() = default;

  QList<ChaosToken> m_chaosTokens;
  QList<ChaosToken> m_revealedChaosTokens;
};

// public-game.schema.json's `phase` (Arkham.Phase.Phase). Closed: an
// unrecognized string fails to decode.
enum class Phase {
  MythosPhase,
  InvestigationPhase,
  EnemyPhase,
  UpkeepPhase,
  ResolutionPhase,
  CampaignPhase,
};

// Decodes a `phase` string.
[[nodiscard]] ValueOrError<Phase> decodePhase(const Json::Value &v,
                                              QStringView path);

// phase-step.schema.json's object branch: {"tag": <phase step kind>,
// "contents": <sub-step>}. Both levels are closed: an unknown tag fails, and
// so does a sub-step that does not belong to the tag's own enum. The
// schema's JSON null branch (no running phase step) is the enclosing
// PublicGame field's concern; fromRawJson() requires an object.
class PhaseStep {
public:
  enum class Kind {
    MythosPhaseStep,
    InvestigationPhaseStep,
    EnemyPhaseStep,
    UpkeepPhaseStep,
  };
  // Every sub-step of every Kind, named exactly as on the wire.
  enum class Step {
    // MythosPhaseStep
    MythosPhaseBeginsStep,
    PlaceDoomOnAgendaStep,
    CheckDoomThresholdStep,
    EachInvestigatorDrawsEncounterCardStep,
    MythosPhaseWindow,
    MythosPhaseEndsStep,
    // InvestigationPhaseStep
    InvestigationPhaseBeginsStep,
    InvestigationPhaseBeginsWindow,
    NextInvestigatorsTurnBeginsStep,
    NextInvestigatorsTurnBeginsWindow,
    InvestigatorTakesActionStep,
    InvestigatorsTurnEndsStep,
    InvestigationPhaseEndsStep,
    // EnemyPhaseStep
    EnemyPhaseBeginsStep,
    HunterEnemiesMoveStep,
    ResolveAttacksWindow,
    ResolveAttacksStep,
    AfterResolveAttacksWindow,
    EnemyPhaseEndsStep,
    // UpkeepPhaseStep
    UpkeepPhaseBeginsStep,
    UpkeepPhaseBeginsWindow,
    ResetActionsStep,
    ReadyExhaustedStep,
    DrawCardAndGainResourceStep,
    CheckHandSizeStep,
    UpkeepPhaseEndsStep,
  };

  [[nodiscard]] static ValueOrError<PhaseStep> fromRawJson(const Json::Value &v,
                                                           QStringView path);
  [[nodiscard]] static ValueOrError<PhaseStep>
  fromRawBytes(QByteArrayView bytes, QStringView path);

  [[nodiscard]] Kind kind() const noexcept { return m_kind; }
  // Always one of the sub-steps listed under kind() above.
  [[nodiscard]] Step step() const noexcept { return m_step; }

  friend bool operator==(const PhaseStep &, const PhaseStep &) = default;

private:
  PhaseStep(Kind kind, Step step) : m_kind(kind), m_step(step) {}

  Kind m_kind;
  Step m_step;
};

// Arkham.Act.Sequence.ActSide.
enum class ActSide { A, B, C, D, E, F, G, H };

// act.schema.json: one entry of PublicGame.acts.
class Act {
public:
  [[nodiscard]] static ValueOrError<Act> fromRawJson(const Json::Value &v,
                                                     QStringView path);
  [[nodiscard]] static ValueOrError<Act> fromRawBytes(QByteArrayView bytes,
                                                      QStringView path);

  [[nodiscard]] const CardCode &id() const noexcept { return m_id; }
  [[nodiscard]] const CardId &cardId() const noexcept { return m_cardId; }
  // `sequence` is a [step, side] pair on the wire, e.g. [1, "A"].
  [[nodiscard]] qint64 sequenceStep() const noexcept { return m_sequenceStep; }
  [[nodiscard]] ActSide sequenceSide() const noexcept { return m_sequenceSide; }
  [[nodiscard]] qint64 deckId() const noexcept { return m_deckId; }
  [[nodiscard]] bool flipped() const noexcept { return m_flipped; }
  // ActAttrs.actAdvanceCost: required key whose value is a Cost object
  // (cost.schema.json, a broad union this slice does not model) or JSON
  // null. Kept verbatim as the lossless AST; only the outer object/null
  // type is checked.
  [[nodiscard]] const Json::Value &advanceCost() const noexcept {
    return m_advanceCost;
  }

  friend bool operator==(const Act &, const Act &) = default;

private:
  Act(CardCode id, CardId cardId, qint64 sequenceStep, ActSide sequenceSide,
      qint64 deckId, bool flipped, Json::Value advanceCost);

  CardCode m_id;
  CardId m_cardId;
  qint64 m_sequenceStep;
  ActSide m_sequenceSide;
  qint64 m_deckId;
  bool m_flipped;
  Json::Value m_advanceCost;
};

// Arkham.Agenda.Sequence.AgendaSide.
enum class AgendaSide { A, B, C, D };

// agenda.schema.json: one entry of PublicGame.agendas.
class Agenda {
public:
  [[nodiscard]] static ValueOrError<Agenda> fromRawJson(const Json::Value &v,
                                                        QStringView path);
  [[nodiscard]] static ValueOrError<Agenda> fromRawBytes(QByteArrayView bytes,
                                                         QStringView path);

  [[nodiscard]] const CardCode &id() const noexcept { return m_id; }
  [[nodiscard]] const CardId &cardId() const noexcept { return m_cardId; }
  // `sequence` is {"agendaSequenceSide", "agendaSequenceStep"} on the wire.
  [[nodiscard]] qint64 sequenceStep() const noexcept { return m_sequenceStep; }
  [[nodiscard]] AgendaSide sequenceSide() const noexcept {
    return m_sequenceSide;
  }
  [[nodiscard]] qint64 deckId() const noexcept { return m_deckId; }
  // Never negative: a negative doom count fails to decode.
  [[nodiscard]] qint64 doom() const noexcept { return m_doom; }
  // Required key; JSON null decodes to std::nullopt.
  [[nodiscard]] const std::optional<GameValue> &doomThreshold() const noexcept {
    return m_doomThreshold;
  }
  [[nodiscard]] bool flipped() const noexcept { return m_flipped; }

  friend bool operator==(const Agenda &, const Agenda &) = default;

private:
  Agenda(CardCode id, CardId cardId, qint64 sequenceStep,
         AgendaSide sequenceSide, qint64 deckId, qint64 doom,
         std::optional<GameValue> doomThreshold, bool flipped);

  CardCode m_id;
  CardId m_cardId;
  qint64 m_sequenceStep;
  AgendaSide m_sequenceSide;
  qint64 m_deckId;
  qint64 m_doom;
  std::optional<GameValue> m_doomThreshold;
  bool m_flipped;
};

} // namespace Arkham
