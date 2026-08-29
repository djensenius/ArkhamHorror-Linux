#pragma once

#include "JsonDecode.h"
#include "ValueOrError.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringView>
#include <QUuid>
#include <optional>
#include <utility>

namespace Arkham {

// A UUID-backed identifier, distinguished at compile time by Tag so that a
// GameId can never be passed where a PlayerId is expected even though both
// wrap the same underlying representation. Always non-null: parse()/
// fromJson() reject both malformed strings and the all-zero UUID, and the
// canonical stored form is QUuid::WithoutBraces (matching this project's
// existing ServerProfile/QSettingsProfileStore convention).
template <typename Tag> class TypedId {
public:
  [[nodiscard]] static ValueOrError<TypedId> parse(const QString &text) {
    const QUuid parsed(text);
    if (parsed.isNull())
      return failure(
          QStringLiteral("not a valid non-null uuid: \"%1\"").arg(text));
    return TypedId(parsed.toString(QUuid::WithoutBraces));
  }

  [[nodiscard]] static ValueOrError<TypedId> fromJson(const QJsonValue &v,
                                                      QStringView path) {
    auto uuid = Json::decodeUuid(v, path);
    if (!uuid)
      return failure(uuid.error());
    return TypedId(uuid->toString(QUuid::WithoutBraces));
  }
  // Canonical byte-level decode overload: identical logic, operating
  // directly on the lossless AST (see RawJson.h) via Json::decodeUuid's
  // own Json::Value overload. A UUID has no numeric-precision concern of
  // its own, but routing a fromRawJson()-driven aggregate decode (e.g.
  // GameListRow::fromRawJson) through this overload keeps the whole
  // decode on Json::Value end-to-end rather than dropping to QJsonValue
  // partway through for this one field.
  [[nodiscard]] static ValueOrError<TypedId> fromJson(const Json::Value &v,
                                                      QStringView path) {
    auto uuid = Json::decodeUuid(v, path);
    if (!uuid)
      return failure(uuid.error());
    return TypedId(uuid->toString(QUuid::WithoutBraces));
  }

  [[nodiscard]] const QString &value() const noexcept { return m_value; }
  [[nodiscard]] QJsonValue toJson() const { return m_value; }

  friend bool operator==(const TypedId &, const TypedId &) = default;

  // Explicitly declared (rather than left to the compiler's implicit
  // move constructor/assignment) specifically to suppress move: m_value
  // is a plain QString, and QString's move constructor/assignment leaves
  // the moved-from string empty, which would silently turn a moved-from
  // TypedId into one whose value() is an empty string -- even though
  // this class's whole reason for existing is to make "a validated
  // non-null uuid" a structural invariant every live TypedId instance
  // upholds (parse()/fromJson() are its only construction paths). A
  // user-declared copy constructor/assignment here means there is no
  // user-declared move constructor/assignment for the compiler to
  // implicitly generate, so std::move(id) instead binds to this copy
  // constructor (an rvalue can bind to `const TypedId&`), leaving the
  // moved-from source completely unchanged and still valid -- QString's
  // copy constructor is noexcept and O(1) (implicit sharing: a refcount
  // increment, never a deep copy), so this costs nothing relative to a
  // "real" move while making an invariant-violating moved-from TypedId
  // structurally impossible to observe, including through a container
  // (QList/QMap/std::optional) that relocates/moves its elements.
  TypedId(const TypedId &) = default;
  TypedId &operator=(const TypedId &) = default;

private:
  explicit TypedId(QString canonical) : m_value(std::move(canonical)) {}
  QString m_value;
};

struct GameIdTag {};
using GameId = TypedId<GameIdTag>;
struct PlayerIdTag {};
using PlayerId = TypedId<PlayerIdTag>;
struct DeckIdTag {};
using DeckId = TypedId<DeckIdTag>;

// A non-empty wire string, distinguished at compile time by Tag. Used for
// identifiers the backend deliberately keeps as open/unvalidated strings
// rather than validated CardCode -- e.g. the investigatorId accepted by
// chooseDeck/claimSeat, which contracts/README.md documents as accepting
// "prefixed or raw codes" -- so client code cannot accidentally normalize or
// reject a value the backend itself treats permissively.
template <typename Tag> class NonEmptyString {
public:
  [[nodiscard]] static ValueOrError<NonEmptyString> parse(QString text) {
    if (text.isEmpty())
      return failure(QStringLiteral("must not be empty"));
    return NonEmptyString(std::move(text));
  }

  [[nodiscard]] static ValueOrError<NonEmptyString>
  fromJson(const QJsonValue &v, QStringView path) {
    return fromJsonImpl(v, path);
  }
  // Canonical byte-level decode overload: see TypedId::fromJson's
  // Json::Value overload doc comment -- identical rationale, since this
  // type has no numeric-precision concern of its own either.
  [[nodiscard]] static ValueOrError<NonEmptyString>
  fromJson(const Json::Value &v, QStringView path) {
    return fromJsonImpl(v, path);
  }

  [[nodiscard]] const QString &value() const noexcept { return m_value; }
  [[nodiscard]] QJsonValue toJson() const { return m_value; }

  friend bool operator==(const NonEmptyString &,
                         const NonEmptyString &) = default;

  // See TypedId's identically-reasoned copy constructor/assignment above:
  // m_value is a plain QString, whose implicit move would leave a
  // moved-from NonEmptyString holding an empty (invariant-violating)
  // string despite parse()/fromJson() being its only construction paths.
  // Declaring these explicitly suppresses the compiler's implicit move
  // constructor/assignment, so std::move() falls back to this (noexcept,
  // O(1) due to QString's implicit sharing) copy instead.
  NonEmptyString(const NonEmptyString &) = default;
  NonEmptyString &operator=(const NonEmptyString &) = default;

private:
  explicit NonEmptyString(QString v) : m_value(std::move(v)) {}

  template <typename V>
  [[nodiscard]] static ValueOrError<NonEmptyString>
  fromJsonImpl(const V &v, QStringView path) {
    if (!v.isString())
      return failure(QStringLiteral("%1: expected string, got %2")
                         .arg(path, Json::typeName(v)));
    auto result = parse(v.toString());
    if (!result)
      return failure(QStringLiteral("%1: %2").arg(path, result.error()));
    return *result;
  }

  QString m_value;
};

struct InvestigatorRefTag {};
// Permissive investigator identifier used at the chooseDeck/claimSeat/
// deck-list boundary. May or may not carry the CardCode "c" prefix; the
// backend accepts both forms there (unlike the strict CardCode below, which
// backs the catalog and the already-normalized openSeats/deck-list codes).
using InvestigatorRef = NonEmptyString<InvestigatorRefTag>;
struct CampaignIdTag {};
using CampaignId = NonEmptyString<CampaignIdTag>;
struct ScenarioIdTag {};
using ScenarioId = NonEmptyString<ScenarioIdTag>;

// A validated card code: a literal 'c' followed by at least one more
// character, matching contracts/schemas/catalog.schema.json's `^c.+$`
// pattern. Covers both official codes ("c01020") and homebrew codes
// ("c:homebrew:151"). Unlike InvestigatorRef, this type never accepts the
// unprefixed legacy ArkhamDB form -- it backs fields the backend has already
// normalized (CardDef.cardCode, openSeats, normalized deck-list slot keys).
class CardCode {
public:
  [[nodiscard]] static ValueOrError<CardCode> parse(const QString &text);
  [[nodiscard]] static ValueOrError<CardCode> fromJson(const QJsonValue &v,
                                                       QStringView path);
  // Canonical byte-level decode overload: see TypedId::fromJson's
  // Json::Value overload doc comment -- CardCode is a plain validated
  // string with no numeric-precision concern of its own.
  [[nodiscard]] static ValueOrError<CardCode> fromJson(const Json::Value &v,
                                                       QStringView path);

  [[nodiscard]] const QString &value() const noexcept { return m_value; }
  [[nodiscard]] QJsonValue toJson() const { return m_value; }

  [[nodiscard]] friend bool operator==(const CardCode &a,
                                       const CardCode &b) noexcept {
    return a.m_value == b.m_value;
  }
  // Strict, exact string ordering only -- sufficient for deterministic
  // QMap<CardCode, ...> iteration; not the backend's side-aware (a/b/c/d)
  // gameplay equality from Arkham.Card.CardCode, which is out of scope here.
  [[nodiscard]] friend bool operator<(const CardCode &a,
                                      const CardCode &b) noexcept {
    return a.m_value < b.m_value;
  }

  // See TypedId's identically-reasoned copy constructor/assignment above:
  // m_value is a plain QString backing this class's "starts with 'c',
  // non-empty payload" invariant (parse()/fromJson() are its only
  // construction paths), which an implicit move would silently violate
  // for the moved-from source. Declaring these explicitly suppresses the
  // compiler's implicit move constructor/assignment, so std::move()
  // falls back to this (noexcept, O(1) due to QString's implicit
  // sharing) copy instead.
  CardCode(const CardCode &) = default;
  CardCode &operator=(const CardCode &) = default;

private:
  explicit CardCode(QString v) : m_value(std::move(v)) {}
  QString m_value;
};

// {title, subtitle} pair shared verbatim by catalog.schema.json's CardDef
// name/revealedName and game-list.schema.json's scenario name.
struct CardName {
  QString title;
  std::optional<QString> subtitle;

  [[nodiscard]] static ValueOrError<CardName> fromJson(const QJsonValue &v,
                                                       QStringView path);
  // Canonical byte-level decode overload: see TypedId::fromJson's
  // Json::Value overload doc comment -- title/subtitle are plain strings
  // with no numeric-precision concern of their own.
  [[nodiscard]] static ValueOrError<CardName> fromJson(const Json::Value &v,
                                                       QStringView path);
  [[nodiscard]] QJsonObject toJson() const;
  // title/subtitle have no numeric-precision concern of their own, but
  // exposing this alongside toJson() lets an enclosing aggregate (e.g.
  // CardDef::toRawJson()) compose its own encode entirely in Json::Value
  // without ever dropping to QJsonObject for this field specifically.
  [[nodiscard]] Json::Value toRawJson() const;

  friend bool operator==(const CardName &, const CardName &) = default;
};

} // namespace Arkham
