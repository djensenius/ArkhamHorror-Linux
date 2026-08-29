#pragma once

#include "CardCatalog.h"
#include "Decks.h"
#include "Identifiers.h"
#include "JsonDecode.h"
#include "ValueOrError.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringView>
#include <QUuid>
#include <optional>

// Models for contracts/schemas/game-list.schema.json and
// game-lifecycle.schema.json: the list of in-progress games and the
// lifecycle requests a client sends to create/join/staff one. PublicGame
// (the full per-game state snapshot returned by GET /games/:id) and all
// networking/caching layers are out of scope; see game-list.schema.json's
// gameDetails/failedGameDetails and game-lifecycle.schema.json's
// createGameRequest/chooseDeckRequest/claimSeatRequest/openSeats.

namespace Arkham {

// `difficulty` enum, shared by scenario/campaign summaries and
// createGameRequest.
enum class Difficulty { Easy, Standard, Hard, Expert };

// `multiplayerVariant` enum.
enum class MultiplayerVariant { Solo, WithFriends };

// `currentCampaignMode` enum (nullable; only The Dream-Eaters' two parts
// exist as of schema revision 0.1.12). Closed: the backend's own FromJSON
// (Generic-derived over CampaignPart) hard-fails on any other string, so a
// permissive client would only ever mask a genuine backend/client mismatch.
enum class CampaignPart { TheDreamQuest, TheWebOfDreams };

// `asIfRuling` enum. Two real values, but the backend's hand-written
// FromJSON accepts two wire spellings per value (the current short form and
// a legacy PascalCase form still emitted by old saved games) -- both decode
// to the same enum value, and encode always uses the short form.
enum class AsIfRulingValue { Chapter1AsIfRuling, Chapter2AsIfRuling };

// `ultimatumOrBoon` enum: a flat union of Boon and Ultimatum constructor
// names (Arkham.UltimatumsAndBoons.Types deliberately encodes the union
// flat, disjoint by prefix, predating the Haskell-side sum type). Closed:
// derived via aeson-th defaultOptions on each half, hard failing on any
// other string.
enum class UltimatumOrBoon {
  BoonOfTheAncients,
  BoonOfAthena,
  BoonOfDestiny,
  BoonOfHades,
  BoonOfHermes,
  BoonOfThoth,
  BoonOfOsiris,
  BoonOfTheMorrigan,
  BoonOfPersephone,
  BoonOfTheExplorer,
  BoonOfTheChild,
  UltimatumOfAgony,
  UltimatumOfBrokenPromises,
  UltimatumOfTheBrokenVeil,
  UltimatumOfChaos,
  UltimatumOfDisaster,
  UltimatumOfDread,
  UltimatumOfFailure,
  UltimatumOfFinality,
  UltimatumOfForbiddenKnowledge,
  UltimatumOfHardship,
  UltimatumOfTheHighlander,
  UltimatumOfInduction,
  UltimatumOfOrthodoxy,
  UltimatumOfTheScream,
  UltimatumOfSurvival,
  UltimatumOfUltimatums,
  UltimatumOfExile,
  UltimatumOfTheSpiral,
  UltimatumOfMalevolence,
};

// One entry of gameDetails.investigators/otherInvestigators.
struct InvestigatorSummary {
  CardCode id;
  ClassSymbol classSymbol;

  [[nodiscard]] static ValueOrError<InvestigatorSummary>
  fromJson(const QJsonValue &v, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const InvestigatorSummary &,
                         const InvestigatorSummary &) = default;
};

// gameDetails.scenario (null for a campaign game between scenarios, or
// absent entirely for a from-scratch campaign that has not started its
// first scenario).
struct ScenarioSummary {
  CardCode id;
  Difficulty difficulty{};
  CardName name;
  std::optional<QString> variant;

  [[nodiscard]] static ValueOrError<ScenarioSummary>
  fromJson(const QJsonValue &v, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const ScenarioSummary &,
                         const ScenarioSummary &) = default;
};

// gameDetails.campaign (null for a standalone scenario game). Unlike
// ScenarioSummary/InvestigatorSummary's `id` (backend type CardCode,
// serialized "c"-prefixed), CampaignDetails.id is backend type Text --
// e.g. the fixture's campaign id "06" is never "c"-prefixed -- so it stays
// the permissive CampaignId rather than the strict CardCode.
struct CampaignSummary {
  CampaignId id;
  Difficulty difficulty{};
  std::optional<CampaignPart> currentCampaignMode;

  [[nodiscard]] static ValueOrError<CampaignSummary>
  fromJson(const QJsonValue &v, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const CampaignSummary &,
                         const CampaignSummary &) = default;
};

// game-state.schema.json's GameState, decoded forward-compatibly: the four
// known tags are exhaustive as of schema revision 0.1.12, but this is a
// live state machine a future backend release can extend, so an unrecognized
// tag decodes to Kind::Unknown (preserving its wire spelling) rather than
// failing the whole containing GameListRow.
struct GameState {
  enum class Kind { Pending, ChooseDecks, Active, Over, Unknown };

  Kind kind{Kind::Unknown};
  // Only meaningful for Pending/ChooseDecks: the seats still owed a deck.
  QList<QUuid> playerIds;
  // Only meaningful for Unknown: the tag string as received, so a decoded
  // value that could not be interpreted can still be logged/displayed and
  // re-encoded without inventing a tag.
  QString unknownTag;
  // Unknown only: the raw "contents" value the unrecognized tag object
  // carried, if any (Undefined if it had none), preserved verbatim -- like
  // CampaignOption's own unknown-tag handling -- so re-encoding an unknown
  // state can never silently drop part of what the server sent.
  QJsonValue unknownContents{QJsonValue::Undefined};

  [[nodiscard]] static ValueOrError<GameState> fromJson(const QJsonValue &v,
                                                        QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const GameState &, const GameState &) = default;
};

// One row of the top-level game-list.json array: either a gameDetails
// object or a failedGameDetails `{"error": ...}` object. The schema
// disambiguates the two by shape, not by an explicit tag (mirroring the
// backend's hand-written `GameDetailsEntry` ToJSON: `FailedGameDetails`
// encodes as a bare `{"error": message}`, `SuccessGameDetails` as the raw
// GameDetails object), so decode does the same: a top-level "error" key
// means Failure, otherwise a full gameDetails decode is attempted.
//
// success()/failure() are the only ways to build one (mirroring
// CampaignOrScenario above): the private constructor makes a Success row
// with a missing id/gameState/multiplayerVariant unrepresentable, so
// toJson() never needs to guard against -- or fail on -- an invalid
// instance that was never possible to construct in the first place.
class GameListRow {
public:
  enum class Kind { Success, Failure };

  [[nodiscard]] static GameListRow
  success(GameId id, std::optional<ScenarioSummary> scenario,
          std::optional<CampaignSummary> campaign, GameState gameState,
          QString name, QList<InvestigatorSummary> investigators,
          QList<InvestigatorSummary> otherInvestigators,
          MultiplayerVariant multiplayerVariant, bool hasOpenSeats);
  // Named `failed`, not `failure`, so that it cannot shadow the free
  // function Arkham::failure() (declared in ValueOrError.h) used pervasively
  // by this class's own fromJson() for its ValueOrError<GameListRow> error
  // returns: an unqualified `failure(...)` call inside a GameListRow member
  // function resolves to a same-named static member ahead of the enclosing
  // namespace, which would otherwise silently turn every decode-error
  // return into a "successful" GameListRow with Kind::Failure instead of an
  // actual ValueOrError failure.
  [[nodiscard]] static GameListRow failed(QString error);

  [[nodiscard]] static ValueOrError<GameListRow> fromJson(const QJsonValue &v,
                                                          QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  [[nodiscard]] Kind kind() const noexcept { return m_kind; }
  // Success-only accessors (Kind::Success only; unset/empty for Failure).
  [[nodiscard]] const std::optional<GameId> &id() const noexcept {
    return m_id;
  }
  [[nodiscard]] const std::optional<ScenarioSummary> &
  scenario() const noexcept {
    return m_scenario;
  }
  [[nodiscard]] const std::optional<CampaignSummary> &
  campaign() const noexcept {
    return m_campaign;
  }
  [[nodiscard]] const std::optional<GameState> &gameState() const noexcept {
    return m_gameState;
  }
  [[nodiscard]] const QString &name() const noexcept { return m_name; }
  [[nodiscard]] const QList<InvestigatorSummary> &
  investigators() const noexcept {
    return m_investigators;
  }
  [[nodiscard]] const QList<InvestigatorSummary> &
  otherInvestigators() const noexcept {
    return m_otherInvestigators;
  }
  [[nodiscard]] const std::optional<MultiplayerVariant> &
  multiplayerVariant() const noexcept {
    return m_multiplayerVariant;
  }
  [[nodiscard]] bool hasOpenSeats() const noexcept { return m_hasOpenSeats; }
  // Failure-only accessor (Kind::Failure only; empty for Success).
  [[nodiscard]] const QString &error() const noexcept { return m_error; }

  friend bool operator==(const GameListRow &, const GameListRow &) = default;

private:
  GameListRow() = default;

  Kind m_kind{Kind::Failure};
  std::optional<GameId> m_id;
  std::optional<ScenarioSummary> m_scenario;
  std::optional<CampaignSummary> m_campaign;
  std::optional<GameState> m_gameState;
  QString m_name;
  QList<InvestigatorSummary> m_investigators;
  QList<InvestigatorSummary> m_otherInvestigators;
  std::optional<MultiplayerVariant> m_multiplayerVariant;
  bool m_hasOpenSeats{false};
  QString m_error;
};

[[nodiscard]] ValueOrError<QList<GameListRow>>
decodeGameList(const QJsonValue &v, QStringView path);
[[nodiscard]] QJsonArray encodeGameList(const QList<GameListRow> &rows);

// createGameRequest's known `options` entries (Arkham.Campaign.Option's
// CampaignOption, minus its one payload-carrying constructor,
// `CampaignVariant Text`, modeled separately below).
enum class KnownCampaignOption {
  PerformIntro,
  PlayersDoNotControlStoryAssetClues,
  AddLitaChantler,
  Cheated,
  TakeArmitage,
  TakeWarrenRice,
  TakeFrancisMorgan,
  TakeZebulonWhately,
  TakeEarlSawyer,
  TakePowderOfIbnGhazi,
  TakeTheNecronomicon,
  AddAcrossSpaceAndTime,
  UseSwarmPlaceholders,
  TakeBlackBook,
  TakePuzzleBox,
  ProceedToInterlude3,
  DebugOption,
  ManuallyPickCamp,
  ManuallyPickKilledInPlaneCrash,
  AddGreenSoapstone,
  AddWoodenSledge,
  AddDynamite,
  AddMiasmicCrystal,
  AddMineralSpecimen,
  AddSmallRadio,
  AddSpareParts,
  IncludePartners,
  FatalMiragePart1,
  FatalMiragePart2,
  FatalMiragePart3,
  PlayAsMiniCampaign,
  PlayWithTheBlobThatAteEverythingElse,
};

// One entry of createGameRequest.options. The backend's own CampaignOption
// is a closed 32-constructor sum type (hard failing on an unrecognized
// tag), but campaign content -- and therefore this option list -- is added
// with nearly every release, so this client models it forward-compatibly
// per the issue: a decoded-but-unrecognized tag/payload is preserved as
// Kind::Unknown rather than dropped or misread as some other case. The
// private constructor is deliberate: the only way to produce a
// Kind::Unknown value is by decoding one from the wire (via fromJson) --
// there is no public factory that lets calling code fabricate one -- so an
// option this client itself composes for a createGameRequest can never be
// silently treated as if the server already understood it.
class CampaignOption {
public:
  enum class Kind { Known, Variant, Unknown };

  [[nodiscard]] static CampaignOption knownOption(KnownCampaignOption option);
  [[nodiscard]] static CampaignOption variantOption(QString contents);

  [[nodiscard]] static ValueOrError<CampaignOption>
  fromJson(const QJsonValue &v, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  [[nodiscard]] Kind kind() const noexcept { return m_kind; }
  [[nodiscard]] std::optional<KnownCampaignOption> known() const noexcept {
    return m_known;
  }
  // Variant's contents (Kind::Variant) or the unrecognized tag string
  // (Kind::Unknown); empty for Kind::Known.
  [[nodiscard]] const QString &text() const noexcept { return m_text; }
  // Kind::Unknown only: the raw "contents" value the unrecognized tag
  // object carried, if any (Undefined if it had none), preserved verbatim
  // alongside the tag itself so re-encoding an unknown option never
  // silently drops part of what the server sent.
  [[nodiscard]] const QJsonValue &unknownContents() const noexcept {
    return m_unknownContents;
  }

  friend bool operator==(const CampaignOption &,
                         const CampaignOption &) = default;

private:
  CampaignOption() = default;

  Kind m_kind{Kind::Unknown};
  std::optional<KnownCampaignOption> m_known;
  QString m_text;
  QJsonValue m_unknownContents{QJsonValue::Undefined};
};

// The campaignId-xor-scenarioId invariant createGameRequest's schema
// expresses via `anyOf`: creating a game is either continuing a campaign or
// starting a standalone scenario, never both, never neither. The private
// constructor makes an invalid (both-set or neither-set) instance
// unrepresentable -- campaign()/scenario() are the only ways to build one,
// and fromJson rejects a wire object satisfying neither (or, defensively,
// both).
class CampaignOrScenario {
public:
  [[nodiscard]] static CampaignOrScenario campaign(CampaignId id);
  [[nodiscard]] static CampaignOrScenario scenario(ScenarioId id);

  [[nodiscard]] static ValueOrError<CampaignOrScenario>
  fromJson(const QJsonObject &requestObj, QStringView path);
  // Inserts this request's resolved "campaignId"/"scenarioId" keys (one
  // real, the other explicit JSON null) into `obj`, matching the fixture's
  // own encoding (both keys always present).
  void insertInto(QJsonObject &obj) const;

  [[nodiscard]] bool isCampaign() const noexcept {
    return m_campaignId.has_value();
  }
  [[nodiscard]] const std::optional<CampaignId> &campaignId() const noexcept {
    return m_campaignId;
  }
  [[nodiscard]] const std::optional<ScenarioId> &scenarioId() const noexcept {
    return m_scenarioId;
  }

  friend bool operator==(const CampaignOrScenario &,
                         const CampaignOrScenario &) = default;

private:
  CampaignOrScenario() = default;

  std::optional<CampaignId> m_campaignId;
  std::optional<ScenarioId> m_scenarioId;
};

// game-lifecycle.schema.json's createGameRequest. `ultimatumsAndBoons` and
// `achievementsEnabled` are stored already resolved to their backend
// default (empty list / true respectively) rather than as optionals: the
// backend's own hand-written FromJSON parses both with `.:? ... .!= <default>`,
// which -- like plain `.:?` -- folds an absent key and an explicit JSON
// null to the exact same value, so there is nothing left for this client to
// distinguish or preserve. `strictAsIfAt`/`asIfRuling` have no such default
// and stay std::optional (absent-or-null collapse to unset; toJson omits
// the key when unset).
struct CreateGameRequest {
  QList<std::optional<QUuid>> deckIds;
  int playerCount{};
  CampaignOrScenario campaignOrScenario;
  Difficulty difficulty{};
  QString campaignName;
  MultiplayerVariant multiplayerVariant{};
  bool includeTarotReadings{};
  QList<CampaignOption> options;
  std::optional<bool> strictAsIfAt;
  std::optional<AsIfRulingValue> asIfRuling;
  QList<UltimatumOrBoon> ultimatumsAndBoons;
  bool achievementsEnabled{true};

  [[nodiscard]] static ValueOrError<CreateGameRequest>
  fromJson(const QJsonValue &v, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const CreateGameRequest &,
                         const CreateGameRequest &) = default;
};

// game-lifecycle.schema.json's chooseDeckRequest, and (same shape)
// continueWithoutUpgrade. `deckUrl`/`deckList` fold absent-or-null to unset
// identically (the backend's UpgradeDeckPost parses both as plain `Maybe`
// fields via Aeson's generic derivation); submitting neither is exactly the
// documented "continue without upgrade" request.
struct ChooseDeckRequest {
  InvestigatorRef investigatorId;
  std::optional<QString> deckUrl;
  std::optional<DeckListInput> deckList;

  [[nodiscard]] static ValueOrError<ChooseDeckRequest>
  fromJson(const QJsonValue &v, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const ChooseDeckRequest &,
                         const ChooseDeckRequest &) = default;
};

// game-lifecycle.schema.json's claimSeatRequest.
struct ClaimSeatRequest {
  InvestigatorRef investigatorId;

  [[nodiscard]] static ValueOrError<ClaimSeatRequest>
  fromJson(const QJsonValue &v, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const ClaimSeatRequest &,
                         const ClaimSeatRequest &) = default;
};

// game-lifecycle.schema.json's openSeats: the already-normalized (always
// "c"-prefixed) card codes of investigators still owed a seat.
[[nodiscard]] ValueOrError<QList<CardCode>> decodeOpenSeats(const QJsonValue &v,
                                                            QStringView path);
[[nodiscard]] QJsonArray encodeOpenSeats(const QList<CardCode> &seats);

} // namespace Arkham
