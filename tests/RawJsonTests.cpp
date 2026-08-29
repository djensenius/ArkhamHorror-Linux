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
  void toQJsonConvertsEveryKind();
  void skipsLeadingAndTrailingWhitespace();

  // toExactInt64()/fromInt64()/qint64-exactness (issue #19 review round 3,
  // item 4: contract integers must be exact qint64, never rounded through
  // a double).
  void toExactInt64AcceptsIntegralDecimalAndExponentForms();
  void toExactInt64RejectsGenuinelyFractionalValues();
  void toExactInt64HandlesQint64BoundariesExactly();
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
  void parseLimitsRejectsNumberExceedingMaxNumberDigits();
  void parseLimitsRejectsArrayExceedingMaxArrayElements();
  void parseLimitsRejectsObjectExceedingMaxObjectMembers();
  void parseLimitsRejectsTotalNodesExceedingMaxTotalNodes();
  void toJsonBytesRejectsDepthExceedingLimitOnProgrammaticAst();

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

QTEST_APPLESS_MAIN(RawJsonTests)

#include "RawJsonTests.moc"
