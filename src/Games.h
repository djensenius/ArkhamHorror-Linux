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

// One entry of gameDetails.investigators/otherInvestigators. `id` is
// game-list.schema.json's plain, unconstrained `investigator.id` string --
// but the backend's actual InvestigatorDetails.id field (Api.Arkham.Types.
// Game, backend commit 6a1befbd7b) is typed Arkham.Id.InvestigatorId, a
// newtype directly wrapping (and `deriving newtype` its ToJSON/FromJSON
// from) Arkham.Card.CardCode -- so every value on the wire is genuinely
// "c"-prefixed despite the schema not encoding that pattern. CardCode here
// reflects the real backend guarantee, not an invented client-side
// restriction; see CampaignSummary below for the contrasting case where the
// backend type really is unconstrained Text.
struct InvestigatorSummary {
  CardCode id;
  ClassSymbol classSymbol;

  [[nodiscard]] static ValueOrError<InvestigatorSummary>
  fromJson(const QJsonValue &v, QStringView path);
  // Canonical byte-level decode overload: identical logic (shared via a
  // private template, see Games.cpp), operating directly on the lossless
  // AST (see RawJson.h). This type has no numeric-precision concern of
  // its own, but a raw aggregate decoder (e.g. GameListRow::fromRawJson)
  // needs this overload to stay on Json::Value end-to-end for its
  // nested investigators/otherInvestigators arrays.
  [[nodiscard]] static ValueOrError<InvestigatorSummary>
  fromJson(const Json::Value &v, QStringView path);
  [[nodiscard]] ValueOrError<QJsonObject> toJson() const;

  friend bool operator==(const InvestigatorSummary &,
                         const InvestigatorSummary &) = default;
};

// gameDetails.scenario is a required key whose value may be `null` (for a
// campaign game between scenarios, or a from-scratch campaign that has
// not started its first scenario yet) -- the pinned game-list.schema.json
// requires the "scenario" key, and the decoder in Games.cpp enforces its
// presence via obj.contains("scenario"); it is never an absent/omitted
// key, only ever present-with-null. `id` is game-list.schema.json's
// plain, unconstrained `scenario.id` string -- but the backend's actual
// ScenarioDetails.id field is typed Arkham.Id.ScenarioId, a newtype
// directly wrapping (and `deriving newtype` its ToJSON/FromJSON from)
// Arkham.Card.CardCode, same as InvestigatorSummary.id above -- so
// CardCode here is likewise the real backend guarantee, not
// over-validation relative to the pinned contract commit.
struct ScenarioSummary {
  CardCode id;
  Difficulty difficulty{};
  CardName name;
  std::optional<QString> variant;

  [[nodiscard]] static ValueOrError<ScenarioSummary>
  fromJson(const QJsonValue &v, QStringView path);
  // Canonical byte-level decode overload: see InvestigatorSummary's
  // Json::Value overload doc comment above.
  [[nodiscard]] static ValueOrError<ScenarioSummary>
  fromJson(const Json::Value &v, QStringView path);
  [[nodiscard]] ValueOrError<QJsonObject> toJson() const;

  friend bool operator==(const ScenarioSummary &,
                         const ScenarioSummary &) = default;
};

// gameDetails.campaign (null for a standalone scenario game). Unlike
// ScenarioSummary/InvestigatorSummary's `id` (backend type
// Arkham.Id.ScenarioId/InvestigatorId, both wrapping CardCode, serialized
// "c"-prefixed), CampaignDetails.id is backend type Arkham.Id.CampaignId, a
// newtype wrapping unconstrained Text -- e.g. the fixture's campaign id
// "06" is never "c"-prefixed -- so it stays the permissive CampaignId
// rather than the strict CardCode.
struct CampaignSummary {
  CampaignId id;
  Difficulty difficulty{};
  std::optional<CampaignPart> currentCampaignMode;

  [[nodiscard]] static ValueOrError<CampaignSummary>
  fromJson(const QJsonValue &v, QStringView path);
  // Canonical byte-level decode overload: see InvestigatorSummary's
  // Json::Value overload doc comment above.
  [[nodiscard]] static ValueOrError<CampaignSummary>
  fromJson(const Json::Value &v, QStringView path);
  [[nodiscard]] ValueOrError<QJsonObject> toJson() const;

  friend bool operator==(const CampaignSummary &,
                         const CampaignSummary &) = default;
};

// game-state.schema.json's GameState, decoded forward-compatibly: the four
// known tags are exhaustive as of schema revision 0.1.12, but this is a
// live state machine a future backend release can extend, so an unrecognized
// tag decodes to Kind::Unknown (preserving its wire spelling and complete
// raw "contents") rather than failing the whole containing GameListRow.
// The private constructor makes every inconsistent state (a payload on
// Active/Over, or a Pending/ChooseDecks player list containing the
// all-zero/null uuid -- a seat can never genuinely be owed to "no one")
// unrepresentable: pending()/chooseDecks()/active()/over() are the only
// ways to build a known-kind instance, and Kind::Unknown has no public
// factory at all (decoder-only, exactly like CampaignOption's own
// unknown-tag handling), so production code can never fabricate one.
class GameState {
public:
  enum class Kind { Pending, ChooseDecks, Active, Over, Unknown };

  // Fails if any id in `playerIds` is the null (all-zero) uuid.
  [[nodiscard]] static ValueOrError<GameState> pending(QList<QUuid> playerIds);
  [[nodiscard]] static ValueOrError<GameState>
  chooseDecks(QList<QUuid> playerIds);
  [[nodiscard]] static GameState active();
  [[nodiscard]] static GameState over();

  [[nodiscard]] static ValueOrError<GameState> fromJson(const QJsonValue &v,
                                                        QStringView path);
  // Canonical byte-level decode: identical logic to fromJson() above
  // (shared via a private template, see Games.cpp), operating directly on
  // the lossless AST (see RawJson.h) so an Unknown tag's complete raw
  // object -- including any numeric literal nested inside a future
  // payload -- survives exactly rather than only as closely as
  // QJsonValue's double-backed storage allows.
  [[nodiscard]] static ValueOrError<GameState> fromRawJson(const Json::Value &v,
                                                           QStringView path);
  // Parses `bytes` through the canonical raw-byte parser (see RawJson.h)
  // and decodes via fromRawJson() above.
  [[nodiscard]] static ValueOrError<GameState>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  [[nodiscard]] QJsonObject toJson() const;
  // Canonical byte-level encode: composes the lossless AST directly (see
  // RawJson.h) -- toJson() above is now implemented in terms of this --
  // so unknownRaw()'s complete original object (including any numeric
  // literal beyond double precision) survives an encode-then-decode round
  // trip byte-exact via toJsonBytes(), not merely as closely as
  // Json::Value::toQJson() allows.
  [[nodiscard]] Json::Value toRawJson() const;
  [[nodiscard]] ValueOrError<QByteArray> toJsonBytes() const;

  [[nodiscard]] Kind kind() const noexcept { return m_kind; }
  // Only meaningful for Pending/ChooseDecks: the seats still owed a deck.
  [[nodiscard]] const QList<QUuid> &playerIds() const noexcept {
    return m_playerIds;
  }
  // Only meaningful for Unknown: the tag string as received, so a decoded
  // value that could not be interpreted can still be logged/displayed and
  // re-encoded without inventing a tag.
  [[nodiscard]] const QString &unknownTag() const noexcept {
    return m_unknownTag;
  }
  // Unknown only: the complete raw decoded object (its "tag" and, if
  // present, "contents", plus any additive sibling key a future backend
  // release adds alongside them), preserved verbatim as a lossless
  // Json::Value (see RawJson.h) -- never QJsonObject/QJsonValue, so no
  // additive key beside "contents" is silently dropped. This is the only
  // representation this class guarantees round-trips byte-exact
  // (including a huge numeric literal this client cannot interpret): use
  // unknownRaw().toJsonBytes() (or decode it further yourself) whenever
  // exactness beyond a double's 2^53/int64 range matters. Note that a
  // duplicate object key anywhere in the payload is rejected by
  // Json::Value::parse() itself (see RawJson.cpp) and therefore never
  // reaches this representation in the first place. toJson() below is a
  // QJsonObject-typed convenience for the Unknown case only as exact as
  // Json::Value::toQJson() itself is (see its doc comment in RawJson.h):
  // an exact-int64 numeric literal survives, but anything requiring more
  // precision than QJsonValue's double-backed storage allows does not.
  [[nodiscard]] const Json::Value &unknownRaw() const noexcept {
    return m_unknownRaw;
  }
  // Convenience view derived from unknownRaw(): the raw "contents" the
  // unrecognized tag object carried, if any (Undefined if it had none).
  // Never the source of truth for encoding -- toJson()/unknownRaw() are.
  [[nodiscard]] Json::Value unknownContents() const {
    return m_unknownRaw.isObject()
               ? m_unknownRaw.value(QLatin1StringView("contents"))
               : Json::Value{};
  }

  friend bool operator==(const GameState &, const GameState &) = default;

private:
  GameState() = default;

  // Shared decode body for fromJson()/fromRawJson() above: V is QJsonValue
  // or Json::Value. Defined in Games.cpp; a private member template
  // (rather than a free function) so it may use the private constructor
  // above directly.
  template <typename V>
  [[nodiscard]] static ValueOrError<GameState> fromValueImpl(const V &v,
                                                             QStringView path);

  Kind m_kind{Kind::Unknown};
  QList<QUuid> m_playerIds;
  QString m_unknownTag;
  Json::Value m_unknownRaw;
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
  // Canonical byte-level decode: identical logic to fromJson() above
  // (shared via a private template, see Games.cpp), operating directly on
  // the lossless AST (see RawJson.h) so a GameState::Unknown tag's complete
  // raw object and any playerCount value outside IEEE-754 double's exact
  // integer range survive undamaged end-to-end.
  [[nodiscard]] static ValueOrError<GameListRow>
  fromRawJson(const Json::Value &v, QStringView path);
  // Parses `bytes` through the canonical raw-byte parser (see RawJson.h),
  // rejecting duplicate object keys before this (or any nested) decode
  // runs, and decodes via fromRawJson() above.
  [[nodiscard]] static ValueOrError<GameListRow>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  [[nodiscard]] ValueOrError<QJsonObject> toJson() const;
  // Canonical byte-level encode: composes the lossless AST directly (see
  // RawJson.h), routing gameState through GameState::toRawJson() rather
  // than its QJsonObject-typed toJson() -- so an Unknown gameState's
  // numeric literal outside IEEE-754 double's exact-integer range
  // survives an encode-then-reparse round trip through this *aggregate*,
  // not merely GameState in isolation. toJson() above remains a
  // QJsonObject-typed convenience only as exact as Json::Value::toQJson()
  // allows (see its doc comment in RawJson.h) and is implemented in terms
  // of this.
  [[nodiscard]] ValueOrError<Json::Value> toRawJson() const;
  [[nodiscard]] ValueOrError<QByteArray> toJsonBytes() const;

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

  template <typename V>
  [[nodiscard]] static ValueOrError<GameListRow>
  fromValueImpl(const V &v, QStringView path);

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
// Canonical byte-level decode: identical logic to decodeGameList() above
// (shared via a template, see Games.cpp), operating directly on the
// lossless AST (see RawJson.h).
[[nodiscard]] ValueOrError<QList<GameListRow>>
decodeGameListFromRawJson(const Json::Value &v, QStringView path);
// Parses `bytes` through the canonical raw-byte parser (see RawJson.h),
// rejecting duplicate object keys before any nested row decode runs, and
// decodes via decodeGameListFromRawJson() above.
[[nodiscard]] ValueOrError<QList<GameListRow>>
decodeGameListFromRawBytes(QByteArrayView bytes, QStringView path);
[[nodiscard]] ValueOrError<QJsonArray>
encodeGameList(const QList<GameListRow> &rows);
// Canonical byte-level encode: composes the lossless AST directly (see
// RawJson.h), routing each row through GameListRow::toRawJson() rather
// than its QJsonObject-typed toJson() -- so an unrecognized gameState's
// exact numeric literal survives an encode-then-reparse round trip
// through the *whole list*, not merely one row's GameState in isolation.
[[nodiscard]] ValueOrError<Json::Value>
encodeGameListToRawJson(const QList<GameListRow> &rows);
[[nodiscard]] ValueOrError<QByteArray>
encodeGameListToJsonBytes(const QList<GameListRow> &rows);

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
class CampaignOptionRequest;

class CampaignOption {
public:
  enum class Kind { Known, Variant, Unknown };

  // Fails if `option` is not one of kKnownCampaignOptionTable's entries:
  // unlike a decode (which only ever iterates that same table and can
  // never produce an out-of-table value), this factory accepts an
  // arbitrary caller-supplied KnownCampaignOption -- including one
  // fabricated via static_cast from a value outside the enum's real
  // range -- so it must validate rather than trust its argument. This is
  // what makes toJson()/toRawJson()'s encodeClosedEnum() call below
  // provably safe: a CampaignOption's m_known, once set, is always
  // table-valid.
  [[nodiscard]] static ValueOrError<CampaignOption>
  knownOption(KnownCampaignOption option);
  [[nodiscard]] static CampaignOption variantOption(QString contents);

  [[nodiscard]] static ValueOrError<CampaignOption>
  fromJson(const QJsonValue &v, QStringView path);
  // Canonical byte-level decode: identical logic to fromJson() above
  // (shared via a private template, see Games.cpp), operating directly on
  // the lossless AST (see RawJson.h) so an Unknown tag's complete raw
  // object -- including any numeric literal nested inside a future
  // payload -- survives exactly rather than only as closely as
  // QJsonValue's double-backed storage allows.
  [[nodiscard]] static ValueOrError<CampaignOption>
  fromRawJson(const Json::Value &v, QStringView path);
  // Parses `bytes` through the canonical raw-byte parser (see RawJson.h)
  // and decodes via fromRawJson() above.
  [[nodiscard]] static ValueOrError<CampaignOption>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  [[nodiscard]] ValueOrError<QJsonObject> toJson() const;
  // Canonical byte-level encode: composes the lossless AST directly (see
  // RawJson.h) -- toJson() above is now implemented in terms of this --
  // so rawJson()'s complete original object survives an encode-then-
  // decode round trip byte-exact via toJsonBytes(), not merely as
  // closely as Json::Value::toQJson() allows. Fails only if this
  // instance's KnownCampaignOption was fabricated via static_cast from an
  // out-of-table value bypassing knownOption()'s validation (impossible
  // through any public API this class itself exposes).
  [[nodiscard]] ValueOrError<Json::Value> toRawJson() const;
  [[nodiscard]] ValueOrError<QByteArray> toJsonBytes() const;

  [[nodiscard]] Kind kind() const noexcept { return m_kind; }
  [[nodiscard]] std::optional<KnownCampaignOption> known() const noexcept {
    return m_known;
  }
  // Variant's contents (Kind::Variant) or the unrecognized tag string
  // (Kind::Unknown); empty for Kind::Known.
  [[nodiscard]] const QString &text() const noexcept { return m_text; }
  // The complete raw decoded object -- "tag" and, if present, "contents",
  // plus any additive sibling key a future backend release adds alongside
  // them -- preserved verbatim as a lossless Json::Value (see RawJson.h)
  // for EVERY kind this instance decoded as (Known and Variant, not only
  // Unknown): a known tag's response object can carry additive/nullable
  // fields this client does not model just as easily as an unrecognized
  // one can, and round 6's review requires those survive too, not merely
  // the recognized tag/contents. Undefined if this instance was built via
  // knownOption()/variantOption() rather than decoded from JSON (there is
  // no "original wire object" to preserve in that case). This is the only
  // representation guaranteed to round-trip byte-exact (including a huge
  // numeric literal this client cannot interpret): use
  // rawJson().toJsonBytes() (or decode it further yourself) whenever
  // exactness beyond a double's 2^53/int64 range matters. Note that a
  // duplicate object key anywhere in the payload is rejected by
  // Json::Value::parse() itself (see RawJson.cpp) and therefore never
  // reaches this representation in the first place. toJson() below is a
  // QJsonObject-typed convenience only as exact as Json::Value::toQJson()
  // itself is (see its doc comment in RawJson.h): an exact-int64 numeric
  // literal survives, but anything requiring more precision than
  // QJsonValue's double-backed storage allows does not.
  [[nodiscard]] const Json::Value &rawJson() const noexcept { return m_raw; }
  // Convenience view derived from rawJson(): the raw "contents" the
  // decoded object carried, if any (Undefined if it had none or this
  // instance was not decoded from JSON). Never the source of truth for
  // encoding -- toJson()/rawJson() are. Named for its original (and still
  // primary) use identifying an unrecognized tag's payload; Kind::Known's
  // KnownCampaignOption constructors are all nullary per the pinned
  // backend (see kKnownCampaignOptionTable), so a Known instance's
  // rawJson() ordinarily has no "contents" key at all -- one appearing
  // there (even null) is itself the additive-field case toRequestOption()
  // below refuses to silently narrow past.
  [[nodiscard]] Json::Value unknownContents() const {
    return m_raw.isObject() ? m_raw.value(QLatin1StringView("contents"))
                            : Json::Value{};
  }

  // Narrows this decoded (possibly Kind::Unknown) option down to the closed
  // CampaignOptionRequest a createGameRequest may actually submit. Fails
  // for Kind::Unknown: an option this client could not interpret must
  // never be silently resubmitted as if the server already understood it.
  // Also fails for Kind::Known/Kind::Variant whenever this instance was
  // decoded from a raw object whose keys are not *exactly* the closed
  // request shape (just "tag" for Known; "tag" and "contents" for
  // Variant) -- e.g. an explicit "contents": null beside a known nullary
  // tag, or any additive sibling key -- rather than silently discarding
  // whatever this client could not interpret and resubmitting a
  // deceptively "clean" request. A CampaignOption built via
  // knownOption()/variantOption() (rawJson() Undefined, nothing to lose)
  // always narrows successfully.
  [[nodiscard]] ValueOrError<CampaignOptionRequest>
  toRequestOption(QStringView path) const;

  friend bool operator==(const CampaignOption &lhs, const CampaignOption &rhs) {
    // Deliberately excludes rawJson(): equality reflects this option's
    // logical value (kind/known/text), not which exact bytes happened to
    // accompany a particular decode -- knownOption(X) and a fresh decode
    // of X's canonical {"tag": ...} must compare equal even though only
    // the latter populates rawJson(). toRequestOption()'s additive-field
    // refusal (not operator==) is what protects a decode's extra raw data
    // from being silently discarded.
    return lhs.m_kind == rhs.m_kind && lhs.m_known == rhs.m_known &&
           lhs.m_text == rhs.m_text;
  }

private:
  CampaignOption() = default;

  // Shared decode body for fromJson()/fromRawJson() above: V is QJsonValue
  // or Json::Value. Defined in Games.cpp; a private member template
  // (rather than a free function) so it may use the private constructor
  // above directly.
  template <typename V>
  [[nodiscard]] static ValueOrError<CampaignOption>
  fromValueImpl(const V &v, QStringView path);

  Kind m_kind{Kind::Unknown};
  std::optional<KnownCampaignOption> m_known;
  QString m_text;
  Json::Value m_raw;
};

// createGameRequest.options's actual element type. Unlike CampaignOption
// (which also has to represent whatever an already-decoded GameListRow's
// campaign happened to carry, including a tag this client cannot
// interpret), a request this client itself composes can only ever contain
// options the client understands: there is deliberately no Kind::Unknown
// here and no public factory that could fabricate one, so "an unknown
// option cannot be submitted" is a property of the type, not a runtime
// check callers might skip.
class CampaignOptionRequest {
public:
  enum class Kind { Known, Variant };

  // Fails if `option` is not one of kKnownCampaignOptionTable's entries
  // (see CampaignOption::knownOption()'s identical rationale above).
  [[nodiscard]] static ValueOrError<CampaignOptionRequest>
  knownOption(KnownCampaignOption option);
  [[nodiscard]] static CampaignOptionRequest variantOption(QString contents);

  // Fails on any tag this client does not recognize (including
  // "CampaignVariant" with a non-string contents), or on any recognized
  // tag whose object carries a key beyond the exact closed request shape
  // (just "tag" for a known option; "tag" and "contents" for
  // "CampaignVariant") -- there is no forward-compatible fallback for a
  // request-bound value.
  [[nodiscard]] static ValueOrError<CampaignOptionRequest>
  fromJson(const QJsonValue &v, QStringView path);
  // Canonical byte-level decode overload: identical logic (shared via a
  // private template, see Games.cpp), operating directly on the lossless
  // AST (see RawJson.h) so a raw aggregate decoder (e.g.
  // CreateGameRequest::fromRawJson) can decode its nested "options" array
  // without dropping to QJsonValue for this element type.
  [[nodiscard]] static ValueOrError<CampaignOptionRequest>
  fromJson(const Json::Value &v, QStringView path);
  [[nodiscard]] ValueOrError<QJsonObject> toJson() const;

  [[nodiscard]] Kind kind() const noexcept { return m_kind; }
  [[nodiscard]] std::optional<KnownCampaignOption> known() const noexcept {
    return m_known;
  }
  [[nodiscard]] const QString &text() const noexcept { return m_text; }

  friend bool operator==(const CampaignOptionRequest &,
                         const CampaignOptionRequest &) = default;

private:
  CampaignOptionRequest() = default;

  template <typename V>
  [[nodiscard]] static ValueOrError<CampaignOptionRequest>
  fromValueImpl(const V &v, QStringView path);

  Kind m_kind{Kind::Known};
  std::optional<KnownCampaignOption> m_known;
  QString m_text;
};

// createGameRequest's real invariant, per the backend's own dispatch
// (Api/Handler/Arkham/Games.hs postApiV1ArkhamGamesR): `campaignId` set
// dispatches to `newCampaign cid scenarioId ...` regardless of whether
// `scenarioId` is also set (a campaign may start at a specific scenario),
// so campaign-only and campaign-with-starting-scenario are both valid.
// Only when `campaignId` is absent does `scenarioId` become required (a
// standalone scenario); "neither set" is the sole invalid combination
// (`error "missing either a campign id or a scenario id"` [sic] on the
// backend -- this is a verbatim quotation of the backend's own typo'd
// error string at Api/Handler/Arkham/Games.hs:160, not a mistake in this
// comment).
// The private constructor makes that one invalid (neither-set) state
// unrepresentable -- campaign()/campaignWithStartingScenario()/scenario()
// are the only ways to build an instance, and fromJson rejects a wire
// object satisfying neither.
class CampaignOrScenario {
public:
  [[nodiscard]] static CampaignOrScenario campaign(CampaignId id);
  [[nodiscard]] static CampaignOrScenario
  campaignWithStartingScenario(CampaignId campaignId, ScenarioId scenarioId);
  [[nodiscard]] static CampaignOrScenario scenario(ScenarioId id);

  [[nodiscard]] static ValueOrError<CampaignOrScenario>
  fromJson(const QJsonObject &requestObj, QStringView path);
  // Canonical byte-level decode overload: identical logic (shared via a
  // private template, see Games.cpp), operating directly on the lossless
  // AST (see RawJson.h) for CreateGameRequest::fromRawJson.
  [[nodiscard]] static ValueOrError<CampaignOrScenario>
  fromJson(const Json::Value &requestObj, QStringView path);
  // Inserts this request's resolved "campaignId"/"scenarioId" keys (each
  // either the real id or explicit JSON null) into `obj`, matching the
  // fixture's own encoding (both keys always present).
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

  template <typename Obj>
  [[nodiscard]] static ValueOrError<CampaignOrScenario>
  fromValueImpl(const Obj &requestObj, QStringView path);

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
  qint64 playerCount{};
  CampaignOrScenario campaignOrScenario;
  Difficulty difficulty{};
  QString campaignName;
  MultiplayerVariant multiplayerVariant{};
  bool includeTarotReadings{};
  QList<CampaignOptionRequest> options;
  std::optional<bool> strictAsIfAt;
  std::optional<AsIfRulingValue> asIfRuling;
  QList<UltimatumOrBoon> ultimatumsAndBoons;
  bool achievementsEnabled{true};

  [[nodiscard]] static ValueOrError<CreateGameRequest>
  fromJson(const QJsonValue &v, QStringView path);
  // Canonical byte-level decode: identical logic to fromJson() above
  // (shared via a template, see Games.cpp), operating directly on the
  // lossless AST (see RawJson.h) so playerCount survives exactly outside
  // IEEE-754 double's exact-integer range, an unrecognized CampaignOption
  // tag's complete raw payload is visible for diagnostics before this
  // request-bound decode rejects it, and nested numeric literals anywhere
  // in the request body are never silently rounded.
  [[nodiscard]] static ValueOrError<CreateGameRequest>
  fromRawJson(const Json::Value &v, QStringView path);
  // Parses `bytes` through the canonical raw-byte parser (see RawJson.h),
  // rejecting duplicate object keys before any nested decode runs, and
  // decodes via fromRawJson() above.
  [[nodiscard]] static ValueOrError<CreateGameRequest>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  // Fails (rather than silently emitting an invalid request) if any
  // present entry in `deckIds` is the null (all-zero) uuid: this struct's
  // fields are public for ergonomic aggregate construction, so unlike
  // fromJson()/fromRawJson() (which route every uuid through
  // Json::decodeUuid and can never produce a null one), a caller
  // constructing/mutating a CreateGameRequest by hand could otherwise
  // bypass that invariant and emit a wire value the backend -- and this
  // client's own decoder -- would reject.
  [[nodiscard]] ValueOrError<QJsonObject> toJson() const;

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
  // Canonical byte-level decode: identical logic to fromJson() above
  // (shared via decodeChooseDeckRequest<Obj>, see Games.cpp), operating
  // directly on the lossless AST (see RawJson.h) so a numeric literal
  // nested inside deckList's sideSlots survives exactly rather than only
  // as closely as QJsonValue's double-backed storage allows.
  [[nodiscard]] static ValueOrError<ChooseDeckRequest>
  fromRawJson(const Json::Value &v, QStringView path);
  // Parses `bytes` through the canonical raw-byte parser (see RawJson.h)
  // and decodes via fromRawJson() above -- never collapsing to QJsonValue
  // first, unlike this method's previous "decode via fromJson(), then
  // patch deckList back in from the raw tree" implementation.
  [[nodiscard]] static ValueOrError<ChooseDeckRequest>
  fromRawBytes(QByteArrayView bytes, QStringView path);
  // See Decks.h's DeckListInput::toJson() doc comment: fails (rather
  // than silently rounding) whenever a present deckList's `id` cannot be
  // exactly represented as a QJsonValue.
  [[nodiscard]] ValueOrError<QJsonObject> toJson() const;
  // Precision-preserving equivalent of toJson(); see
  // DeckListInput::toJsonBytes().
  [[nodiscard]] ValueOrError<QByteArray> toJsonBytes() const;

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
