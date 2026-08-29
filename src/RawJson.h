#pragma once

#include "ValueOrError.h"

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
  // every other literal (including "-0", "1.0", and "1e2", which -- while
  // mathematically integral -- are not bare-integer *literals* and are left
  // to the caller to interpret, since collapsing them would blur the exact
  // distinction between "the field held a decimal/scientific literal" and
  // "the field held a plain integer" that ExternalDeckId's tests care
  // about).
  [[nodiscard]] std::optional<qint64> toInt64() const;

  // Best-effort IEEE-754 double, for callers that do not need exact
  // fidelity beyond what QJsonValue already provides elsewhere in this
  // codebase.
  [[nodiscard]] double toDouble() const;

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

  // Recursively converts to a standard Qt JSON value (see file comment for
  // the one precision caveat this entails for numbers).
  [[nodiscard]] QJsonValue toQJson() const;

  friend bool operator==(const Value &, const Value &) = default;

  // Parses `bytes` as a complete JSON document per RFC 8259 exactly:
  // rejects trailing content after the top-level value, duplicate object
  // keys (compared by decoded text), malformed number/string syntax,
  // invalid UTF-8, and invalid/lone surrogate escapes. Bounds recursion via
  // a fixed nesting-depth cap (see RawJson.cpp's kMaxNestingDepth) so a
  // pathological input cannot overflow the stack; an overall byte-size cap
  // is a network-layer concern deliberately left to that (currently
  // out-of-scope) layer.
  [[nodiscard]] static ValueOrError<Value> parse(QByteArrayView bytes,
                                                 QStringView path);

private:
  friend class Parser;

  Kind m_kind{Kind::Undefined};
  bool m_bool = false;
  RawNumber m_number;
  QString m_string;
  QList<Value> m_array;
  QList<std::pair<QString, Value>> m_object;
};

} // namespace Arkham::Json
