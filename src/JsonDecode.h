#pragma once

#include "RawJson.h"
#include "ValueOrError.h"

#include <QByteArray>
#include <QByteArrayView>
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
// Same, for the canonical lossless AST (see RawJson.h). Every decode
// helper below is overloaded for both QJsonValue/QJsonObject (the
// existing, QJsonDocument-fed "convenience" path -- lossless only up to
// whatever precision QJsonValue's own double-backed storage already
// carries) and Json::Value (the byte-parsed, arbitrary-precision
// canonical path): a caller decoding directly from Json::Value::parse()'s
// result never touches a QJsonValue/double at all, so a number outside
// int64 range, a long fraction, or a huge exponent nested at any depth
// survives exactly rather than only "as closely as IEEE-754 double
// allows".
[[nodiscard]] QString typeName(const Json::Value &v);

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
[[nodiscard]] FieldPresence fieldPresence(const Json::Value &obj,
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
// Json::Value overloads (see typeName(const Json::Value&) above): `v` is
// returned by value (Json::Value is a small value type, same convention
// RawJson.h itself uses throughout), letting a canonical fromRawJson()
// decoder validate a subtree's outer kind without ever touching
// QJsonValue/QJsonObject. requireArray's success value is the *decomposed*
// element list (QList<Value>, via Value::toArray()) rather than the
// wrapping Value itself, deliberately mirroring QJsonArray's shape so
// templated decode logic shared between both value families (see
// CardCatalog.cpp/Games.cpp) can index/iterate it identically either way.
[[nodiscard]] ValueOrError<Json::Value> requireObject(const Json::Value &v,
                                                      QStringView path);
[[nodiscard]] ValueOrError<QList<Json::Value>>
requireArray(const Json::Value &v, QStringView path);
[[nodiscard]] ValueOrError<QString> requireStringValue(const Json::Value &v,
                                                       QStringView path);
// Decodes a JSON number as an exact 64-bit integer: rejects any value that
// is not a JSON number at all, or does not fit qint64's range. The
// "integral" check below runs on the double Qt has already narrowed the
// underlying JSON number into (QJsonValue::toDouble()) -- not the original
// JSON source text -- so it rejects a value whose *double representation*
// has a nonzero fractional part (e.g. "1.5") but is necessarily a
// best-effort judgment for values outside double's +-2^53 integer-exact
// range: an extreme fractional literal that Qt has already rounded to a
// whole-number double before this function ever sees it would pass this
// check even though the original JSON text was not itself an integer.
// Reads the value via QJsonValue::toInteger() rather than
// toDouble()+static_cast, so a value that reached this QJsonValue via
// RawJson.h's Value::toQJson() int64-exact path (see that function's doc
// comment) decodes to the identical qint64 -- including magnitudes no
// double can represent exactly, e.g. 9223372036854775807. A QJsonValue
// built directly from a double (as every contract-domain integer this
// codebase's non-byte fromJson() entry points still decode, when not
// reached via a fromRawBytes()-style raw-parse-first path) is exact only
// within IEEE-754 double's integer-exact range (+-2^53); this is the most
// fidelity obtainable from that representation, not a shortcut this
// function takes for its own convenience.
[[nodiscard]] ValueOrError<qint64> requireIntValue(const QJsonValue &v,
                                                   QStringView path);
[[nodiscard]] ValueOrError<bool> requireBoolValue(const QJsonValue &v,
                                                  QStringView path);
// Json::Value overload: reads RawNumber::toExactInt64() directly (see
// RawJson.h) -- exact across qint64's *entire* range with no double
// involved at any point, so there is no boundary ambiguity to document
// here at all (unlike the QJsonValue overload above).
[[nodiscard]] ValueOrError<qint64> requireIntValue(const Json::Value &v,
                                                   QStringView path);
[[nodiscard]] ValueOrError<bool> requireBoolValue(const Json::Value &v,
                                                  QStringView path);

// Required-field decoders: fail with a path-qualified, actionable error if
// the key is absent or the value has the wrong JSON type.
[[nodiscard]] ValueOrError<QString>
requireString(const QJsonObject &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<qint64>
requireInt(const QJsonObject &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<bool>
requireBool(const QJsonObject &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<QJsonObject>
requireObjectField(const QJsonObject &obj, QLatin1StringView key,
                   QStringView path);
[[nodiscard]] ValueOrError<QJsonArray> requireArrayField(const QJsonObject &obj,
                                                         QLatin1StringView key,
                                                         QStringView path);
// Json::Value overloads of the six required-field decoders above, for a
// canonical fromRawJson() decoder operating on an already-parsed
// Json::Value object (see RawJson.h) instead of QJsonObject.
[[nodiscard]] ValueOrError<QString>
requireString(const Json::Value &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<qint64>
requireInt(const Json::Value &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<bool>
requireBool(const Json::Value &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<Json::Value>
requireObjectField(const Json::Value &obj, QLatin1StringView key,
                   QStringView path);
[[nodiscard]] ValueOrError<QList<Json::Value>>
requireArrayField(const Json::Value &obj, QLatin1StringView key,
                  QStringView path);

// Generic required-field wrapper for callers decoding a field through a
// strongly-typed factory (e.g. SomeId::fromJson, CardCode::fromJson) not
// covered by one of the concrete requireString/requireInt/... wrappers
// above: reports a missing key with the exact same "missing required
// field" phrasing those wrappers use, rather than letting the raw
// QJsonValue::Undefined/absent-key value fall through to the factory's
// own type-check and surface as a less specific "expected <type>, got
// missing". A present value (of any type, including the wrong one) is
// still forwarded to `valueDecoder` unchanged, so type errors from
// `valueDecoder` itself are untouched. Every requireString/requireInt/...
// wrapper above is itself implemented in terms of this one helper (see
// JsonDecode.cpp); it is exposed here as a template so it can also be
// called directly wherever a call site's value decoder is some other
// type's fromJson()-shaped factory instead of one of requireStringValue/
// requireIntValue/requireBoolValue/requireObject/requireArray.
template <typename Obj, typename ValueDecoder>
[[nodiscard]] auto requireField(const Obj &obj, QLatin1StringView key,
                                QStringView path, ValueDecoder valueDecoder)
    -> decltype(valueDecoder(obj.value(key), path)) {
  // A single obj.value(key) lookup -- rather than fieldPresence()'s own
  // lookup followed by a second, separate obj.value(key) call here --
  // since both QJsonObject::value() and Json::Value::value() already
  // return an explicit Undefined value for an absent key (matching what
  // fieldPresence() itself would report), and this is the workhorse most
  // require*() field decoders funnel through for nearly every CardDef
  // field during a catalog fixture decode.
  auto value = obj.value(key);
  if (value.isUndefined())
    return failure(
        QStringLiteral("%1: missing required field \"%2\"").arg(path, key));
  return valueDecoder(std::move(value), path);
}

// Required-but-unconstrained-value decoder: fails only if the key itself is
// absent. Unlike every other require* helper, a present value -- of any
// type, including explicit null -- decodes verbatim with no type check.
// Matches schema fields typed fully open (`{}`) that are nonetheless
// required, e.g. CardCost's MaxDynamicCost/AnyMatchingCardCost/
// MatchingEnemyFieldCost `contents`, so a fixture that omits the field
// still fails to decode instead of silently round-tripping without it.
[[nodiscard]] ValueOrError<QJsonValue> requireRawField(const QJsonObject &obj,
                                                       QLatin1StringView key,
                                                       QStringView path);
// Json::Value overload: the canonical form for a schema-unconstrained
// field, since the returned Json::Value preserves arbitrary nested
// numeric precision/duplicate-key rejection that requireRawField()'s
// QJsonValue result cannot.
[[nodiscard]] ValueOrError<Json::Value> requireRawField(const Json::Value &obj,
                                                        QLatin1StringView key,
                                                        QStringView path);

// Optional-field decoders: an absent key or an explicit JSON null both
// decode to std::nullopt. A present value of the wrong type still fails.
// Matches a backend field whose own parser folds absence and null to the
// same result (verified per-field against the backend's Aeson `.:?`/plain
// generic-Maybe derivation -- see e.g. Games.h's ChooseDeckRequest and
// Decks.h's saved-Deck fields) -- i.e. the *response semantics* genuinely
// do not distinguish the two, not merely "this client doesn't care".
[[nodiscard]] ValueOrError<std::optional<QString>>
optionalString(const QJsonObject &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<std::optional<qint64>>
optionalInt(const QJsonObject &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<std::optional<bool>>
optionalBool(const QJsonObject &obj, QLatin1StringView key, QStringView path);
// Json::Value overloads of the three optional-field decoders above.
[[nodiscard]] ValueOrError<std::optional<QString>>
optionalString(const Json::Value &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<std::optional<qint64>>
optionalInt(const Json::Value &obj, QLatin1StringView key, QStringView path);
[[nodiscard]] ValueOrError<std::optional<bool>>
optionalBool(const Json::Value &obj, QLatin1StringView key, QStringView path);

// Optional-but-non-nullable decoders: an absent key decodes to
// std::nullopt, matching a schema field typed only "string"/"integer"/
// "boolean" (no "null" in its type union) that is simply not required --
// e.g. catalog.schema.json's cardDef `level`/`unique`/`errata`. Unlike
// optionalString/Int/Bool above, an explicit JSON null is *not* folded
// into std::nullopt here: the schema never allows null for these fields,
// so a present null is exactly as malformed as a present value of any
// other wrong type, and must fail rather than silently disappearing into
// "absent".
[[nodiscard]] ValueOrError<std::optional<QString>>
optionalNonNullString(const QJsonObject &obj, QLatin1StringView key,
                      QStringView path);
[[nodiscard]] ValueOrError<std::optional<qint64>>
optionalNonNullInt(const QJsonObject &obj, QLatin1StringView key,
                   QStringView path);
[[nodiscard]] ValueOrError<std::optional<bool>>
optionalNonNullBool(const QJsonObject &obj, QLatin1StringView key,
                    QStringView path);
// Json::Value overloads of the three optional-but-non-nullable decoders
// above.
[[nodiscard]] ValueOrError<std::optional<QString>>
optionalNonNullString(const Json::Value &obj, QLatin1StringView key,
                      QStringView path);
[[nodiscard]] ValueOrError<std::optional<qint64>>
optionalNonNullInt(const Json::Value &obj, QLatin1StringView key,
                   QStringView path);
[[nodiscard]] ValueOrError<std::optional<bool>>
optionalNonNullBool(const Json::Value &obj, QLatin1StringView key,
                    QStringView path);

// Required-but-nullable decoders: the key itself must be present (schemas
// such as decks.schema.json's `deckList` and catalog.schema.json's `name`
// declare these fields required while typing their value ["string"/"integer",
// "null"]) but its value may be JSON null. Absent keys fail; present null
// decodes to std::nullopt; a present value of the wrong type still fails.
[[nodiscard]] ValueOrError<std::optional<QString>>
requireNullableString(const QJsonObject &obj, QLatin1StringView key,
                      QStringView path);
[[nodiscard]] ValueOrError<std::optional<qint64>>
requireNullableInt(const QJsonObject &obj, QLatin1StringView key,
                   QStringView path);
// Json::Value overloads of the two required-but-nullable decoders above.
[[nodiscard]] ValueOrError<std::optional<QString>>
requireNullableString(const Json::Value &obj, QLatin1StringView key,
                      QStringView path);
[[nodiscard]] ValueOrError<std::optional<qint64>>
requireNullableInt(const Json::Value &obj, QLatin1StringView key,
                   QStringView path);

// Optional-and-outer-typed raw decoders: match a schema field that is not
// required and, when present, constrains only its *outer* JSON type
// (array/object) while leaving everything nested inside fully
// unconstrained (e.g. catalog.schema.json's cardDef `keywords`/`limits`
// (array) and `meta` (object)). An absent key decodes to
// QJsonValue(QJsonValue::Undefined) (preserved distinctly from an
// explicit null, exactly like requireRawField, so an untouched fixture
// entry round-trips byte-faithfully); a present value must have the
// declared outer type -- including rejecting an explicit null, which
// matches neither "array" nor "object" -- but its contents, once that
// outer shape is confirmed, are preserved verbatim with no further
// validation.
[[nodiscard]] ValueOrError<QJsonValue>
optionalRawArrayField(const QJsonObject &obj, QLatin1StringView key,
                      QStringView path);
[[nodiscard]] ValueOrError<QJsonValue>
optionalRawObjectField(const QJsonObject &obj, QLatin1StringView key,
                       QStringView path);
// Json::Value overloads of the two optional-and-outer-typed raw decoders
// above: the canonical form for these fields (see requireRawField's
// Json::Value overload doc comment) -- an absent key decodes to
// Json::Value's default Kind::Undefined (Json::Value::isUndefined()),
// preserving arbitrary nested numeric precision the QJsonValue overloads
// above cannot.
[[nodiscard]] ValueOrError<Json::Value>
optionalRawArrayField(const Json::Value &obj, QLatin1StringView key,
                      QStringView path);
[[nodiscard]] ValueOrError<Json::Value>
optionalRawObjectField(const Json::Value &obj, QLatin1StringView key,
                       QStringView path);

// Strict non-null UUID decode: fails on null, on a malformed string, and on
// the all-zero UUID (never a valid backend-assigned identity).
[[nodiscard]] ValueOrError<QUuid> decodeUuid(const QJsonValue &v,
                                             QStringView path);
[[nodiscard]] ValueOrError<QUuid> decodeUuid(const Json::Value &v,
                                             QStringView path);

// Decodes a UUID array slot that may be JSON null (e.g. an unassigned deck
// slot in createGameRequest.deckIds): null -> std::nullopt, a valid UUID
// string -> the parsed value, anything else -> failure.
[[nodiscard]] ValueOrError<std::optional<QUuid>>
decodeNullableUuid(const QJsonValue &v, QStringView path);
[[nodiscard]] ValueOrError<std::optional<QUuid>>
decodeNullableUuid(const Json::Value &v, QStringView path);

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
// the number as a double. Fails rather than crashing for a non-finite
// double -- a syntactically valid but astronomically large-exponent JSON
// number literal (e.g. "1e400") parses to +/-Infinity once Qt's JSON
// parser has already narrowed it to a double, and neither Infinity nor NaN
// has a finite decimal Scientific-style expansion to produce.
[[nodiscard]] ValueOrError<QString> scientificShow(double value,
                                                   QStringView path);

// Uniform (key, value) member iteration for a validated object, shared by
// templated decode logic that walks a map-shaped field (alternateSkills,
// alternateErrata, cardQuantityMap/Input) identically for both a
// QJsonObject (returned by requireObject(QJsonValue)) and a Json::Value
// object (returned by requireObject(Json::Value)/requireObjectField):
// QJsonObject has no directly equivalent member-list accessor, so this
// materializes one; Json::Value already stores its members exactly this
// way (see RawJson.h's Value::members()) and this overload is a plain,
// non-owning passthrough.
[[nodiscard]] QList<std::pair<QString, QJsonValue>>
objectMembers(const QJsonObject &obj);
[[nodiscard]] const QList<std::pair<QString, Json::Value>> &
objectMembers(const Json::Value &obj);

// Converts an already-extracted field value to the lossless AST type every
// scoped contract domain type (CardDef/DeckListInput/GameState/
// CampaignOption/etc.) stores its schema-unconstrained/unknown-tag fields
// as: a no-op passthrough when the surrounding decode is already running
// on Json::Value (the canonical fromRawJson()/fromRawBytes() family), or
// an explicit, individually-failable Json::Value::fromQJson() conversion
// when running on QJsonValue (the fromJson() convenience family) -- never
// a silent best-effort fallback. This is the ONLY place in this codebase a
// QJsonValue-to-Json::Value conversion should happen, and callers should
// invoke it per-field rather than for a whole subtree at once, so a single
// malformed/unrepresentable numeric field fails with a precise path
// rather than corrupting or discarding its siblings.
[[nodiscard]] ValueOrError<Json::Value> toLosslessRaw(const QJsonValue &v);
[[nodiscard]] ValueOrError<Json::Value> toLosslessRaw(const Json::Value &v);

// Fails if `obj` carries any key other than those listed in `allowed`.
// For a schema branch whose additionalProperties is false -- specifically
// a known tagged union's exact "tag"/"contents" shape (SkillIcon/CardCost/
// GameValue/GameState's known branches), closed/fixed-shape summary
// objects used to positively disambiguate a response's shape (GameListRow's
// success row and its nested scenario/campaign/investigator summaries),
// and every other pinned-contract shape whose schema likewise declares
// `additionalProperties: false` (CardDef/CardName -- as of
// round-10-cumulative-review item 4, superseding this client's earlier
// policy of tolerating an unknown key on those two specifically; a future
// backend field not yet modeled here is now a hard decode error, matching
// the pinned schema literally rather than pre-emptively working around a
// hypothetical future revision) -- this rejects an unexpected extra key
// rather than silently accepting it and then re-emitting only the keys
// this client models, dropping the rest. Generic over Obj (QJsonObject or
// Json::Value, see RawJson.h) via objectMembers() above.
template <typename Obj>
[[nodiscard]] ValueOrError<bool>
requireExactKeys(const Obj &obj,
                 std::initializer_list<QLatin1StringView> allowed,
                 QStringView path) {
  for (const auto &member : objectMembers(obj)) {
    const QString &key = member.first;
    bool isKnown = false;
    for (const auto &candidate : allowed) {
      if (key == candidate) {
        isKnown = true;
        break;
      }
    }
    if (!isKnown)
      return failure(
          QStringLiteral("%1: unexpected field \"%2\"").arg(path, key));
  }
  return true;
}

// Decodes a closed enum from a JSON string against an explicit
// wire-name/value table. Unlike this codebase's open wire-string wrappers
// (ContractRevision-adjacent "forward compatible" types), an unrecognized
// string here is a hard decode error -- this matches the `enum` keyword the
// contract schemas use for e.g. cardType/classSymbol/slotType, which the
// issue scopes as closed (only the game-list/lifecycle tag types need
// explicit unknown-value forward compatibility).
// Generic over the input value's own type (QJsonValue or Json::Value, see
// RawJson.h): both support isString()/toString() identically, and there is
// no numeric/precision concern for a bare string comparison, so a single
// template body serves both a QJsonValue-based fromJson() and a
// Json::Value-based fromRawJson() decoder with no duplicated logic.
template <typename Enum, typename V, std::size_t N>
[[nodiscard]] ValueOrError<Enum> decodeClosedEnum(
    const V &v, QStringView path,
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
// `decodeClosedEnum` was given. `value` is ordinarily always present in
// `table` since a value produced via `decodeClosedEnum` (or one of this
// codebase's own validating factories, e.g. CampaignOption::knownOption())
// can never be anything else -- but unlike decode, an encode-time caller
// can also reach this function with a value obtained no other way than
// `static_cast<Enum>(someOutOfRangeInt)` against a public enum-typed
// request field/factory parameter this class cannot itself validate at
// construction (round 6 review: `Difficulty(999)`,
// `KnownCampaignOption(999)`, etc., previously reached Q_UNREACHABLE here
// -- UB in an optimized build, an assert-abort in a debug one, either way
// a crash on public-API misuse). Returning a typed failure instead makes
// every caller -- transitively, every toJson()/toRawJson()/toJsonBytes()
// this function is reachable from -- fail cleanly rather than crash.
template <typename Enum, std::size_t N>
[[nodiscard]] ValueOrError<QString> encodeClosedEnum(
    Enum value,
    const std::array<std::pair<QLatin1StringView, Enum>, N> &table) {
  for (const auto &[wire, candidate] : table) {
    if (candidate == value)
      return QString(wire);
  }
  return failure(QStringLiteral(
      "encodeClosedEnum: value has no corresponding wire representation "
      "in this closed enum's table"));
}

} // namespace Arkham::Json
