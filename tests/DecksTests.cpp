#include <QFile>
#include <QJsonDocument>
#include <QtTest>
#include <array>
#include <cmath>
#include <limits>

#include "Decks.h"
#include "JsonDecode.h"
#include "RawJson.h"

using namespace Arkham;
using namespace Qt::StringLiterals;

class DecksTests final : public QObject {
  Q_OBJECT

private slots:
  // Fixture round trips ─────────────────────────────────────────────────────
  void decodesCreateDeckRequestFromFixture();
  void decodesFetchDeckRequestFromFixture();
  void decodesValidateDeckListInputFromFixture();
  void decodesNormalizedDeckListFromFixture();
  void decodesDeckFromFixture();
  void decodesValidationErrorsFromFixture();
  void decodesValidationSuccessFromFixture();
  void decodesOperationErrorFromFixture();

  // ExternalDeckId: string/integer/decimal/null preservation ───────────────
  void externalIdAbsentPreserved();
  void externalIdExplicitNullPreserved();
  void externalIdStringPreserved();
  void externalIdLargeIntegerPreservedWithoutPrecisionLoss();
  void externalIdDecimalPreserved();
  void externalIdWrongTypeRejected();

  // ExternalDeckId via the raw-byte parser: genuine precision preservation
  // (HIGH #3) -- no double-rounding at all, unlike the QJsonValue-based
  // fromJson()/fromObject() path above, which can only preserve whatever
  // precision Qt's own parser already left intact.
  void externalIdFromRawBytesPreservesLargeIntegerExactly();
  void externalIdFromRawBytesPreservesLongDecimalExactly();
  void externalIdFromRawBytesPreservesHugeExponentExactly();
  void externalIdFromRawBytesRoundTripsThroughToJsonBytesExactly();
  void externalIdFromRawBytesRejectsMalformedNumberLiteral();
  void toJsonBytesEscapesInjectionAttemptInStringValue();
  void rawJsonObjectBuilderEscapesQuotesAndBackslashesInKey();
  void rawJsonObjectBuilderEscapesControlCharactersAndUtf8InKey();
  void createDeckRequestFromRawBytesPreservesEveryIdVariantExactly();

  // sideSlots malformed/absent behavior ──────────────────────────────────────
  void sideSlotsAbsentStaysUndefinedNotEmptyMap();
  void sideSlotsMalformedArrayPreservedVerbatim();
  void sideSlotsAlreadyNormalizedMapDistinguishableFromMalformed();
  void sideSlotsNormalizedLookingMalformedObjectPreservedVerbatim();

  // Quantity map keys ────────────────────────────────────────────────────────
  void inputQuantityMapAcceptsAnyNonEmptyKey();
  void inputQuantityMapRejectsEmptyKey();
  void normalizedQuantityMapRequiresCardCodeKeys();
  void normalizedQuantityMapRejectsMalformedCardCodeKey();

  // Nullable metadata/url/taboo fields ────────────────────────────────────────
  void nullableFieldsRoundTripAsExplicitNull();
  void normalizedDeckListMissingRequiredKeyRejected();

  // DeckValidationError / empty success ─────────────────────────────────────
  void unimplementedCardTagRequired();
  void emptyArrayIsValidationSuccess();
  void deckValidationResultErrorsFactoryRejectsEmptyList();
  void deckValidationResultSuccessAndErrorsAreDistinctKinds();

  // scientificShow (backend Aeson Scientific Show semantics) ────────────────
  void scientificShowMatchesKnownBackendFormatting();
  void scientificShowRejectsNonFiniteRatherThanCrashing();
  void externalIdOverflowingToInfinityRejectedNotCrashed();

  // Wrong types / malformed ──────────────────────────────────────────────────
  void wrongTypeForInvestigatorCodeRejected();
  void deckListInputMissingInvestigatorCodeRejected();
};

namespace {

QJsonObject loadFixtureObject(const QString &fileName) {
  QFile f(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + u"/fixtures/" + fileName);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return QJsonDocument::fromJson(f.readAll()).object();
}

QJsonObject decksFixture() {
  return loadFixtureObject(QStringLiteral("decks.json"));
}

// Returns a copy of `obj` with top-level key `key` removed -- used to build
// the "expected re-encode" shape for fixture entries that deliberately carry
// an additive field this client does not model (and therefore correctly
// drops on re-encode).
QJsonObject withoutKey(QJsonObject obj, QLatin1StringView key) {
  obj.remove(key);
  return obj;
}

Json::Value objectMember(const Json::Value &obj, const QString &key) {
  if (!obj.isObject())
    return {};
  for (const auto &[memberKey, memberValue] : obj.members()) {
    if (memberKey == key)
      return memberValue;
  }
  return {};
}

} // namespace

void DecksTests::decodesCreateDeckRequestFromFixture() {
  const QJsonObject fixture = decksFixture();
  QVERIFY(!fixture.isEmpty());
  const QJsonValue createDeck = fixture.value("createDeck"_L1);

  const auto result = CreateDeckRequest::fromJson(createDeck, u"createDeck");
  if (!result)
    QFAIL(qPrintable(result.error()));

  QCOMPARE(result->deckId, QStringLiteral("external-4242"));
  QCOMPARE(result->deckName, QStringLiteral("Contract deck"));
  QCOMPARE(*result->deckUrl,
           QStringLiteral("https://arkhamdb.com/decklist/view/4242"));
  QCOMPARE(result->deckList.cardSlots.size(), 2);
  QCOMPARE(result->deckList.cardSlots.value(QStringLiteral("01016")), 2);
  QCOMPARE(result->deckList.investigatorCode.value(), QStringLiteral("01001"));
  QCOMPARE(result->deckList.id.kind(), ExternalDeckId::Kind::Number);
  // Decoded via fromJson(), so this is only as precise as Qt's parsed
  // QJsonValue -- but for a plain integral value like 4242, the new
  // ExternalDeckId path reconstructs the canonical bare-integer literal
  // instead of the old lossy "4242.0" scientificShow text. See
  // createDeckRequestFromRawBytesPreservesEveryIdVariantExactly() below
  // for the genuinely byte-exact raw parser entry point.
  QCOMPARE(result->deckList.id.number().literal(), QStringLiteral("4242"));
  // sideSlots is `[]` on the wire -- a malformed/legacy shape, preserved
  // verbatim rather than coerced into an already-normalized empty map.
  QVERIFY(result->deckList.sideSlots.isArray());
  QVERIFY(result->deckList.sideSlots.toArray().isEmpty());

  // The fixture's nested deckList deliberately carries an additive
  // "externalField" this client does not model; re-encoding correctly drops
  // it. "taboo_id" is present as an explicit JSON null in the fixture, but
  // DeckListInput -- like its other optional fields -- collapses an absent
  // key and an explicit null identically, and toJson() omits the key
  // entirely once unset; so the expected shape strips both before an
  // otherwise byte-exact comparison.
  QJsonObject expected = createDeck.toObject();
  QJsonObject expectedDeckList = expected.value("deckList"_L1).toObject();
  expectedDeckList = withoutKey(expectedDeckList, "externalField"_L1);
  expectedDeckList = withoutKey(expectedDeckList, "taboo_id"_L1);
  expected.insert(QStringLiteral("deckList"), expectedDeckList);
  QCOMPARE(result->toJson(), expected);
}

void DecksTests::decodesFetchDeckRequestFromFixture() {
  const QJsonObject fixture = decksFixture();
  const auto result =
      FetchDeckRequest::fromJson(fixture.value("fetchDeck"_L1), u"fetchDeck");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->url,
           QStringLiteral("https://arkhamdb.com/decklist/view/4242"));
  QCOMPARE(result->toJson(), fixture.value("fetchDeck"_L1).toObject());
}

void DecksTests::decodesValidateDeckListInputFromFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("validateDeckList"_L1);
  const auto result = DeckListInput::fromJson(v, u"validateDeckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->id.kind(), ExternalDeckId::Kind::Number);
  QCOMPARE(result->id.number().literal(), QStringLiteral("4242"));
  // "externalField" is present in the fixture but not modeled -- must decode
  // safely and never reappear on re-encode. "taboo_id" is present as an
  // explicit JSON null in the fixture; DeckListInput collapses absent/null
  // to unset and toJson() omits the key once unset, so it too must be
  // stripped from the expected shape.
  QVERIFY(!result->toJson().contains(QStringLiteral("externalField")));
  QJsonObject expected = withoutKey(v.toObject(), "externalField"_L1);
  expected = withoutKey(expected, "taboo_id"_L1);
  QCOMPARE(result->toJson(), expected);
}

void DecksTests::decodesNormalizedDeckListFromFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("normalizedDeckList"_L1);
  const auto result = DeckList::fromJson(v, u"normalizedDeckList");
  if (!result)
    QFAIL(qPrintable(result.error()));

  QCOMPARE(result->cardSlots.size(), 2);
  QVERIFY(result->cardSlots.contains(*CardCode::parse(u"c01016"_s)));
  QCOMPARE(result->sideSlots.size(), 0);
  QCOMPARE(result->investigatorCode.value(), QStringLiteral("c01001"));
  QCOMPARE(result->investigatorName, QStringLiteral("Roland Banks"));
  QVERIFY(!result->tabooId.has_value());
  QCOMPARE(*result->id, QStringLiteral("4242.0"));
  QCOMPARE(*result->name, QStringLiteral("Contract deck"));

  QCOMPARE(result->toJson(), v.toObject());
}

void DecksTests::decodesDeckFromFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("deck"_L1);
  const auto result = Deck::fromJson(v, u"deck");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->id.value(),
           QStringLiteral("00000000-0000-0000-0000-000000000017"));
  QCOMPARE(result->userId, qint64(7));
  QCOMPARE(result->name, QStringLiteral("Contract deck"));
  QCOMPARE(result->investigatorName, QStringLiteral("Roland Banks"));
  QCOMPARE(result->list.investigatorCode.value(), QStringLiteral("c01001"));

  QCOMPARE(result->toJson(), v.toObject());
}

void DecksTests::decodesValidationErrorsFromFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("validationErrors"_L1);
  const auto result = DeckValidationResult::fromJson(v, u"validationErrors");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->kind(), DeckValidationResult::Kind::Errors);
  QVERIFY(!result->isSuccess());
  QCOMPARE(result->errorList().size(), 1);
  QCOMPARE(result->errorList().at(0).cardCode.value(),
           QStringLiteral("c99999"));
  QCOMPARE(result->toJson(), v.toArray());
}

void DecksTests::decodesValidationSuccessFromFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("validationSuccess"_L1);
  const auto result = DeckValidationResult::fromJson(v, u"validationSuccess");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->kind(), DeckValidationResult::Kind::Success);
  QVERIFY(result->isSuccess());
  QVERIFY(result->errorList().isEmpty());
  QCOMPARE(result->toJson(), v.toArray());
}

void DecksTests::decodesOperationErrorFromFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("operationError"_L1);
  const auto result = DeckOperationError::fromJson(v, u"operationError");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->errorMsg, QStringLiteral("Could not sync deck"));
  QCOMPARE(result->toJson(), v.toObject());
}

void DecksTests::externalIdAbsentPreserved() {
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->id.kind(), ExternalDeckId::Kind::Absent);
  // Omitted on re-encode -- never fabricates a null/zero id.
  QVERIFY(!result->toJson().contains(QStringLiteral("id")));
}

void DecksTests::externalIdExplicitNullPreserved() {
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
      {QStringLiteral("id"), QJsonValue()},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->id.kind(), ExternalDeckId::Kind::Null);
  QVERIFY(result->toJson().value(QStringLiteral("id")).isNull());
}

void DecksTests::externalIdStringPreserved() {
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
      {QStringLiteral("id"), QStringLiteral("external-9999")},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->id.kind(), ExternalDeckId::Kind::Text);
  QCOMPARE(result->id.text(), QStringLiteral("external-9999"));
  QCOMPARE(result->toJson().value(QStringLiteral("id")).toString(),
           QStringLiteral("external-9999"));
}

void DecksTests::externalIdLargeIntegerPreservedWithoutPrecisionLoss() {
  // fromJson() can preserve only the already-parsed QJsonValue it is given.
  // For an integral double like 2^53, the new path reconstructs the
  // canonical integer literal from that QJsonValue rather than formatting a
  // lossy "scientificShow" string with a forced trailing ".0".
  const double large = 9007199254740992.0; // 2^53, exactly representable.
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
      {QStringLiteral("id"), large},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->id.kind(), ExternalDeckId::Kind::Number);
  QCOMPARE(result->id.number().literal(), QStringLiteral("9007199254740992"));
  const auto exact = result->id.number().toExactInt64();
  QVERIFY(exact.has_value());
  QCOMPARE(*exact, qint64(9007199254740992LL));
  QCOMPARE(result->toJson().value(QStringLiteral("id")).toDouble(), large);
}

void DecksTests::externalIdDecimalPreserved() {
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
      {QStringLiteral("id"), 42.5},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->id.kind(), ExternalDeckId::Kind::Number);
  QCOMPARE(result->id.number().literal(), QStringLiteral("42.5"));
  QVERIFY(!result->id.number().toExactInt64().has_value());
  QCOMPARE(result->toJson().value(QStringLiteral("id")).toDouble(), 42.5);
}

void DecksTests::externalIdWrongTypeRejected() {
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
      {QStringLiteral("id"), QJsonArray{1, 2, 3}},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("id")),
           qPrintable(result.error()));
}

void DecksTests::externalIdFromRawBytesPreservesLargeIntegerExactly() {
  // 9007199254740993 == 2^53 + 1, the smallest positive integer a double
  // cannot represent exactly -- ArkhamDB is known to hand out deck ids in
  // this range. fromJson()/fromObject() cannot recover this precision
  // (QJsonDocument has already rounded it through a double by the time
  // either sees it); fromRawBytes() parses the original bytes through the
  // canonical RawJson parser (see RawJson.h) instead, so the literal
  // survives untouched. The literal bytes below are parsed directly --
  // never constructed via a double -- per the issue's requirement that
  // these tests prove genuine byte-level fidelity.
  const auto result = DeckListInput::fromRawBytes(
      R"({"slots":{},"investigator_code":"01001","id":9007199254740993})",
      u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->id.kind(), ExternalDeckId::Kind::Number);
  QCOMPARE(result->id.number().literal(), QStringLiteral("9007199254740993"));
  const auto exact = result->id.number().toExactInt64();
  QVERIFY(exact.has_value());
  QCOMPARE(*exact, qint64(9007199254740993LL));
}

void DecksTests::externalIdFromRawBytesPreservesLongDecimalExactly() {
  const auto result =
      DeckListInput::fromRawBytes(R"({"slots":{},"investigator_code":"01001",)"
                                  R"("id":1.123456789012345678901234567890})",
                                  u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->id.kind(), ExternalDeckId::Kind::Number);
  QCOMPARE(result->id.number().literal(),
           QStringLiteral("1.123456789012345678901234567890"));
  QVERIFY(!result->id.number().toExactInt64().has_value());
}

void DecksTests::externalIdFromRawBytesPreservesHugeExponentExactly() {
  const auto result = DeckListInput::fromRawBytes(
      R"({"slots":{},"investigator_code":"01001","id":1e128})", u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->id.kind(), ExternalDeckId::Kind::Number);
  QCOMPARE(result->id.number().literal(), QStringLiteral("1e128"));
  QVERIFY(!result->id.number().toExactInt64().has_value());
}

void DecksTests::externalIdFromRawBytesRoundTripsThroughToJsonBytesExactly() {
  const auto result = DeckListInput::fromRawBytes(
      R"({"slots":{},"investigator_code":"01001","id":9007199254740993})",
      u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  const auto reencoded = result->toJsonBytes();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  // Byte-level splice must have written the exact literal, not a
  // double-rounded approximation -- re-parsing it through the same
  // canonical parser must recover the identical literal.
  auto reparsed = Json::Value::parse(*reencoded, u"reencoded");
  if (!reparsed)
    QFAIL(qPrintable(reparsed.error()));
  QVERIFY(reparsed->isObject());
  QVERIFY(reparsed->value("id"_L1).isNumber());
  QCOMPARE(reparsed->value("id"_L1).toRawNumber().literal(),
           QStringLiteral("9007199254740993"));
}

void DecksTests::externalIdFromRawBytesRejectsMalformedNumberLiteral() {
  // A malformed number literal can never become a Number-kind ExternalDeckId:
  // the raw parser rejects it before DeckListInput is constructed, with no
  // fallback to 0, stringification, or any other substitute value.
  const auto result = DeckListInput::fromRawBytes(
      R"({"slots":{},"investigator_code":"01001","id":1.})", u"deckList");
  QVERIFY(!result.has_value());
  QVERIFY2(
      result.error().contains(QStringLiteral("expected a digit after '.'")),
      qPrintable(result.error()));
}

void DecksTests::toJsonBytesEscapesInjectionAttemptInStringValue() {
  const QString maliciousDeckName =
      QStringLiteral(R"(Contract deck","evil":true,"x)");
  const QString maliciousListName =
      QStringLiteral(R"(Nested deck","evil":true,"x)");

  CreateDeckRequest input{
      .deckId = QStringLiteral("external-4242"),
      .deckName = maliciousDeckName,
      .deckList =
          DeckListInput{
              .investigatorCode =
                  *InvestigatorRef::parse(QStringLiteral("01001")),
              .name = maliciousListName,
          },
  };
  const auto reencoded = input.toJsonBytes();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  auto reparsed = Json::Value::parse(*reencoded, u"reencoded");
  if (!reparsed)
    QFAIL(qPrintable(QStringLiteral("toJsonBytes() produced invalid JSON: %1")
                         .arg(reparsed.error())));
  QVERIFY(reparsed->isObject());
  QCOMPARE(reparsed->value("deckName"_L1).toString(), maliciousDeckName);
  QVERIFY(!reparsed->contains("evil"_L1));

  const Json::Value deckList = reparsed->value("deckList"_L1);
  QVERIFY(deckList.isObject());
  QCOMPARE(deckList.value("name"_L1).toString(), maliciousListName);
  QVERIFY(!deckList.contains("evil"_L1));
}

void DecksTests::rawJsonObjectBuilderEscapesQuotesAndBackslashesInKey() {
  const QString maliciousKey = QStringLiteral(R"("evil":true,"x\y)");
  const Json::Value obj = Json::Value::makeObject(
      {{QStringLiteral("existing"),
        Json::Value::makeNumber(Json::RawNumber::fromInt64(1))},
       {maliciousKey,
        Json::Value::makeNumber(Json::RawNumber::fromInt64(42))}});
  const auto bytes = obj.toJsonBytes();
  if (!bytes)
    QFAIL(qPrintable(bytes.error()));
  auto reparsed = Json::Value::parse(*bytes, u"reencoded");
  if (!reparsed)
    QFAIL(qPrintable(QStringLiteral("Json::Value::toJsonBytes() produced "
                                    "invalid JSON: %1")
                         .arg(reparsed.error())));
  QVERIFY(reparsed->isObject());
  QCOMPARE(reparsed->keys().size(), 2);
  QVERIFY(!reparsed->contains("evil"_L1));
  QCOMPARE(objectMember(*reparsed, maliciousKey).toRawNumber().literal(),
           QStringLiteral("42"));
}

void DecksTests::rawJsonObjectBuilderEscapesControlCharactersAndUtf8InKey() {
  const QString key = QStringLiteral("line\nbreak☃");
  const Json::Value obj = Json::Value::makeObject(
      {{key, Json::Value::makeNumber(Json::RawNumber::fromInt64(1))}});
  const auto bytes = obj.toJsonBytes();
  if (!bytes)
    QFAIL(qPrintable(bytes.error()));
  auto reparsed = Json::Value::parse(*bytes, u"reencoded");
  if (!reparsed)
    QFAIL(qPrintable(QStringLiteral("Json::Value::toJsonBytes() produced "
                                    "invalid JSON: %1")
                         .arg(reparsed.error())));
  QVERIFY(reparsed->isObject());
  QCOMPARE(reparsed->keys(), QStringList{key});
  QCOMPARE(objectMember(*reparsed, key).toRawNumber().literal(),
           QStringLiteral("1"));
  // The raw control byte must not appear unescaped in the output bytes.
  QVERIFY(!bytes->contains('\n'));
}

void DecksTests::createDeckRequestFromRawBytesPreservesEveryIdVariantExactly() {
  struct Case {
    QByteArray bytes;
    ExternalDeckId::Kind kind;
    QString expectedNumberLiteral;
    QString expectedText;
  };
  const std::array<Case, 5> cases{
      Case{R"({"deckId":"d","deckName":"n","deckList":{"slots":{},)"
           R"("investigator_code":"01001","id":9007199254740993}})",
           ExternalDeckId::Kind::Number,
           QStringLiteral("9007199254740993"),
           {}},
      Case{R"({"deckId":"d","deckName":"n","deckList":{"slots":{},)"
           R"("investigator_code":"01001","id":42.5}})",
           ExternalDeckId::Kind::Number,
           QStringLiteral("42.5"),
           {}},
      Case{R"({"deckId":"d","deckName":"n","deckList":{"slots":{},)"
           R"("investigator_code":"01001","id":"external-9999"}})",
           ExternalDeckId::Kind::Text,
           {},
           QStringLiteral("external-9999")},
      Case{R"({"deckId":"d","deckName":"n","deckList":{"slots":{},)"
           R"("investigator_code":"01001","id":null}})",
           ExternalDeckId::Kind::Null,
           {},
           {}},
      Case{R"({"deckId":"d","deckName":"n","deckList":{"slots":{},)"
           R"("investigator_code":"01001"}})",
           ExternalDeckId::Kind::Absent,
           {},
           {}},
  };
  for (const Case &c : cases) {
    const auto result = CreateDeckRequest::fromRawBytes(c.bytes, u"createDeck");
    if (!result)
      QFAIL(qPrintable(result.error()));
    QCOMPARE(result->deckList.id.kind(), c.kind);
    if (c.kind == ExternalDeckId::Kind::Number) {
      QCOMPARE(result->deckList.id.number().literal(), c.expectedNumberLiteral);
      if (const auto exact = result->deckList.id.number().toExactInt64();
          exact.has_value()) {
        QCOMPARE(*exact, qint64(9007199254740993LL));
      }
    }
    if (c.kind == ExternalDeckId::Kind::Text)
      QCOMPARE(result->deckList.id.text(), c.expectedText);

    const auto reencoded = result->toJsonBytes();
    if (!reencoded)
      QFAIL(qPrintable(reencoded.error()));
    auto reparsed = Json::Value::parse(*reencoded, u"reencoded");
    if (!reparsed)
      QFAIL(qPrintable(reparsed.error()));
    const Json::Value deckList = reparsed->value("deckList"_L1);
    QVERIFY(deckList.isObject());
    if (c.kind == ExternalDeckId::Kind::Absent) {
      QVERIFY(!deckList.contains("id"_L1));
    } else if (c.kind == ExternalDeckId::Kind::Null) {
      QVERIFY(deckList.value("id"_L1).isNull());
    } else if (c.kind == ExternalDeckId::Kind::Text) {
      QCOMPARE(deckList.value("id"_L1).toString(), c.expectedText);
    } else {
      QCOMPARE(deckList.value("id"_L1).toRawNumber().literal(),
               c.expectedNumberLiteral);
    }
  }
}

void DecksTests::sideSlotsAbsentStaysUndefinedNotEmptyMap() {
  const auto result = DeckListInput::fromRawBytes(
      R"({"slots":{},"investigator_code":"01001"})", u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->sideSlots.isUndefined());
  QVERIFY(!result->toJson().contains(QStringLiteral("sideSlots")));
  const auto reencoded = result->toJsonBytes();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  auto reparsed = Json::Value::parse(*reencoded, u"reencoded");
  if (!reparsed)
    QFAIL(qPrintable(reparsed.error()));
  QVERIFY(!reparsed->contains("sideSlots"_L1));
}

void DecksTests::sideSlotsMalformedArrayPreservedVerbatim() {
  // The fixture's own createDeck.deckList.sideSlots is `[]` -- a legacy
  // array shape, not an already-normalized map. Preserving it verbatim
  // (rather than coercing to {}) means a caller can tell the two apart.
  const auto result = DeckListInput::fromRawBytes(
      R"({"slots":{},"investigator_code":"01001","sideSlots":[]})",
      u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->sideSlots.isArray());
  QVERIFY(!result->sideSlots.isObject());
  QCOMPARE(result->sideSlots.toArray().size(), 0);
}

void DecksTests::sideSlotsAlreadyNormalizedMapDistinguishableFromMalformed() {
  const auto result = DeckListInput::fromRawBytes(
      R"({"slots":{},"investigator_code":"01001","sideSlots":{"c00001":2}})",
      u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->sideSlots.isObject());
  QVERIFY(!result->sideSlots.isArray());
  const Json::Value entry = result->sideSlots.value("c00001"_L1);
  QVERIFY(entry.isNumber());
  const auto quantity = entry.toRawNumber().toExactInt64();
  QVERIFY(quantity.has_value());
  QCOMPARE(*quantity, qint64(2));
}

void DecksTests::sideSlotsNormalizedLookingMalformedObjectPreservedVerbatim() {
  // Keys that *look* normalized (valid CardCode text) still must not trigger
  // silent coercion if the values are not the normalized quantity-map shape.
  const auto result = DeckListInput::fromRawBytes(
      R"({"slots":{},"investigator_code":"01001","sideSlots":{"c00001":[9007199254740993]}})",
      u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->sideSlots.isObject());
  const Json::Value nested = result->sideSlots.value("c00001"_L1);
  QVERIFY(nested.isArray());
  QCOMPARE(nested.toArray().size(), 1);
  QVERIFY(nested.toArray().at(0).isNumber());
  QCOMPARE(nested.toArray().at(0).toRawNumber().literal(),
           QStringLiteral("9007199254740993"));

  const auto reencoded = result->toJsonBytes();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  auto reparsed = Json::Value::parse(*reencoded, u"reencoded");
  if (!reparsed)
    QFAIL(qPrintable(reparsed.error()));
  const Json::Value sideSlots = reparsed->value("sideSlots"_L1);
  QVERIFY(sideSlots.isObject());
  QVERIFY(sideSlots.value("c00001"_L1).isArray());
  QCOMPARE(sideSlots.value("c00001"_L1).toArray().at(0).toRawNumber().literal(),
           QStringLiteral("9007199254740993"));
}

void DecksTests::inputQuantityMapAcceptsAnyNonEmptyKey() {
  // DeckListInput.slots keys need only be non-empty -- not CardCode-shaped
  // (unlike the normalized DeckList).
  const QJsonObject obj{
      {QStringLiteral("slots"),
       QJsonObject{{QStringLiteral("not-a-card-code-at-all"), 3},
                   {QStringLiteral("01016"), 2}}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->cardSlots.value(QStringLiteral("not-a-card-code-at-all")),
           3);
}

void DecksTests::inputQuantityMapRejectsEmptyKey() {
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{{QStringLiteral(""), 1}}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("empty")),
           qPrintable(result.error()));
}

void DecksTests::normalizedQuantityMapRequiresCardCodeKeys() {
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{{QStringLiteral("c01016"), 2}}},
      {QStringLiteral("sideSlots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("c01001")},
      {QStringLiteral("investigator_name"), QStringLiteral("Roland Banks")},
      {QStringLiteral("meta"), QJsonValue()},
      {QStringLiteral("taboo_id"), QJsonValue()},
      {QStringLiteral("url"), QJsonValue()},
      {QStringLiteral("id"), QJsonValue()},
      {QStringLiteral("name"), QJsonValue()},
  };
  const auto result = DeckList::fromJson(obj, u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->cardSlots.contains(*CardCode::parse(u"c01016"_s)));
}

void DecksTests::normalizedQuantityMapRejectsMalformedCardCodeKey() {
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{{QStringLiteral("01016"), 2}}},
      {QStringLiteral("sideSlots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("c01001")},
      {QStringLiteral("investigator_name"), QStringLiteral("Roland Banks")},
      {QStringLiteral("meta"), QJsonValue()},
      {QStringLiteral("taboo_id"), QJsonValue()},
      {QStringLiteral("url"), QJsonValue()},
      {QStringLiteral("id"), QJsonValue()},
      {QStringLiteral("name"), QJsonValue()},
  };
  const auto result = DeckList::fromJson(obj, u"deckList");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("slots")),
           qPrintable(result.error()));
}

void DecksTests::nullableFieldsRoundTripAsExplicitNull() {
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("sideSlots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("c01001")},
      {QStringLiteral("investigator_name"), QStringLiteral("Roland Banks")},
      {QStringLiteral("meta"), QJsonValue()},
      {QStringLiteral("taboo_id"), QJsonValue()},
      {QStringLiteral("url"), QJsonValue()},
      {QStringLiteral("id"), QJsonValue()},
      {QStringLiteral("name"), QJsonValue()},
  };
  const auto result = DeckList::fromJson(obj, u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(!result->meta.has_value());
  QVERIFY(!result->tabooId.has_value());
  QVERIFY(!result->url.has_value());
  QVERIFY(!result->id.has_value());
  QVERIFY(!result->name.has_value());
  QCOMPARE(result->toJson(), obj);
}

void DecksTests::normalizedDeckListMissingRequiredKeyRejected() {
  // Unlike DeckListInput, every DeckList key is required -- absent (not
  // merely null) must fail.
  QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("sideSlots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("c01001")},
      {QStringLiteral("investigator_name"), QStringLiteral("Roland Banks")},
      {QStringLiteral("meta"), QJsonValue()},
      {QStringLiteral("taboo_id"), QJsonValue()},
      {QStringLiteral("url"), QJsonValue()},
      {QStringLiteral("id"), QJsonValue()},
      // "name" key entirely absent.
  };
  const auto result = DeckList::fromJson(obj, u"deckList");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("name")),
           qPrintable(result.error()));
}

void DecksTests::unimplementedCardTagRequired() {
  const QJsonObject obj{{QStringLiteral("tag"), QStringLiteral("SomeOtherTag")},
                        {QStringLiteral("contents"), QStringLiteral("c00001")}};
  const auto result = DeckValidationError::fromJson(obj, u"error");
  QVERIFY(!result.has_value());
}

void DecksTests::emptyArrayIsValidationSuccess() {
  const auto result =
      DeckValidationResult::fromJson(QJsonArray{}, u"validationResult");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->isSuccess());
  QVERIFY(result->errorList().isEmpty());
}

void DecksTests::deckValidationResultErrorsFactoryRejectsEmptyList() {
  // deckValidationErrors requires minItems:1 -- an empty list is not a
  // valid alternate spelling of success at the type level, so the
  // errors() factory itself must refuse to construct one.
  const auto result = DeckValidationResult::errors({});
  QVERIFY(!result.has_value());
}

void DecksTests::deckValidationResultSuccessAndErrorsAreDistinctKinds() {
  const auto success = DeckValidationResult::success();
  const auto errorsResult = DeckValidationResult::errors(
      {DeckValidationError{.cardCode = *CardCode::parse(u"c99999"_s)}});
  if (!errorsResult)
    QFAIL(qPrintable(errorsResult.error()));
  QVERIFY(success != *errorsResult);
  QCOMPARE(success.toJson(), QJsonArray{});
  QCOMPARE(errorsResult->toJson(),
           (QJsonArray{QJsonObject{
               {QStringLiteral("tag"), QStringLiteral("UnimplementedCard")},
               {QStringLiteral("contents"), QStringLiteral("c99999")}}}));
}

void DecksTests::scientificShowMatchesKnownBackendFormatting() {
  // Matches Data.Scientific's Show instance: fixed-point with a mandatory
  // ".0" for 0.1 <= |x| < 1e7, scientific notation otherwise.
  QCOMPARE(*Json::scientificShow(4242.0, u"test"), QStringLiteral("4242.0"));
  QCOMPARE(*Json::scientificShow(0.0, u"test"), QStringLiteral("0.0"));
  QCOMPARE(*Json::scientificShow(42.5, u"test"), QStringLiteral("42.5"));
  QCOMPARE(*Json::scientificShow(-4242.0, u"test"), QStringLiteral("-4242.0"));
  QCOMPARE(*Json::scientificShow(1.0e7, u"test"), QStringLiteral("1.0e7"));
}

void DecksTests::scientificShowRejectsNonFiniteRatherThanCrashing() {
  // A syntactically valid JSON number can overflow to +/-Infinity once
  // Qt's JSON parser has already narrowed it to a double (e.g. the huge
  // exponent "1e400" would); scientificShow() must reject this explicitly
  // rather than asserting/crashing on std::to_chars's non-scientific
  // "inf"/"nan" textual output, which has no exponent to extract.
  const double huge = 1.0e308 * 10.0; // overflows to +Infinity.
  QVERIFY(std::isinf(huge));
  const auto result = Json::scientificShow(huge, u"deckList.id");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("deckList.id")));

  const auto negResult = Json::scientificShow(-huge, u"deckList.id");
  QVERIFY(!negResult.has_value());

  const auto nanResult = Json::scientificShow(
      std::numeric_limits<double>::quiet_NaN(), u"deckList.id");
  QVERIFY(!nanResult.has_value());
}

void DecksTests::externalIdOverflowingToInfinityRejectedNotCrashed() {
  // A deck id with an astronomically large exponent is syntactically a
  // valid JSON number but overflows to +Infinity once parsed as a double;
  // this must be a clean decode failure, never a crash.
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
      {QStringLiteral("id"), 1.0e308 * 10.0},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  QVERIFY(!result.has_value());
}

void DecksTests::wrongTypeForInvestigatorCodeRejected() {
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), 12345},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("investigator_code")),
           qPrintable(result.error()));
}

void DecksTests::deckListInputMissingInvestigatorCodeRejected() {
  const QJsonObject obj{{QStringLiteral("slots"), QJsonObject{}}};
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  QVERIFY(!result.has_value());
}

QTEST_APPLESS_MAIN(DecksTests)

#include "DecksTests.moc"
