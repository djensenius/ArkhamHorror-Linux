#pragma once

#include "Identifiers.h"
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
  [[nodiscard]] static SkillIcon skillType(SkillType type);
  [[nodiscard]] static SkillIcon wild();
  [[nodiscard]] static SkillIcon wildMinus();

  [[nodiscard]] static ValueOrError<SkillIcon> fromJson(const QJsonValue &v,
                                                        QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  [[nodiscard]] SkillIconTag tag() const noexcept { return m_tag; }
  // Populated only when tag() == SkillIconTag::SkillIcon.
  [[nodiscard]] std::optional<SkillType> skill() const noexcept {
    return m_skill;
  }
  // Tag::Unknown only: the complete raw decoded object (its "tag" and, if
  // present, "contents"), preserved verbatim.
  [[nodiscard]] const QJsonObject &unknownRaw() const noexcept {
    return m_unknownRaw;
  }

  friend bool operator==(const SkillIcon &, const SkillIcon &) = default;

private:
  SkillIcon() = default;

  SkillIconTag m_tag{SkillIconTag::WildIcon};
  std::optional<SkillType> m_skill;
  QJsonObject m_unknownRaw;
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
  [[nodiscard]] static CardCost staticCost(int amount);
  [[nodiscard]] static CardCost dynamicCost();
  [[nodiscard]] static CardCost discardAmountCost();
  [[nodiscard]] static CardCost deferredCost();
  [[nodiscard]] static CardCost maxDynamicCost(QJsonValue contents);
  [[nodiscard]] static CardCost anyMatchingCardCost(QJsonValue contents);
  [[nodiscard]] static CardCost matchingEnemyFieldCost(QJsonValue contents);

  [[nodiscard]] static ValueOrError<CardCost> fromJson(const QJsonValue &v,
                                                       QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  [[nodiscard]] CardCostTag tag() const noexcept { return m_tag; }
  // Populated only when tag() == CardCostTag::StaticCost.
  [[nodiscard]] std::optional<int> staticAmount() const noexcept {
    return m_staticAmount;
  }
  // Populated only when tag() is MaxDynamicCost / AnyMatchingCardCost /
  // MatchingEnemyFieldCost; schema-unconstrained, preserved verbatim.
  [[nodiscard]] const QJsonValue &rawContents() const noexcept {
    return m_rawContents;
  }
  // Tag::Unknown only: the complete raw decoded object, preserved verbatim.
  [[nodiscard]] const QJsonObject &unknownRaw() const noexcept {
    return m_unknownRaw;
  }

  friend bool operator==(const CardCost &, const CardCost &) = default;

private:
  CardCost() = default;

  CardCostTag m_tag{CardCostTag::DynamicCost};
  std::optional<int> m_staticAmount;
  QJsonValue m_rawContents{QJsonValue::Undefined};
  QJsonObject m_unknownRaw;
};

// `gameValue`: a single static amount, a (static, perPlayer) pair, four
// by-player-count amounts, or one of three no-payload variants. As with
// SkillIcon/CardCost above, the private constructor/validated factories
// make every inconsistent state unrepresentable (in particular, the
// StaticWithPerPlayer/ByPlayerCount factories take their fixed-arity
// payload as separate int parameters rather than a QList<int>, so a
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
  [[nodiscard]] static GameValue staticValue(int amount);
  [[nodiscard]] static GameValue perPlayer(int amount);
  [[nodiscard]] static GameValue staticWithPerPlayer(int staticAmount,
                                                     int perPlayerAmount);
  [[nodiscard]] static GameValue byPlayerCount(int oneOrTwo, int three,
                                               int four, int fiveOrMore);
  [[nodiscard]] static GameValue valueX();
  [[nodiscard]] static GameValue valueStar();
  [[nodiscard]] static GameValue valueUnknown();

  [[nodiscard]] static ValueOrError<GameValue> fromJson(const QJsonValue &v,
                                                        QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  [[nodiscard]] GameValueTag tag() const noexcept { return m_tag; }
  // Populated only for tag() == Static/PerPlayer.
  [[nodiscard]] std::optional<int> singleAmount() const noexcept {
    return m_singleAmount;
  }
  // Size 2 for StaticWithPerPlayer, 4 for ByPlayerCount, empty otherwise.
  [[nodiscard]] const QList<int> &contents() const noexcept {
    return m_contents;
  }
  // Tag::Unknown only: the complete raw decoded object, preserved verbatim.
  [[nodiscard]] const QJsonObject &unknownRaw() const noexcept {
    return m_unknownRaw;
  }

  friend bool operator==(const GameValue &, const GameValue &) = default;

private:
  GameValue() = default;

  GameValueTag m_tag{GameValueTag::ValueX};
  std::optional<int> m_singleAmount;
  QList<int> m_contents;
  QJsonObject m_unknownRaw;
};

// A single card definition, covering every top-level property documented by
// catalog.schema.json's `cardDef`. Fields the schema leaves fully
// unconstrained (`{}`, or an array/object whose element/property shape is
// unconstrained) are preserved verbatim as raw QJsonValue -- absent fields
// decode to QJsonValue(QJsonValue::Undefined) and are omitted again by
// toJson(), so a round trip of an untouched fixture entry is byte-faithful
// modulo key order. Required fields are cardCode, name, cardType, and art;
// every other field is optional, decoding to std::nullopt / an empty
// container when absent. The schema types level/victoryPoints/
// vengeancePoints/overrideActionPlayableIfCriteriaMet/permanent/
// encounterSet/encounterSetQuantity/unique/doubleSided/exceptional/
// playableFromDiscard/stage/grantedXp/canReplace/skipPlayWindows/
// beforeEffect/canCommitWhenNoIcons/commitTrigger/errata strictly as
// integer/boolean/string (no "null" in the type union), so -- unlike a
// backend field genuinely typed nullable -- an explicit JSON null for any
// of these is malformed input and fromJson() rejects it rather than
// collapsing it into "absent".
struct CardDef {
  CardCode cardCode;
  CardName name;
  CardType cardType{};
  QString art;

  std::optional<CardName> revealedName;
  std::optional<CardCost> cost;
  std::optional<int> level;
  std::optional<CardSubType> cardSubType;
  QList<ClassSymbol> classSymbols;
  QList<SkillIcon> skills;
  QStringList cardTraits;
  QStringList revealedCardTraits;
  std::optional<Revelation> revelation;
  std::optional<int> victoryPoints;
  std::optional<int> vengeancePoints;
  std::optional<bool> overrideActionPlayableIfCriteriaMet;
  std::optional<bool> permanent;
  std::optional<QString> encounterSet;
  std::optional<int> encounterSetQuantity;
  std::optional<bool> unique;
  std::optional<bool> doubleSided;
  std::optional<bool> exceptional;
  std::optional<bool> playableFromDiscard;
  std::optional<int> stage;
  QList<SlotType> cardSlots;
  QList<CardCode> alternateCardCodes;
  std::optional<int> grantedXp;
  std::optional<bool> canReplace;
  QList<std::pair<int, CardCode>> bondedWith;
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
  // verbatim including absent-vs-null (see class comment).
  QJsonValue additionalCost{QJsonValue::Undefined};
  QJsonValue fastWindow{QJsonValue::Undefined};
  QJsonValue actions{QJsonValue::Undefined};
  QJsonValue criteria{QJsonValue::Undefined};
  QJsonValue uses{QJsonValue::Undefined};
  QJsonValue locationSymbol{QJsonValue::Undefined};
  QJsonValue locationRevealedSymbol{QJsonValue::Undefined};
  QJsonValue purchaseTrauma{QJsonValue::Undefined};
  QJsonValue customizations{QJsonValue::Undefined};

  // Outer-typed-but-inner-unconstrained fields: the schema constrains only
  // the outer JSON shape ("type":"array" or "type":"object", with no
  // further validation of elements/properties). fromJson() validates and
  // rejects a present value of the wrong outer shape (including an
  // explicit null, which matches neither "array" nor "object"), then
  // preserves the validated value's contents verbatim.
  QJsonValue keywords{QJsonValue::Undefined};
  QJsonValue commitRestrictions{QJsonValue::Undefined};
  QJsonValue attackOfOpportunityModifiers{QJsonValue::Undefined};
  QJsonValue limits{QJsonValue::Undefined};
  QJsonValue locationConnections{QJsonValue::Undefined};
  QJsonValue locationRevealedConnections{QJsonValue::Undefined};
  QJsonValue deckRestrictions{QJsonValue::Undefined};
  QJsonValue meta{QJsonValue::Undefined};

  [[nodiscard]] static ValueOrError<CardDef> fromJson(const QJsonValue &v,
                                                      QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const CardDef &, const CardDef &) = default;
};

} // namespace Arkham
