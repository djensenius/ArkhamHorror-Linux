#include <QtTest>

#include <limits>

#include "RawJson.h"

using Arkham::Failure;
using Arkham::Json::ParseLimits;
using Arkham::Json::RawNumber;
using Arkham::Json::Value;

using namespace Qt::StringLiterals;

class RawJsonTests final : public QObject {
  Q_OBJECT

private slots:
  void parsesEveryScalarKind();
  void parsesNestedArraysAndObjects();
  void preservesLargeIntegerLiteralExactly();
  void preservesLongFractionLiteralExactly();
  void preservesHugePositiveExponentLiteralExactly();
  void preservesHugeNegativeExponentLiteralExactly();
  void preservesNegativeZeroSignAndValue();
  void toInt64RejectsFractionalAndExponentLiterals();
  void defaultConstructedRawNumberIsCanonicalZeroNotVacuous();
  void rejectsDuplicateObjectKeys();
  void rejectsEscapeEquivalentDuplicateObjectKeys();
  void acceptsDistinctKeysThatAreNotEscapeEquivalent();
  void decodesAllSingleCharacterEscapes();
  void decodesAstralCharacterViaSurrogatePair();
  void rejectsLoneHighSurrogate();
  void rejectsLoneLowSurrogate();
  void rejectsInvalidUtf8ContinuationByte();
  void rejectsOverlongUtf8Encoding();
  void rejectsUnescapedControlCharacter();
  void rejectsLeadingZeroInInteger();
  void rejectsMissingDigitsAfterDecimalPoint();
  void rejectsMissingDigitsInExponent();
  void rejectsPlusSignOnBareInteger();
  void rejectsInfinityAndNanTokens();
  void rejectsTrailingCommaInArray();
  void rejectsTrailingCommaInObject();
  void rejectsTrailingContentAfterTopLevelValue();
  void rejectsExcessiveNestingDepth();
  void acceptsEmptyArrayAndObject();
  void objectAccessorsFindPresentKeysAndMissAbsentOnes();
  // Review round 5 (RawJson.cpp:476 HIGH finding): contains()/value() must
  // remain correct once backed by an index rather than a linear scan, at
  // a large member count built via the unbounded makeObject() constructor
  // (not Value::parse(), so ParseLimits::production()'s maxObjectMembers
  // does not apply here), and for the (invalid, but constructible via
  // makeObject()) transient duplicate-key case toJsonBytes() rejects.
  void objectAccessorLookupIsCorrectAcrossManyMembers();
  void makeObjectDuplicateKeyResolvesToFirstOccurrence();
  void toQJsonConvertsEveryKind();
  void skipsLeadingAndTrailingWhitespace();

  // toExactInt64()/fromInt64()/qint64-exactness (issue #19 review round 3,
  // item 4: contract integers must be exact qint64, never rounded through
  // a double).
  void toExactInt64AcceptsIntegralDecimalAndExponentForms();
  void toExactInt64RejectsGenuinelyFractionalValues();
  void toExactInt64HandlesQint64BoundariesExactly();
  // Review round 7, item 1: a nonzero coefficient with an exponent near
  // qint64's own extremes must not signed-overflow the internal
  // decimal-point-position addition (undefined behavior otherwise); a
  // zero coefficient must short-circuit to exact 0 before that addition
  // even runs, since 0 * 10^e == 0 regardless of how huge (but validly
  // parseable) e is.
  void toExactInt64RejectsNonzeroCoefficientWithHugeExponentWithoutOverflow();
  void toExactInt64ZeroCoefficientWithHugeExponentIsExactZero();
  void fromInt64RoundTripsFullRange();
  void toQJsonPreservesInt64ExactlyBeyondDoublePrecision();
  void fromQJsonConvertsQJsonTreeRecursively();
  void fromQJsonPreservesInt64MaxExactlyAtBoundary();
  void fromQJsonPreservesInt64MinExactlyAtBoundary();
  void fromQJsonRejectsNonFiniteDouble();
  void fromQJsonPreservesLargeIntegerNestedInsideArray();

  // Value::make*()/toJsonBytes() lossless AST builder+serializer (review
  // round 3, item 2: spliceRawJsonMember's replacement).
  void toJsonBytesRoundTripsBuiltObject();
  void toJsonBytesRejectsDuplicateObjectKeys();
  void toJsonBytesRejectsUndefinedValue();
  void toJsonBytesEscapesInjectionAttemptsInStringsAndKeys();

  // ParseLimits (review round 3, item 8: explicit configurable resource
  // bounds). Each below constructs a deliberately tight limit and checks
  // both the boundary (accepted) and boundary+1 (rejected) where
  // meaningful.
  void parseLimitsRejectsInputExceedingMaxInputBytes();
  void parseLimitsRejectsDepthExceedingMaxDepth();
  void parseLimitsRejectsStringExceedingMaxStringLength();
  void parseLimitsBoundsSurrogatePairAppendAgainstMaxStringLength();
  void parseLimitsBoundsRawUtf8AstralAppendAgainstMaxStringLength();
  void parseLimitsRejectsNumberExceedingMaxNumberDigits();
  void parseLimitsRejectsArrayExceedingMaxArrayElements();
  void parseLimitsRejectsObjectExceedingMaxObjectMembers();
  void parseLimitsRejectsTotalNodesExceedingMaxTotalNodes();
  void toJsonBytesRejectsDepthExceedingLimitOnProgrammaticAst();
  // Round 5 item 9: production()'s own (now-lowered) defaults must be
  // exercised directly -- not merely a tiny custom ParseLimits -- at
  // realistic worst-case "dense scalars"/"wide object" scale, proving the
  // new bounds are both large enough for this client's real catalog
  // workload and actually enforced at that scale (not merely on paper).
  void productionLimitsAcceptDenseArrayAtBoundaryAndRejectOneOver();
  void productionLimitsAcceptWideObjectAtBoundaryAndRejectOneOver();
  void productionLimitsAcceptTotalNodesAtBoundaryAndRejectOneOver();

  // toJsonBytes() must bound every ParseLimits field symmetrically with
  // parse() (except maxInputBytes, which has no meaning for an
  // already-in-memory AST) so a pathological programmatically-built AST
  // cannot emit unbounded output any more than a malicious input document
  // can be parsed (review round 4 finding: RawJson.h's doc comment
  // overstated this before toJsonBytesInner() actually enforced it).
  void toJsonBytesRejectsStringExceedingMaxStringLength();
  void toJsonBytesRejectsObjectKeyExceedingMaxStringLength();
  void toJsonBytesRejectsNumberExceedingMaxNumberDigits();
  void toJsonBytesRejectsArrayExceedingMaxArrayElements();
  void toJsonBytesRejectsObjectExceedingMaxObjectMembers();
  void toJsonBytesRejectsTotalNodesExceedingMaxTotalNodes();
  void toJsonBytesAcceptsValuesExactlyAtEveryLimitBoundary();
  // Round 7 item 5: a lone (unpaired) UTF-16 surrogate in a
  // programmatically-built string value or object key cannot be encoded
  // as valid UTF-8, so serialization must fail with a typed error rather
  // than emit an invalid byte sequence; a valid surrogate pair must still
  // encode successfully, including when nested inside an aggregate.
  void toJsonBytesRejectsLoneHighSurrogateInStringValue();
  void toJsonBytesRejectsLoneLowSurrogateInStringValue();
  void toJsonBytesRejectsLoneSurrogateInObjectKey();
  void toJsonBytesRejectsLoneSurrogateNestedInsideArray();
  void toJsonBytesAcceptsValidSurrogatePairInStringAndKey();
  void parseAcceptsEverySerializerSuccess();
};

namespace {
Value mustParse(QByteArrayView bytes) {
  auto result = Value::parse(bytes, u"test");
  if (!result)
    qFatal("expected successful parse but got: %s", qPrintable(result.error()));
  return *result;
}
} // namespace

void RawJsonTests::parsesEveryScalarKind() {
  QVERIFY(mustParse("null").isNull());
  QVERIFY(mustParse("true").isBool());
  QCOMPARE(mustParse("true").toBool(), true);
  QVERIFY(mustParse("false").isBool());
  QCOMPARE(mustParse("false").toBool(), false);
  QVERIFY(mustParse("42").isNumber());
  QCOMPARE(mustParse("42").toRawNumber().integerDigits(), u"42"_s);
  QVERIFY(mustParse("\"hi\"").isString());
  QCOMPARE(mustParse("\"hi\"").toString(), u"hi"_s);
}

void RawJsonTests::parsesNestedArraysAndObjects() {
  auto value = mustParse(R"({"a":[1,2,{"b":true}],"c":null})");
  QVERIFY(value.isObject());
  QVERIFY(value.value("a"_L1).isArray());
  QCOMPARE(value.value("a"_L1).toArray().size(), 3);
  QVERIFY(value.value("a"_L1).toArray().at(2).isObject());
  QVERIFY(value.value("a"_L1).toArray().at(2).value("b"_L1).toBool());
  QVERIFY(value.value("c"_L1).isNull());
}

void RawJsonTests::preservesLargeIntegerLiteralExactly() {
  // 2^53 + 1: the smallest positive integer a double cannot represent
  // exactly. ArkhamDB is known to hand out deck ids in this range.
  auto value = mustParse("9007199254740993");
  const auto &number = value.toRawNumber();
  QVERIFY(!number.hasFraction());
  QVERIFY(!number.hasExponent());
  QCOMPARE(number.integerDigits(), u"9007199254740993"_s);
  QVERIFY(number.toInt64().has_value());
  QCOMPARE(*number.toInt64(), 9007199254740993LL);
  QCOMPARE(number.literal(), u"9007199254740993"_s);
}

void RawJsonTests::preservesLongFractionLiteralExactly() {
  auto value = mustParse("1.123456789012345678901234567890");
  const auto &number = value.toRawNumber();
  QVERIFY(number.hasFraction());
  QCOMPARE(number.fractionDigits(), u"123456789012345678901234567890"_s);
  QVERIFY(!number.toInt64().has_value());
  QCOMPARE(number.literal(), u"1.123456789012345678901234567890"_s);
}

void RawJsonTests::preservesHugePositiveExponentLiteralExactly() {
  auto value = mustParse("1e128");
  const auto &number = value.toRawNumber();
  QVERIFY(number.hasExponent());
  QVERIFY(!number.exponentIsNegative());
  QCOMPARE(number.exponentDigits(), u"128"_s);
  QVERIFY(!number.toInt64().has_value());
  QCOMPARE(number.literal(), u"1e128"_s);
}

void RawJsonTests::preservesHugeNegativeExponentLiteralExactly() {
  auto value = mustParse("5e-300");
  const auto &number = value.toRawNumber();
  QVERIFY(number.hasExponent());
  QVERIFY(number.exponentIsNegative());
  QCOMPARE(number.exponentDigits(), u"300"_s);
  QCOMPARE(number.literal(), u"5e-300"_s);
}

void RawJsonTests::preservesNegativeZeroSignAndValue() {
  auto value = mustParse("-0");
  const auto &number = value.toRawNumber();
  QVERIFY(number.isNegative());
  QCOMPARE(number.integerDigits(), u"0"_s);
  QCOMPARE(number.literal(), u"-0"_s);
  // Mathematically integral, so toInt64() reports 0 -- but literal()
  // still round-trips the sign, which is what callers that must
  // distinguish "-0" from "0" byte-for-byte (if any ever need to) rely on.
  QVERIFY(number.toInt64().has_value());
  QCOMPARE(*number.toInt64(), 0LL);
}

void RawJsonTests::toInt64RejectsFractionalAndExponentLiterals() {
  QVERIFY(!mustParse("1.0").toRawNumber().toInt64().has_value());
  QVERIFY(!mustParse("1e2").toRawNumber().toInt64().has_value());
}

void RawJsonTests::defaultConstructedRawNumberIsCanonicalZeroNotVacuous() {
  // Round 6 review: a default-constructed RawNumber previously left every
  // digit string empty, so toExactInt64() vacuously reported 0 (its "every
  // digit is zero" loop trivially holds over an empty string) while
  // toJsonBytes() correctly rejected that same digit-less value as
  // unrepresentable ("number has no digits") -- an inconsistency directly
  // observable via Value::makeNumber(RawNumber{}). The default must now be
  // the canonical "0" literal, consistent across every accessor.
  const RawNumber number;
  QVERIFY(!number.isNegative());
  QVERIFY(!number.hasFraction());
  QVERIFY(!number.hasExponent());
  QCOMPARE(number.integerDigits(), u"0"_s);
  QCOMPARE(number.literal(), u"0"_s);
  QVERIFY(number.toInt64().has_value());
  QCOMPARE(*number.toInt64(), 0LL);
  QVERIFY(number.toExactInt64().has_value());
  QCOMPARE(*number.toExactInt64(), 0LL);
  QCOMPARE(number.toDouble(), 0.0);

  // The value must actually be encodable -- unlike the old vacuous-zero
  // state, which toExactInt64() called valid but toJsonBytes() rejected.
  const Value value = Value::makeNumber(number);
  const auto bytes = value.toJsonBytes();
  QVERIFY(bytes.has_value());
  QCOMPARE(*bytes, QByteArrayLiteral("0"));
  const auto reparsed = Value::parse(*bytes, u"test");
  if (!reparsed)
    QFAIL(qPrintable(reparsed.error()));
  QCOMPARE(*reparsed, value);

  // fromInt64(0) must agree exactly with the default (both are the
  // canonical zero literal, not merely equal-valued).
  QCOMPARE(RawNumber::fromInt64(0), number);
}

void RawJsonTests::rejectsDuplicateObjectKeys() {
  auto result = Value::parse(R"({"id":1,"id":2})", u"test");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(u"duplicate"_s), qPrintable(result.error()));
}

void RawJsonTests::rejectsEscapeEquivalentDuplicateObjectKeys() {
  // "\u0069d" decodes to the same text as "id" (\u0069 == 'i'), so this
  // must be rejected as a duplicate key exactly like a literal repeat.
  auto result = Value::parse(R"({"id":1,"\u0069d":2})", u"test");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(u"duplicate"_s), qPrintable(result.error()));
}

void RawJsonTests::acceptsDistinctKeysThatAreNotEscapeEquivalent() {
  auto value = mustParse(R"({"id":1,"other":2})");
  QVERIFY(value.isObject());
  QCOMPARE(value.keys().size(), 2);
}

void RawJsonTests::decodesAllSingleCharacterEscapes() {
  auto value = mustParse(R"("\"\\\/\b\f\n\r\t")");
  QCOMPARE(value.toString(), QStringLiteral("\"\\/\b\f\n\r\t"));
}

void RawJsonTests::decodesAstralCharacterViaSurrogatePair() {
  // U+1F600 GRINNING FACE, encoded both as a \u surrogate pair and as raw
  // UTF-8 bytes; both must decode to the identical QString.
  auto viaEscape = mustParse(R"("\ud83d\ude00")");
  auto viaRawUtf8 = mustParse(QByteArrayLiteral("\"\xF0\x9F\x98\x80\""));
  QCOMPARE(viaEscape.toString(), viaRawUtf8.toString());
  QCOMPARE(viaEscape.toString(), QString::fromUtf8("\xF0\x9F\x98\x80"));
}

void RawJsonTests::rejectsLoneHighSurrogate() {
  auto result = Value::parse(R"("\ud83d")", u"test");
  QVERIFY(!result.has_value());
}

void RawJsonTests::rejectsLoneLowSurrogate() {
  auto result = Value::parse(R"("\ude00")", u"test");
  QVERIFY(!result.has_value());
}

void RawJsonTests::rejectsInvalidUtf8ContinuationByte() {
  // 0xC2 is a valid two-byte UTF-8 lead byte but 0x20 (space) is not a
  // valid continuation byte.
  auto result = Value::parse(QByteArrayLiteral("\"\xC2\x20\""), u"test");
  QVERIFY(!result.has_value());
}

void RawJsonTests::rejectsOverlongUtf8Encoding() {
  // 0xC0 0x80 is an overlong two-byte encoding of NUL (U+0000), which must
  // be encoded as a single 0x00 byte -- disallowed by RFC 8259 regardless.
  auto result = Value::parse(QByteArrayLiteral("\"\xC0\x80\""), u"test");
  QVERIFY(!result.has_value());
}

void RawJsonTests::rejectsUnescapedControlCharacter() {
  auto result = Value::parse(QByteArrayLiteral("\"\x01\""), u"test");
  QVERIFY(!result.has_value());
}

void RawJsonTests::rejectsLeadingZeroInInteger() {
  QVERIFY(!Value::parse("01", u"test").has_value());
}

void RawJsonTests::rejectsMissingDigitsAfterDecimalPoint() {
  QVERIFY(!Value::parse("1.", u"test").has_value());
}

void RawJsonTests::rejectsMissingDigitsInExponent() {
  QVERIFY(!Value::parse("1e", u"test").has_value());
  QVERIFY(!Value::parse("1e+", u"test").has_value());
}

void RawJsonTests::rejectsPlusSignOnBareInteger() {
  QVERIFY(!Value::parse("+1", u"test").has_value());
}

void RawJsonTests::rejectsInfinityAndNanTokens() {
  QVERIFY(!Value::parse("Infinity", u"test").has_value());
  QVERIFY(!Value::parse("NaN", u"test").has_value());
}

void RawJsonTests::rejectsTrailingCommaInArray() {
  QVERIFY(!Value::parse("[1,2,]", u"test").has_value());
}

void RawJsonTests::rejectsTrailingCommaInObject() {
  QVERIFY(!Value::parse(R"({"a":1,})", u"test").has_value());
}

void RawJsonTests::rejectsTrailingContentAfterTopLevelValue() {
  QVERIFY(!Value::parse("1 2", u"test").has_value());
  QVERIFY(!Value::parse("{}garbage", u"test").has_value());
}

void RawJsonTests::rejectsExcessiveNestingDepth() {
  QString pathological;
  for (int i = 0; i < 500; ++i)
    pathological += u'[';
  for (int i = 0; i < 500; ++i)
    pathological += u']';
  auto result = Value::parse(pathological.toUtf8(), u"test");
  QVERIFY(!result.has_value());
}

void RawJsonTests::acceptsEmptyArrayAndObject() {
  auto array = mustParse("[]");
  QVERIFY(array.isArray());
  QCOMPARE(array.toArray().size(), 0);
  auto object = mustParse("{}");
  QVERIFY(object.isObject());
  QCOMPARE(object.keys().size(), 0);
}

void RawJsonTests::objectAccessorsFindPresentKeysAndMissAbsentOnes() {
  auto value = mustParse(R"({"present":1})");
  QVERIFY(value.contains("present"_L1));
  QVERIFY(!value.contains("absent"_L1));
  QVERIFY(value.value("present"_L1).isNumber());
  QVERIFY(value.value("absent"_L1).isUndefined());
}

void RawJsonTests::objectAccessorLookupIsCorrectAcrossManyMembers() {
  // Exercises a member count well beyond ParseLimits::production()'s
  // maxObjectMembers default (built via the unbounded makeObject()
  // constructor, not Value::parse(), so that default does not apply) so
  // an index-backed contains()/value() is proven correct (not merely
  // fast) at a realistic worst-case size: every key must resolve to its
  // own distinct value, the last-inserted key must be found exactly like
  // the first, and an absent key must still correctly report "not found"
  // rather than colliding with a present one.
  constexpr qsizetype kMemberCount = 50'000;
  QList<std::pair<QString, Value>> members;
  members.reserve(kMemberCount);
  for (qsizetype i = 0; i < kMemberCount; ++i) {
    members.append({QStringLiteral("key%1").arg(i),
                    Value::makeNumber(RawNumber::fromInt64(i))});
  }
  const Value obj = Value::makeObject(members);
  QCOMPARE(obj.members().size(), kMemberCount);
  QVERIFY(obj.contains(QLatin1StringView("key0")));
  QCOMPARE(obj.value(QLatin1StringView("key0")).toRawNumber().literal(),
           QStringLiteral("0"));
  const QByteArray lastKeyUtf8 =
      QStringLiteral("key%1").arg(kMemberCount - 1).toUtf8();
  const QLatin1StringView lastKeyView(lastKeyUtf8.constData());
  QVERIFY(obj.contains(lastKeyView));
  QCOMPARE(obj.value(lastKeyView).toRawNumber().literal(),
           QString::number(kMemberCount - 1));
  QVERIFY(!obj.contains(QLatin1StringView("not-present")));
  QVERIFY(obj.value(QLatin1StringView("not-present")).isUndefined());
  // A middle key, to confirm the index is not merely correct at the two
  // extremes.
  const QByteArray midKeyUtf8 =
      QStringLiteral("key%1").arg(kMemberCount / 2).toUtf8();
  const QLatin1StringView midKeyView(midKeyUtf8.constData());
  QCOMPARE(obj.value(midKeyView).toRawNumber().literal(),
           QString::number(kMemberCount / 2));
}

void RawJsonTests::makeObjectDuplicateKeyResolvesToFirstOccurrence() {
  // makeObject() (unlike Value::parse(), which rejects duplicate keys
  // outright) can transiently construct an object with a duplicate key;
  // this state cannot survive toJsonBytes() (see
  // toJsonBytesRejectsDuplicateObjectKeys()), but contains()/value() must
  // still behave exactly as the pre-index linear scan did: the first
  // occurrence wins, never the last.
  QList<std::pair<QString, Value>> members{
      {QStringLiteral("a"), Value::makeString(QStringLiteral("first"))},
      {QStringLiteral("b"), Value::makeNull()},
      {QStringLiteral("a"), Value::makeString(QStringLiteral("second"))},
  };
  const Value obj = Value::makeObject(members);
  QVERIFY(obj.contains("a"_L1));
  QCOMPARE(obj.value("a"_L1).toString(), QStringLiteral("first"));
}

void RawJsonTests::toQJsonConvertsEveryKind() {
  auto value = mustParse(R"({"a":1,"b":"s","c":true,"d":null,"e":[1,2]})");
  auto json = value.toQJson();
  QVERIFY(json.isObject());
  auto object = json.toObject();
  QCOMPARE(object.value("a"_L1).toDouble(), 1.0);
  QCOMPARE(object.value("b"_L1).toString(), u"s"_s);
  QCOMPARE(object.value("c"_L1).toBool(), true);
  QVERIFY(object.value("d"_L1).isNull());
  QVERIFY(object.value("e"_L1).isArray());
  QCOMPARE(object.value("e"_L1).toArray().size(), 2);
}

void RawJsonTests::skipsLeadingAndTrailingWhitespace() {
  auto value = mustParse("  \n\t 42 \r\n ");
  QVERIFY(value.isNumber());
  QCOMPARE(value.toRawNumber().integerDigits(), u"42"_s);
}

void RawJsonTests::toExactInt64AcceptsIntegralDecimalAndExponentForms() {
  QCOMPARE(*mustParse("1").toRawNumber().toExactInt64(), 1LL);
  QCOMPARE(*mustParse("1.0").toRawNumber().toExactInt64(), 1LL);
  QCOMPARE(*mustParse("1e2").toRawNumber().toExactInt64(), 100LL);
  QCOMPARE(*mustParse("100e-2").toRawNumber().toExactInt64(), 1LL);
  QCOMPARE(*mustParse("10e-1").toRawNumber().toExactInt64(), 1LL);
  QCOMPARE(*mustParse("0").toRawNumber().toExactInt64(), 0LL);
  QCOMPARE(*mustParse("-0").toRawNumber().toExactInt64(), 0LL);
  // 2^53 + 1: exact via toExactInt64() even though it exceeds double's
  // exact-integer range, unlike a value that had to round through one.
  QCOMPARE(*mustParse("9007199254740993").toRawNumber().toExactInt64(),
           9007199254740993LL);
}

void RawJsonTests::toExactInt64RejectsGenuinelyFractionalValues() {
  QVERIFY(!mustParse("1.5").toRawNumber().toExactInt64().has_value());
  QVERIFY(!mustParse("1e-1").toRawNumber().toExactInt64().has_value());
  QVERIFY(!mustParse("1.23e1").toRawNumber().toExactInt64().has_value());
}

void RawJsonTests::toExactInt64HandlesQint64BoundariesExactly() {
  QCOMPARE(*mustParse("9223372036854775807").toRawNumber().toExactInt64(),
           std::numeric_limits<qint64>::max());
  QVERIFY(!mustParse("9223372036854775808")
               .toRawNumber()
               .toExactInt64()
               .has_value());
  QCOMPARE(*mustParse("-9223372036854775808").toRawNumber().toExactInt64(),
           std::numeric_limits<qint64>::min());
  QVERIFY(!mustParse("-9223372036854775809")
               .toRawNumber()
               .toExactInt64()
               .has_value());
}

void RawJsonTests::
    toExactInt64RejectsNonzeroCoefficientWithHugeExponentWithoutOverflow() {
  // "1e9223372036854775807": exponent digit text itself parses to exactly
  // qint64's max magnitude, so intDigits.size() (1) + exponent would
  // overflow qint64 by exactly 1 if computed with plain (unchecked)
  // addition -- undefined behavior that ASan/UBSan must not flag here,
  // since the checked-addition fix must reject this as out-of-range
  // instead of computing it.
  QVERIFY(!mustParse("1e9223372036854775807")
               .toRawNumber()
               .toExactInt64()
               .has_value());
  QVERIFY(!mustParse("-1e9223372036854775807")
               .toRawNumber()
               .toExactInt64()
               .has_value());
  // A multi-digit coefficient pushes the addition even further past
  // qint64::max, still must not overflow/crash, must reject cleanly.
  QVERIFY(!mustParse("123456789e9223372036854775807")
               .toRawNumber()
               .toExactInt64()
               .has_value());
  // Huge *negative* exponent with a nonzero coefficient: every digit
  // becomes fractional (magnitude < 1), a legitimate (non-overflow)
  // rejection path, but must still not overflow computing the
  // decimal-point position itself.
  QVERIFY(!mustParse("1e-9223372036854775807")
               .toRawNumber()
               .toExactInt64()
               .has_value());
}

void RawJsonTests::toExactInt64ZeroCoefficientWithHugeExponentIsExactZero() {
  // 0 * 10^e == 0 for any (validly parseable) exponent, however huge --
  // must short-circuit to exact 0 rather than attempting (and having to
  // reject) the decimal-point-position arithmetic the nonzero-coefficient
  // path above needs.
  QCOMPARE(*mustParse("0e9223372036854775807").toRawNumber().toExactInt64(),
           0LL);
  QCOMPARE(*mustParse("-0e9223372036854775807").toRawNumber().toExactInt64(),
           0LL);
  QCOMPARE(*mustParse("0e-9223372036854775807").toRawNumber().toExactInt64(),
           0LL);
  // A zero coefficient spelled with a fractional part, still exactly
  // zero.
  QCOMPARE(*mustParse("0.000e9223372036854775807").toRawNumber().toExactInt64(),
           0LL);
}

void RawJsonTests::fromInt64RoundTripsFullRange() {
  for (const qint64 v : {0LL, 1LL, -1LL, std::numeric_limits<qint64>::max(),
                         std::numeric_limits<qint64>::min(), 100LL, -100LL}) {
    const auto number = RawNumber::fromInt64(v);
    QVERIFY(number.toExactInt64().has_value());
    QCOMPARE(*number.toExactInt64(), v);
  }
}

void RawJsonTests::toQJsonPreservesInt64ExactlyBeyondDoublePrecision() {
  auto value = mustParse(R"({"id":9007199254740993})");
  auto json = value.toQJson();
  // toInteger() recovers the exact qint64 even though .isDouble()/
  // .toDouble() report the rounded double -- see Value::toQJson()'s doc
  // comment.
  QCOMPARE(json.toObject().value("id"_L1).toInteger(), 9007199254740993LL);
}

void RawJsonTests::fromQJsonConvertsQJsonTreeRecursively() {
  QJsonObject obj;
  obj.insert(QStringLiteral("n"), QJsonValue(qint64(9007199254740993LL)));
  obj.insert(QStringLiteral("s"), QStringLiteral("hi"));
  obj.insert(QStringLiteral("b"), true);
  obj.insert(QStringLiteral("z"), QJsonValue());
  QJsonArray arr{1, 2, 3};
  obj.insert(QStringLiteral("a"), arr);
  auto convertedResult = Value::fromQJson(obj);
  if (!convertedResult)
    QFAIL(qPrintable(convertedResult.error()));
  const Value &converted = *convertedResult;
  QVERIFY(converted.isObject());
  QCOMPARE(converted.value("n"_L1).toRawNumber().toExactInt64().value_or(-1),
           9007199254740993LL);
  QCOMPARE(converted.value("s"_L1).toString(), u"hi"_s);
  QCOMPARE(converted.value("b"_L1).toBool(), true);
  QVERIFY(converted.value("z"_L1).isNull());
  QVERIFY(converted.value("a"_L1).isArray());
  QCOMPARE(converted.value("a"_L1).toArray().size(), 3);
}

void RawJsonTests::fromQJsonPreservesInt64MaxExactlyAtBoundary() {
  // qint64::max() (2^63-1) rounds UP to 2^63 as a double, so a version of
  // fromQJson() that gated its exact-integer recovery on `std::abs(d) <
  // 9.2e18` would incorrectly fall through to the lossy decimal-text
  // fallback here and silently corrupt the value.
  constexpr qint64 kMax = std::numeric_limits<qint64>::max();
  const QJsonValue qv(kMax);
  auto result = Value::fromQJson(qv);
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->isNumber());
  QCOMPARE(result->toRawNumber().toExactInt64().value_or(0), kMax);
}

void RawJsonTests::fromQJsonPreservesInt64MinExactlyAtBoundary() {
  constexpr qint64 kMin = std::numeric_limits<qint64>::min();
  const QJsonValue qv(kMin);
  auto result = Value::fromQJson(qv);
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->isNumber());
  QCOMPARE(result->toRawNumber().toExactInt64().value_or(0), kMin);
}

void RawJsonTests::fromQJsonRejectsNonFiniteDouble() {
  // A QJsonValue can only ever hold a non-finite double via direct C++
  // construction (JSON text itself has no NaN/Infinity literal); previously
  // this silently produced a valid-looking numeric literal "0" instead of
  // failing.
  const QJsonValue nanValue(std::numeric_limits<double>::quiet_NaN());
  auto nanResult = Value::fromQJson(nanValue);
  QVERIFY(!nanResult.has_value());

  const QJsonValue infValue(std::numeric_limits<double>::infinity());
  auto infResult = Value::fromQJson(infValue);
  QVERIFY(!infResult.has_value());
}

void RawJsonTests::fromQJsonPreservesLargeIntegerNestedInsideArray() {
  // The top-level scalar id path (ExternalDeckId::fromObject) has its own
  // isfinite()/isNumber() guard, but fromQJson() itself must also recover
  // full precision for numbers nested arbitrarily deep inside sideSlots
  // (or any other open/unconstrained subtree), since no per-field caller
  // wraps every nested value individually.
  constexpr qint64 kMax = std::numeric_limits<qint64>::max();
  QJsonArray arr;
  arr.append(QJsonValue(kMax));
  QJsonObject obj;
  obj.insert(QStringLiteral("nested"), arr);
  auto result = Value::fromQJson(obj);
  if (!result)
    QFAIL(qPrintable(result.error()));
  const auto nested = result->value("nested"_L1);
  QVERIFY(nested.isArray());
  const auto element = nested.toArray().at(0);
  QCOMPARE(element.toRawNumber().toExactInt64().value_or(0), kMax);
}

void RawJsonTests::toJsonBytesRoundTripsBuiltObject() {
  QList<std::pair<QString, Value>> members;
  members.append({QStringLiteral("id"),
                  Value::makeNumber(RawNumber::fromInt64(9007199254740993LL))});
  members.append(
      {QStringLiteral("name"), Value::makeString(QStringLiteral("a\"b\\c"))});
  members.append(
      {QStringLiteral("nested"),
       Value::makeArray({Value::makeBool(true), Value::makeNull()})});
  const Value obj = Value::makeObject(members);
  auto bytes = obj.toJsonBytes();
  QVERIFY(bytes.has_value());
  auto reparsed = Value::parse(*bytes, u"test");
  QVERIFY(reparsed.has_value());
  QCOMPARE(*reparsed, obj);
  QCOMPARE(reparsed->value("id"_L1).toRawNumber().toExactInt64().value_or(-1),
           9007199254740993LL);
}

void RawJsonTests::toJsonBytesRejectsDuplicateObjectKeys() {
  QList<std::pair<QString, Value>> members{
      {QStringLiteral("a"), Value::makeNull()},
      {QStringLiteral("a"), Value::makeBool(true)},
  };
  const Value obj = Value::makeObject(members);
  auto bytes = obj.toJsonBytes();
  QVERIFY(!bytes.has_value());
}

void RawJsonTests::toJsonBytesRejectsUndefinedValue() {
  QVERIFY(!Value{}.toJsonBytes().has_value());
  QList<std::pair<QString, Value>> members{{QStringLiteral("a"), Value{}}};
  QVERIFY(!Value::makeObject(members).toJsonBytes().has_value());
}

void RawJsonTests::toJsonBytesEscapesInjectionAttemptsInStringsAndKeys() {
  // A malicious value/key attempting to inject an extra key or break out
  // of its string context must round-trip as a single string value/key,
  // never as raw unescaped bytes.
  QList<std::pair<QString, Value>> members{
      {QStringLiteral("evil\"key"),
       Value::makeString(QStringLiteral(R"(1,"evil":true)"))},
  };
  const Value obj = Value::makeObject(members);
  auto bytes = obj.toJsonBytes();
  QVERIFY(bytes.has_value());
  auto reparsed = Value::parse(*bytes, u"test");
  QVERIFY(reparsed.has_value());
  QVERIFY(reparsed->isObject());
  QCOMPARE(reparsed->keys().size(), 1);
  QCOMPARE(reparsed->value("evil\"key"_L1).toString(),
           QStringLiteral(R"(1,"evil":true)"));
}

void RawJsonTests::parseLimitsRejectsInputExceedingMaxInputBytes() {
  ParseLimits limits;
  limits.maxInputBytes = 4;
  QVERIFY(Value::parse("1234", u"test", limits).has_value());
  QVERIFY(!Value::parse("12345", u"test", limits).has_value());
}

void RawJsonTests::parseLimitsRejectsDepthExceedingMaxDepth() {
  ParseLimits limits;
  limits.maxDepth = 2;
  QVERIFY(Value::parse("[[1]]", u"test", limits).has_value());
  QVERIFY(!Value::parse("[[[1]]]", u"test", limits).has_value());
}

void RawJsonTests::parseLimitsRejectsStringExceedingMaxStringLength() {
  ParseLimits limits;
  limits.maxStringLength = 3;
  QVERIFY(Value::parse(R"("abc")", u"test", limits).has_value());
  QVERIFY(!Value::parse(R"("abcd")", u"test", limits).has_value());
}

void RawJsonTests::
    parseLimitsBoundsSurrogatePairAppendAgainstMaxStringLength() {
  // U+1F600 GRINNING FACE via a \ud83d\ude00 surrogate-pair escape decodes
  // to exactly 2 UTF-16 code units in one appending step. The bound must
  // be enforced as a genuine hard maximum -- never transiently exceeded --
  // rather than only being noticed one iteration after the pair has
  // already been appended (review round 5 finding: RawJson.cpp's
  // parseStringText() checked `out.size() > maxStringLength` once per
  // loop iteration, which let a single iteration that appends two code
  // units at once push `out` two units past the limit before the next
  // iteration's check caught it).
  ParseLimits exactLimit;
  exactLimit.maxStringLength = 2;
  QVERIFY(Value::parse(R"("\ud83d\ude00")", u"test", exactLimit).has_value());

  ParseLimits oneUnderLimit;
  oneUnderLimit.maxStringLength = 1;
  QVERIFY(
      !Value::parse(R"("\ud83d\ude00")", u"test", oneUnderLimit).has_value());

  // The same two-unit append preceded by one ordinary character must
  // still be bounded correctly: exactly at the limit succeeds, one over
  // fails.
  ParseLimits exactLimitWithPrefix;
  exactLimitWithPrefix.maxStringLength = 3;
  QVERIFY(Value::parse(R"("a\ud83d\ude00")", u"test", exactLimitWithPrefix)
              .has_value());

  ParseLimits oneOverLimitWithPrefix;
  oneOverLimitWithPrefix.maxStringLength = 2;
  QVERIFY(!Value::parse(R"("a\ud83d\ude00")", u"test", oneOverLimitWithPrefix)
               .has_value());
}

void RawJsonTests::
    parseLimitsBoundsRawUtf8AstralAppendAgainstMaxStringLength() {
  // Same U+1F600 codepoint, but as a raw UTF-8 byte sequence (decoded via
  // parseUtf8Sequence() and re-encoded as a surrogate pair), exercising
  // the second two-unit append site independently of the \u-escape path.
  const QByteArray exactLimitBytes = QByteArrayLiteral("\"\xF0\x9F\x98\x80\"");
  ParseLimits exactLimit;
  exactLimit.maxStringLength = 2;
  QVERIFY(Value::parse(exactLimitBytes, u"test", exactLimit).has_value());

  ParseLimits oneUnderLimit;
  oneUnderLimit.maxStringLength = 1;
  QVERIFY(!Value::parse(exactLimitBytes, u"test", oneUnderLimit).has_value());
}

void RawJsonTests::parseLimitsRejectsNumberExceedingMaxNumberDigits() {
  ParseLimits limits;
  limits.maxNumberDigits = 3;
  QVERIFY(Value::parse("123", u"test", limits).has_value());
  QVERIFY(!Value::parse("1234", u"test", limits).has_value());
}

void RawJsonTests::parseLimitsRejectsArrayExceedingMaxArrayElements() {
  ParseLimits limits;
  limits.maxArrayElements = 2;
  QVERIFY(Value::parse("[1,2]", u"test", limits).has_value());
  QVERIFY(!Value::parse("[1,2,3]", u"test", limits).has_value());
}

void RawJsonTests::parseLimitsRejectsObjectExceedingMaxObjectMembers() {
  ParseLimits limits;
  limits.maxObjectMembers = 1;
  QVERIFY(Value::parse(R"({"a":1})", u"test", limits).has_value());
  QVERIFY(!Value::parse(R"({"a":1,"b":2})", u"test", limits).has_value());
}

void RawJsonTests::parseLimitsRejectsTotalNodesExceedingMaxTotalNodes() {
  ParseLimits limits;
  limits.maxArrayElements = 100;
  limits.maxTotalNodes = 3;
  QVERIFY(Value::parse("[1,2]", u"test", limits).has_value());
  QVERIFY(!Value::parse("[1,2,3]", u"test", limits).has_value());
}

void RawJsonTests::
    productionLimitsAcceptDenseArrayAtBoundaryAndRejectOneOver() {
  // "Dense scalars": a single flat array of small numbers at production's
  // actual maxArrayElements boundary (20,000 -- see ParseLimits::
  // production()'s doc comment: ~3.4x headroom over the full pinned card
  // catalog's measured ~5,929-element top-level array).
  const ParseLimits limits = ParseLimits::production();
  auto buildFlatArray = [](qsizetype count) {
    QByteArray bytes = "[";
    for (qsizetype i = 0; i < count; ++i) {
      if (i > 0)
        bytes += ",";
      bytes += "0";
    }
    bytes += "]";
    return bytes;
  };
  const QByteArray atLimit = buildFlatArray(limits.maxArrayElements);
  const auto atLimitResult = Value::parse(atLimit, u"test", limits);
  if (!atLimitResult)
    QFAIL(qPrintable(atLimitResult.error()));
  QCOMPARE(atLimitResult->toArray().size(), limits.maxArrayElements);

  const QByteArray overLimit = buildFlatArray(limits.maxArrayElements + 1);
  QVERIFY(!Value::parse(overLimit, u"test", limits).has_value());
}

void RawJsonTests::
    productionLimitsAcceptWideObjectAtBoundaryAndRejectOneOver() {
  // "Wide objects": a single object at production's actual
  // maxObjectMembers boundary (1,024 -- ~14x headroom over the full
  // pinned card catalog's measured largest single object of 70 members).
  const ParseLimits limits = ParseLimits::production();
  auto buildWideObject = [](qsizetype count) {
    QByteArray bytes = "{";
    for (qsizetype i = 0; i < count; ++i) {
      if (i > 0)
        bytes += ",";
      bytes += "\"k" + QByteArray::number(i) + "\":0";
    }
    bytes += "}";
    return bytes;
  };
  const QByteArray atLimit = buildWideObject(limits.maxObjectMembers);
  const auto atLimitResult = Value::parse(atLimit, u"test", limits);
  if (!atLimitResult)
    QFAIL(qPrintable(atLimitResult.error()));
  QCOMPARE(atLimitResult->keys().size(), limits.maxObjectMembers);

  const QByteArray overLimit = buildWideObject(limits.maxObjectMembers + 1);
  QVERIFY(!Value::parse(overLimit, u"test", limits).has_value());
}

void RawJsonTests::
    productionLimitsAcceptTotalNodesAtBoundaryAndRejectOneOver() {
  // maxTotalNodes (400,000) is production()'s dominant memory bound (see
  // its doc comment: sizeof(Value) == 176 bytes, so this caps worst-case
  // `Value` storage at ~70MB, ~1.75x headroom over the full pinned card
  // catalog's measured ~228,000 nodes). Every individual array here stays
  // well under maxArrayElements (20,000) -- 453 outer elements, 882 inner
  // elements each -- so only maxTotalNodes is exercised: 1 (outer array)
  // + 453 (inner array nodes) + 453*882 (leaf number nodes) == 400,000
  // exactly.
  const ParseLimits limits = ParseLimits::production();
  constexpr qsizetype kOuterCount = 453;
  constexpr qsizetype kInnerCount = 882;
  QVERIFY(1 + kOuterCount + kOuterCount * kInnerCount == limits.maxTotalNodes);

  auto buildNested = [](qsizetype outerCount, qsizetype innerCount,
                        qsizetype extraElementsOnLastInner) {
    QByteArray bytes = "[";
    for (qsizetype o = 0; o < outerCount; ++o) {
      if (o > 0)
        bytes += ",";
      const qsizetype thisInnerCount =
          (o == outerCount - 1) ? innerCount + extraElementsOnLastInner
                                : innerCount;
      bytes += "[";
      for (qsizetype i = 0; i < thisInnerCount; ++i) {
        if (i > 0)
          bytes += ",";
        bytes += "0";
      }
      bytes += "]";
    }
    bytes += "]";
    return bytes;
  };

  const QByteArray atLimit = buildNested(kOuterCount, kInnerCount, 0);
  const auto atLimitResult = Value::parse(atLimit, u"test", limits);
  if (!atLimitResult)
    QFAIL(qPrintable(atLimitResult.error()));

  // Exactly one additional leaf node, appended only to the last inner
  // array, pushes the total to precisely maxTotalNodes + 1 -- proving the
  // off-by-one boundary exactly, not merely "some over-limit input fails".
  const QByteArray overLimit = buildNested(kOuterCount, kInnerCount, 1);
  QVERIFY(!Value::parse(overLimit, u"test", limits).has_value());
}

void RawJsonTests::toJsonBytesRejectsDepthExceedingLimitOnProgrammaticAst() {
  ParseLimits limits;
  limits.maxDepth = 1;
  const Value nested = Value::makeArray(
      {Value::makeArray({Value::makeNumber(RawNumber::fromInt64(1))})});
  QVERIFY(!nested.toJsonBytes(limits).has_value());
}

void RawJsonTests::toJsonBytesRejectsStringExceedingMaxStringLength() {
  ParseLimits limits;
  limits.maxStringLength = 3;
  QVERIFY(
      Value::makeString(QStringLiteral("abc")).toJsonBytes(limits).has_value());
  QVERIFY(!Value::makeString(QStringLiteral("abcd"))
               .toJsonBytes(limits)
               .has_value());
}

void RawJsonTests::toJsonBytesRejectsObjectKeyExceedingMaxStringLength() {
  ParseLimits limits;
  limits.maxStringLength = 3;
  QList<std::pair<QString, Value>> okMembers{
      {QStringLiteral("abc"), Value::makeNull()}};
  QVERIFY(Value::makeObject(okMembers).toJsonBytes(limits).has_value());
  QList<std::pair<QString, Value>> tooLongMembers{
      {QStringLiteral("abcd"), Value::makeNull()}};
  QVERIFY(!Value::makeObject(tooLongMembers).toJsonBytes(limits).has_value());
}

void RawJsonTests::toJsonBytesRejectsNumberExceedingMaxNumberDigits() {
  ParseLimits limits;
  limits.maxNumberDigits = 3;
  QVERIFY(Value::makeNumber(RawNumber::fromInt64(123))
              .toJsonBytes(limits)
              .has_value());
  QVERIFY(!Value::makeNumber(RawNumber::fromInt64(1234))
               .toJsonBytes(limits)
               .has_value());

  auto tooManyFractionDigits = Value::parse("1.1234", u"test");
  QVERIFY(tooManyFractionDigits.has_value());
  QVERIFY(!tooManyFractionDigits->toJsonBytes(limits).has_value());

  auto tooManyExponentDigits = Value::parse("1e1234", u"test");
  QVERIFY(tooManyExponentDigits.has_value());
  QVERIFY(!tooManyExponentDigits->toJsonBytes(limits).has_value());
}

void RawJsonTests::toJsonBytesRejectsArrayExceedingMaxArrayElements() {
  ParseLimits limits;
  limits.maxArrayElements = 2;
  QVERIFY(Value::makeArray({Value::makeNull(), Value::makeNull()})
              .toJsonBytes(limits)
              .has_value());
  QVERIFY(!Value::makeArray(
               {Value::makeNull(), Value::makeNull(), Value::makeNull()})
               .toJsonBytes(limits)
               .has_value());
}

void RawJsonTests::toJsonBytesRejectsObjectExceedingMaxObjectMembers() {
  ParseLimits limits;
  limits.maxObjectMembers = 1;
  QList<std::pair<QString, Value>> oneMember{
      {QStringLiteral("a"), Value::makeNull()}};
  QVERIFY(Value::makeObject(oneMember).toJsonBytes(limits).has_value());
  QList<std::pair<QString, Value>> twoMembers{
      {QStringLiteral("a"), Value::makeNull()},
      {QStringLiteral("b"), Value::makeNull()}};
  QVERIFY(!Value::makeObject(twoMembers).toJsonBytes(limits).has_value());
}

void RawJsonTests::toJsonBytesRejectsTotalNodesExceedingMaxTotalNodes() {
  ParseLimits limits;
  limits.maxArrayElements = 100;
  limits.maxTotalNodes = 3;
  const Value twoElements =
      Value::makeArray({Value::makeNull(), Value::makeNull()});
  QVERIFY(twoElements.toJsonBytes(limits).has_value());
  const Value threeElements = Value::makeArray(
      {Value::makeNull(), Value::makeNull(), Value::makeNull()});
  QVERIFY(!threeElements.toJsonBytes(limits).has_value());
}

void RawJsonTests::toJsonBytesAcceptsValuesExactlyAtEveryLimitBoundary() {
  // Every limit's exact boundary value must still serialize successfully;
  // only boundary+1 should fail (see the paired rejects* tests above),
  // proving toJsonBytesInner()'s checks use the same off-by-one semantics
  // as Parser's (`> limits.field`, not `>= limits.field`).
  ParseLimits limits;
  limits.maxDepth = 2;
  limits.maxStringLength = 3;
  limits.maxNumberDigits = 3;
  limits.maxArrayElements = 2;
  limits.maxObjectMembers = 2;
  limits.maxTotalNodes = 100;

  QList<std::pair<QString, Value>> members{
      {QStringLiteral("abc"), Value::makeString(QStringLiteral("abc"))},
      {QStringLiteral("num"), Value::makeNumber(RawNumber::fromInt64(123))},
  };
  const Value obj =
      Value::makeArray({Value::makeObject(members), Value::makeNull()});
  auto bytes = obj.toJsonBytes(limits);
  QVERIFY(bytes.has_value());
  auto reparsed = Value::parse(*bytes, u"test");
  QVERIFY(reparsed.has_value());
  QCOMPARE(*reparsed, obj);
}

void RawJsonTests::toJsonBytesRejectsLoneHighSurrogateInStringValue() {
  // U+D800 is a high surrogate with no following low surrogate: naively
  // UTF-8-"encoding" it (as CESU-8-style code does) produces a byte
  // sequence RFC 3629/RFC 8259 both forbid. toJsonBytes() must fail with
  // a typed error rather than emit it.
  QString lone;
  lone += QChar(0xD800);
  const Value value = Value::makeString(lone);
  const auto result = value.toJsonBytes();
  QVERIFY(!result.has_value());
}

void RawJsonTests::toJsonBytesRejectsLoneLowSurrogateInStringValue() {
  QString lone;
  lone += QChar(0xDC00);
  const Value value = Value::makeString(lone);
  const auto result = value.toJsonBytes();
  QVERIFY(!result.has_value());
}

void RawJsonTests::toJsonBytesRejectsLoneSurrogateInObjectKey() {
  QString lone;
  lone += QChar(0xD800);
  QList<std::pair<QString, Value>> members{{lone, Value::makeNull()}};
  const Value obj = Value::makeObject(members);
  const auto result = obj.toJsonBytes();
  QVERIFY(!result.has_value());
}

void RawJsonTests::toJsonBytesRejectsLoneSurrogateNestedInsideArray() {
  // The lone surrogate must be caught no matter how deeply it is nested,
  // not only when it is the top-level value.
  QString lone;
  lone += QChar(0xDC00);
  const Value nested = Value::makeArray(
      {Value::makeObject({{QStringLiteral("deep"), Value::makeString(lone)}})});
  const auto result = nested.toJsonBytes();
  QVERIFY(!result.has_value());
}

void RawJsonTests::toJsonBytesAcceptsValidSurrogatePairInStringAndKey() {
  // A genuine astral-plane character (U+1F600, a valid high+low surrogate
  // pair) must still encode successfully and round-trip byte-exact,
  // proving the lone-surrogate check above does not over-reject valid
  // input.
  QString astral;
  astral += QChar::highSurrogate(0x1F600);
  astral += QChar::lowSurrogate(0x1F600);
  QList<std::pair<QString, Value>> members{{astral, Value::makeString(astral)}};
  const Value obj = Value::makeObject(members);
  auto bytes = obj.toJsonBytes();
  QVERIFY(bytes.has_value());
  auto reparsed = Value::parse(*bytes, u"test");
  QVERIFY(reparsed.has_value());
  QCOMPARE(*reparsed, obj);
}

void RawJsonTests::parseAcceptsEverySerializerSuccess() {
  // Every value toJsonBytes() successfully serializes must be accepted by
  // parse() and reproduce an identical tree -- proving the serializer and
  // parser agree on what counts as valid JSON, not merely that each
  // rejects its own known-bad inputs in isolation.
  const QList<Value> values{
      Value::makeNull(),
      Value::makeBool(true),
      Value::makeBool(false),
      Value::makeNumber(RawNumber::fromInt64(-9223372036854775807LL - 1)),
      Value::makeNumber(RawNumber::fromInt64(9223372036854775807LL)),
      Value::makeString(QStringLiteral("plain")),
      Value::makeString(QStringLiteral("a\"b\\c\n\t\x01")),
      Value::makeArray({Value::makeNull(), Value::makeBool(false)}),
      Value::makeObject(
          {{QStringLiteral("k"), Value::makeString(QStringLiteral("v"))}}),
  };
  for (const auto &value : values) {
    auto bytes = value.toJsonBytes();
    QVERIFY(bytes.has_value());
    auto reparsed = Value::parse(*bytes, u"test");
    if (!reparsed)
      QFAIL(qPrintable(reparsed.error()));
    QCOMPARE(*reparsed, value);
  }
}

QTEST_APPLESS_MAIN(RawJsonTests)

#include "RawJsonTests.moc"
