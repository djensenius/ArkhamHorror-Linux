#pragma once

#include "Identifiers.h"
#include "RawJson.h"
#include "ValueOrError.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <optional>
#include <utility>

namespace Arkham {

// contracts/schemas/catalog.schema.json's `cardType` enum. Closed: the issue
// scopes "unknown tags/enums must remain explicit" to the game-list/
// lifecycle section, not to the catalog's own closed enums, so an
// unrecognized value here is a hard decode error rather than a forward
// -compatible fallback.
enum class CardType {
  AssetType,
  EventType,
  SkillType,
  PlayerTreacheryType,
  PlayerEnemyType,
  TreacheryType,
  EnemyType,
  LocationType,
  EnemyLocationCardType,
  EncounterAssetType,
  EncounterEventType,
  ActType,
  AgendaType,
  StoryType,
  InvestigatorType,
  ScenarioType,
  KeyType,
};

// `cardSubType` enum.
enum class CardSubType {
  Weakness,
  BasicWeakness,
};

// `classSymbol` enum.
enum class ClassSymbol {
  Guardian,
  Seeker,
  Survivor,
  Rogue,
  Mystic,
  Neutral,
  Mythos,
};

// `revelation` enum.
enum class Revelation {
  NoRevelation,
  IsRevelation,
  CannotBeCanceledRevelation,
};

// `slotType` enum.
enum class SlotType {
  HandSlot,
  BodySlot,
  AllySlot,
  AccessorySlot,
  ArcaneSlot,
  TarotSlot,
  HeadSlot,
};

// `whenDiscarded` enum.
enum class WhenDiscarded {
  ToDiscard,
  ToBonded,
  ToSetAside,
};

// `outOfPlayEffects` item enum.
enum class OutOfPlayEffect {
  InHandEffect,
  InDiscardEffect,
  InSearchEffect,
  OnTopOfDeckEffect,
};

// The skill type named by a `SkillIcon`-tagged `skillIcon` entry.
enum class SkillType {
  SkillWillpower,
  SkillIntellect,
  SkillCombat,
  SkillAgility,
};

// `skillIcon`: a `SkillIcon` (with a nested SkillType) or a wild/wild-minus
// icon (no contents). The private constructor makes an inconsistent state
// (e.g. tag == SkillIcon with no skill type set, or a nullary tag carrying
// a payload) unrepresentable, so toJson() never needs to guard against --
// or abort on -- a combination its own factories/fromJson could never
// produce. An unrecognized tag decodes to Tag::Unknown, preserving the
// complete raw decoded object verbatim (its "tag" and, if present,
// "contents"); Unknown has no public factory, so production code composing
// a skillIcon can never fabricate one -- only fromJson can produce it, and
// toJson() re-encodes exactly what was decoded, never participating in any
// known-tag behavior.
enum class SkillIconTag { SkillIcon, WildIcon, WildMinusIcon, Unknown };
class SkillIcon {
public:
  // Fails if `type` is not one of kSkillTypeTable's entries -- e.g. a
  // SkillType fabricated via static_cast from outside the enum's real
  // range -- so toJson()/toRawJson()'s encodeClosedEnum() call below is
  // provably safe once this succeeds (see CampaignOption::knownOption()'s
  // identical rationale in Games.h).
  [[nodiscard]] static ValueOrError<SkillIcon> skillType(SkillType type);
  [[nodiscard]] static SkillIcon wild();
  [[nodiscard]] static SkillIcon wildMinus();

  [[nodiscard]] static ValueOrError<SkillIcon> fromJson(const QJsonValue &v,
                                                        QStringView path);
  // Canonical byte-level decode: identical logic to fromJson() above
  // (shared via a private template, see CardCatalog.cpp), operating
  // directly on the lossless AST (see RawJson.h) so an Unknown tag's
  // complete raw object -- including any numeric literal nested inside a
  // future payload -- survives exactly rather than only as closely as
  // QJsonValue's double-backed storage allows.
  [[nodiscard]] static ValueOrError<SkillIcon> fromRawJson(const Json::Value &v,
                                                           QStringView path);
  [[nodiscard]] ValueOrError<QJsonObject> toJson() const;
  // Canonical byte-level encode: composes the lossless AST directly (see
  // RawJson.h) rather than through toJson()'s QJsonObject -- toJson()
  // itself is now implemented in terms of this -- so an Unknown tag's
  // unknownRaw() (or, once nested inside a CardDef/other aggregate, a
  // numeric literal outside qint64 range anywhere within it) survives an
  // encode-then-decode round trip byte-exact. Fails only if this
  // instance's SkillType was fabricated via static_cast from an
  // out-of-table value bypassing skillType()'s validation (impossible
  // through any public API this class itself exposes).
  [[nodiscard]] ValueOrError<Json::Value> toRawJson() const;
  [[nodiscard]] ValueOrError<QByteArray> toJsonBytes() const;

  [[nodiscard]] SkillIconTag tag() const noexcept { return m_tag; }
  // Populated only when tag() == SkillIconTag::SkillIcon.
  [[nodiscard]] std::optional<SkillType> skill() const noexcept {
    return m_skill;
  }
  // Tag::Unknown only: the complete raw decoded object (its "tag" and, if
  // present, "contents", plus any additive keys a future backend release
  // adds), preserved verbatim as a lossless Json::Value (see RawJson.h) --
  // never QJsonObject, so a huge numeric literal this client cannot
  // interpret still round-trips byte-exact. Note that a duplicate object
  // key anywhere in the payload is rejected by Json::Value::parse()
  // itself (see RawJson.cpp) and therefore never reaches this
  // representation in the first place.
  [[nodiscard]] const Json::Value &unknownRaw() const noexcept {
    return m_unknownRaw;
  }

  friend bool operator==(const SkillIcon &, const SkillIcon &) = default;

private:
  SkillIcon() = default;

  // Shared decode body for fromJson()/fromRawJson() above: V is QJsonValue
  // or Json::Value. Defined in CardCatalog.cpp; a private member template
  // (rather than a free function) so it may use the private constructor
  // above directly.
  template <typename V>
  [[nodiscard]] static ValueOrError<SkillIcon> fromValueImpl(const V &v,
                                                             QStringView path);

  SkillIconTag m_tag{SkillIconTag::WildIcon};
  std::optional<SkillType> m_skill;
  Json::Value m_unknownRaw;
};

// `cardCost`: a static integer, one of three no-payload dynamic variants, or
// one of three variants whose contents the schema leaves fully unconstrained
// (`{}`), preserved verbatim as raw JSON. As with SkillIcon above, the
// private constructor/validated factories make every inconsistent state
// (missing payload for a payload-carrying tag, or a payload present on a
// nullary tag) unrepresentable; an unrecognized tag decodes to
// Tag::Unknown, preserving the complete raw object and never participating
// in known-tag encoding.
enum class CardCostTag {
  StaticCost,
  DynamicCost,
  DiscardAmountCost,
  DeferredCost,
  MaxDynamicCost,
  AnyMatchingCardCost,
  MatchingEnemyFieldCost,
  Unknown,
};
class CardCost {
public:
  [[nodiscard]] static CardCost staticCost(qint64 amount);
  [[nodiscard]] static CardCost dynamicCost();
  [[nodiscard]] static CardCost discardAmountCost();
  [[nodiscard]] static CardCost deferredCost();
  // Rejects Kind::Undefined contents (a caller must supply an explicit,
  // present JSON value -- the schema leaves its shape unconstrained, but
  // "unconstrained" is not the same as "may be omitted") -- see class
  // comment. `contents` is the lossless Json::Value AST (see RawJson.h),
  // not QJsonValue: this schema-unconstrained payload may itself carry a
  // number outside QJsonValue's exact range (e.g. nested arbitrarily deep
  // inside a future backend's richer cost description), which only
  // Json::Value can preserve through a decode-then-encode round trip.
  [[nodiscard]] static ValueOrError<CardCost>
  maxDynamicCost(Json::Value contents);
  [[nodiscard]] static ValueOrError<CardCost>
  anyMatchingCardCost(Json::Value contents);
  // Additionally requires `contents` to be a JSON array of exactly two
  // elements: the pinned backend's `MatchingEnemyFieldCost EnemyMatcher
  // EnemyCostField` (Arkham.Card.Cost, backend commit 6a1befbd7b) is a
  // genuine two-argument constructor, and Aeson's default TaggedObject
  // derivation for a multi-argument constructor encodes `contents` as a
  // JSON array of exactly that many elements -- so, unlike
  // MaxDynamicCost/AnyMatchingCardCost's single-argument constructors,
  // this tag's wire shape is NOT actually schema-unconstrained despite
  // catalog.schema.json's conservative `contents: {}`. The two elements'
  // own internal structure remains genuinely unconstrained.
  [[nodiscard]] static ValueOrError<CardCost>
  matchingEnemyFieldCost(Json::Value contents);

  [[nodiscard]] static ValueOrError<CardCost> fromJson(const QJsonValue &v,
                                                       QStringView path);
  // Canonical byte-level decode: see SkillIcon::fromRawJson()'s doc
  // comment -- identical logic, operating directly on Json::Value so
  // rawContents()/unknownRaw() below are exact even for a deeply nested
  // number no QJsonValue could represent.
  [[nodiscard]] static ValueOrError<CardCost> fromRawJson(const Json::Value &v,
                                                          QStringView path);
  [[nodiscard]] QJsonObject toJson() const;
  // Canonical byte-level encode: see SkillIcon::toRawJson()'s doc
  // comment -- identical rationale, since rawContents()/unknownRaw() may
  // themselves hold a number outside qint64's exact range.
  [[nodiscard]] Json::Value toRawJson() const;
  [[nodiscard]] ValueOrError<QByteArray> toJsonBytes() const;

  [[nodiscard]] CardCostTag tag() const noexcept { return m_tag; }
  // Populated only when tag() == CardCostTag::StaticCost.
  [[nodiscard]] std::optional<qint64> staticAmount() const noexcept {
    return m_staticAmount;
  }
  // Populated only when tag() is MaxDynamicCost / AnyMatchingCardCost /
  // MatchingEnemyFieldCost; schema-unconstrained, preserved verbatim as a
  // lossless Json::Value (see RawJson.h).
  [[nodiscard]] const Json::Value &rawContents() const noexcept {
    return m_rawContents;
  }
  // Tag::Unknown only: the complete raw decoded object, preserved verbatim
  // as a lossless Json::Value.
  [[nodiscard]] const Json::Value &unknownRaw() const noexcept {
    return m_unknownRaw;
  }

  friend bool operator==(const CardCost &, const CardCost &) = default;

private:
  CardCost() = default;

  template <typename V>
  [[nodiscard]] static ValueOrError<CardCost> fromValueImpl(const V &v,
                                                            QStringView path);

  CardCostTag m_tag{CardCostTag::DynamicCost};
  std::optional<qint64> m_staticAmount;
  Json::Value m_rawContents;
  Json::Value m_unknownRaw;
};

// `gameValue`: a single static amount, a (static, perPlayer) pair, four
// by-player-count amounts, or one of three no-payload variants. As with
// SkillIcon/CardCost above, the private constructor/validated factories
// make every inconsistent state unrepresentable (in particular, the
// StaticWithPerPlayer/ByPlayerCount factories take their fixed-arity
// payload as separate qint64 parameters rather than a QList<qint64>, so a
// wrong-size list can never be constructed in the first place). An
// unrecognized tag decodes to Tag::Unknown, preserving the complete raw
// object; note this is distinct from the *known* backend tag literally
// named "ValueUnknown" (GameValueTag::ValueUnknown), which is a normal
// nullary variant like ValueX/ValueStar.
enum class GameValueTag {
  Static,
  PerPlayer,
  StaticWithPerPlayer,
  ByPlayerCount,
  ValueX,
  ValueStar,
  ValueUnknown,
  Unknown,
};
class GameValue {
public:
  [[nodiscard]] static GameValue staticValue(qint64 amount);
  [[nodiscard]] static GameValue perPlayer(qint64 amount);
  [[nodiscard]] static GameValue staticWithPerPlayer(qint64 staticAmount,
                                                     qint64 perPlayerAmount);
  // The pinned backend's `ByPlayerCount Int Int Int Int` (Arkham.GameValue,
  // backend commit 6a1befbd7b) is looked up by `fromGameValue` via an exact
  // `case pc of 1 -> ...; 2 -> ...; 3 -> ...; 4 -> ...`, i.e. these are four
  // distinct positional values for exactly 1/2/3/4 players -- not the
  // "oneOrTwo/three/four/fiveOrMore" grouping an earlier revision of this
  // client mistakenly named them after (there is no separate 5-or-more
  // slot; the backend errors for any other player count).
  [[nodiscard]] static GameValue byPlayerCount(qint64 onePlayer,
                                               qint64 twoPlayers,
                                               qint64 threePlayers,
                                               qint64 fourPlayers);
  [[nodiscard]] static GameValue valueX();
  [[nodiscard]] static GameValue valueStar();
  [[nodiscard]] static GameValue valueUnknown();

  [[nodiscard]] static ValueOrError<GameValue> fromJson(const QJsonValue &v,
                                                        QStringView path);
  // Canonical byte-level decode: see SkillIcon::fromRawJson()'s doc
  // comment.
  [[nodiscard]] static ValueOrError<GameValue> fromRawJson(const Json::Value &v,
                                                           QStringView path);
  [[nodiscard]] QJsonObject toJson() const;
  // Canonical byte-level encode: see SkillIcon::toRawJson()'s doc comment.
  [[nodiscard]] Json::Value toRawJson() const;
  [[nodiscard]] ValueOrError<QByteArray> toJsonBytes() const;

  [[nodiscard]] GameValueTag tag() const noexcept { return m_tag; }
  // Populated only for tag() == Static/PerPlayer.
  [[nodiscard]] std::optional<qint64> singleAmount() const noexcept {
    return m_singleAmount;
  }
  // Size 2 for StaticWithPerPlayer (staticAmount, perPlayerAmount); size 4
  // for ByPlayerCount (onePlayer, twoPlayers, threePlayers, fourPlayers,
  // in that exact positional order -- see byPlayerCount()'s doc comment);
  // empty otherwise.
  [[nodiscard]] const QList<qint64> &contents() const noexcept {
    return m_contents;
  }
  // Tag::Unknown only: the complete raw decoded object, preserved verbatim
  // as a lossless Json::Value (see RawJson.h).
  [[nodiscard]] const Json::Value &unknownRaw() const noexcept {
    return m_unknownRaw;
  }

  friend bool operator==(const GameValue &, const GameValue &) = default;

private:
  GameValue() = default;

  template <typename V>
  [[nodiscard]] static ValueOrError<GameValue> fromValueImpl(const V &v,
                                                             QStringView path);

  GameValueTag m_tag{GameValueTag::ValueX};
  std::optional<qint64> m_singleAmount;
  QList<qint64> m_contents;
  Json::Value m_unknownRaw;
};

// A single card definition, covering every top-level property documented by
// catalog.schema.json's `cardDef`. Fields the schema leaves fully
// unconstrained (`{}`, or an array/object whose element/property shape is
// unconstrained) are preserved verbatim as the lossless Json::Value AST
// (see RawJson.h), never QJsonValue: catalog.json is a production
// response this client decodes via the canonical byte-level fromRawBytes()
// entry point below, and a number nested at any depth inside e.g.
// `criteria`/`meta`/`customizations` must survive a decode-then-encode
// round trip exactly, which only the byte-parsed AST (not QJsonValue's
// double-backed storage) can guarantee. A duplicate object key anywhere in
// the payload is rejected by Json::Value::parse() itself (see
// RawJson.cpp), never silently collapsed. Absent fields decode to Json::Value's
// default Kind::Undefined and are omitted again by toJson()/toRawJson(), so a
// round trip of an untouched fixture entry is byte-faithful modulo key order.
// Required fields are cardCode, name, cardType, and art; every other field is
// optional, decoding to std::nullopt / an empty container when absent.
//
// Deliberate additive-field policy: any top-level object key this type
// does not itself model is silently ignored by fromJson()/fromRawJson(),
// not rejected -- even though the vendored catalog.schema.json actually
// declares `additionalProperties: false` for cardDef. This is a
// deliberate CLIENT policy divergence from the schema's own strictness,
// not something the schema itself specifies: intentional
// forward-compatible leniency for a read-only response type -- CardDef is
// decoded from the card catalog endpoint and is never itself re-encoded as part
// of an outbound request, so tolerating a field this client version does not
// yet recognize carries no outbound-safety risk (contrast with e.g.
// CampaignOption/CampaignOptionRequest's split, where only the closed *Request
// side must reject anything unrecognized because it IS submitted). Every field
// this type does model is still validated exactly: a known field's wrong JSON
// type, a required field's absence, or a malformed value for a field with real
// constraints (duplicate uniqueItems entries, wrong outer array/object shape,
// etc.) is a hard decode failure, never silently dropped into "absent" -- only
// genuinely *unrecognized* keys are ignored. The schema types
// level/victoryPoints/vengeancePoints/overrideActionPlayableIfCriteriaMet/
// permanent/encounterSet/encounterSetQuantity/unique/doubleSided/
// exceptional/playableFromDiscard/stage/grantedXp/canReplace/
// skipPlayWindows/beforeEffect/canCommitWhenNoIcons/commitTrigger/errata
// strictly as integer/boolean/string (no "null" in the type union), so --
// unlike a backend field genuinely typed nullable -- an explicit JSON null
// for any of these is malformed input and fromJson()/fromRawJson() rejects
// it rather than collapsing it into "absent".
struct CardDef {
  CardCode cardCode;
  CardName name;
  CardType cardType{};
  QString art;

  std::optional<CardName> revealedName;
  std::optional<CardCost> cost;
  std::optional<qint64> level;
  std::optional<CardSubType> cardSubType;
  QList<ClassSymbol> classSymbols;
  QList<SkillIcon> skills;
  QStringList cardTraits;
  QStringList revealedCardTraits;
  std::optional<Revelation> revelation;
  std::optional<qint64> victoryPoints;
  std::optional<qint64> vengeancePoints;
  std::optional<bool> overrideActionPlayableIfCriteriaMet;
  std::optional<bool> permanent;
  std::optional<QString> encounterSet;
  std::optional<qint64> encounterSetQuantity;
  std::optional<bool> unique;
  std::optional<bool> doubleSided;
  std::optional<bool> exceptional;
  std::optional<bool> playableFromDiscard;
  std::optional<qint64> stage;
  QList<SlotType> cardSlots;
  QList<CardCode> alternateCardCodes;
  std::optional<qint64> grantedXp;
  std::optional<bool> canReplace;
  QList<std::pair<qint64, CardCode>> bondedWith;
  std::optional<bool> skipPlayWindows;
  std::optional<bool> beforeEffect;
  std::optional<CardCode> otherSide;
  std::optional<WhenDiscarded> whenDiscarded;
  std::optional<bool> canCommitWhenNoIcons;
  std::optional<bool> commitTrigger;
  QStringList tags;
  QList<OutOfPlayEffect> outOfPlayEffects;
  std::optional<GameValue> health;
  std::optional<GameValue> fight;
  std::optional<GameValue> evade;
  std::optional<GameValue> healthDamage;
  std::optional<GameValue> sanityDamage;
  QMap<QString, QList<SkillIcon>> alternateSkills;
  QMap<QString, QString> alternateErrata;
  std::optional<QString> errata;

  // Schema-unconstrained fields ("{}" in catalog.schema.json), preserved
  // verbatim (including absent-vs-null) as a lossless Json::Value -- see
  // class comment.
  Json::Value additionalCost;
  Json::Value fastWindow;
  Json::Value actions;
  Json::Value criteria;
  Json::Value uses;
  Json::Value locationSymbol;
  Json::Value locationRevealedSymbol;
  Json::Value purchaseTrauma;
  Json::Value customizations;

  // Outer-typed-but-inner-unconstrained fields: the schema constrains only
  // the outer JSON shape ("type":"array" or "type":"object", with no
  // further validation of elements/properties). fromJson()/fromRawJson()
  // validate and reject a present value of the wrong outer shape
  // (including an explicit null, which matches neither "array" nor
  // "object"), then preserve the validated value's contents verbatim as a
  // lossless Json::Value.
  Json::Value keywords;
  Json::Value commitRestrictions;
  Json::Value attackOfOpportunityModifiers;
  Json::Value limits;
  Json::Value locationConnections;
  Json::Value locationRevealedConnections;
  Json::Value deckRestrictions;
  Json::Value meta;

  [[nodiscard]] static ValueOrError<CardDef> fromJson(const QJsonValue &v,
                                                      QStringView path);
  // Canonical byte-level decode: identical logic to fromJson() above
  // (shared via a private template, see CardCatalog.cpp), operating
  // directly on the lossless AST (see RawJson.h) parsed by
  // Json::Value::parse() -- never converting the whole tree to QJsonValue
  // first -- so every schema-unconstrained field above survives a
  // decode-then-encode round trip byte-exact, including a number outside
  // qint64 range, a long fraction, or a huge exponent nested at any depth.
  // A duplicate object key anywhere in the payload is rejected by
  // Json::Value::parse() itself (see RawJson.cpp), never silently
  // collapsed. The one production entry point governed fixtures
  // (contracts/fixtures/catalog.json) and any future catalog-endpoint
  // response must use.
  [[nodiscard]] static ValueOrError<CardDef> fromRawJson(const Json::Value &v,
                                                         QStringView path);
  // Parses `bytes` per RFC 8259 exactly (see RawJson.h's Value::parse())
  // and decodes via fromRawJson() above.
  [[nodiscard]] static ValueOrError<CardDef> fromRawBytes(QByteArrayView bytes,
                                                          QStringView path);
  [[nodiscard]] ValueOrError<QJsonObject> toJson() const;
  // Canonical byte-level encode: composes the lossless AST directly (see
  // RawJson.h), recursing into every nested CardCost/GameValue/SkillIcon's
  // own toRawJson() and embedding every schema-unconstrained/outer-typed
  // field's already-native Json::Value verbatim -- never converting
  // through QJsonObject first -- so a decode(fromRawBytes)-then-encode
  // round trip of e.g. a governed catalog.json entry is exact for every
  // numeric literal nested at any depth inside criteria/meta/
  // customizations/etc., not merely as exact as QJsonObject's
  // double-backed storage allows. toJson() above is now implemented in
  // terms of this. Fails only if a nested closed-enum field (cardType,
  // cardSubType, revelation, whenDiscarded, classSymbols/slots/
  // outOfPlayEffects, or a nested SkillIcon's skill) was fabricated via
  // static_cast from an out-of-table value bypassing this codebase's own
  // validating factories/decoders -- impossible through any public API
  // this class itself exposes.
  [[nodiscard]] ValueOrError<Json::Value> toRawJson() const;
  [[nodiscard]] ValueOrError<QByteArray> toJsonBytes() const;

  friend bool operator==(const CardDef &, const CardDef &) = default;
};

// Decodes the card-catalog endpoint's top-level array response shape (a
// bare `[CardDef, ...]`) via the canonical byte-level parser (see
// RawJson.h), never QJsonDocument -- see CardDef::fromRawJson()'s doc
// comment for why this matters for this type specifically (every entry's
// schema-unconstrained fields). Note this is NOT the vendored
// contracts/fixtures/catalog.json fixture *bundle* shape, which is a
// top-level object ({"cards": [...], "homebrewCards": [...],
// "investigators": [...]}); a caller decoding that fixture must first
// extract its "cards" (or "homebrewCards") array member and pass that
// array's bytes here.
[[nodiscard]] ValueOrError<QList<CardDef>>
decodeCatalogFromRawBytes(QByteArrayView bytes, QStringView path);

} // namespace Arkham
