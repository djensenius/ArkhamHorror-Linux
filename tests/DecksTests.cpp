#include <QFile>
#include <QJsonDocument>
#include <QtTest>

#include "Decks.h"
#include "JsonDecode.h"

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

  // sideSlots malformed/absent behavior ──────────────────────────────────────
  void sideSlotsAbsentStaysUndefinedNotEmptyMap();
  void sideSlotsMalformedArrayPreservedVerbatim();
  void sideSlotsAlreadyNormalizedMapDistinguishableFromMalformed();

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

  // scientificShow (backend Aeson Scientific Show semantics) ────────────────
  void scientificShowMatchesKnownBackendFormatting();

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
  QCOMPARE(result->deckList.id.tag, ExternalDeckIdTag::Number);
  QCOMPARE(result->deckList.id.number, 4242.0);
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
  QCOMPARE(result->id.tag, ExternalDeckIdTag::Number);
  QCOMPARE(result->id.number, 4242.0);
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
  QCOMPARE(result->userId, 7);
  QCOMPARE(result->name, QStringLiteral("Contract deck"));
  QCOMPARE(result->investigatorName, QStringLiteral("Roland Banks"));
  QCOMPARE(result->list.investigatorCode.value(), QStringLiteral("c01001"));

  QCOMPARE(result->toJson(), v.toObject());
}

void DecksTests::decodesValidationErrorsFromFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("validationErrors"_L1);
  const auto result = decodeDeckValidationResult(v, u"validationErrors");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->size(), 1);
  QCOMPARE(result->at(0).cardCode.value(), QStringLiteral("c99999"));
  QCOMPARE(encodeDeckValidationResult(*result), v.toArray());
}

void DecksTests::decodesValidationSuccessFromFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("validationSuccess"_L1);
  const auto result = decodeDeckValidationResult(v, u"validationSuccess");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->isEmpty());
  QCOMPARE(encodeDeckValidationResult(*result), v.toArray());
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
  QCOMPARE(result->id.tag, ExternalDeckIdTag::Absent);
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
  QCOMPARE(result->id.tag, ExternalDeckIdTag::Null);
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
  QCOMPARE(result->id.tag, ExternalDeckIdTag::Text);
  QCOMPARE(result->id.text, QStringLiteral("external-9999"));
  QCOMPARE(result->toJson().value(QStringLiteral("id")).toString(),
           QStringLiteral("external-9999"));
}

void DecksTests::externalIdLargeIntegerPreservedWithoutPrecisionLoss() {
  // 2^53 + 1 == 9007199254740993 cannot be exactly represented as a double
  // (Qt's JSON storage) -- this proves the client preserves whatever value
  // Qt's parser itself already gave it (no *additional* client-side
  // precision loss on top of what Qt's JSON layer already does), rather
  // than rounding again via e.g. an intermediate int64 conversion.
  const double large = 9007199254740992.0; // 2^53, exactly representable.
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
      {QStringLiteral("id"), large},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->id.tag, ExternalDeckIdTag::Number);
  QCOMPARE(result->id.number, large);
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
  QCOMPARE(result->id.tag, ExternalDeckIdTag::Number);
  QCOMPARE(result->id.number, 42.5);
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

void DecksTests::sideSlotsAbsentStaysUndefinedNotEmptyMap() {
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->sideSlots.isUndefined());
  QVERIFY(!result->toJson().contains(QStringLiteral("sideSlots")));
}

void DecksTests::sideSlotsMalformedArrayPreservedVerbatim() {
  // The fixture's own createDeck.deckList.sideSlots is `[]` -- a legacy
  // array shape, not an already-normalized map. Preserving it verbatim
  // (rather than coercing to {}) means a caller can tell the two apart.
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
      {QStringLiteral("sideSlots"), QJsonArray{}},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->sideSlots.isArray());
  QVERIFY(!result->sideSlots.isObject());
}

void DecksTests::sideSlotsAlreadyNormalizedMapDistinguishableFromMalformed() {
  const QJsonObject obj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
      {QStringLiteral("sideSlots"), QJsonObject{{QStringLiteral("c00001"), 2}}},
  };
  const auto result = DeckListInput::fromJson(obj, u"deckList");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->sideSlots.isObject());
  QVERIFY(!result->sideSlots.isArray());
  QCOMPARE(result->sideSlots.toObject().value(QStringLiteral("c00001")).toInt(),
           2);
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
      decodeDeckValidationResult(QJsonArray{}, u"validationResult");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->isEmpty());
}

void DecksTests::scientificShowMatchesKnownBackendFormatting() {
  // Matches Data.Scientific's Show instance: fixed-point with a mandatory
  // ".0" for 0.1 <= |x| < 1e7, scientific notation otherwise.
  QCOMPARE(Json::scientificShow(4242.0), QStringLiteral("4242.0"));
  QCOMPARE(Json::scientificShow(0.0), QStringLiteral("0.0"));
  QCOMPARE(Json::scientificShow(42.5), QStringLiteral("42.5"));
  QCOMPARE(Json::scientificShow(-4242.0), QStringLiteral("-4242.0"));
  QCOMPARE(Json::scientificShow(1.0e7), QStringLiteral("1.0e7"));
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
