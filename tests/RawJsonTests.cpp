#include <QtTest>

#include "RawJson.h"

using Arkham::Failure;
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

QTEST_APPLESS_MAIN(RawJsonTests)

#include "RawJsonTests.moc"
