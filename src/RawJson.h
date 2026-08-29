#pragma once

#include "ValueOrError.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <optional>
#include <utility>

// A strict, self-contained RFC 8259 JSON parser: the canonical raw-byte
// decode boundary this client's JsonDecode-based fromJson() machinery sits
// on top of. QJsonDocument::fromJson() is deliberately NOT used as that
// boundary for two reasons this module fixes:
//
//  1. Qt's parser silently collapses duplicate object keys (last one wins)
//     and never validates that a string's raw UTF-8 bytes / \u escapes /
//     surrogate pairs are well-formed -- both of which let a malformed or
//     adversarial payload masquerade as a normal one.
//  2. QJsonValue stores every JSON number as a C++ double, so a number
//     literal like "9007199254740993" (2^53 + 1, ArkhamDB is known to
//     hand out deck ids past double's exact-integer range) or "1e128" is
//     already corrupted before any of this codebase's decoders see it --
//     no amount of care in requireIntValue()/optionalInt() etc. can
//     recover precision QJsonDocument has already destroyed.
//
// Value::parse() below preserves every number's exact literal (as its
// sign/integer/fraction/exponent parts, never rounded through a double),
// rejects duplicate keys (comparing their *decoded* text, so "id" and
// "\u0069d" are recognized as the same key), and validates string content
// (UTF-8, escapes, surrogate pairs) and number syntax against the RFC 8259
// grammar exactly. Value::toQJson() converts the validated tree to standard
// Qt JSON types for this codebase's existing QJsonValue-based decoders --
// which is lossless for every field except the one the issue calls out
// explicitly (ExternalDeckId's numeric "id"; see Decks.h), since QJsonValue
// itself cannot hold more precision than a double no matter how carefully
// the bytes were parsed.

namespace Arkham::Json {

// Resource bounds enforced by Value::parse() (and, symmetrically, by
// Value::toJsonBytes() against a programmatically-built AST): a network
// response is not yet in scope for this client, but the parser is the
// canonical boundary every governed fixture and future wire caller must go
// through, so it is bounded unconditionally rather than trusting an outer
// layer that does not exist yet. production() below is deliberately
// generous relative to every fixture and real game-list/catalog response
// this client currently decodes (megabytes of headroom on size, hundreds
// of headroom on element counts) while still rejecting the pathological
// inputs each field guards against (a single-token flood, a deeply nested
// bomb, an unbounded number literal). Tests may construct a tighter
// ParseLimits to exercise each boundary without needing to build
// gigabyte-scale fixtures.
struct ParseLimits {
  // Total input size, checked once before parsing begins.
  qsizetype maxInputBytes = 16 * 1024 * 1024;
  // Maximum array/object nesting depth (mirrors the previous hardcoded
  // kMaxNestingDepth, now a configurable field with the same default).
  int maxDepth = 200;
  // Maximum decoded length (in QChar/UTF-16 code units) of any single
  // string value or object key.
  qsizetype maxStringLength = 1 * 1024 * 1024;
  // Maximum digit count of a number literal's integer, fraction, or
  // exponent part (checked independently), bounding e.g. "1e<9000 more
  // digits>" without rejecting any realistic finite JSON number.
  qsizetype maxNumberDigits = 1024;
  // Maximum element count of any single array.
  qsizetype maxArrayElements = 100'000;
  // Maximum member count of any single object.
  qsizetype maxObjectMembers = 100'000;
  // Maximum total value count (every scalar/array/object node) across the
  // whole document, bounding a flat "wide" document (many small siblings)
  // that individually satisfies every other limit.
  qsizetype maxTotalNodes = 1'000'000;

  [[nodiscard]] static ParseLimits production() { return ParseLimits{}; }

  friend bool operator==(const ParseLimits &, const ParseLimits &) = default;
};

// A single JSON number's exact lexical parts (RFC 8259: `-? int frac? exp?`),
// captured without ever rounding through a double. literal() reconstructs a
// valid JSON number text carrying the identical value (canonicalizing e.g.
// a redundant leading '+' on the exponent away, never altering precision),
// so decode-then-encode round-trips are value-exact even for literals no
// double could represent.
class RawNumber {
public:
  RawNumber() = default;

  [[nodiscard]] bool isNegative() const noexcept { return m_negative; }
  [[nodiscard]] bool hasFraction() const noexcept {
    return !m_fracDigits.isEmpty();
  }
  [[nodiscard]] bool hasExponent() const noexcept { return m_hasExponent; }
  [[nodiscard]] const QString &integerDigits() const noexcept {
    return m_intDigits;
  }
  [[nodiscard]] const QString &fractionDigits() const noexcept {
    return m_fracDigits;
  }
  [[nodiscard]] bool exponentIsNegative() const noexcept {
    return m_exponentNegative;
  }
  [[nodiscard]] const QString &exponentDigits() const noexcept {
    return m_expDigits;
  }

  // Exact reconstructed JSON number text; always syntactically valid and
  // semantically identical to the parsed literal (see class comment).
  [[nodiscard]] QString literal() const;

  // The exact value as a qint64, iff this literal has no fraction/exponent
  // (a bare, possibly-signed integer) and fits qint64's range; nullopt for
  // every other literal (including "1.0" and "1e2", which -- while
  // mathematically integral -- are not bare-integer *literals* and are left
  // to the caller to interpret, since collapsing them would blur the exact
  // distinction between "the field held a decimal/scientific literal" and
  // "the field held a plain integer" that ExternalDeckId's tests care
  // about). "-0" IS a bare-integer literal by this definition and returns
  // 0 -- literal() is the only accessor that preserves its sign spelling.
  [[nodiscard]] std::optional<qint64> toInt64() const;

  // The exact value as a qint64 iff this literal is *mathematically*
  // integral -- i.e. every digit at or past the effective decimal point
  // (after applying the exponent) is zero -- and its magnitude fits
  // qint64's range; nullopt otherwise. Unlike toInt64() above (which only
  // recognizes a bare-integer *literal spelling*), this accepts every
  // integral Aeson/JSON form a contract-domain integer field may
  // legitimately carry, per the backend's Aeson Scientific-based decoder:
  // "1", "1.0", "1e2", "100e-2", etc. all decode to the exact qint64 100
  // (or in the last two cases' family, whichever integer they spell), and
  // "1.5"/"1e-1" correctly decode to nullopt (genuinely fractional).
  // Never rounds through a double, so this is exact across qint64's full
  // range, including magnitudes a double cannot represent exactly (e.g.
  // 9223372036854775807).
  [[nodiscard]] std::optional<qint64> toExactInt64() const;

  // Best-effort IEEE-754 double, for callers that do not need exact
  // fidelity beyond what QJsonValue already provides elsewhere in this
  // codebase.
  [[nodiscard]] double toDouble() const;

  // Builds the canonical bare-integer literal for `value` (no fraction,
  // no exponent) -- the inverse of toExactInt64() for a value with no
  // fractional part in its original spelling. Used to construct a
  // RawNumber programmatically (e.g. Json::Value AST builders) rather
  // than only ever producing one via Value::parse().
  [[nodiscard]] static RawNumber fromInt64(qint64 value);

  friend bool operator==(const RawNumber &, const RawNumber &) = default;

private:
  friend class Parser;

  bool m_negative = false;
  QString m_intDigits;
  QString m_fracDigits;
  bool m_hasExponent = false;
  bool m_exponentNegative = false;
  QString m_expDigits;
};

// A parsed-and-validated JSON value. Immutable once produced by
// Value::parse(); mirrors QJsonValue/QJsonObject/QJsonArray's read API
// closely enough that callers already familiar with those feel at home.
class Value {
public:
  enum class Kind { Undefined, Null, Bool, Number, String, Array, Object };

  // Kind::Undefined: matches QJsonValue's default state, used for e.g. a
  // missing object key.
  Value() = default;

  [[nodiscard]] Kind kind() const noexcept { return m_kind; }
  [[nodiscard]] bool isUndefined() const noexcept {
    return m_kind == Kind::Undefined;
  }
  [[nodiscard]] bool isNull() const noexcept { return m_kind == Kind::Null; }
  [[nodiscard]] bool isBool() const noexcept { return m_kind == Kind::Bool; }
  [[nodiscard]] bool isNumber() const noexcept {
    return m_kind == Kind::Number;
  }
  [[nodiscard]] bool isString() const noexcept {
    return m_kind == Kind::String;
  }
  [[nodiscard]] bool isArray() const noexcept { return m_kind == Kind::Array; }
  [[nodiscard]] bool isObject() const noexcept {
    return m_kind == Kind::Object;
  }

  // Kind-mismatched accessors return a default-constructed result rather
  // than asserting/aborting: callers are expected to branch on kind()/
  // isXxx() first, exactly as with QJsonValue.
  [[nodiscard]] bool toBool() const noexcept {
    return m_kind == Kind::Bool && m_bool;
  }
  [[nodiscard]] const RawNumber &toRawNumber() const noexcept {
    return m_number;
  }
  [[nodiscard]] const QString &toString() const noexcept { return m_string; }
  [[nodiscard]] const QList<Value> &toArray() const noexcept { return m_array; }

  // Object accessors. Duplicate keys are rejected during parsing (see
  // Value::parse()), so every key maps to at most one value.
  [[nodiscard]] bool contains(QLatin1StringView key) const;
  // Undefined if the object has no such key, or *this is not an object.
  [[nodiscard]] Value value(QLatin1StringView key) const;
  [[nodiscard]] QStringList keys() const;
  [[nodiscard]] const QList<std::pair<QString, Value>> &members() const {
    return m_object;
  }

  // Recursively converts to a standard Qt JSON value. Lossless for every
  // Kind except a Number whose literal is not RawNumber::toExactInt64()
  // (i.e. a genuine decimal or an integral value outside qint64's range),
  // which round-trips only as closely as IEEE-754 double allows; an
  // exact-int64 literal is preserved exactly via Qt's own QCborValue-backed
  // QJsonValue(qint64) storage (verified: QJsonValue::toInteger() on such
  // a value returns the identical qint64, unlike a value constructed from
  // a double).
  [[nodiscard]] QJsonValue toQJson() const;

  friend bool operator==(const Value &, const Value &) = default;

  // Parses `bytes` as a complete JSON document per RFC 8259 exactly:
  // rejects trailing content after the top-level value, duplicate object
  // keys (compared by decoded text), malformed number/string syntax,
  // invalid UTF-8, and invalid/lone surrogate escapes. Bounds recursion,
  // total input size, string/number-literal length, and array/object
  // element counts per `limits` (see ParseLimits), so a pathological or
  // adversarial input cannot overflow the stack or exhaust memory; an
  // additional network-layer cap (if any) is that layer's concern, not
  // this parser's.
  [[nodiscard]] static ValueOrError<Value>
  parse(QByteArrayView bytes, QStringView path,
        const ParseLimits &limits = ParseLimits::production());

  // Programmatic AST builders: the only way (besides parse()) to produce a
  // Value, used to compose a request body byte-exactly (e.g. splicing an
  // ExternalDeckId::Number's exact literal into a DeckListInput) without
  // ever routing it through a lossy QJsonValue double.
  [[nodiscard]] static Value makeNull();
  [[nodiscard]] static Value makeBool(bool value);
  [[nodiscard]] static Value makeNumber(RawNumber value);
  [[nodiscard]] static Value makeString(QString value);
  [[nodiscard]] static Value makeArray(QList<Value> elements);
  [[nodiscard]] static Value
  makeObject(QList<std::pair<QString, Value>> members);
  // Recursively converts a QJsonValue to a Value, for composing an AST out
  // of fields that are not precision-sensitive (any QJsonValue::Double is
  // necessarily already as lossy as that QJsonValue itself; see
  // makeNumber()/RawNumber::fromInt64() for the lossless alternative).
  [[nodiscard]] static Value fromQJson(const QJsonValue &v);

  // Serializes this value back to bytes per RFC 8259, exactly (RawNumber
  // literals are emitted via RawNumber::literal(), never rounded through a
  // double; strings are UTF-8 encoded with only the characters RFC 8259
  // requires escaped). Bounds recursion/element counts per `limits`
  // exactly like parse(), so a pathological programmatically-built AST
  // (e.g. one nested past `limits.maxDepth`) cannot recurse unboundedly or
  // crash -- it fails with a typed error instead.
  [[nodiscard]] ValueOrError<QByteArray>
  toJsonBytes(const ParseLimits &limits = ParseLimits::production()) const;

private:
  [[nodiscard]] ValueOrError<QByteArray>
  toJsonBytesInner(const ParseLimits &limits, int depth) const;
  friend class Parser;

  Kind m_kind{Kind::Undefined};
  bool m_bool = false;
  RawNumber m_number;
  QString m_string;
  QList<Value> m_array;
  QList<std::pair<QString, Value>> m_object;
};

} // namespace Arkham::Json
