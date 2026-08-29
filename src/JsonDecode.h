#pragma once

#include "ValueOrError.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QString>
#include <QStringView>
#include <QUuid>
#include <array>
#include <limits>
#include <optional>

namespace Arkham::Json {

// Human-readable name of a QJsonValue's runtime type, for actionable error
// messages such as "cardCode: expected string, got number". Never includes
// the value itself, so decode errors are safe to log verbatim.
[[nodiscard]] QString typeName(const QJsonValue &v);

// Builds a qualified path for a nested field, e.g. joinPath("cards[3]",
// "cost") -> "cards[3].cost". `parent` may be empty for a top-level field.
[[nodiscard]] QString joinPath(QStringView parent, QStringView field);

// Builds a qualified path for an array element, e.g. indexPath("cards", 3)
// -> "cards[3]".
[[nodiscard]] QString indexPath(QStringView parent, qsizetype index);

// Whether an object key is absent, present with JSON null, or present with a
// non-null value. Needed only where the two absent/null cases decode
// differently (see DeckListInput's id field); everywhere else, both are
// folded into std::nullopt by the optional* helpers below.
enum class FieldPresence { Absent, Null, Present };
[[nodiscard]] FieldPresence fieldPresence(const QJsonObject &obj,
                                          QLatin1StringView key);

// Bare-value required-type decoders: fail with a path-qualified, actionable
// error if the value has the wrong JSON type. Useful directly for array
// elements (which have no object key of their own); the obj+key overloads
// below are thin wrappers over these.
[[nodiscard]] ValueOrError<QJsonObject> requireObject(const QJsonValue &v,
                                                      QStringView path);
[[nodiscard]] ValueOrError<QJsonArray> requireArray(const QJsonValue &v,
                                                    QStringView path);
[[nodiscard]] ValueOrError<QString> requireStringValue(const QJsonValue &v,
                                                       QStringView path);
[[nodiscard]] ValueOrError<int> requireIntValue(const QJsonValue &v,
                                                QStringView path);
[[nodiscard]] ValueOrError<bool> requireBoolValue(const QJsonValue &v,
                                                  QStringView path);

// Required-field decoders: fail with a path-qualified, actionable error if
// the key is absent or the value has the wrong JSON type.
[[nodiscard]] ValueOrError<QString>
requireString(const QJsonObject &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<int>
requireInt(const QJsonObject &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<bool>
requireBool(const QJsonObject &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<QJsonObject>
requireObjectField(const QJsonObject &obj, QLatin1StringView key,
                   QStringView path);
[[nodiscard]] ValueOrError<QJsonArray> requireArrayField(const QJsonObject &obj,
                                                         QLatin1StringView key,
                                                         QStringView path);

// Optional-field decoders: an absent key or an explicit JSON null both
// decode to std::nullopt. A present value of the wrong type still fails.
[[nodiscard]] ValueOrError<std::optional<QString>>
optionalString(const QJsonObject &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<std::optional<int>>
optionalInt(const QJsonObject &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<std::optional<bool>>
optionalBool(const QJsonObject &obj, QLatin1StringView key, QStringView path);

// Required-but-nullable decoders: the key itself must be present (schemas
// such as decks.schema.json's `deckList` and catalog.schema.json's `name`
// declare these fields required while typing their value ["string"/"integer",
// "null"]) but its value may be JSON null. Absent keys fail; present null
// decodes to std::nullopt; a present value of the wrong type still fails.
[[nodiscard]] ValueOrError<std::optional<QString>>
requireNullableString(const QJsonObject &obj, QLatin1StringView key,
                      QStringView path);
[[nodiscard]] ValueOrError<std::optional<int>>
requireNullableInt(const QJsonObject &obj, QLatin1StringView key,
                   QStringView path);

// Strict non-null UUID decode: fails on null, on a malformed string, and on
// the all-zero UUID (never a valid backend-assigned identity).
[[nodiscard]] ValueOrError<QUuid> decodeUuid(const QJsonValue &v,
                                             QStringView path);

// Decodes a UUID array slot that may be JSON null (e.g. an unassigned deck
// slot in createGameRequest.deckIds): null -> std::nullopt, a valid UUID
// string -> the parsed value, anything else -> failure.
[[nodiscard]] ValueOrError<std::optional<QUuid>>
decodeNullableUuid(const QJsonValue &v, QStringView path);

// Formats a JSON number exactly as the backend's Aeson `Scientific` Show
// instance would (see Data.Scientific): fixed-point with a mandatory ".0"
// when 0.1 <= |x| < 1e7, scientific notation ("d.ddde<exp>") otherwise. Used
// only for the one field where the backend re-serializes a decoded JSON
// number as text (ArkhamDBDecklist's `id`, via `tshow`). Operates on the
// IEEE-754 double QJsonValue already parsed, using the shortest round-trip
// decimal digits of that double (via std::to_chars); this matches the
// backend's arbitrary-precision Aeson decoder exactly for every value that
// still round-trips through a double (all realistic ArkhamDB deck IDs), and
// is the most fidelity obtainable once Qt's JSON parser has already stored
// the number as a double.
[[nodiscard]] QString scientificShow(double value);

// Decodes a closed enum from a JSON string against an explicit
// wire-name/value table. Unlike this codebase's open wire-string wrappers
// (ContractRevision-adjacent "forward compatible" types), an unrecognized
// string here is a hard decode error -- this matches the `enum` keyword the
// contract schemas use for e.g. cardType/classSymbol/slotType, which the
// issue scopes as closed (only the game-list/lifecycle tag types need
// explicit unknown-value forward compatibility).
template <typename Enum, std::size_t N>
[[nodiscard]] ValueOrError<Enum> decodeClosedEnum(
    const QJsonValue &v, QStringView path,
    const std::array<std::pair<QLatin1StringView, Enum>, N> &table) {
  if (!v.isString())
    return failure(
        QStringLiteral("%1: expected string, got %2").arg(path, typeName(v)));
  const QString s = v.toString();
  for (const auto &[wire, value] : table) {
    if (s == wire)
      return value;
  }
  return failure(QStringLiteral("%1: unrecognized value \"%2\"").arg(path, s));
}

// Encodes a closed enum back to its wire string using the same table
// `decodeClosedEnum` was given. `value` is always expected to be present in
// `table` since closed enums can only ever be constructed via
// `decodeClosedEnum` or a table-covered literal.
template <typename Enum, std::size_t N>
[[nodiscard]] QString encodeClosedEnum(
    Enum value,
    const std::array<std::pair<QLatin1StringView, Enum>, N> &table) {
  for (const auto &[wire, candidate] : table) {
    if (candidate == value)
      return QString(wire);
  }
  Q_UNREACHABLE_RETURN(QString());
}

} // namespace Arkham::Json
