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
// icon (no contents).
enum class SkillIconTag { SkillIcon, WildIcon, WildMinusIcon };
struct SkillIcon {
  SkillIconTag tag{SkillIconTag::WildIcon};
  // Populated only when tag == SkillIcon.
  std::optional<SkillType> skill;

  [[nodiscard]] static ValueOrError<SkillIcon> fromJson(const QJsonValue &v,
                                                        QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const SkillIcon &, const SkillIcon &) = default;
};

// `cardCost`: a static integer, one of three no-payload dynamic variants, or
// one of three variants whose contents the schema leaves fully unconstrained
// (`{}`), preserved verbatim as raw JSON.
enum class CardCostTag {
  StaticCost,
  DynamicCost,
  DiscardAmountCost,
  DeferredCost,
  MaxDynamicCost,
  AnyMatchingCardCost,
  MatchingEnemyFieldCost,
};
struct CardCost {
  CardCostTag tag{CardCostTag::DynamicCost};
  // Populated only when tag == StaticCost.
  std::optional<int> staticAmount;
  // Populated only when tag is MaxDynamicCost / AnyMatchingCardCost /
  // MatchingEnemyFieldCost; schema-unconstrained, preserved verbatim.
  QJsonValue rawContents{QJsonValue::Undefined};

  [[nodiscard]] static ValueOrError<CardCost> fromJson(const QJsonValue &v,
                                                       QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const CardCost &, const CardCost &) = default;
};

// `gameValue`: a single static amount, a (static, perPlayer) pair, four
// by-player-count amounts, or one of three no-payload variants.
enum class GameValueTag {
  Static,
  PerPlayer,
  StaticWithPerPlayer,
  ByPlayerCount,
  ValueX,
  ValueStar,
  ValueUnknown,
};
struct GameValue {
  GameValueTag tag{GameValueTag::ValueX};
  // Populated only for Static/PerPlayer (schema types their `contents` as a
  // bare integer, not an array).
  std::optional<int> singleAmount;
  // Size 2 for StaticWithPerPlayer, 4 for ByPlayerCount, empty otherwise.
  QList<int> contents;

  [[nodiscard]] static ValueOrError<GameValue> fromJson(const QJsonValue &v,
                                                        QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const GameValue &, const GameValue &) = default;
};

// A single card definition, covering every top-level property documented by
// catalog.schema.json's `cardDef`. Fields the schema leaves fully
// unconstrained (`{}`, or an array/object whose element/property shape is
// unconstrained) are preserved verbatim as raw QJsonValue -- absent fields
// decode to QJsonValue(QJsonValue::Undefined) and are omitted again by
// toJson(), so a round trip of an untouched fixture entry is byte-faithful
// modulo key order. Required fields are cardCode, name, cardType, and art;
// every other field is optional, decoding to std::nullopt / an empty
// container when absent.
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

  // Schema-unconstrained fields, preserved verbatim (see class comment).
  QJsonValue additionalCost{QJsonValue::Undefined};
  QJsonValue keywords{QJsonValue::Undefined};
  QJsonValue fastWindow{QJsonValue::Undefined};
  QJsonValue actions{QJsonValue::Undefined};
  QJsonValue criteria{QJsonValue::Undefined};
  QJsonValue commitRestrictions{QJsonValue::Undefined};
  QJsonValue attackOfOpportunityModifiers{QJsonValue::Undefined};
  QJsonValue limits{QJsonValue::Undefined};
  QJsonValue uses{QJsonValue::Undefined};
  QJsonValue locationSymbol{QJsonValue::Undefined};
  QJsonValue locationRevealedSymbol{QJsonValue::Undefined};
  QJsonValue locationConnections{QJsonValue::Undefined};
  QJsonValue locationRevealedConnections{QJsonValue::Undefined};
  QJsonValue purchaseTrauma{QJsonValue::Undefined};
  QJsonValue deckRestrictions{QJsonValue::Undefined};
  QJsonValue customizations{QJsonValue::Undefined};
  QJsonValue meta{QJsonValue::Undefined};

  [[nodiscard]] static ValueOrError<CardDef> fromJson(const QJsonValue &v,
                                                      QStringView path);
  [[nodiscard]] QJsonObject toJson() const;

  friend bool operator==(const CardDef &, const CardDef &) = default;
};

} // namespace Arkham
