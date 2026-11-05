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
#include <utility>

// Models of the board snapshot that GET /games/:id (get-game.schema.json)
// and the GameUpdate WebSocket message (server-message.schema.json) both
// carry inside PublicGame: the chaos bag and its tokens, the current phase
// and phase step, the act and agenda entities, the core subset of the
// location and investigator entities, the game mode and the PublicGame
// aggregate itself. These are response-only, read-only types. Each one
// exposes only the canonical raw entry points (fromRawJson()/fromRawBytes(),
// or decodePhase() for the bare Phase enum); there are no QJsonValue
// overloads. The GetGame envelope and ServerMessage's GameUpdate branch
// (Games.h) both decode their PublicGame through PublicGame::fromRawJson().
//
// Response decoding policy (see JsonDecode.h): object keys these types do
// not model are ignored, so additive server fields never break decoding.
// Modeled fields stay strict: required keys, JSON types, closed enums,
// UUIDs, card codes and value ranges are all validated. Broad fields the
// board does not need yet are not parsed at all. Board card codes (act,
// agenda and investigator `id`, location and investigator `cardCode`, a
// location's `investigators`, a non-null chaosTokenRevealedBy) use the
// board schemas' `^c[0-9a-z:._-]+$`, which is narrower than CardCode's
// catalog grammar.

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

// Arkham.Token.Tokens (a `Map Token Int`): the tokens on a location or an
// investigator, encoded as an array of [tokenName, count] pairs. Token
// names are an open string set: the core names (Clue, Doom, Damage, Horror,
// Resource, and so on) plus "custom:<slug>" for a custom set's own tokens.
// Every count must be a non-negative integer, and a token name may appear
// only once, as in the backend's Map.
class TokenCounts {
public:
  [[nodiscard]] static ValueOrError<TokenCounts>
  fromRawJson(const Json::Value &v, QStringView path);
  [[nodiscard]] static ValueOrError<TokenCounts>
  fromRawBytes(QByteArrayView bytes, QStringView path);

  // [name, count] pairs, in wire order.
  [[nodiscard]] const QList<std::pair<QString, qint64>> &
  entries() const noexcept {
    return m_entries;
  }
  // The count for `name`, or 0 when it is absent (the backend drops a token
  // from the map when its count reaches 0).
  [[nodiscard]] qint64 count(QStringView name) const noexcept;

  friend bool operator==(const TokenCounts &, const TokenCounts &) = default;

private:
  TokenCounts() = default;

  QList<std::pair<QString, qint64>> m_entries;
};

// location.schema.json's locationSymbol (Arkham.LocationSymbol). Closed: an
// unrecognized string fails to decode.
enum class LocationSymbol {
  Circle,
  Square,
  Triangle,
  Plus,
  Diamond,
  Squiggle,
  Moon,
  Hourglass,
  T,
  Equals,
  Heart,
  Star,
  Droplet,
  Trefoil,
  Spade,
  NoSymbol,
};

// location.schema.json: one entry of PublicGame.locations. The map holds two
// disjoint shapes, both decoded here. An ordinary location (LocationAttrs
// plus the server-computed connection and occupant fields) has no
// `enemyLocation` key. An enemy-spawned pseudo-location carries
// `"enemyLocation": true` and `exhausted`, and has no `revealClues`,
// `symbol` or `revealedSymbol`. Only the core fields a board view needs are
// modeled; cost, matcher, placement, card and per-scenario fields are not
// parsed.
class Location {
public:
  enum class Kind { Ordinary, EnemyLocation };

  [[nodiscard]] static ValueOrError<Location> fromRawJson(const Json::Value &v,
                                                          QStringView path);
  [[nodiscard]] static ValueOrError<Location> fromRawBytes(QByteArrayView bytes,
                                                           QStringView path);

  [[nodiscard]] Kind kind() const noexcept { return m_kind; }
  [[nodiscard]] const LocationId &id() const noexcept { return m_id; }
  [[nodiscard]] const CardCode &cardCode() const noexcept { return m_cardCode; }
  [[nodiscard]] const CardId &cardId() const noexcept { return m_cardId; }
  // The scenario's label for this location, e.g. "study".
  [[nodiscard]] const QString &label() const noexcept { return m_label; }
  [[nodiscard]] bool revealed() const noexcept { return m_revealed; }
  [[nodiscard]] const TokenCounts &tokens() const noexcept { return m_tokens; }
  // Token counts by name (Arkham.Location.Base's locationClues and so on).
  [[nodiscard]] qint64 clues() const noexcept;
  [[nodiscard]] qint64 doom() const noexcept;
  [[nodiscard]] qint64 damage() const noexcept;
  [[nodiscard]] qint64 horror() const noexcept;
  [[nodiscard]] qint64 resources() const noexcept;
  // Required key; JSON null decodes to std::nullopt.
  [[nodiscard]] const std::optional<GameValue> &shroud() const noexcept {
    return m_shroud;
  }
  // Kind::Ordinary only (required there); std::nullopt for an enemy
  // location.
  [[nodiscard]] const std::optional<GameValue> &revealClues() const noexcept {
    return m_revealClues;
  }
  [[nodiscard]] std::optional<LocationSymbol> symbol() const noexcept {
    return m_symbol;
  }
  [[nodiscard]] std::optional<LocationSymbol> revealedSymbol() const noexcept {
    return m_revealedSymbol;
  }
  // Kind::EnemyLocation only (required there); std::nullopt for an
  // ordinary location.
  [[nodiscard]] std::optional<bool> exhausted() const noexcept {
    return m_exhausted;
  }
  // Server-computed when the snapshot is built: the locations currently
  // reachable from here, in wire order.
  [[nodiscard]] const QList<LocationId> &connectedLocations() const noexcept {
    return m_connectedLocations;
  }
  // Server-computed: the investigators currently at this location, in wire
  // order. This is the board's source of truth for occupancy.
  [[nodiscard]] const QList<CardCode> &investigators() const noexcept {
    return m_investigators;
  }

  friend bool operator==(const Location &, const Location &) = default;

private:
  Location(LocationId id, CardCode cardCode, CardId cardId, TokenCounts tokens);

  Kind m_kind = Kind::Ordinary;
  LocationId m_id;
  CardCode m_cardCode;
  CardId m_cardId;
  QString m_label;
  bool m_revealed = false;
  TokenCounts m_tokens;
  std::optional<GameValue> m_shroud;
  std::optional<GameValue> m_revealClues;
  std::optional<LocationSymbol> m_symbol;
  std::optional<LocationSymbol> m_revealedSymbol;
  std::optional<bool> m_exhausted;
  QList<LocationId> m_connectedLocations;
  QList<CardCode> m_investigators;
};

// Arkham.Investigator.Types' InvestigatorForm: the alternate form whose card
// the investigator currently shows.
enum class InvestigatorForm {
  Regular,
  Yithian,
  Homunculus,
  Shattered,
  Transfigured
};

// investigator.schema.json: one entry of PublicGame.investigators. Only the
// core fields a board view needs are modeled: identity, printed health and
// sanity, base skills, token counts (damage and horror are tokens), the
// action and elimination state, the placement and the server-computed
// connections, plus the card art, its mutation, the form and the class
// symbol `meta`. Deck, hand, discard, slots, modifiers, prohibitions and
// per-scenario fields are not parsed.
class Investigator {
public:
  [[nodiscard]] static ValueOrError<Investigator>
  fromRawJson(const Json::Value &v, QStringView path);
  [[nodiscard]] static ValueOrError<Investigator>
  fromRawBytes(QByteArrayView bytes, QStringView path);

  // The InvestigatorId (a board card code, e.g. "c01001").
  [[nodiscard]] const CardCode &id() const noexcept { return m_id; }
  [[nodiscard]] const CardCode &cardCode() const noexcept { return m_cardCode; }
  // The art id the web client renders (InvestigatorAttrs.investigatorArt),
  // e.g. "c01001" or a homebrew ":campaign:code". Never empty: decoded
  // tolerantly, it falls back to id() when `art` is absent, null, not a
  // string or empty. Kept verbatim; AssetLocator validates it.
  [[nodiscard]] const QString &art() const noexcept { return m_art; }
  // The art mutation, e.g. "Mutated21" (the asset is <art>_Mutated21).
  // Decoded tolerantly: std::nullopt when `mutated` is absent, null, not a
  // string or not "Mutated" followed by digits; never fails the decode.
  [[nodiscard]] const std::optional<QString> &mutated() const noexcept {
    return m_mutated;
  }
  // The `form` object's tag ({tag, contents?}). Decoded tolerantly: Regular
  // when `form` is absent, not an object, or has an unknown tag, and when a
  // TransfiguredForm's `contents` is not a board card code; never fails the
  // decode.
  [[nodiscard]] InvestigatorForm form() const noexcept { return m_form; }
  // The TransfiguredForm card (its `contents`, e.g. "c10016a"); set only when
  // form() is Transfigured.
  [[nodiscard]] const std::optional<CardCode> &
  transfiguredCardCode() const noexcept {
    return m_transfiguredCardCode;
  }
  // `meta` when it is a ClassSymbol string, e.g. Lola Hayes' current role
  // ("Seeker"). `meta` is free-form per investigator, so it is decoded
  // tolerantly: std::nullopt for anything else; never fails the decode.
  [[nodiscard]] const std::optional<ClassSymbol> &
  metaClassSymbol() const noexcept {
    return m_metaClassSymbol;
  }
  // The seated player. std::nullopt when the wire carries the all-zero
  // UUID: the backend builds killed and other-campaign investigators with
  // `PlayerId nil` (Arkham.Game's lookupInvestigator iid (PlayerId nil)).
  // A malformed UUID or a non-string value still fails to decode.
  [[nodiscard]] const std::optional<PlayerId> &playerId() const noexcept {
    return m_playerId;
  }
  [[nodiscard]] const CardName &name() const noexcept { return m_name; }
  [[nodiscard]] ClassSymbol classSymbol() const noexcept {
    return m_classSymbol;
  }
  // Printed maxima; never negative. Current damage and horror are tokens.
  [[nodiscard]] qint64 health() const noexcept { return m_health; }
  [[nodiscard]] qint64 sanity() const noexcept { return m_sanity; }
  // Base skill values, before modifiers.
  [[nodiscard]] qint64 willpower() const noexcept { return m_willpower; }
  [[nodiscard]] qint64 intellect() const noexcept { return m_intellect; }
  [[nodiscard]] qint64 combat() const noexcept { return m_combat; }
  [[nodiscard]] qint64 agility() const noexcept { return m_agility; }
  [[nodiscard]] const TokenCounts &tokens() const noexcept { return m_tokens; }
  // Token counts by name (Arkham.Investigator.Types' investigatorDamage and
  // so on).
  [[nodiscard]] qint64 damage() const noexcept;
  [[nodiscard]] qint64 horror() const noexcept;
  [[nodiscard]] qint64 resources() const noexcept;
  [[nodiscard]] qint64 clues() const noexcept;
  [[nodiscard]] qint64 doom() const noexcept;
  [[nodiscard]] qint64 remainingActions() const noexcept {
    return m_remainingActions;
  }
  // Can be negative: investigator.schema.json documents that healing more
  // horror in a round than was assigned produces a negative wire value.
  [[nodiscard]] qint64 unhealedHorrorThisRound() const noexcept {
    return m_unhealedHorrorThisRound;
  }
  [[nodiscard]] bool defeated() const noexcept { return m_defeated; }
  [[nodiscard]] bool resigned() const noexcept { return m_resigned; }
  [[nodiscard]] bool eliminated() const noexcept { return m_eliminated; }
  // placement.schema.json's {tag, contents} object, kept raw: `contents`
  // varies by tag and the backend's Arkham.Placement has tags the schema's
  // enum does not list yet, so any string tag is accepted.
  [[nodiscard]] const Json::Value &placement() const noexcept {
    return m_placement;
  }
  // The placement's location when its tag is "AtLocation"; std::nullopt for
  // every other tag. The backend also places investigators through other
  // means (e.g. in a vehicle), so Location::investigators() is the source of
  // truth for occupancy.
  [[nodiscard]] const std::optional<LocationId> &
  placementLocation() const noexcept {
    return m_placementLocation;
  }
  // Server-computed when the snapshot is built: the locations currently
  // reachable from this investigator, in wire order.
  [[nodiscard]] const QList<LocationId> &connectedLocations() const noexcept {
    return m_connectedLocations;
  }

  friend bool operator==(const Investigator &, const Investigator &) = default;

private:
  Investigator(CardCode id, CardCode cardCode, TokenCounts tokens);

  CardCode m_id;
  CardCode m_cardCode;
  QString m_art;
  std::optional<QString> m_mutated;
  InvestigatorForm m_form = InvestigatorForm::Regular;
  std::optional<CardCode> m_transfiguredCardCode;
  std::optional<ClassSymbol> m_metaClassSymbol;
  std::optional<PlayerId> m_playerId;
  CardName m_name;
  ClassSymbol m_classSymbol = ClassSymbol::Neutral;
  qint64 m_health = 0;
  qint64 m_sanity = 0;
  qint64 m_willpower = 0;
  qint64 m_intellect = 0;
  qint64 m_combat = 0;
  qint64 m_agility = 0;
  TokenCounts m_tokens;
  qint64 m_remainingActions = 0;
  qint64 m_unhealedHorrorThisRound = 0;
  bool m_defeated = false;
  bool m_resigned = false;
  bool m_eliminated = false;
  Json::Value m_placement;
  std::optional<LocationId> m_placementLocation;
  QList<LocationId> m_connectedLocations;
};

// mode.schema.json's $defs/scenario: the running scenario (ScenarioAttrs).
// Only the core fields a board view needs are modeled: the scenario's card
// code, its turn and its chaos bag. Decks, stacks, the grid, the campaign
// log and the other broad fields are not parsed.
class ScenarioSnapshot {
public:
  [[nodiscard]] static ValueOrError<ScenarioSnapshot>
  fromRawJson(const Json::Value &v, QStringView path);
  [[nodiscard]] static ValueOrError<ScenarioSnapshot>
  fromRawBytes(QByteArrayView bytes, QStringView path);

  // The scenario's card code, e.g. "c01104" (board card code grammar).
  [[nodiscard]] const CardCode &id() const noexcept { return m_id; }
  // ScenarioAttrs.scenarioTurn: 0 before the first turn begins; never
  // negative.
  [[nodiscard]] qint64 turn() const noexcept { return m_turn; }
  [[nodiscard]] const ChaosBag &chaosBag() const noexcept { return m_chaosBag; }

  friend bool operator==(const ScenarioSnapshot &,
                         const ScenarioSnapshot &) = default;

private:
  ScenarioSnapshot(CardCode id, qint64 turn, ChaosBag chaosBag);

  CardCode m_id;
  qint64 m_turn;
  ChaosBag m_chaosBag;
};

// mode.schema.json: PublicGame.mode, a `These Campaign Scenario` encoded as
// sibling keys of one object: {"This": campaign} (between-scenario campaign
// screen), {"That": scenario} (standalone scenario) or both (a campaign's
// running scenario). There is never a wrapping "These" key. An object with
// neither "This" nor "That" is an unsupported variant and fails to decode;
// other keys are ignored.
class GameMode {
public:
  enum class Kind { CampaignOnly, ScenarioOnly, CampaignAndScenario };

  [[nodiscard]] static ValueOrError<GameMode> fromRawJson(const Json::Value &v,
                                                          QStringView path);
  [[nodiscard]] static ValueOrError<GameMode> fromRawBytes(QByteArrayView bytes,
                                                           QStringView path);

  [[nodiscard]] Kind kind() const noexcept { return m_kind; }
  // The "This" campaign object, kept verbatim (its schema is broad and out
  // of scope); std::nullopt for Kind::ScenarioOnly.
  [[nodiscard]] const std::optional<Json::Value> &campaign() const noexcept {
    return m_campaign;
  }
  // The "That" scenario; std::nullopt for Kind::CampaignOnly.
  [[nodiscard]] const std::optional<ScenarioSnapshot> &
  scenario() const noexcept {
    return m_scenario;
  }

  friend bool operator==(const GameMode &, const GameMode &) = default;

private:
  GameMode() = default;

  Kind m_kind = Kind::ScenarioOnly;
  std::optional<Json::Value> m_campaign;
  std::optional<ScenarioSnapshot> m_scenario;
};

// public-game.schema.json: the authoritative game snapshot that GET
// /games/:id (GetGame.game) and the GameUpdate WebSocket message
// (ServerMessage contents) both carry. Only the CORE board slice is
// modeled: identity, mode, phase and phase step, encounter deck size and
// the location, investigator, act and agenda maps. The question, history,
// settings, log and other entity maps are not parsed.
//
// Every modeled map is a JSON object keyed by its entity's identity:
// `locations` by LocationId (a UUID), `investigators`, `acts` and `agendas`
// by board card code. Each key must decode as that identity type, must be
// unique after decoding (so two spellings of one UUID are a duplicate) and
// must equal the entry's own `id`; any violation fails with the entry's
// path. Entries keep wire order.
class PublicGame {
public:
  [[nodiscard]] static ValueOrError<PublicGame>
  fromRawJson(const Json::Value &v, QStringView path);
  [[nodiscard]] static ValueOrError<PublicGame>
  fromRawBytes(QByteArrayView bytes, QStringView path);

  [[nodiscard]] const GameId &id() const noexcept { return m_id; }
  [[nodiscard]] const QString &name() const noexcept { return m_name; }
  [[nodiscard]] const GameMode &mode() const noexcept { return m_mode; }
  // The running scenario's chaos bag, or nullptr when the mode has no
  // scenario (GameMode::Kind::CampaignOnly). Valid while *this is alive.
  [[nodiscard]] const ChaosBag *chaosBag() const noexcept;
  // Never negative.
  [[nodiscard]] qint64 encounterDeckSize() const noexcept {
    return m_encounterDeckSize;
  }
  [[nodiscard]] Phase phase() const noexcept { return m_phase; }
  // Required key; JSON null (no running phase step) decodes to
  // std::nullopt.
  [[nodiscard]] const std::optional<PhaseStep> &phaseStep() const noexcept {
    return m_phaseStep;
  }
  // In wire order.
  [[nodiscard]] const QList<Location> &locations() const noexcept {
    return m_locations;
  }
  [[nodiscard]] const QList<Investigator> &investigators() const noexcept {
    return m_investigators;
  }
  [[nodiscard]] const QList<Act> &acts() const noexcept { return m_acts; }
  [[nodiscard]] const QList<Agenda> &agendas() const noexcept {
    return m_agendas;
  }
  // Lookups by stable identity; nullptr when absent. Valid while *this is
  // alive.
  [[nodiscard]] const Location *location(const LocationId &id) const noexcept;
  [[nodiscard]] const Investigator *
  investigator(const CardCode &id) const noexcept;
  [[nodiscard]] const Act *act(const CardCode &id) const noexcept;
  [[nodiscard]] const Agenda *agenda(const CardCode &id) const noexcept;

  friend bool operator==(const PublicGame &, const PublicGame &) = default;

private:
  PublicGame(GameId id, GameMode mode);

  GameId m_id;
  QString m_name;
  GameMode m_mode;
  qint64 m_encounterDeckSize = 0;
  Phase m_phase = Phase::MythosPhase;
  std::optional<PhaseStep> m_phaseStep;
  QList<Location> m_locations;
  QList<Investigator> m_investigators;
  QList<Act> m_acts;
  QList<Agenda> m_agendas;
};

} // namespace Arkham
