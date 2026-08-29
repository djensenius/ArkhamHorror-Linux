#pragma once

#include "ValueOrError.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QJsonObject>
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

// Resource bounds enforced by Value::parse(), and -- for every field
// except maxInputBytes, which is meaningless against an AST already
// resident in memory rather than a byte stream still being read -- also
// symmetrically enforced by Value::toJsonBytes() against a
// programmatically-built AST: a network response is not yet in scope for
// this client, but the parser is the canonical boundary every governed
// fixture and future wire caller must go through, so it is bounded
// unconditionally rather than trusting an outer layer that does not exist
// yet.
//
// production()'s defaults below are sized against this client's actual
// worst-case real-world payload, not an arbitrary round number: the full
// pinned-backend card catalog (all ~5,929 cards, ~10.2MB of JSON) measures
// ~228,000 total nodes, a maximum single-array size of ~5,929 elements
// (the top-level card list), a maximum single-object size of 70 members,
// a maximum nesting depth of 7, a maximum string length of ~1,400 UTF-16
// units, and every numeric field well under 10 digits. Every limit below
// keeps multiple-times headroom over those measurements (room for years
// of new-card growth) while still bounding worst-case memory: `Value` is
// 176 bytes on a 64-bit build (measured via sizeof(Value)), so an
// unbounded/needlessly generous maxTotalNodes is a direct memory-
// amplification vector -- a 1,000,000-node prior default meant a
// ~2MB adversarial-but-otherwise-in-bounds payload (e.g. a handful of
// 100,000-element arrays) could force ~176MB of `Value` storage alone,
// before QString/QList/QHash container/allocator overhead, which is
// disproportionate to any input size this client legitimately handles.
// Tests may construct an even tighter ParseLimits to exercise each
// boundary without needing to build megabyte-scale fixtures.
struct ParseLimits {
  // Total input size, checked once before parsing begins. Parse-only:
  // toJsonBytes() has no equivalent input byte stream to bound. ~3x
  // headroom over the current full card catalog (~10.2MB).
  qsizetype maxInputBytes = 32 * 1024 * 1024;
  // Maximum array/object nesting depth (mirrors the previous hardcoded
  // kMaxNestingDepth, now a configurable field). ~9x headroom over the
  // catalog's measured maximum nesting depth of 7; also bounds this
  // parser's/serializer's own recursion depth.
  int maxDepth = 64;
  // Maximum decoded length (in QChar/UTF-16 code units) of any single
  // string value or object key. ~46x headroom over the catalog's longest
  // measured string (~1,400 units of card text).
  qsizetype maxStringLength = 64 * 1024;
  // Maximum digit count of a number literal's integer, fraction, or
  // exponent part (checked independently), bounding e.g. "1e<huge>"
  // without rejecting any realistic finite JSON number. This client's own
  // precision-preservation tests exercise up to a 39-digit fraction and a
  // 19-digit qint64-range exponent, so this stays comfortably above that
  // (real catalog numbers never exceed a handful of digits).
  qsizetype maxNumberDigits = 64;
  // Maximum element count of any single array. ~3.4x headroom over the
  // catalog's top-level card array (~5,929 elements).
  qsizetype maxArrayElements = 20'000;
  // Maximum member count of any single object. ~14x headroom over the
  // catalog's largest single card object (70 members).
  qsizetype maxObjectMembers = 1'024;
  // Maximum total value count (every scalar/array/object node) across the
  // whole document, bounding a flat "wide" document (many small siblings)
  // that individually satisfies every other limit. ~1.75x headroom over
  // the full catalog's measured ~228,000 nodes -- the dominant memory
  // bound given sizeof(Value) == 176 bytes (see doc comment above).
  qsizetype maxTotalNodes = 400'000;

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
  // Default-constructs the canonical "0" literal, not an unrepresentable
  // empty-digit state. Value's non-Number Kinds still need RawNumber to be
  // default-constructible (see m_number below), but leaving integerDigits()
  // empty by default made toExactInt64() vacuously return 0 (its "all
  // digits are zero" loop trivially holds over an empty digit string)
  // while toJsonBytes()'s number encoder correctly rejected that very same
  // digit-less value as unrepresentable ("number has no digits") -- an
  // inconsistency directly observable by any caller default-constructing a
  // RawNumber. A canonical zero closes that gap: literal()/toJsonBytes()
  // both emit "0", and toExactInt64() returns 0 for the same reason (an
  // all-zero coefficient), so every publicly reachable RawNumber -- this
  // default, a parsed literal, or fromInt64()'s output -- is consistently
  // valid and round-trips through toJsonBytes() without exception.
  RawNumber() : m_intDigits(QStringLiteral("0")) {}

  // Explicitly declared (rather than left to the compiler's implicit
  // move constructor/assignment) specifically to suppress move: this
  // class's state is entirely QString members, and QString's move
  // constructor/assignment leaves the moved-from string empty, which
  // would silently turn a moved-from RawNumber's m_intDigits empty --
  // exactly the "unrepresentable digit-less coefficient" state this
  // class's default constructor above was written to make unreachable
  // in the first place (toExactInt64()'s all-zero loop would then
  // vacuously return 0 for a value that literal()/toJsonBytes() reject
  // as invalid). Declaring the copy constructor/assignment here means
  // there is no user-declared move constructor/assignment for the
  // compiler to implicitly generate, so std::move(number) instead binds
  // to this copy constructor (an rvalue can bind to `const RawNumber&`),
  // leaving the moved-from source completely unchanged -- QString's copy
  // constructor is noexcept and O(1) (implicit sharing: a refcount
  // increment, never a deep copy), so this costs nothing relative to a
  // "real" move while making a moved-from RawNumber structurally
  // impossible to observe as invalid, including through a container
  // (QList/QMap/std::optional) that relocates/moves its elements.
  RawNumber(const RawNumber &) = default;
  RawNumber &operator=(const RawNumber &) = default;

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
  // Value::parse()), so every key maps to at most one value. contains()/
  // value() are average-case O(1) via an index built once alongside
  // m_object (see m_objectIndex below), not a linear scan: ParseLimits
  // permits an object up to maxObjectMembers (1,024 by default; see
  // ParseLimits::production() below) entries, and a decode path performing
  // its fixed handful of named-field lookups
  // against a single adversarially-padded object must not degrade to
  // O(members) per lookup.
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
  // Recursive, fallible counterpart of toQJson() above: identical
  // conversion, except a Number node whose literal is not exactly
  // representable as a qint64 (a genuine fraction, or an integral value
  // outside qint64's range -- see RawNumber::toExactInt64()) is a typed
  // failure that propagates out of the whole conversion, rather than
  // toQJson()'s silent fallback to a rounding IEEE-754 double. Intended
  // for a request-bound "convenience QJsonValue" caller that has no other
  // reason to reach for the raw AST/byte APIs directly but still must
  // never submit a silently-rounded/altered request: unlike toQJson(),
  // this enforces the *same* invariants as toJsonBytes() (see its own doc
  // comment) end-to-end -- a duplicate object key, a lone UTF-16 surrogate
  // in any string or key, a nesting depth/array-or-object size/total-node
  // count past ParseLimits::production(), and a Kind::Undefined value
  // nested inside an array element or object member (as opposed to the
  // whole top-level value itself, which legitimately stays Undefined --
  // see e.g. DeckListInput::toJson()'s guarded sideSlots call) are every
  // one a typed failure here, never a silently altered/truncated
  // QJsonValue tree. Still prefer toJsonBytes()/the raw AST directly
  // whenever the canonical wire representation is what actually matters.
  [[nodiscard]] ValueOrError<QJsonValue> toExactQJson() const;

  // toExactQJson() above, narrowed to the (overwhelmingly common) case
  // where *this is already known to be an Object -- exactly the shape
  // every outbound request's toJson() convenience composes via
  // toRawJson(). A typed failure (never Q_ASSERT/Q_UNREACHABLE) if kind()
  // is not Kind::Object, so a future caller that accidentally calls this
  // on a non-object Value gets a clear error instead of undefined
  // behavior from an unchecked toObject() cast. This is the one encoder
  // every request-facing toJson() should call after building its own
  // toRawJson() AST, so a QJsonObject convenience view can never expose
  // weaker validation (lone UTF-16 surrogates, duplicate keys, a nested
  // Kind::Undefined, or a size/depth/node-count past
  // ParseLimits::production()) than the canonical toJsonBytes() path --
  // see e.g. FetchDeckRequest::toJson()'s doc comment in Decks.h.
  [[nodiscard]] ValueOrError<QJsonObject> toExactQJsonObject() const;

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
  // Recursively converts a QJsonValue to a Value. A QJsonValue::Double can
  // only ever carry as much precision as that QJsonValue itself already
  // stores -- this recovers the exact qint64 whenever the underlying
  // storage round-trips through QJsonValue::toInteger() (which, unlike
  // toDouble(), reads the value's true stored representation and is
  // therefore exact all the way to the qint64 boundary, not just up to
  // double's 2^53 exact-integer range), and otherwise the shortest
  // round-trip decimal text for a non-integral double. Explicitly fails
  // (rather than silently substituting 0) for a non-finite double or an
  // internal round-trip inconsistency, so a corrupted/unsupported value
  // can never masquerade as a valid-looking numeric literal. See
  // makeNumber()/RawNumber::fromInt64() for the fully lossless
  // alternative when the source is not already a QJsonValue.
  [[nodiscard]] static ValueOrError<Value> fromQJson(const QJsonValue &v);

  // Serializes this value back to bytes per RFC 8259, exactly (RawNumber
  // literals are emitted via RawNumber::literal(), never rounded through a
  // double; strings are UTF-8 encoded with only the characters RFC 8259
  // requires escaped). Bounds recursion depth, array/object element
  // counts, string/key length, number-literal digit counts, and total
  // node count per `limits` exactly like parse() (every ParseLimits field
  // except maxInputBytes, which has no meaning against an already
  // in-memory AST), so a pathological programmatically-built AST (e.g.
  // one nested past `limits.maxDepth`, or containing a single
  // multi-megabyte string) cannot recurse unboundedly, emit unbounded
  // output, or crash -- it fails with a typed error instead.
  [[nodiscard]] ValueOrError<QByteArray>
  toJsonBytes(const ParseLimits &limits = ParseLimits::production()) const;

private:
  [[nodiscard]] ValueOrError<QByteArray>
  toJsonBytesInner(const ParseLimits &limits, int depth,
                   qsizetype &totalNodes) const;
  // Shared recursive body for toExactQJson() above; see its doc comment
  // for the full list of invariants enforced (duplicate keys, embedded
  // Undefined, lone surrogates, ParseLimits bounds).
  [[nodiscard]] ValueOrError<QJsonValue>
  toExactQJsonInner(const ParseLimits &limits, int depth,
                    qsizetype &totalNodes) const;
  friend class Parser;

  Kind m_kind{Kind::Undefined};
  bool m_bool = false;
  RawNumber m_number;
  QString m_string;
  QList<Value> m_array;
  QList<std::pair<QString, Value>> m_object;
  // key -> index into m_object, built once (by makeObject()/Parser, the
  // only two places an Object-kind Value is constructed) rather than
  // lazily, so it needs no `mutable` state and stays a plain deterministic
  // function of m_object for operator==() purposes. A key present more
  // than once (a transiently-constructible but invalid state that
  // toJsonBytes()/parse() both reject -- see their duplicate-key checks)
  // maps to its first occurrence, matching contains()/value()'s prior
  // linear-scan behavior exactly.
  QHash<QString, qsizetype> m_objectIndex;
};

} // namespace Arkham::Json
