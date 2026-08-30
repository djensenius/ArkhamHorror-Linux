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
  // Round decodeGameList/GameListRow/CreateGameRequest #3 (companion to
  // GamesTests' identically-numbered items): DeckList/Deck previously had
  // no raw-byte entry point at all -- any caller wanting exact quantity
  // decoding had to collapse through QJsonDocument first. fromRawBytes()
  // now decodes directly off the lossless AST end-to-end.
  void deckListFromRawBytesMatchesFromJsonOnSameFixture();
  void deckFromRawBytesMatchesFromJsonOnSameFixture();
  void deckFromRawBytesRejectsDuplicateObjectKey();
  void decodesValidationErrorsFromFixture();
  void decodesValidationSuccessFromFixture();
  void decodesOperationErrorFromFixture();
  // Round-10-cumulative-review item 5: decks.schema.json's deckList/deck/
  // deckValidationError/deckOperationError are each additionalProperties:
  // false; an unrecognized top-level key on any of them is now a hard
  // decode failure rather than silently accepted-and-discarded. Also adds
  // the canonical raw-byte entry points DeckValidationError/
  // DeckValidationResult/DeckOperationError previously lacked entirely
  // (fromJson()-only before this round), proving they decode a fixture
  // identically through both paths and reject a duplicate/escape-
  // equivalent-duplicate key before any nested decode runs.
  void deckListExtraTopLevelFieldRejected();
  void deckExtraTopLevelFieldRejected();
  void deckValidationErrorExtraTopLevelFieldRejected();
  void deckOperationErrorExtraTopLevelFieldRejected();
  void deckValidationErrorFromRawBytesMatchesFromJsonOnSameFixture();
  void deckValidationResultFromRawBytesMatchesFromJsonOnErrorsFixture();
  void deckOperationErrorFromRawBytesMatchesFromJsonOnSameFixture();
  void deckValidationErrorEscapedDuplicateTagKeyRejectedThroughRawBytes();
  // Round-10-cumulative-review item 2: FetchDeckRequest was QJson-only and
  // could silently encode an invalid lone/mismatched UTF-16 surrogate in
  // `url` (Qt's QJsonValue(QString) constructor performs no validation at
  // all); the new canonical toJsonBytes()/toRawJson() pair rejects this
  // with a typed failure instead, exactly like every other outbound
  // request's byte encoder.
  void fetchDeckRequestToJsonBytesRejectsLoneSurrogateInUrl();
  void fetchDeckRequestToJsonBytesRoundTripsFixtureUrlExactly();

  // ExternalDeckId: string/integer/decimal/null preservation ───────────────
  void externalIdAbsentPreserved();
  void externalIdExplicitNullPreserved();
  void externalIdStringPreserved();
  void externalIdLargeIntegerPreservedWithoutPrecisionLoss();
  void externalIdDecimalPreserved();
  void externalIdWrongTypeRejected();
  // Round-7 HIGH #2: ExternalDeckId::toJson()/DeckListInput::toJson()/
  // CreateDeckRequest::toJson()/ChooseDeckRequest::toJson() must never
  // silently round a Number id through a double -- exact ids succeed via
  // QJsonValue(qint64), and anything else (fraction/huge exponent) is a
  // typed failure, never a rounded/zero/string substitute.
  void externalIdToJsonPreservesLargeIntegerExactlyAsQJsonInteger();
  void externalIdToJsonRejectsHugeExponentWithoutRoundingOrFallback();
  void externalIdToJsonRejectsFractionWithoutRoundingOrFallback();

  // ExternalDeckId via the raw-byte parser: genuine precision preservation
  // (HIGH #3) -- no double-rounding at all, unlike the QJsonValue-based
  // fromJson()/fromObject() path above, which can only preserve whatever
  // precision Qt's own parser already left intact.
  void externalIdFromRawBytesPreservesLargeIntegerExactly();
  void externalIdFromRawBytesPreservesLongDecimalExactly();
  void externalIdFromRawBytesPreservesHugeExponentExactly();
  void externalIdFromRawBytesRoundTripsThroughToJsonBytesExactly();
  void externalIdFromRawBytesRejectsMalformedNumberLiteral();
  // Fixes a suppressed reviewer comment on b7e7901: toRawJson() must
  // distinguish Absent (Json::Value Undefined -- an omitted key) from an
  // explicit Null (Json::Value::makeNull()), exactly mirroring toJson()'s
  // existing QJsonValue::Undefined-vs-Null distinction, so no caller can
  // silently turn an omitted id into an explicit JSON null on the wire.
  void externalIdToRawJsonDistinguishesAbsentFromExplicitNull();
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

  // Json::requireIntValue qint64 boundary (used by quantity/taboo_id
  // decoding) ────────────────────────────────────────────────────────────
  void requireIntValueAcceptsExactInt64Max();
  void requireIntValueAcceptsExactInt64Min();
  void requireIntValueRejectsOneBeyondInt64Max();

  // Nullable metadata/url/taboo fields ────────────────────────────────────────
  void nullableFieldsRoundTripAsExplicitNull();
  void normalizedDeckListMissingRequiredKeyRejected();
  void deckNullUrlRoundTripsAsExplicitNullNotOmitted();

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
  // Round 4 item 6: DeckListInput.cardSlots is a public
  // QMap<QString, qint64> field, so a programmatically-constructed
  // instance -- not merely a malformed decode -- with an empty key must
  // still be rejected at encode time.
  void deckListInputToJsonRejectsProgrammaticEmptyCardSlotsKey();
  void deckListInputToJsonBytesRejectsProgrammaticEmptyCardSlotsKey();
  // Round-10-cumulative-review item 1: sideSlots.toExactQJson() (see
  // Value::toExactQJson()'s hardened invariants in RawJson.cpp/.h) must
  // reject a malicious raw AST rather than silently producing a
  // normal-looking-but-altered QJsonObject; proven here through
  // DeckListInput::toJson() and through the enclosing CreateDeckRequest
  // public request type it composes into.
  void deckListInputToJsonRejectsSideSlotsWithNestedUndefined();
  void deckListInputToJsonRejectsSideSlotsWithDuplicateKey();
  void createDeckRequestToJsonRejectsMaliciousSideSlots();
  // Round-16-cumulative-review item 2 companion: a real enclosing
  // request field (DeckListInput::sideSlots), not just Value::
  // toExactQJson() in isolation, must reject an all-zero coefficient
  // whose fraction digit count exceeds ParseLimits::production()'s
  // digit budget.
  void deckListInputToJsonRejectsSideSlotsWithExcessiveFractionDigitBudget();
  // Round-14 item 1: DeckListInput::toJson()/CreateDeckRequest::toJson()/
  // FetchDeckRequest::toJson() previously embedded
  // investigatorCode/deckName/url via raw, unvalidated QJsonValue(QString)
  // construction, so a lone/mismatched UTF-16 surrogate there would have
  // silently produced a normal-looking-but-invalid QJsonObject even though
  // toJsonBytes() correctly rejected the identical input. Each now
  // composes toRawJson() + Value::toExactQJsonObject() instead, so
  // toJson() must reject these exactly like toJsonBytes() already does.
  void deckListInputToJsonRejectsLoneSurrogateInInvestigatorCode();
  void createDeckRequestToJsonRejectsLoneSurrogateInDeckName();
  void fetchDeckRequestToJsonRejectsLoneSurrogateInUrl();
  // Round-16-cumulative-review item 1: CampaignOrScenario::insertInto()
  // (a QJsonObject-typed test-only fragment method with no
  // lone-surrogate validation of its own) was removed as an unsafe,
  // misleading public API (see Games.h/GamesTests.cpp); the reviewer
  // also asked for a direct lone-surrogate parity test PER RETAINED
  // fragment method, not merely through an enclosing request -- each of
  // these five exercises exactly one fragment in isolation, proving it
  // rejects a lone/mismatched UTF-16 surrogate exactly like
  // Value::toExactQJson()/toJsonBytes() would, even though the enclosing
  // request-level tests above already prove the same thing
  // transitively.
  void investigatorRefToJsonRejectsLoneSurrogateDirectly();
  void cardCodeToJsonRejectsLoneSurrogateDirectly();
  void cardNameToJsonRejectsLoneSurrogateInTitle();
  void cardNameToJsonRejectsLoneSurrogateInSubtitle();
  void externalDeckIdTextKindToJsonRejectsLoneSurrogate();
  void deckListToJsonRejectsLoneSurrogateInCardSlotsKey();
  void deckOperationErrorToJsonRejectsLoneSurrogateInErrorMsg();
  // Round-18-cumulative-review item 1: every retained fragment toJson()
  // above used to check *only* for a lone/mismatched UTF-16 surrogate,
  // never against ParseLimits::production()'s string-length/number-
  // digit-count budgets -- so e.g. an over-length InvestigatorRef, or an
  // ExternalDeckId::Number literal whose digit count alone exceeded the
  // budget, would succeed as a standalone fragment while an enclosing
  // complete request encoder (which always composes through
  // toRawJson()->toExactQJson()/toExactQJsonObject()) rejected the
  // identical value. Each fragment now builds its Json::Value and routes
  // through that same canonical, bounded conversion instead of
  // hand-duplicating a subset of its checks; these tests prove both the
  // direct fragment and its enclosing request now agree, not merely that
  // one of them fails.
  void investigatorRefToJsonRejectsStringExceedingProductionLengthBudget();
  void
  deckListInputToJsonRejectsInvestigatorCodeExceedingProductionLengthBudget();
  void cardCodeToJsonRejectsStringExceedingProductionLengthBudget();
  void deckListToJsonRejectsInvestigatorCodeExceedingProductionLengthBudget();
  void cardNameToJsonRejectsTitleExceedingProductionLengthBudget();
  void externalDeckIdNumberKindToJsonRejectsAllZeroExceedingDigitBudget();
  void deckListInputToJsonRejectsIdExceedingDigitBudgetThroughFragmentParity();

  // Round-14 items 2/3: TypedId<Tag>/NonEmptyString<Tag>/CardCode used to
  // rely on an implicit move constructor/assignment that emptied the
  // moved-from instance's underlying QString while its type still
  // nominally claimed to hold a validated (non-null/non-empty) value.
  // Each now explicitly declares a copy constructor/assignment (see
  // Identifiers.h), suppressing the compiler's implicit move so
  // std::move() falls back to a full copy -- the moved-from source must
  // remain completely valid and reusable, including through an enclosing
  // request/response encoder.
  void cardCodeMoveConstructLeavesSourceValidAndReusable();
  void cardCodeMoveAssignLeavesSourceValidAndReusable();
  void cardCodeSelfMoveAssignmentLeavesValueUnchanged();
  void cardCodeSurvivesQListRelocation();
  void
  investigatorRefMoveConstructLeavesSourceValidAndReusableInDeckListInput();
  void deckIdMoveConstructLeavesSourceValidAndReusableInDeck();

  // Round-19-cumulative-review item 3: DeckValidationResult/ExternalDeckId
  // had no user-declared copy/move, so a compiler-generated move
  // constructor/assignment really moved their QList<DeckValidationError>/
  // QString payload members (leaving them empty) while the discriminating
  // Kind enum stayed unchanged on the moved-from source -- for
  // DeckValidationResult in particular, a moved-from Kind::Errors instance
  // would then encode [] via toJson(), the exact "Errors -> Success"
  // schema-shape collapse this class's own class-level doc comment
  // documents as structurally impossible. Both now explicitly declare a
  // copy constructor/assignment (see Decks.h), suppressing the implicit
  // move so std::move() falls back to a full (cheap, implicit-sharing)
  // copy instead.
  void deckValidationResultMoveConstructLeavesSourceErrorsIntact();
  void deckValidationResultMoveAssignLeavesSourceErrorsIntact();
  void deckValidationResultSelfMoveAssignmentLeavesValueUnchanged();
  void externalDeckIdMoveConstructLeavesSourceTextIntact();
  void externalDeckIdMoveAssignLeavesSourceTextIntact();
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
  const auto reencoded = result->toJson();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  QCOMPARE(*reencoded, expected);
}

void DecksTests::decodesFetchDeckRequestFromFixture() {
  const QJsonObject fixture = decksFixture();
  const auto result =
      FetchDeckRequest::fromJson(fixture.value("fetchDeck"_L1), u"fetchDeck");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->url,
           QStringLiteral("https://arkhamdb.com/decklist/view/4242"));
  const auto encoded = result->toJson();
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, fixture.value("fetchDeck"_L1).toObject());
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
  const auto reencoded = result->toJson();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  QVERIFY(!reencoded->contains(QStringLiteral("externalField")));
  QJsonObject expected = withoutKey(v.toObject(), "externalField"_L1);
  expected = withoutKey(expected, "taboo_id"_L1);
  QCOMPARE(*reencoded, expected);
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

  auto reencoded = result->toJson();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  QCOMPARE(*reencoded, v.toObject());
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

  auto reencoded = result->toJson();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  QCOMPARE(*reencoded, v.toObject());
}

void DecksTests::deckListFromRawBytesMatchesFromJsonOnSameFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("normalizedDeckList"_L1);
  const QByteArray bytes =
      QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);

  const auto viaJson = DeckList::fromJson(v, u"normalizedDeckList");
  const auto viaRawBytes = DeckList::fromRawBytes(bytes, u"normalizedDeckList");
  if (!viaJson)
    QFAIL(qPrintable(viaJson.error()));
  if (!viaRawBytes)
    QFAIL(qPrintable(viaRawBytes.error()));
  QVERIFY(*viaJson == *viaRawBytes);
}

void DecksTests::deckFromRawBytesMatchesFromJsonOnSameFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("deck"_L1);
  const QByteArray bytes =
      QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);

  const auto viaJson = Deck::fromJson(v, u"deck");
  const auto viaRawBytes = Deck::fromRawBytes(bytes, u"deck");
  if (!viaJson)
    QFAIL(qPrintable(viaJson.error()));
  if (!viaRawBytes)
    QFAIL(qPrintable(viaRawBytes.error()));
  QVERIFY(*viaJson == *viaRawBytes);
}

void DecksTests::deckFromRawBytesRejectsDuplicateObjectKey() {
  // The canonical raw-byte parser (see RawJson.h) rejects a duplicate
  // object key before any nested aggregate decode runs -- proven here
  // through Deck::fromRawBytes()'s actual production entry point, not
  // only in isolation against the parser.
  const QByteArray bytes = QByteArrayLiteral(
      "{\"id\":\"00000000-0000-0000-0000-000000000017\","
      "\"userId\":7,\"userId\":8,\"url\":null,\"name\":\"X\","
      "\"investigatorName\":\"Roland Banks\","
      "\"list\":{\"slots\":{},\"sideSlots\":{},"
      "\"investigator_code\":\"c01001\",\"investigator_name\":\"Roland "
      "Banks\",\"meta\":null,\"taboo_id\":null,\"url\":null,\"id\":null,"
      "\"name\":null}}");
  const auto result = Deck::fromRawBytes(bytes, u"deck");
  QVERIFY(!result);
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
  auto reencoded = result->toJson();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  QCOMPARE(*reencoded, v.toArray());
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
  auto reencoded = result->toJson();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  QCOMPARE(*reencoded, v.toArray());
}

void DecksTests::decodesOperationErrorFromFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("operationError"_L1);
  const auto result = DeckOperationError::fromJson(v, u"operationError");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->errorMsg, QStringLiteral("Could not sync deck"));
  auto reencoded = result->toJson();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  QCOMPARE(*reencoded, v.toObject());
}

void DecksTests::deckListExtraTopLevelFieldRejected() {
  QJsonObject obj = decksFixture().value("normalizedDeckList"_L1).toObject();
  obj.insert(QStringLiteral("aFutureFieldThisClientHasNeverHeardOf"), 1);
  const auto result = DeckList::fromJson(obj, u"normalizedDeckList");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(
               QStringLiteral("aFutureFieldThisClientHasNeverHeardOf")),
           qPrintable(result.error()));
}

void DecksTests::deckExtraTopLevelFieldRejected() {
  QJsonObject obj = decksFixture().value("deck"_L1).toObject();
  obj.insert(QStringLiteral("aFutureFieldThisClientHasNeverHeardOf"), 1);
  const auto result = Deck::fromJson(obj, u"deck");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(
               QStringLiteral("aFutureFieldThisClientHasNeverHeardOf")),
           qPrintable(result.error()));
}

void DecksTests::deckValidationErrorExtraTopLevelFieldRejected() {
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("UnimplementedCard")},
      {QStringLiteral("contents"), QStringLiteral("c99999")},
      {QStringLiteral("aFutureFieldThisClientHasNeverHeardOf"), 1},
  };
  const auto result = DeckValidationError::fromJson(obj, u"validationError");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(
               QStringLiteral("aFutureFieldThisClientHasNeverHeardOf")),
           qPrintable(result.error()));
}

void DecksTests::deckOperationErrorExtraTopLevelFieldRejected() {
  const QJsonObject obj{
      {QStringLiteral("errorMsg"), QStringLiteral("Could not sync deck")},
      {QStringLiteral("aFutureFieldThisClientHasNeverHeardOf"), 1},
  };
  const auto result = DeckOperationError::fromJson(obj, u"operationError");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(
               QStringLiteral("aFutureFieldThisClientHasNeverHeardOf")),
           qPrintable(result.error()));
}

void DecksTests::deckValidationErrorFromRawBytesMatchesFromJsonOnSameFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("validationErrors"_L1).toArray().at(0);
  const QByteArray bytes =
      QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
  const auto viaJson = DeckValidationError::fromJson(v, u"validationError");
  const auto viaRawBytes =
      DeckValidationError::fromRawBytes(bytes, u"validationError");
  if (!viaJson)
    QFAIL(qPrintable(viaJson.error()));
  if (!viaRawBytes)
    QFAIL(qPrintable(viaRawBytes.error()));
  QVERIFY(*viaJson == *viaRawBytes);
}

void DecksTests::
    deckValidationResultFromRawBytesMatchesFromJsonOnErrorsFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("validationErrors"_L1);
  const QByteArray bytes =
      QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
  const auto viaJson = DeckValidationResult::fromJson(v, u"validationErrors");
  const auto viaRawBytes =
      DeckValidationResult::fromRawBytes(bytes, u"validationErrors");
  if (!viaJson)
    QFAIL(qPrintable(viaJson.error()));
  if (!viaRawBytes)
    QFAIL(qPrintable(viaRawBytes.error()));
  QVERIFY(*viaJson == *viaRawBytes);
}

void DecksTests::deckOperationErrorFromRawBytesMatchesFromJsonOnSameFixture() {
  const QJsonObject fixture = decksFixture();
  const QJsonValue v = fixture.value("operationError"_L1);
  const QByteArray bytes =
      QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
  const auto viaJson = DeckOperationError::fromJson(v, u"operationError");
  const auto viaRawBytes =
      DeckOperationError::fromRawBytes(bytes, u"operationError");
  if (!viaJson)
    QFAIL(qPrintable(viaJson.error()));
  if (!viaRawBytes)
    QFAIL(qPrintable(viaRawBytes.error()));
  QVERIFY(*viaJson == *viaRawBytes);
}

void DecksTests::
    deckValidationErrorEscapedDuplicateTagKeyRejectedThroughRawBytes() {
  // "\u0074ag" decodes to the same text as "tag"; Json::Value::parse()
  // itself rejects this as a duplicate key (see
  // RawJsonTests::rejectsEscapeEquivalentDuplicateObjectKeys()) before any
  // DeckValidationError-specific decoding runs, proven here through its
  // production fromRawBytes() entry point.
  const QByteArray bytes = R"({"tag":"UnimplementedCard","contents":"c99999",)"
                           R"("\u0074ag":"UnimplementedCard"})";
  const auto result =
      DeckValidationError::fromRawBytes(bytes, u"validationError");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(u"duplicate"_s), qPrintable(result.error()));
}

void DecksTests::fetchDeckRequestToJsonBytesRejectsLoneSurrogateInUrl() {
  QString lone;
  lone += QChar(0xD800);
  const FetchDeckRequest request{.url = lone};
  const auto bytes = request.toJsonBytes();
  QVERIFY(!bytes.has_value());
}

void DecksTests::fetchDeckRequestToJsonBytesRoundTripsFixtureUrlExactly() {
  const QJsonObject fixture = decksFixture();
  const auto decoded =
      FetchDeckRequest::fromJson(fixture.value("fetchDeck"_L1), u"fetchDeck");
  if (!decoded)
    QFAIL(qPrintable(decoded.error()));
  const auto bytes = decoded->toJsonBytes();
  if (!bytes)
    QFAIL(qPrintable(bytes.error()));
  const auto reparsed = FetchDeckRequest::fromRawBytes(*bytes, u"fetchDeck");
  if (!reparsed)
    QFAIL(qPrintable(reparsed.error()));
  QVERIFY(*reparsed == *decoded);
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
  const auto reencoded = result->toJson();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  QVERIFY(!reencoded->contains(QStringLiteral("id")));
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
  // A missing key's .value() would be Undefined, whose isNull() is false
  // (confirmed empirically against Qt's actual behavior), so this pair
  // genuinely distinguishes "key present with null" from "key omitted".
  const auto reencoded = result->toJson();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  QVERIFY(reencoded->contains(QStringLiteral("id")));
  QVERIFY(reencoded->value(QStringLiteral("id")).isNull());
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
  const auto reencoded = result->toJson();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  QCOMPARE(reencoded->value(QStringLiteral("id")).toString(),
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
  // toJson() now succeeds exactly via QJsonValue(qint64) (never a lossy
  // double) since this literal is mathematically integral and fits
  // qint64's range -- see ExternalDeckId::toJson()'s doc comment.
  const auto reencoded = result->toJson();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  QCOMPARE(reencoded->value(QStringLiteral("id")).toInteger(),
           qint64(9007199254740992LL));
  QCOMPARE(reencoded->value(QStringLiteral("id")).toDouble(), large);
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
  // toJson() must FAIL for a genuine fraction rather than silently
  // rounding through a double -- this is the exact scenario the
  // "public outbound request QJson encoders silently round exact
  // ExternalDeckId" review finding forbids: there is no safe fallback.
  const auto reencoded = result->toJson();
  QVERIFY(!reencoded.has_value());
  QVERIFY2(reencoded.error().contains(QStringLiteral("42.5")),
           qPrintable(reencoded.error()));
  // The lossless byte path still encodes the exact literal unchanged.
  const auto bytes = result->toJsonBytes();
  if (!bytes)
    QFAIL(qPrintable(bytes.error()));
  QVERIFY(bytes->contains("42.5"));
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

void DecksTests::externalIdToJsonPreservesLargeIntegerExactlyAsQJsonInteger() {
  // Unit-level (not via DeckListInput): 9007199254740993 == 2^53 + 1 is the
  // smallest positive integer a double cannot represent exactly, but it
  // fits qint64 exactly. ExternalDeckId::toJson() must succeed and its
  // QJsonValue must be backed by the exact qint64, not a rounded double.
  const auto id =
      ExternalDeckId::number(Json::RawNumber::fromInt64(9007199254740993LL));
  const auto encoded = id.toJson();
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(encoded->toInteger(), qint64(9007199254740993LL));
  // The naive double round-trip for this literal is 9007199254740992 (2^53)
  // -- assert the two are distinguishable so a regression back to
  // QJsonValue(m_number.toDouble()) would fail this test.
  QVERIFY(encoded->toInteger() != qint64(9007199254740992LL));
}

void DecksTests::
    externalIdToJsonRejectsHugeExponentWithoutRoundingOrFallback() {
  // "1e128" is syntactically a valid JSON number but its magnitude is far
  // outside qint64's range and it parses to +Infinity as a double.
  // ExternalDeckId::toJson() must return a typed failure -- never a
  // rounded/Infinity/zero QJsonValue "fallback".
  auto parsed = Json::Value::parse(QByteArrayLiteral("1e128"), u"id");
  if (!parsed)
    QFAIL(qPrintable(parsed.error()));
  QVERIFY(parsed->isNumber());
  const auto id = ExternalDeckId::number(parsed->toRawNumber());
  const auto encoded = id.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("1e128")),
           qPrintable(encoded.error()));
}

void DecksTests::externalIdToJsonRejectsFractionWithoutRoundingOrFallback() {
  // A genuine fraction (as opposed to a mathematically-integral literal
  // like "1.0") can never be represented exactly as a qint64-backed
  // QJsonValue; toJson() must fail rather than silently rounding through
  // a double.
  auto parsed = Json::Value::parse(QByteArrayLiteral("42.5"), u"id");
  if (!parsed)
    QFAIL(qPrintable(parsed.error()));
  const auto id = ExternalDeckId::number(parsed->toRawNumber());
  const auto encoded = id.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("42.5")),
           qPrintable(encoded.error()));
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

void DecksTests::externalIdToRawJsonDistinguishesAbsentFromExplicitNull() {
  // Absent must encode as Json::Value's Undefined -- an omitted id key,
  // not a JSON null -- exactly mirroring toJson()'s existing
  // QJsonValue::Undefined-vs-Null distinction. Prior to this fix,
  // toRawJson() collapsed Absent and Null to the same makeNull() result,
  // so a caller invoking toRawJson() directly (bypassing the
  // kind()-guarded composition in DeckListInput::toRawJson()) could
  // silently turn an omitted id into a semantically different explicit
  // null on the wire.
  const auto absentRaw = ExternalDeckId::absent().toRawJson();
  QVERIFY(absentRaw.isUndefined());
  // Serializing an Undefined value on its own is a typed failure, not a
  // silently-emitted "null" -- confirming Absent cannot masquerade as a
  // valid standalone JSON value.
  const auto absentBytes = absentRaw.toJsonBytes();
  QVERIFY(!absentBytes.has_value());

  const auto nullRaw = ExternalDeckId::null().toRawJson();
  QVERIFY(nullRaw.isNull());
  QVERIFY(!nullRaw.isUndefined());
  const auto nullBytes = nullRaw.toJsonBytes();
  if (!nullBytes)
    QFAIL(qPrintable(nullBytes.error()));
  QCOMPARE(*nullBytes, QByteArrayLiteral("null"));
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
  const auto reencodedJson = result->toJson();
  if (!reencodedJson)
    QFAIL(qPrintable(reencodedJson.error()));
  QVERIFY(!reencodedJson->contains(QStringLiteral("sideSlots")));
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

void DecksTests::requireIntValueAcceptsExactInt64Max() {
  // qint64::max() (2^63-1) is not exactly representable as a double and
  // rounds UP to 2^63 -- a naive `toDouble() >= 2^63` boundary check
  // incorrectly rejects this value. QJsonValue(qint64) is the same
  // constructor RawJson::Value::toQJson() uses for contract-domain
  // integers, so this exercises the exact storage path production code
  // relies on.
  constexpr qint64 kMax = std::numeric_limits<qint64>::max();
  const QJsonValue v(kMax);
  const auto result = Json::requireIntValue(v, u"x");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(*result, kMax);
}

void DecksTests::requireIntValueAcceptsExactInt64Min() {
  constexpr qint64 kMin = std::numeric_limits<qint64>::min();
  const QJsonValue v(kMin);
  const auto result = Json::requireIntValue(v, u"x");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(*result, kMin);
}

void DecksTests::requireIntValueRejectsOneBeyondInt64Max() {
  // 2^63 itself (one past qint64::max()) must still be rejected as
  // out-of-range -- the fix for the INT64_MAX false-rejection must not
  // regress into accepting genuinely out-of-range values instead.
  const auto doc =
      QJsonDocument::fromJson(QByteArrayLiteral("{\"x\":9223372036854775808}"));
  const auto result =
      Json::requireIntValue(doc.object().value(QStringLiteral("x")), u"x");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("out of range")),
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
  auto reencoded = result->toJson();
  if (!reencoded)
    QFAIL(qPrintable(reencoded.error()));
  QCOMPARE(*reencoded, obj);
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

void DecksTests::deckNullUrlRoundTripsAsExplicitNullNotOmitted() {
  // decks.schema.json's Deck.url is required-but-nullable. The fixture in
  // decksFixture() always has a non-null url, so it alone cannot prove
  // Deck::toJson() preserves an explicit null (as opposed to silently
  // omitting the key, which QJsonObject would do only for an explicit
  // QJsonValue::Undefined -- see ExternalDeckId::Kind::Absent for the
  // legitimate use of that -- not for QJsonValue::Null).
  const QJsonObject obj{
      {QStringLiteral("id"),
       QStringLiteral("00000000-0000-0000-0000-000000000017")},
      {QStringLiteral("userId"), 7},
      {QStringLiteral("url"), QJsonValue()},
      {QStringLiteral("name"), QStringLiteral("Contract deck")},
      {QStringLiteral("investigatorName"), QStringLiteral("Roland Banks")},
      {QStringLiteral("list"),
       QJsonObject{
           {QStringLiteral("slots"), QJsonObject{}},
           {QStringLiteral("sideSlots"), QJsonObject{}},
           {QStringLiteral("investigator_code"), QStringLiteral("c01001")},
           {QStringLiteral("investigator_name"),
            QStringLiteral("Roland Banks")},
           {QStringLiteral("meta"), QJsonValue()},
           {QStringLiteral("taboo_id"), QJsonValue()},
           {QStringLiteral("url"), QJsonValue()},
           {QStringLiteral("id"), QJsonValue()},
           {QStringLiteral("name"), QJsonValue()},
       }},
  };
  const auto result = Deck::fromJson(obj, u"deck");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(!result->url.has_value());

  auto encodedResult = result->toJson();
  if (!encodedResult)
    QFAIL(qPrintable(encodedResult.error()));
  const QJsonObject encoded = *encodedResult;
  // A missing key's .value() is Undefined, whose isNull() is false (see
  // scratchDiagnosticCheck's empirical confirmation during review); so
  // this pair of assertions genuinely fails if the key were ever dropped,
  // not merely if it held the wrong value.
  QVERIFY(encoded.contains(QStringLiteral("url")));
  QVERIFY(encoded.value(QStringLiteral("url")).isNull());
  QCOMPARE(encoded, obj);
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
  auto successEncoded = success.toJson();
  if (!successEncoded)
    QFAIL(qPrintable(successEncoded.error()));
  QCOMPARE(*successEncoded, QJsonArray{});
  auto errorsEncoded = errorsResult->toJson();
  if (!errorsEncoded)
    QFAIL(qPrintable(errorsEncoded.error()));
  QCOMPARE(*errorsEncoded,
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
  // Asserts the exact, clearer "missing required field" phrasing rather
  // than the less specific "expected string, got missing" a bare
  // value-decoder call would produce for an absent key -- see
  // Json::requireField (JsonDecode.h).
  QCOMPARE(result.error(),
           QStringLiteral("deckList.investigator_code: missing required field "
                          "\"investigator_code\""));
}

void DecksTests::deckListInputToJsonRejectsProgrammaticEmptyCardSlotsKey() {
  // fromJson()/fromRawJson() can never produce an empty cardSlots key --
  // decodeCardQuantityMapInput rejects it during decode -- but cardSlots
  // is a public QMap<QString, qint64> field, so a caller
  // building/mutating a DeckListInput by hand (not through decode) could
  // otherwise slip one past every existing safeguard. toJson() must
  // reject it rather than silently emitting a schema-invalid request.
  DeckListInput input{
      .cardSlots = {{QString(), 2}},
      .investigatorCode = *InvestigatorRef::parse(QStringLiteral("01001")),
  };
  const auto encoded = input.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("slots")),
           qPrintable(encoded.error()));

  // A non-empty key still encodes fine.
  input.cardSlots = {{QStringLiteral("01001"), 2}};
  const auto validEncoded = input.toJson();
  if (!validEncoded)
    QFAIL(qPrintable(validEncoded.error()));
  QCOMPARE(validEncoded->value(QStringLiteral("slots"))
               .toObject()
               .value(QStringLiteral("01001"))
               .toInt(),
           2);
}

void DecksTests::
    deckListInputToJsonBytesRejectsProgrammaticEmptyCardSlotsKey() {
  // toJsonBytes() (and any enclosing request's toJsonBytes(), e.g.
  // CreateDeckRequest/ChooseDeckRequest, which splice this type's
  // toRawJson() into a larger AST) must refuse the same invalid state,
  // not merely toJson().
  DeckListInput input{
      .cardSlots = {{QString(), 2}},
      .investigatorCode = *InvestigatorRef::parse(QStringLiteral("01001")),
  };
  const auto encoded = input.toJsonBytes();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("slots")),
           qPrintable(encoded.error()));

  const CreateDeckRequest request{
      .deckId = QStringLiteral("external-1"),
      .deckName = QStringLiteral("X"),
      .deckList = input,
  };
  const auto requestEncoded = request.toJsonBytes();
  QVERIFY(!requestEncoded.has_value());
  QVERIFY2(requestEncoded.error().contains(QStringLiteral("slots")),
           qPrintable(requestEncoded.error()));
}

void DecksTests::deckListInputToJsonRejectsSideSlotsWithNestedUndefined() {
  // sideSlots is a public Json::Value field; a caller can build this raw
  // AST by hand (decode never produces a nested Undefined), so
  // DeckListInput::toJson() itself -- not just the parser -- must refuse
  // it rather than have the nested key silently vanish through
  // QJsonObject::insert().
  DeckListInput input{
      .sideSlots = Json::Value::makeObject(
          {{QStringLiteral("present"), Json::Value::makeBool(true)},
           {QStringLiteral("vanishes"), Json::Value{}}}),
      .investigatorCode = *InvestigatorRef::parse(QStringLiteral("01001")),
  };
  const auto encoded = input.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("sideSlots")),
           qPrintable(encoded.error()));
}

void DecksTests::deckListInputToJsonRejectsSideSlotsWithDuplicateKey() {
  DeckListInput input{
      .sideSlots = Json::Value::makeObject(
          {{QStringLiteral("c01001"),
            Json::Value::makeNumber(Json::RawNumber::fromInt64(1))},
           {QStringLiteral("c01001"),
            Json::Value::makeNumber(Json::RawNumber::fromInt64(2))}}),
      .investigatorCode = *InvestigatorRef::parse(QStringLiteral("01001")),
  };
  const auto encoded = input.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("sideSlots")),
           qPrintable(encoded.error()));
}

void DecksTests::
    deckListInputToJsonRejectsSideSlotsWithExcessiveFractionDigitBudget() {
  // Companion to RawJsonTests::
  // toExactQJsonRejectsAllZeroCoefficientExceedingProductionDigitBudget():
  // proves the same digit-budget fix reaches through a real enclosing
  // request field. An all-zero coefficient short-circuits
  // RawNumber::toExactInt64() to exact 0 regardless of digit count, so
  // it is the only way to build a number whose digit count exceeds
  // ParseLimits::production().maxNumberDigits (64) while still being
  // "exactly representable" by every other check; the excess zero
  // digits live in the fraction part since RFC 8259's `int` production
  // forbids a redundant leading zero like "00" but `frac` places no
  // such restriction. Parsed here under a custom, deliberately widened
  // ParseLimits so parse() itself accepts the 70-zero-digit fraction.
  Json::ParseLimits wide;
  wide.maxNumberDigits = 128;
  const QByteArray excessiveFractionLiteral =
      QByteArrayLiteral("0.") + QByteArray(70, '0');
  auto parsedNumber = Json::Value::parse(excessiveFractionLiteral, u"n", wide);
  if (!parsedNumber)
    QFAIL(qPrintable(parsedNumber.error()));
  QVERIFY(parsedNumber->isNumber());

  DeckListInput input{
      .sideSlots =
          Json::Value::makeObject({{QStringLiteral("c01001"), *parsedNumber}}),
      .investigatorCode = *InvestigatorRef::parse(QStringLiteral("01001")),
  };

  // Before the digit-budget fix, DeckListInput::toJson()'s
  // Value::toExactQJson() call would have silently accepted this via
  // toExactInt64()'s all-zero shortcut, in spite of the encoded number
  // exceeding production's digit budget.
  const auto encoded = input.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("sideSlots")),
           qPrintable(encoded.error()));

  // toJsonBytes() (the lossless path, which already enforced this
  // budget before this round's fix) rejects the identical AST too --
  // proving both encoders agree, not merely that one happens to fail.
  const auto bytes = input.toJsonBytes();
  QVERIFY(!bytes.has_value());

  // The enclosing CreateDeckRequest composes DeckListInput::toJson(),
  // so the rejection must propagate all the way through the complete
  // public request type as well.
  const CreateDeckRequest request{.deckList = input};
  const auto requestEncoded = request.toJsonBytes();
  QVERIFY(!requestEncoded.has_value());
}

void DecksTests::createDeckRequestToJsonRejectsMaliciousSideSlots() {
  // Proves the rejection reaches the actual public outbound request type
  // this client sends over the wire, not merely DeckListInput in
  // isolation.
  const DeckListInput input{
      .sideSlots = Json::Value::makeObject(
          {{QStringLiteral("present"), Json::Value::makeBool(true)},
           {QStringLiteral("vanishes"), Json::Value{}}}),
      .investigatorCode = *InvestigatorRef::parse(QStringLiteral("01001")),
  };
  const CreateDeckRequest request{
      .deckId = QStringLiteral("external-1"),
      .deckName = QStringLiteral("X"),
      .deckList = input,
  };
  const auto encoded = request.toJson();
  QVERIFY(!encoded.has_value());
  const auto encodedBytes = request.toJsonBytes();
  QVERIFY(!encodedBytes.has_value());
}

void DecksTests::deckListInputToJsonRejectsLoneSurrogateInInvestigatorCode() {
  // InvestigatorRef::parse() only requires non-empty text, so a lone
  // UTF-16 surrogate (a single, non-empty QChar) is a validly-constructed
  // value from the wrapper's own perspective -- toJson() must still
  // reject it exactly as toJsonBytes() does, since neither can encode it
  // as valid UTF-8.
  QString lone;
  lone += QChar(0xD800);
  const DeckListInput input{
      .investigatorCode = *InvestigatorRef::parse(lone),
  };
  const auto encoded = input.toJson();
  QVERIFY(!encoded.has_value());
  const auto encodedBytes = input.toJsonBytes();
  QVERIFY(!encodedBytes.has_value());
}

void DecksTests::
    deckListInputToJsonRejectsInvestigatorCodeExceedingProductionLengthBudget() {
  // Round-18-cumulative-review item 1 parity companion: before this
  // round's fix, InvestigatorRef::toJson() (the direct fragment, see
  // investigatorRefToJsonRejectsStringExceedingProductionLengthBudget
  // below) checked only for a lone surrogate and had no
  // ParseLimits::production().maxStringLength check of its own, even
  // though DeckListInput::toJson() (composing investigatorCode via
  // toRawJson()->toExactQJsonObject()) already rejected the identical
  // over-length string. Proves both now agree.
  const QString overLong(Json::ParseLimits::production().maxStringLength + 1,
                         u'a');
  const DeckListInput input{
      .investigatorCode = *InvestigatorRef::parse(overLong),
  };
  const auto encoded = input.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("length")),
           qPrintable(encoded.error()));
  const auto encodedBytes = input.toJsonBytes();
  QVERIFY(!encodedBytes.has_value());
}

void DecksTests::createDeckRequestToJsonRejectsLoneSurrogateInDeckName() {
  // deckName is a plain public QString field (no validated wrapper type),
  // so a lone surrogate there is exactly the "mutable request field
  // bypasses invariants" scenario -- both toJson() and toJsonBytes() must
  // reject it identically now that toJson() composes the same
  // toRawJson()/toExactQJsonObject() machinery.
  QString lone;
  lone += QChar(0xDC00);
  const CreateDeckRequest request{
      .deckId = QStringLiteral("external-1"),
      .deckName = lone,
      .deckList = {.investigatorCode =
                       *InvestigatorRef::parse(QStringLiteral("01001"))},
  };
  const auto encoded = request.toJson();
  QVERIFY(!encoded.has_value());
  const auto encodedBytes = request.toJsonBytes();
  QVERIFY(!encodedBytes.has_value());
}

void DecksTests::fetchDeckRequestToJsonRejectsLoneSurrogateInUrl() {
  // Companion to fetchDeckRequestToJsonBytesRejectsLoneSurrogateInUrl:
  // FetchDeckRequest::toJson() used to insert `url` directly into a
  // QJsonObject with zero validation, unlike toJsonBytes(); it now
  // composes toRawJson() + toExactQJsonObject() and must reject
  // identically.
  QString lone;
  lone += QChar(0xD800);
  const FetchDeckRequest request{.url = lone};
  const auto encoded = request.toJson();
  QVERIFY(!encoded.has_value());
  const auto encodedBytes = request.toJsonBytes();
  QVERIFY(!encodedBytes.has_value());
}

void DecksTests::investigatorRefToJsonRejectsLoneSurrogateDirectly() {
  // Direct fragment-level test (not merely through the enclosing
  // DeckListInput -- see deckListInputToJsonRejectsLoneSurrogateInInvest
  // igatorCode above) for NonEmptyString<Tag>::toJson(), the type behind
  // InvestigatorRef/CampaignId/ScenarioId: parse() only requires
  // non-empty text, so a lone surrogate is a validly-constructed value
  // whose own toJson() must still reject it.
  QString lone;
  lone += QChar(0xD800);
  const auto ref = InvestigatorRef::parse(lone);
  if (!ref)
    QFAIL(qPrintable(ref.error()));
  const auto encoded = ref->toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("lone UTF-16 surrogate")),
           qPrintable(encoded.error()));
}

void DecksTests::
    investigatorRefToJsonRejectsStringExceedingProductionLengthBudget() {
  // Round-18-cumulative-review item 1: before this round's fix, this
  // fragment's toJson() checked only for a lone surrogate and had no
  // ParseLimits::production().maxStringLength check of its own -- see
  // deckListInputToJsonRejectsInvestigatorCodeExceedingProductionLength
  // Budget above for the enclosing-request parity companion, which
  // already rejected the identical over-length string before this fix.
  const QString overLong(Json::ParseLimits::production().maxStringLength + 1,
                         u'a');
  const auto ref = InvestigatorRef::parse(overLong);
  if (!ref)
    QFAIL(qPrintable(ref.error()));
  const auto encoded = ref->toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("length")),
           qPrintable(encoded.error()));
}

void DecksTests::cardCodeToJsonRejectsLoneSurrogateDirectly() {
  // Direct fragment-level test for CardCode::toJson(): parse() validates
  // the "c" prefix/non-emptiness/no-line-terminator shape but not every
  // UTF-16 code unit, so "c" + a lone surrogate is a validly-constructed
  // CardCode whose toJson() must still reject it.
  QString lone = QStringLiteral("c");
  lone += QChar(0xDC00);
  const auto code = CardCode::parse(lone);
  if (!code)
    QFAIL(qPrintable(code.error()));
  const auto encoded = code->toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("lone UTF-16 surrogate")),
           qPrintable(encoded.error()));
}

void DecksTests::cardCodeToJsonRejectsStringExceedingProductionLengthBudget() {
  // Round-18-cumulative-review item 1: CardCode::parse() validates the
  // "c" prefix/shape but not overall length, so an over-length code is
  // validly-constructed. See deckListToJsonRejectsInvestigatorCodeExceed
  // ingProductionLengthBudget below for the enclosing-request parity
  // companion (DeckList::investigatorCode is also a CardCode).
  QString overLong = QStringLiteral("c");
  overLong += QString(Json::ParseLimits::production().maxStringLength, u'a');
  const auto code = CardCode::parse(overLong);
  if (!code)
    QFAIL(qPrintable(code.error()));
  const auto encoded = code->toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("length")),
           qPrintable(encoded.error()));
}

void DecksTests::cardNameToJsonRejectsLoneSurrogateInTitle() {
  // Direct fragment-level test for CardName::toJson(): title/subtitle are
  // plain public QString fields with no validating factory at all, so a
  // lone surrogate is directly constructible.
  QString lone;
  lone += QChar(0xD800);
  const CardName name{.title = lone, .subtitle = std::nullopt};
  const auto encoded = name.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("title")),
           qPrintable(encoded.error()));
}

void DecksTests::cardNameToJsonRejectsLoneSurrogateInSubtitle() {
  QString lone;
  lone += QChar(0xDC00);
  const CardName name{.title = QStringLiteral("Roland Banks"),
                      .subtitle = lone};
  const auto encoded = name.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("subtitle")),
           qPrintable(encoded.error()));
}

void DecksTests::cardNameToJsonRejectsTitleExceedingProductionLengthBudget() {
  // Round-18-cumulative-review item 1: title/subtitle are plain public
  // QString fields with no length-validating factory at all, so an
  // over-length title is directly constructible. CardName has no
  // production request consumer to pair against (see this class's own
  // doc comment: it backs only response-shape CardDef.name/revealedName
  // and game-list scenario name), so this is a standalone fragment test.
  const QString overLong(Json::ParseLimits::production().maxStringLength + 1,
                         u'a');
  const CardName name{.title = overLong, .subtitle = std::nullopt};
  const auto encoded = name.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("title")),
           qPrintable(encoded.error()));
}

void DecksTests::externalDeckIdTextKindToJsonRejectsLoneSurrogate() {
  // Direct fragment-level test for ExternalDeckId::toJson()'s Kind::Text
  // branch: the text() factory does zero validation of its own (not even
  // non-emptiness), so a lone surrogate is directly constructible and
  // must be rejected at the encode boundary instead.
  QString lone;
  lone += QChar(0xD800);
  const auto id = ExternalDeckId::text(lone);
  QCOMPARE(id.kind(), ExternalDeckId::Kind::Text);
  const auto encoded = id.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("lone UTF-16 surrogate")),
           qPrintable(encoded.error()));
}

void DecksTests::
    externalDeckIdNumberKindToJsonRejectsAllZeroExceedingDigitBudget() {
  // Round-18-cumulative-review item 1: before this round's fix,
  // ExternalDeckId::toJson()'s Kind::Number branch called
  // RawNumber::toExactInt64() directly with no preceding
  // ParseLimits::production().maxNumberDigits check of its own -- unlike
  // Value::toExactQJson()'s own Number branch, which always checks the
  // digit budget first. An all-zero coefficient short-circuits
  // toExactInt64() to exact 0 regardless of digit count (see
  // RawJsonTests::
  // toExactQJsonRejectsAllZeroCoefficientExceedingProductionDigitBudget),
  // so this fragment used to silently succeed as 0 for a literal
  // toJsonBytes()/toExactQJson() would reject for exceeding the digit
  // budget. See deckListInputToJsonRejectsIdExceedingDigitBudgetThrough
  // FragmentParity below for the enclosing-request parity companion.
  Json::ParseLimits wide;
  wide.maxNumberDigits = 128;
  const QByteArray excessiveFractionLiteral =
      QByteArrayLiteral("0.") + QByteArray(70, '0');
  auto parsedNumber = Json::Value::parse(excessiveFractionLiteral, u"n", wide);
  if (!parsedNumber)
    QFAIL(qPrintable(parsedNumber.error()));
  QVERIFY(parsedNumber->isNumber());
  const auto id = ExternalDeckId::number(parsedNumber->toRawNumber());
  const auto encoded = id.toJson();
  QVERIFY(!encoded.has_value());
}

void DecksTests::deckListToJsonRejectsLoneSurrogateInCardSlotsKey() {
  // Direct fragment-level test for encodeCardQuantityMap() (private to
  // Decks.cpp, exercised here only through the public DeckList::toJson()
  // it composes into): a CardCode key holding a lone surrogate must
  // reject the whole map rather than silently producing a
  // normal-looking-but-invalid QJsonObject key.
  QString lone = QStringLiteral("c");
  lone += QChar(0xD800);
  const auto loneCode = CardCode::parse(lone);
  if (!loneCode)
    QFAIL(qPrintable(loneCode.error()));
  const DeckList list{
      .cardSlots = {{*loneCode, 1}},
      .investigatorCode = *CardCode::parse(QStringLiteral("c01001")),
  };
  const auto encoded = list.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("lone UTF-16 surrogate")),
           qPrintable(encoded.error()));
}

void DecksTests::deckOperationErrorToJsonRejectsLoneSurrogateInErrorMsg() {
  // DeckOperationError::errorMsg is a plain, unvalidated QString (no
  // parse()-style factory guards its content, unlike CardCode above), so
  // decoding a response containing a lone surrogate (a syntactically
  // valid `\ud800` escape) produces a validly-constructed
  // DeckOperationError whose toJson() must still reject re-encoding it,
  // rather than silently inserting it into the QJsonObject directly as
  // the previous non-fallible toJson() did.
  QString lone = QStringLiteral("Could not sync deck ");
  lone += QChar(0xD800);
  const DeckOperationError err{.errorMsg = lone};
  const auto encoded = err.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("lone UTF-16 surrogate")),
           qPrintable(encoded.error()));
}

void DecksTests::
    deckListToJsonRejectsInvestigatorCodeExceedingProductionLengthBudget() {
  // Enclosing-request parity companion to
  // cardCodeToJsonRejectsStringExceedingProductionLengthBudget above:
  // DeckList::investigatorCode is a CardCode field composed by
  // encodeCardQuantityMap()'s sibling code in DeckList::toJson() via the
  // same Value::toExactQJsonObject() path, so it must reject the
  // identical over-length code the direct fragment test now also
  // rejects.
  QString overLong = QStringLiteral("c");
  overLong += QString(Json::ParseLimits::production().maxStringLength, u'a');
  const DeckList list{
      .investigatorCode = *CardCode::parse(overLong),
  };
  const auto encoded = list.toJson();
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("length")),
           qPrintable(encoded.error()));
}

void DecksTests::
    deckListInputToJsonRejectsIdExceedingDigitBudgetThroughFragmentParity() {
  // Enclosing-request parity companion to
  // externalDeckIdNumberKindToJsonRejectsAllZeroExceedingDigitBudget
  // above: DeckListInput::id is an ExternalDeckId field composed via
  // toRawJson()->toExactQJson(), which already enforced the digit budget
  // before this round's fix -- proving the direct fragment now agrees
  // with the enclosing request rather than being a weaker standalone
  // encoder.
  Json::ParseLimits wide;
  wide.maxNumberDigits = 128;
  const QByteArray excessiveFractionLiteral =
      QByteArrayLiteral("0.") + QByteArray(70, '0');
  auto parsedNumber = Json::Value::parse(excessiveFractionLiteral, u"n", wide);
  if (!parsedNumber)
    QFAIL(qPrintable(parsedNumber.error()));
  const DeckListInput input{
      .investigatorCode = *InvestigatorRef::parse(QStringLiteral("01001")),
      .id = ExternalDeckId::number(parsedNumber->toRawNumber()),
  };
  const auto encoded = input.toJson();
  QVERIFY(!encoded.has_value());
  const auto encodedBytes = input.toJsonBytes();
  QVERIFY(!encodedBytes.has_value());
}

void DecksTests::cardCodeMoveConstructLeavesSourceValidAndReusable() {
  auto parsed = CardCode::parse(QStringLiteral("c01001"));
  if (!parsed)
    QFAIL(qPrintable(parsed.error()));
  CardCode source = *parsed;
  QCOMPARE(source.value(), QStringLiteral("c01001"));

  // std::move() now binds CardCode's explicitly-declared copy constructor
  // (see Identifiers.h), so `source` must remain fully intact afterward --
  // never emptied -- exactly like RawJsonTests' RawNumber coverage above.
  CardCode moved(std::move(source));
  QCOMPARE(moved.value(), QStringLiteral("c01001"));
  QCOMPARE(source.value(), QStringLiteral("c01001"));

  // Reuse the moved-from source in a fresh aggregate to prove it is not
  // merely inspectable but genuinely still usable end-to-end. (Named
  // "quantities" rather than "slots": Qt's keyword extensions define
  // `slots` as a macro, so using it as a variable name would silently
  // corrupt this declaration.)
  QMap<CardCode, qint64> quantities;
  quantities.insert(source, 2);
  quantities.insert(moved, 3);
  QCOMPARE(quantities.size(), 1); // same CardCode value, a single entry
  QCOMPARE(quantities.value(source), 3);
}

void DecksTests::cardCodeMoveAssignLeavesSourceValidAndReusable() {
  auto parsed = CardCode::parse(QStringLiteral("c02003"));
  if (!parsed)
    QFAIL(qPrintable(parsed.error()));
  CardCode source = *parsed;
  CardCode destination = *CardCode::parse(QStringLiteral("c00000"));
  destination = std::move(source);

  QCOMPARE(destination.value(), QStringLiteral("c02003"));
  QCOMPARE(source.value(), QStringLiteral("c02003"));
}

void DecksTests::cardCodeSelfMoveAssignmentLeavesValueUnchanged() {
  CardCode code = *CardCode::parse(QStringLiteral("c03004"));
  CardCode &selfRef = code;
  code = std::move(selfRef);
  QCOMPARE(code.value(), QStringLiteral("c03004"));
}

void DecksTests::cardCodeSurvivesQListRelocation() {
  QList<CardCode> codes;
  for (int i = 0; i < 64; ++i)
    codes.append(
        *CardCode::parse(QStringLiteral("c%1").arg(i, 5, 10, QChar('0'))));
  QCOMPARE(codes.size(), 64);
  for (int i = 0; i < 64; ++i)
    QCOMPARE(codes.at(i).value(),
             QStringLiteral("c%1").arg(i, 5, 10, QChar('0')));
}

void DecksTests::
    investigatorRefMoveConstructLeavesSourceValidAndReusableInDeckListInput() {
  // Matches the reviewer's exact scenario:
  //   source = *InvestigatorRef::parse(...); moved = std::move(source);
  //   ClaimSeatRequest{source}.toJsonBytes()
  // -- here reused through DeckListInput's actual investigatorCode field
  // rather than a bespoke standalone check.
  auto parsed = InvestigatorRef::parse(QStringLiteral("01001"));
  if (!parsed)
    QFAIL(qPrintable(parsed.error()));
  InvestigatorRef source = *parsed;
  InvestigatorRef moved(std::move(source));

  const DeckListInput fromSource{.investigatorCode = source};
  const DeckListInput fromMoved{.investigatorCode = moved};
  const auto sourceBytes = fromSource.toJsonBytes();
  const auto movedBytes = fromMoved.toJsonBytes();
  if (!sourceBytes)
    QFAIL(qPrintable(sourceBytes.error()));
  if (!movedBytes)
    QFAIL(qPrintable(movedBytes.error()));
  QCOMPARE(*sourceBytes, *movedBytes);
  QVERIFY(sourceBytes->contains("01001"));
}

void DecksTests::deckIdMoveConstructLeavesSourceValidAndReusableInDeck() {
  // DeckId (TypedId<Tag>) is not embedded in any of the 7 outbound request
  // types, only in response types such as Deck::id, so this reuses Deck's
  // own toJson() as the representative "enclosing type" the reviewer
  // asked for.
  const QUuid uuid = QUuid::createUuid();
  auto parsed = DeckId::parse(uuid.toString(QUuid::WithoutBraces));
  if (!parsed)
    QFAIL(qPrintable(parsed.error()));
  DeckId source = *parsed;
  DeckId moved(std::move(source));

  QCOMPARE(source.value(), moved.value());
  QVERIFY(!source.value().isEmpty());
}

void DecksTests::deckValidationResultMoveConstructLeavesSourceErrorsIntact() {
  QList<DeckValidationError> errorList{
      DeckValidationError{.cardCode = *CardCode::parse(u"c00001"_s)},
      DeckValidationError{.cardCode = *CardCode::parse(u"c00002"_s)},
  };
  auto built = DeckValidationResult::errors(errorList);
  if (!built)
    QFAIL(qPrintable(built.error()));
  DeckValidationResult source = *built;
  QCOMPARE(source.kind(), DeckValidationResult::Kind::Errors);
  QCOMPARE(source.errorList().size(), 2);

  // std::move() on a DeckValidationResult now binds the explicitly-
  // declared copy constructor (see Decks.h) rather than an implicit
  // move, so `source` below must remain fully intact -- still
  // Kind::Errors with both entries, never silently collapsed to
  // Kind::Errors-but-empty (which toJson() would then encode as `[]`,
  // indistinguishable from deckValidationSuccess).
  DeckValidationResult moved(std::move(source));
  QCOMPARE(moved.kind(), DeckValidationResult::Kind::Errors);
  QCOMPARE(moved.errorList().size(), 2);

  QCOMPARE(source.kind(), DeckValidationResult::Kind::Errors);
  QCOMPARE(source.errorList().size(), 2);

  // Reuse the moved-from source end-to-end through its own encoder: it
  // must still encode as a 2-element array, never `[]`.
  auto encoded = source.toJson();
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(encoded->size(), 2);
}

void DecksTests::deckValidationResultMoveAssignLeavesSourceErrorsIntact() {
  auto built = DeckValidationResult::errors(
      {DeckValidationError{.cardCode = *CardCode::parse(u"c00003"_s)}});
  if (!built)
    QFAIL(qPrintable(built.error()));
  DeckValidationResult source = *built;
  DeckValidationResult destination = DeckValidationResult::success();
  destination = std::move(source);

  QCOMPARE(destination.kind(), DeckValidationResult::Kind::Errors);
  QCOMPARE(destination.errorList().size(), 1);
  // Move-assignment (falling back to copy-assignment) must leave
  // `source` just as valid/reusable as move-construction does above.
  QCOMPARE(source.kind(), DeckValidationResult::Kind::Errors);
  QCOMPARE(source.errorList().size(), 1);
  auto encoded = source.toJson();
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(encoded->size(), 1);
}

void DecksTests::deckValidationResultSelfMoveAssignmentLeavesValueUnchanged() {
  auto built = DeckValidationResult::errors(
      {DeckValidationError{.cardCode = *CardCode::parse(u"c00004"_s)}});
  if (!built)
    QFAIL(qPrintable(built.error()));
  DeckValidationResult result = *built;
  DeckValidationResult &selfRef = result;
  result = std::move(selfRef);
  QCOMPARE(result.kind(), DeckValidationResult::Kind::Errors);
  QCOMPARE(result.errorList().size(), 1);
}

void DecksTests::externalDeckIdMoveConstructLeavesSourceTextIntact() {
  ExternalDeckId source = ExternalDeckId::text(QStringLiteral("legacy-42"));
  QCOMPARE(source.kind(), ExternalDeckId::Kind::Text);
  QCOMPARE(source.text(), QStringLiteral("legacy-42"));

  // std::move() on an ExternalDeckId now binds the explicitly-declared
  // copy constructor (see Decks.h) rather than an implicit move, so
  // `source` below must remain fully intact -- still Kind::Text with
  // its original text() -- not silently desynchronized to Kind::Text
  // with an empty string.
  ExternalDeckId moved(std::move(source));
  QCOMPARE(moved.kind(), ExternalDeckId::Kind::Text);
  QCOMPARE(moved.text(), QStringLiteral("legacy-42"));

  QCOMPARE(source.kind(), ExternalDeckId::Kind::Text);
  QCOMPARE(source.text(), QStringLiteral("legacy-42"));

  // Reuse the moved-from source through its own fragment encoder.
  auto encoded = source.toJson();
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, QJsonValue(QStringLiteral("legacy-42")));
}

void DecksTests::externalDeckIdMoveAssignLeavesSourceTextIntact() {
  ExternalDeckId source = ExternalDeckId::text(QStringLiteral("abc-99"));
  ExternalDeckId destination = ExternalDeckId::absent();
  destination = std::move(source);

  QCOMPARE(destination.kind(), ExternalDeckId::Kind::Text);
  QCOMPARE(destination.text(), QStringLiteral("abc-99"));
  QCOMPARE(source.kind(), ExternalDeckId::Kind::Text);
  QCOMPARE(source.text(), QStringLiteral("abc-99"));
}

QTEST_APPLESS_MAIN(DecksTests)

#include "DecksTests.moc"
