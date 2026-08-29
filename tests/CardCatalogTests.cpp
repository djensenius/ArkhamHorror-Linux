#include <QFile>
#include <QJsonDocument>
#include <QtTest>

#include "CardCatalog.h"
#include "RawJson.h"

using namespace Arkham;
using namespace Qt::StringLiterals;

namespace {
// Test-only convenience conversion standing in for a real byte-parsed
// Json::Value (see RawJson.h): CardCost/SkillIcon/GameValue's raw payload
// storage is the lossless AST, not QJsonValue, so tests comparing against
// a literal QJsonValue/QJsonObject fixture must first convert it the same
// way. Fails via qFatal only for a malformed *test fixture* (never
// production code), per this project's convention that qFatal is
// permitted solely for impossible-to-reach test setup.
Json::Value toRawJson(const QJsonValue &v) {
  auto result = Json::Value::fromQJson(v);
  if (!result)
    qFatal("test fixture construction must not fail: %s",
           qPrintable(result.error()));
  return *result;
}
} // namespace

class CardCatalogTests final : public QObject {
  Q_OBJECT

private slots:
  // Fixture round trips ─────────────────────────────────────────────────────
  void decodesFullCardFromFixture();
  void decodesHomebrewCardFromFixture();
  void decodesMinimalInvestigatorCardFromFixture();

  // Required fields ─────────────────────────────────────────────────────────
  void missingCardCodeRejected();
  void missingNameRejected();
  void missingCardTypeRejected();
  void missingArtRejected();
  void emptyArtRejected();
  void wrongTypeForCardCodeRejected();
  void wrongTypeForCostRejected();
  void wholeValueNotAnObjectRejected();

  // CardCode ─────────────────────────────────────────────────────────────────
  void officialCardCodeAccepted();
  void homebrewCardCodeAccepted();
  void cardCodeWithoutCPrefixRejected();
  void bareCCardCodeRejected();
  void cardCodeWithEmbeddedLineTerminatorRejected();
  void cardCodeWithTrailingLineTerminatorRejected();
  void cardCodeWithSupplementaryPlaneCharacterAccepted();

  // Closed enums ─────────────────────────────────────────────────────────────
  void unrecognizedCardTypeRejected();
  void unrecognizedClassSymbolRejected();

  // Tagged variants (cardCost/gameValue/skillIcon) ──────────────────────────
  void allCardCostVariantsRoundTrip();
  void allGameValueVariantsRoundTrip();
  void allSkillIconVariantsRoundTrip();
  void unrecognizedCardCostTagPreservedNotRejected();
  void unrecognizedGameValueTagPreservedNotRejected();
  void unrecognizedSkillIconTagPreservedNotRejected();
  void missingContentsRejectedForRawPayloadCardCostTags();
  void rawPayloadCardCostFactoriesValidateContents();
  void nullaryCardCostTagWithContentsRejected();
  void nullaryGameValueTagWithContentsRejected();
  void nullarySkillIconTagWithContentsRejected();

  // Forward compatibility ────────────────────────────────────────────────────
  void unknownAdditiveTopLevelFieldIgnored();
  void unconstrainedFieldsPreservedVerbatim();

  // Collections ──────────────────────────────────────────────────────────────
  void bondedWithRoundTrips();
  void alternateSkillsAndErrataRoundTrip();

  // MEDIUM #6: strictly-typed optional scalar fields (null not permitted) ──
  void explicitNullForNonNullableIntFieldRejected();
  void explicitNullForNonNullableBoolFieldRejected();
  void explicitNullForNonNullableStringFieldRejected();
  void absentNonNullableScalarFieldsDecodeToNullopt();

  // MEDIUM #6: outer-typed-but-inner-unconstrained array/object fields ─────
  void wrongOuterTypeForArrayFieldRejected_data();
  void wrongOuterTypeForArrayFieldRejected();
  void explicitNullForArrayFieldRejected();
  void wrongOuterTypeForObjectFieldRejected();
  void arrayAndObjectFieldsPreservedVerbatimWhenOuterTypeValid();

  // MEDIUM #6: uniqueItems enforcement ──────────────────────────────────────
  void duplicateClassSymbolsRejected();
  void duplicateCardTraitsRejected();
  void duplicateRevealedCardTraitsRejected();
  void duplicateTagsAllowedNoUniquenessConstraint();

  // Canonical byte-level decode (lossless Json::Value AST, see RawJson.h):
  // fromRawJson()/fromRawBytes() must never round-trip through QJsonValue,
  // so a number nested inside an unconstrained field survives byte-exact
  // even outside qint64 range / with a long fraction / a huge exponent,
  // and a duplicate key nested at any depth is rejected -- neither of
  // which the QJsonValue-based fromJson() path (used only for values a
  // caller already collapsed via QJsonDocument, e.g. legacy call sites)
  // can guarantee.
  void rawBytesPreserveNumericPrecisionInUnconstrainedFields();
  void rawBytesRejectDuplicateKeyNestedInUnconstrainedField();
  void decodeCatalogFromRawBytesRoundTripsArray();
};

namespace {

QJsonObject loadFixtureObject(const QString &fileName) {
  QFile f(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + u"/fixtures/" + fileName);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return QJsonDocument::fromJson(f.readAll()).object();
}

QJsonObject parseJson(QLatin1StringView text) {
  return QJsonDocument::fromJson(QByteArray(text.data(), text.size())).object();
}

QJsonObject minimalCardObject() {
  return QJsonObject{
      {QStringLiteral("cardCode"), QStringLiteral("c00001")},
      {QStringLiteral("name"),
       QJsonObject{{QStringLiteral("title"), QStringLiteral("X")},
                   {QStringLiteral("subtitle"), QJsonValue(QJsonValue::Null)}}},
      {QStringLiteral("cardType"), QStringLiteral("AssetType")},
      {QStringLiteral("art"), QStringLiteral("1")},
  };
}

} // namespace

void CardCatalogTests::decodesFullCardFromFixture() {
  const QJsonObject catalog = loadFixtureObject(QStringLiteral("catalog.json"));
  QVERIFY(!catalog.isEmpty());
  const QJsonArray cards = catalog.value("cards"_L1).toArray();
  QCOMPARE(cards.size(), 1);

  const auto result = CardDef::fromJson(cards.at(0), u"cards[0]");
  if (!result)
    QFAIL(qPrintable(result.error()));

  QCOMPARE(result->cardCode.value(), QStringLiteral("c01020"));
  QCOMPARE(result->name.title, QStringLiteral("Machete"));
  QVERIFY(!result->name.subtitle.has_value());
  QVERIFY(result->cost.has_value());
  QCOMPARE(result->cost->tag(), CardCostTag::StaticCost);
  QCOMPARE(*result->cost->staticAmount(), qint64(3));
  QCOMPARE(*result->level, qint64(0));
  QCOMPARE(result->cardType, CardType::AssetType);
  QCOMPARE(result->classSymbols, (QList<ClassSymbol>{ClassSymbol::Guardian}));
  QCOMPARE(result->skills.size(), 1);
  QCOMPARE(result->skills.at(0).tag(), SkillIconTag::SkillIcon);
  QCOMPARE(*result->skills.at(0).skill(), SkillType::SkillCombat);
  QCOMPARE(result->cardTraits,
           (QStringList{u"Item"_s, u"Melee"_s, u"Weapon"_s}));
  QCOMPARE(result->cardSlots, (QList<SlotType>{SlotType::HandSlot}));
  QCOMPARE(result->alternateCardCodes.size(), 2);
  QCOMPARE(result->alternateCardCodes.at(0).value(), QStringLiteral("c01520"));
  QCOMPARE(result->alternateCardCodes.at(1).value(), QStringLiteral("c12020"));
  QCOMPARE(result->art, QStringLiteral("01020"));
  QCOMPARE(result->alternateErrata.size(), 1);
  QVERIFY(result->alternateErrata.contains(QStringLiteral("c12020")));

  // Byte-faithful round trip (QJsonObject equality is key-set and value
  // exact, independent only of key ordering).
  QCOMPARE(result->toJson(), cards.at(0).toObject());
}

void CardCatalogTests::decodesHomebrewCardFromFixture() {
  const QJsonObject catalog = loadFixtureObject(QStringLiteral("catalog.json"));
  const QJsonArray homebrew = catalog.value("homebrewCards"_L1).toArray();
  QCOMPARE(homebrew.size(), 1);

  const auto result = CardDef::fromJson(homebrew.at(0), u"homebrewCards[0]");
  if (!result)
    QFAIL(qPrintable(result.error()));

  QCOMPARE(result->cardCode.value(), QStringLiteral("c:dark-matter:151"));
  QCOMPARE(result->cardType, CardType::EnemyType);
  QCOMPARE(*result->encounterSet,
           QStringLiteral(":dark-matter:in_the_shadow_of_earth"));
  QCOMPARE(*result->encounterSetQuantity, qint64(3));
  QVERIFY(result->health.has_value());
  QCOMPARE(result->health->tag(), GameValueTag::Static);
  QCOMPARE(*result->health->singleAmount(), qint64(1));
  QCOMPARE(result->fight->tag(), GameValueTag::Static);
  QCOMPARE(*result->fight->singleAmount(), qint64(1));
  QCOMPARE(*result->evade->singleAmount(), qint64(3));
  QCOMPARE(*result->healthDamage->singleAmount(), qint64(1));
  QVERIFY(!result->sanityDamage.has_value());

  QCOMPARE(result->toJson(), homebrew.at(0).toObject());
}

void CardCatalogTests::decodesMinimalInvestigatorCardFromFixture() {
  const QJsonObject catalog = loadFixtureObject(QStringLiteral("catalog.json"));
  const QJsonValue card = catalog.value("card"_L1);
  QVERIFY(!card.isUndefined());

  const auto result = CardDef::fromJson(card, u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));

  QCOMPARE(result->cardCode.value(), QStringLiteral("c01001"));
  QCOMPARE(result->name.title, QStringLiteral("Roland Banks"));
  QCOMPARE(*result->name.subtitle, QStringLiteral("The Fed"));
  QCOMPARE(result->cardType, CardType::InvestigatorType);
  QVERIFY(*result->unique);
  // Every optional not present in the fixture stays unset.
  QVERIFY(!result->cost.has_value());
  QVERIFY(!result->level.has_value());
  QVERIFY(!result->cardSubType.has_value());
  QVERIFY(result->skills.isEmpty());
  QVERIFY(result->cardSlots.isEmpty());

  QCOMPARE(result->toJson(), card.toObject());
}

void CardCatalogTests::missingCardCodeRejected() {
  const QJsonObject obj = parseJson(R"({
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1"
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("cardCode")));
}

void CardCatalogTests::missingNameRejected() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "cardType": "AssetType",
    "art": "1"
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("name")));
}

void CardCatalogTests::missingCardTypeRejected() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "art": "1"
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("cardType")));
}

void CardCatalogTests::missingArtRejected() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType"
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  // Asserts the exact, clearer "missing required field" phrasing (shared
  // with requireRawField/requireNullable*) rather than the less specific
  // "expected string, got missing" a bare value-type check would produce.
  QCOMPARE(result.error(),
           QStringLiteral("card.art: missing required field \"art\""));
}

void CardCatalogTests::emptyArtRejected() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": ""
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("art")));
}

void CardCatalogTests::wrongTypeForCardCodeRejected() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": 12345,
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1"
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("cardCode")),
           qPrintable(result.error()));
  QVERIFY2(result.error().contains(QStringLiteral("number")),
           qPrintable(result.error()));
}

void CardCatalogTests::wrongTypeForCostRejected() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "cost": 3
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("cost")),
           qPrintable(result.error()));
}

void CardCatalogTests::wholeValueNotAnObjectRejected() {
  const auto result =
      CardDef::fromJson(QJsonValue(QStringLiteral("nope")), u"card");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("object")));
}

void CardCatalogTests::officialCardCodeAccepted() {
  const auto result = CardCode::parse(QStringLiteral("c01020"));
  QVERIFY(result.has_value());
  QCOMPARE(result->value(), QStringLiteral("c01020"));
}

void CardCatalogTests::homebrewCardCodeAccepted() {
  const auto result = CardCode::parse(QStringLiteral("c:homebrew:151"));
  QVERIFY(result.has_value());
  QCOMPARE(result->value(), QStringLiteral("c:homebrew:151"));
}

void CardCatalogTests::cardCodeWithoutCPrefixRejected() {
  const auto result = CardCode::parse(QStringLiteral("01020"));
  QVERIFY(!result.has_value());
}

void CardCatalogTests::bareCCardCodeRejected() {
  // 'c' with nothing after it fails the `^c.+$` pattern.
  const auto result = CardCode::parse(QStringLiteral("c"));
  QVERIFY(!result.has_value());
}

void CardCatalogTests::cardCodeWithEmbeddedLineTerminatorRejected() {
  // `^c.+$` under plain ECMA-262 semantics: `.` excludes line terminators
  // (LF, CR, U+2028, U+2029) wherever they occur, not just at the end, so
  // a code embedding one mid-string must be rejected -- not silently
  // truncated or accepted.
  for (const QChar terminator :
       {QChar(u'\n'), QChar(u'\r'), QChar(0x2028), QChar(0x2029)}) {
    const QString code =
        QStringLiteral("c01") + terminator + QStringLiteral("020");
    const auto result = CardCode::parse(code);
    QVERIFY2(!result.has_value(),
             qPrintable(QStringLiteral("terminator U+%1 unexpectedly accepted")
                            .arg(static_cast<uint>(terminator.unicode()), 4, 16,
                                 QChar(u'0'))));
  }
}

void CardCatalogTests::cardCodeWithTrailingLineTerminatorRejected() {
  // Unlike some regex flavors' `$` (which may match just before a trailing
  // "\n"), ECMA-262 `$` without the multiline flag matches only the true
  // end of the string, so a trailing terminator is not specially exempted
  // either.
  const auto result = CardCode::parse(QStringLiteral("c01020\n"));
  QVERIFY(!result.has_value());
}

void CardCatalogTests::cardCodeWithSupplementaryPlaneCharacterAccepted() {
  // A supplementary-plane character (outside the BMP) is represented as a
  // UTF-16 surrogate pair -- two code units, each individually matched by
  // `.` (neither half is a line terminator) -- so this is accepted with no
  // special surrogate-pair handling required, confirming the code-unit
  // (not code-point) matching semantics do not spuriously reject valid
  // input either. U+1F0A1 (PLAYING CARD ACE OF SPADES) is used as a
  // representative supplementary-plane character.
  QString code = QStringLiteral("c");
  code.append(QChar::highSurrogate(0x1F0A1));
  code.append(QChar::lowSurrogate(0x1F0A1));
  const auto result = CardCode::parse(code);
  QVERIFY(result.has_value());
  QCOMPARE(result->value(), code);
}

void CardCatalogTests::unrecognizedCardTypeRejected() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "SomeFutureCardType",
    "art": "1"
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("cardType")),
           qPrintable(result.error()));
}

void CardCatalogTests::unrecognizedClassSymbolRejected() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "classSymbols": ["NotARealClass"]
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("classSymbols")),
           qPrintable(result.error()));
}

void CardCatalogTests::allCardCostVariantsRoundTrip() {
  const QList<QLatin1StringView> noPayload{
      "DynamicCost"_L1, "DiscardAmountCost"_L1, "DeferredCost"_L1};
  for (const auto &tag : noPayload) {
    const QJsonObject obj{{QStringLiteral("tag"), QString(tag)}};
    const auto result = CardCost::fromJson(obj, u"cost");
    if (!result)
      QFAIL(qPrintable(result.error()));
    QCOMPARE(result->toJson(), obj);
  }

  // MaxDynamicCost/AnyMatchingCardCost preserve their raw payload verbatim.
  const QList<QLatin1StringView> rawPayload{"MaxDynamicCost"_L1,
                                            "AnyMatchingCardCost"_L1};
  for (const auto &tag : rawPayload) {
    const QJsonObject obj{
        {QStringLiteral("tag"), QString(tag)},
        {QStringLiteral("contents"),
         QJsonObject{{QStringLiteral("nested"), QJsonArray{1, 2, 3}}}}};
    const auto result = CardCost::fromJson(obj, u"cost");
    if (!result)
      QFAIL(qPrintable(result.error()));
    QCOMPARE(result->toJson(), obj);
  }

  const QJsonArray matchingEnemyFieldContents{
      QJsonObject{{QStringLiteral("enemy"), QStringLiteral("Ghoul")}},
      QJsonArray{1, 2, 3}};
  const QJsonObject matchingEnemyFieldObj{
      {QStringLiteral("tag"), QStringLiteral("MatchingEnemyFieldCost")},
      {QStringLiteral("contents"), matchingEnemyFieldContents}};
  const auto matchingEnemyFieldResult =
      CardCost::fromJson(matchingEnemyFieldObj, u"cost");
  if (!matchingEnemyFieldResult)
    QFAIL(qPrintable(matchingEnemyFieldResult.error()));
  QVERIFY(matchingEnemyFieldResult->rawContents() ==
          toRawJson(QJsonValue(matchingEnemyFieldContents)));
  QCOMPARE(matchingEnemyFieldResult->toJson(), matchingEnemyFieldObj);

  const QJsonObject staticObj{
      {QStringLiteral("tag"), QStringLiteral("StaticCost")},
      {QStringLiteral("contents"), 5}};
  const auto staticResult = CardCost::fromJson(staticObj, u"cost");
  if (!staticResult)
    QFAIL(qPrintable(staticResult.error()));
  QCOMPARE(*staticResult->staticAmount(), qint64(5));
  QCOMPARE(staticResult->toJson(), staticObj);
}

void CardCatalogTests::allGameValueVariantsRoundTrip() {
  const QJsonObject staticObj{{QStringLiteral("tag"), QStringLiteral("Static")},
                              {QStringLiteral("contents"), 4}};
  auto r1 = GameValue::fromJson(staticObj, u"gv");
  if (!r1)
    QFAIL(qPrintable(r1.error()));
  QCOMPARE(r1->toJson(), staticObj);

  const QJsonObject perPlayerObj{
      {QStringLiteral("tag"), QStringLiteral("PerPlayer")},
      {QStringLiteral("contents"), 2}};
  auto r2 = GameValue::fromJson(perPlayerObj, u"gv");
  if (!r2)
    QFAIL(qPrintable(r2.error()));
  QCOMPARE(r2->toJson(), perPlayerObj);

  const QJsonObject staticWithPerPlayerObj{
      {QStringLiteral("tag"), QStringLiteral("StaticWithPerPlayer")},
      {QStringLiteral("contents"), QJsonArray{3, 1}}};
  auto r3 = GameValue::fromJson(staticWithPerPlayerObj, u"gv");
  if (!r3)
    QFAIL(qPrintable(r3.error()));
  QCOMPARE(r3->contents(), (QList<qint64>{qint64(3), qint64(1)}));
  QCOMPARE(r3->toJson(), staticWithPerPlayerObj);

  const QJsonObject byPlayerCountObj{
      {QStringLiteral("tag"), QStringLiteral("ByPlayerCount")},
      {QStringLiteral("contents"), QJsonArray{2, 3, 4, 5}}};
  auto r4 = GameValue::fromJson(byPlayerCountObj, u"gv");
  if (!r4)
    QFAIL(qPrintable(r4.error()));
  QCOMPARE(r4->contents(),
           (QList<qint64>{qint64(2), qint64(3), qint64(4), qint64(5)}));
  QCOMPARE(r4->toJson(), byPlayerCountObj);

  for (const auto &tag : {"ValueX"_L1, "ValueStar"_L1, "ValueUnknown"_L1}) {
    const QJsonObject obj{{QStringLiteral("tag"), QString(tag)}};
    auto r = GameValue::fromJson(obj, u"gv");
    if (!r)
      QFAIL(qPrintable(r.error()));
    QCOMPARE(r->toJson(), obj);
  }

  const QJsonObject badStaticWithPerPlayerCount{
      {QStringLiteral("tag"), QStringLiteral("StaticWithPerPlayer")},
      {QStringLiteral("contents"), QJsonArray{1}}};
  auto badStaticWithPerPlayer =
      GameValue::fromJson(badStaticWithPerPlayerCount, u"gv");
  QVERIFY(!badStaticWithPerPlayer.has_value());

  // Wrong element count is rejected, not silently truncated/padded.
  const QJsonObject badCount{
      {QStringLiteral("tag"), QStringLiteral("ByPlayerCount")},
      {QStringLiteral("contents"), QJsonArray{1, 2}}};
  auto bad = GameValue::fromJson(badCount, u"gv");
  QVERIFY(!bad.has_value());
}

void CardCatalogTests::allSkillIconVariantsRoundTrip() {
  for (const auto &[tag, skill] :
       {std::pair{"SkillWillpower"_L1, SkillType::SkillWillpower},
        std::pair{"SkillIntellect"_L1, SkillType::SkillIntellect},
        std::pair{"SkillCombat"_L1, SkillType::SkillCombat},
        std::pair{"SkillAgility"_L1, SkillType::SkillAgility}}) {
    const QJsonObject obj{{QStringLiteral("tag"), QStringLiteral("SkillIcon")},
                          {QStringLiteral("contents"), QString(tag)}};
    auto r = SkillIcon::fromJson(obj, u"skill");
    if (!r)
      QFAIL(qPrintable(r.error()));
    QCOMPARE(*r->skill(), skill);
    QCOMPARE(r->toJson(), obj);
  }

  for (const auto &tag : {"WildIcon"_L1, "WildMinusIcon"_L1}) {
    const QJsonObject obj{{QStringLiteral("tag"), QString(tag)}};
    auto r = SkillIcon::fromJson(obj, u"skill");
    if (!r)
      QFAIL(qPrintable(r.error()));
    QVERIFY(!r->skill().has_value());
    QCOMPARE(r->toJson(), obj);
  }

  const QJsonObject badSkillPayload{
      {QStringLiteral("tag"), QStringLiteral("SkillIcon")},
      {QStringLiteral("contents"),
       QJsonObject{{QStringLiteral("unexpected"), true}}}};
  auto bad = SkillIcon::fromJson(badSkillPayload, u"skill");
  QVERIFY(!bad.has_value());
}

void CardCatalogTests::unrecognizedCardCostTagPreservedNotRejected() {
  // Card content is added with nearly every release -- a decoded-but-
  // unrecognized cardCost must survive round-tripping instead of failing
  // outright, and must never be mistaken for any known tag.
  const QJsonObject withContents{
      {QStringLiteral("tag"), QStringLiteral("SomeFutureCostTag")},
      {QStringLiteral("contents"), QJsonObject{{QStringLiteral("x"), 1}}}};
  const auto result = CardCost::fromJson(withContents, u"cost");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->tag(), CardCostTag::Unknown);
  QVERIFY(!result->staticAmount().has_value());
  QVERIFY(result->unknownRaw() == toRawJson(withContents));
  QCOMPARE(result->toJson(), withContents);

  const QJsonObject withoutContents{
      {QStringLiteral("tag"), QStringLiteral("AnotherFutureCostTag")}};
  const auto result2 = CardCost::fromJson(withoutContents, u"cost");
  if (!result2)
    QFAIL(qPrintable(result2.error()));
  QCOMPARE(result2->tag(), CardCostTag::Unknown);
  QVERIFY(result2->unknownRaw() == toRawJson(withoutContents));
  QCOMPARE(result2->toJson(), withoutContents);

  // There is no public factory that lets calling code fabricate an
  // Unknown-kind CardCost directly.
  QVERIFY(*result != CardCost::dynamicCost());
}

void CardCatalogTests::unrecognizedGameValueTagPreservedNotRejected() {
  const QJsonObject obj{{QStringLiteral("tag"), QStringLiteral("ValueFuture")},
                        {QStringLiteral("contents"), 42}};
  const auto result = GameValue::fromJson(obj, u"gv");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->tag(), GameValueTag::Unknown);
  // Must never be conflated with the *known* nullary tag literally named
  // "ValueUnknown".
  QVERIFY(result->tag() != GameValueTag::ValueUnknown);
  QVERIFY(result->unknownRaw() == toRawJson(obj));
  QCOMPARE(result->toJson(), obj);
}

void CardCatalogTests::unrecognizedSkillIconTagPreservedNotRejected() {
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("SomeFutureIcon")}};
  const auto result = SkillIcon::fromJson(obj, u"skill");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->tag(), SkillIconTag::Unknown);
  QVERIFY(result->unknownRaw() == toRawJson(obj));
  QCOMPARE(result->toJson(), obj);
}

void CardCatalogTests::nullaryCardCostTagWithContentsRejected() {
  const std::array unexpectedContentsCases{
      std::pair{QStringLiteral("explicit null"), QJsonValue(QJsonValue::Null)},
      std::pair{QStringLiteral("number"), QJsonValue(1)},
  };
  for (const auto &tag :
       {"DynamicCost"_L1, "DiscardAmountCost"_L1, "DeferredCost"_L1}) {
    for (const auto &[label, contents] : unexpectedContentsCases) {
      const QJsonObject obj{{QStringLiteral("tag"), QString(tag)},
                            {QStringLiteral("contents"), contents}};
      const auto result = CardCost::fromJson(obj, u"cost");
      QVERIFY2(!result.has_value(),
               qPrintable(QStringLiteral("%1 unexpectedly accepted %2 "
                                         "contents")
                              .arg(tag, label)));
      QVERIFY2(result.error().contains(QStringLiteral("contents")),
               qPrintable(result.error()));
    }
  }
}

void CardCatalogTests::nullaryGameValueTagWithContentsRejected() {
  const std::array unexpectedContentsCases{
      std::pair{QStringLiteral("explicit null"), QJsonValue(QJsonValue::Null)},
      std::pair{QStringLiteral("array"), QJsonValue(QJsonArray{1, 2})},
  };
  for (const auto &tag : {"ValueX"_L1, "ValueStar"_L1, "ValueUnknown"_L1}) {
    for (const auto &[label, contents] : unexpectedContentsCases) {
      const QJsonObject obj{{QStringLiteral("tag"), QString(tag)},
                            {QStringLiteral("contents"), contents}};
      const auto result = GameValue::fromJson(obj, u"gv");
      QVERIFY2(!result.has_value(),
               qPrintable(QStringLiteral("%1 unexpectedly accepted %2 "
                                         "contents")
                              .arg(tag, label)));
      QVERIFY2(result.error().contains(QStringLiteral("contents")),
               qPrintable(result.error()));
    }
  }
}

void CardCatalogTests::nullarySkillIconTagWithContentsRejected() {
  const std::array unexpectedContentsCases{
      std::pair{QStringLiteral("explicit null"), QJsonValue(QJsonValue::Null)},
      std::pair{QStringLiteral("object"),
                QJsonValue(QJsonObject{{QStringLiteral("unexpected"), true}})},
  };
  for (const auto &tag : {"WildIcon"_L1, "WildMinusIcon"_L1}) {
    for (const auto &[label, contents] : unexpectedContentsCases) {
      const QJsonObject obj{{QStringLiteral("tag"), QString(tag)},
                            {QStringLiteral("contents"), contents}};
      const auto result = SkillIcon::fromJson(obj, u"skill");
      QVERIFY2(!result.has_value(),
               qPrintable(QStringLiteral("%1 unexpectedly accepted %2 "
                                         "contents")
                              .arg(tag, label)));
      QVERIFY2(result.error().contains(QStringLiteral("contents")),
               qPrintable(result.error()));
    }
  }
}

void CardCatalogTests::missingContentsRejectedForRawPayloadCardCostTags() {
  // MaxDynamicCost/AnyMatchingCardCost/MatchingEnemyFieldCost's `contents`
  // is schema-unconstrained but still required; omitting the key entirely
  // must fail to decode rather than silently succeeding with an
  // undefined/absent rawContents that would then re-encode without the
  // required key.
  const QList<QLatin1StringView> rawPayload{"MaxDynamicCost"_L1,
                                            "AnyMatchingCardCost"_L1,
                                            "MatchingEnemyFieldCost"_L1};
  for (const auto &tag : rawPayload) {
    const QJsonObject obj{{QStringLiteral("tag"), QString(tag)}};
    const auto result = CardCost::fromJson(obj, u"cost");
    QVERIFY2(!result.has_value(),
             qPrintable(QStringLiteral("%1 unexpectedly decoded without a "
                                       "contents field")
                            .arg(tag)));
    QVERIFY2(result.error().contains(QStringLiteral("contents")),
             qPrintable(result.error()));
  }

  // An explicit JSON null still counts as present for the genuinely
  // unconstrained single-payload constructors.
  for (const auto &tag : {"MaxDynamicCost"_L1, "AnyMatchingCardCost"_L1}) {
    const QJsonObject nullContents{
        {QStringLiteral("tag"), QString(tag)},
        {QStringLiteral("contents"), QJsonValue(QJsonValue::Null)}};
    const auto nullResult = CardCost::fromJson(nullContents, u"cost");
    if (!nullResult)
      QFAIL(qPrintable(nullResult.error()));
    QVERIFY(nullResult->rawContents().isNull());
    QCOMPARE(nullResult->toJson(), nullContents);
  }

  // MatchingEnemyFieldCost has a real fixed-arity wire shape, so null is
  // not a valid stand-in for a missing/unknown payload.
  const QJsonObject nullMatchingEnemyField{
      {QStringLiteral("tag"), QStringLiteral("MatchingEnemyFieldCost")},
      {QStringLiteral("contents"), QJsonValue(QJsonValue::Null)}};
  const auto nullMatchingEnemyFieldResult =
      CardCost::fromJson(nullMatchingEnemyField, u"cost");
  QVERIFY(!nullMatchingEnemyFieldResult.has_value());
  QVERIFY2(
      nullMatchingEnemyFieldResult.error().contains(QStringLiteral("exactly")),
      qPrintable(nullMatchingEnemyFieldResult.error()));
}

void CardCatalogTests::rawPayloadCardCostFactoriesValidateContents() {
  // CardCost's payload factories take the lossless Json::Value AST (see
  // RawJson.h), not QJsonValue -- toRawJson() is this test's own
  // convenience conversion, standing in for a real byte-parsed value.
  const Json::Value rawContents =
      toRawJson(QJsonObject{{QStringLiteral("nested"), QJsonArray{1, 2, 3}}});

  const auto maxDynamicCost = CardCost::maxDynamicCost(rawContents);
  if (!maxDynamicCost)
    QFAIL(qPrintable(maxDynamicCost.error()));
  QCOMPARE(maxDynamicCost->tag(), CardCostTag::MaxDynamicCost);
  QVERIFY(maxDynamicCost->rawContents() == rawContents);

  const auto anyMatchingCardCost = CardCost::anyMatchingCardCost(rawContents);
  if (!anyMatchingCardCost)
    QFAIL(qPrintable(anyMatchingCardCost.error()));
  QCOMPARE(anyMatchingCardCost->tag(), CardCostTag::AnyMatchingCardCost);
  QVERIFY(anyMatchingCardCost->rawContents() == rawContents);

  for (const auto &[label, result] : std::array{
           std::pair{QStringLiteral("maxDynamicCost"),
                     CardCost::maxDynamicCost(Json::Value())},
           std::pair{QStringLiteral("anyMatchingCardCost"),
                     CardCost::anyMatchingCardCost(Json::Value())},
       }) {
    QVERIFY2(!result.has_value(),
             qPrintable(QStringLiteral("%1 unexpectedly accepted undefined "
                                       "contents")
                            .arg(label)));
    QVERIFY2(result.error().contains(QStringLiteral("undefined")),
             qPrintable(result.error()));
  }

  const QJsonArray validMatchingEnemyFieldContentsQJson{
      QJsonObject{{QStringLiteral("enemy"), QStringLiteral("Ghoul")}},
      QJsonObject{{QStringLiteral("field"), QStringLiteral("Health")}}};
  const Json::Value validMatchingEnemyFieldContents =
      toRawJson(validMatchingEnemyFieldContentsQJson);
  const auto matchingEnemyFieldCost =
      CardCost::matchingEnemyFieldCost(validMatchingEnemyFieldContents);
  if (!matchingEnemyFieldCost)
    QFAIL(qPrintable(matchingEnemyFieldCost.error()));
  QCOMPARE(matchingEnemyFieldCost->tag(), CardCostTag::MatchingEnemyFieldCost);
  QVERIFY(matchingEnemyFieldCost->rawContents() ==
          validMatchingEnemyFieldContents);
  QCOMPARE(
      matchingEnemyFieldCost->toJson(),
      (QJsonObject{
          {QStringLiteral("tag"), QStringLiteral("MatchingEnemyFieldCost")},
          {QStringLiteral("contents"), validMatchingEnemyFieldContentsQJson},
      }));

  for (const auto &[label, contents] : std::array{
           std::pair{QStringLiteral("one element"),
                     toRawJson(QJsonValue(QJsonArray{1}))},
           std::pair{QStringLiteral("three elements"),
                     toRawJson(QJsonValue(QJsonArray{1, 2, 3}))},
           std::pair{
               QStringLiteral("non-array"),
               toRawJson(QJsonValue(QJsonObject{{QStringLiteral("x"), 1}}))},
       }) {
    const auto result = CardCost::matchingEnemyFieldCost(contents);
    QVERIFY2(
        !result.has_value(),
        qPrintable(QStringLiteral("%1 unexpectedly accepted %2")
                       .arg(QStringLiteral("matchingEnemyFieldCost"), label)));
    QVERIFY2(result.error().contains(QStringLiteral("exactly two elements")),
             qPrintable(result.error()));
  }
}

void CardCatalogTests::unknownAdditiveTopLevelFieldIgnored() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "aFutureFieldThisClientHasNeverHeardOf": {"anything": [1, 2, 3]}
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->cardCode.value(), QStringLiteral("c00001"));
  // Re-encoding never reproduces a field this client never modeled.
  QVERIFY(!result->toJson().contains(
      QStringLiteral("aFutureFieldThisClientHasNeverHeardOf")));
}

void CardCatalogTests::unconstrainedFieldsPreservedVerbatim() {
  const QJsonObject rawMeta{
      {QStringLiteral("anything"), QStringLiteral("goes")},
      {QStringLiteral("nested"), QJsonArray{1, 2}}};
  const QJsonObject obj{
      {QStringLiteral("cardCode"), QStringLiteral("c00001")},
      {QStringLiteral("name"),
       QJsonObject{{QStringLiteral("title"), QStringLiteral("X")},
                   {QStringLiteral("subtitle"), QJsonValue()}}},
      {QStringLiteral("cardType"), QStringLiteral("AssetType")},
      {QStringLiteral("art"), QStringLiteral("1")},
      {QStringLiteral("meta"), rawMeta},
  };
  const auto result = CardDef::fromJson(obj, u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->meta == toRawJson(QJsonValue(rawMeta)));
  QCOMPARE(result->toJson().value(QStringLiteral("meta")).toObject(), rawMeta);
}

void CardCatalogTests::bondedWithRoundTrips() {
  const QJsonObject obj{
      {QStringLiteral("cardCode"), QStringLiteral("c00001")},
      {QStringLiteral("name"),
       QJsonObject{{QStringLiteral("title"), QStringLiteral("X")},
                   {QStringLiteral("subtitle"), QJsonValue()}}},
      {QStringLiteral("cardType"), QStringLiteral("AssetType")},
      {QStringLiteral("art"), QStringLiteral("1")},
      {QStringLiteral("bondedWith"),
       QJsonArray{QJsonArray{2, QStringLiteral("c00002")},
                  QJsonArray{1, QStringLiteral("c00003")}}},
  };
  const auto result = CardDef::fromJson(obj, u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->bondedWith.size(), 2);
  QCOMPARE(result->bondedWith.at(0).first, qint64(2));
  QCOMPARE(result->bondedWith.at(0).second.value(), QStringLiteral("c00002"));
  QCOMPARE(result->toJson(), obj);
}

void CardCatalogTests::alternateSkillsAndErrataRoundTrip() {
  const QJsonObject obj{
      {QStringLiteral("cardCode"), QStringLiteral("c00001")},
      {QStringLiteral("name"),
       QJsonObject{{QStringLiteral("title"), QStringLiteral("X")},
                   {QStringLiteral("subtitle"), QJsonValue()}}},
      {QStringLiteral("cardType"), QStringLiteral("AssetType")},
      {QStringLiteral("art"), QStringLiteral("1")},
      {QStringLiteral("alternateSkills"),
       QJsonObject{{QStringLiteral("c00002"),
                    QJsonArray{QJsonObject{{QStringLiteral("tag"),
                                            QStringLiteral("WildIcon")}}}}}},
      {QStringLiteral("alternateErrata"),
       QJsonObject{{QStringLiteral("c00002"), QStringLiteral("Errata text")}}},
  };
  const auto result = CardDef::fromJson(obj, u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->alternateSkills.size(), 1);
  QVERIFY(result->alternateSkills.contains(QStringLiteral("c00002")));
  QCOMPARE(result->alternateSkills.value(QStringLiteral("c00002")).size(), 1);
  QCOMPARE(result->alternateErrata.value(QStringLiteral("c00002")),
           QStringLiteral("Errata text"));
  QCOMPARE(result->toJson(), obj);
}

void CardCatalogTests::explicitNullForNonNullableIntFieldRejected() {
  // catalog.schema.json types "level" strictly as "integer" (no "null" in
  // its type union) and does not require it -- so an absent key decodes
  // to std::nullopt, but an explicit JSON null is exactly as malformed as
  // a present value of any other wrong type, and must fail rather than
  // silently collapsing into "absent".
  for (const QString &fieldName :
       {QStringLiteral("level"), QStringLiteral("victoryPoints"),
        QStringLiteral("vengeancePoints"),
        QStringLiteral("encounterSetQuantity"), QStringLiteral("stage"),
        QStringLiteral("grantedXp")}) {
    QJsonObject obj = minimalCardObject();
    obj.insert(fieldName, QJsonValue(QJsonValue::Null));
    const auto result = CardDef::fromJson(obj, u"card");
    QVERIFY2(
        !result.has_value(),
        qPrintable(
            QStringLiteral("%1 unexpectedly accepted null").arg(fieldName)));
    QVERIFY2(result.error().contains(fieldName), qPrintable(result.error()));
  }
}

void CardCatalogTests::explicitNullForNonNullableBoolFieldRejected() {
  for (const QString &fieldName :
       {QStringLiteral("overrideActionPlayableIfCriteriaMet"),
        QStringLiteral("permanent"), QStringLiteral("unique"),
        QStringLiteral("doubleSided"), QStringLiteral("exceptional"),
        QStringLiteral("playableFromDiscard"), QStringLiteral("canReplace"),
        QStringLiteral("skipPlayWindows"), QStringLiteral("beforeEffect"),
        QStringLiteral("canCommitWhenNoIcons"),
        QStringLiteral("commitTrigger")}) {
    QJsonObject obj = minimalCardObject();
    obj.insert(fieldName, QJsonValue(QJsonValue::Null));
    const auto result = CardDef::fromJson(obj, u"card");
    QVERIFY2(
        !result.has_value(),
        qPrintable(
            QStringLiteral("%1 unexpectedly accepted null").arg(fieldName)));
    QVERIFY2(result.error().contains(fieldName), qPrintable(result.error()));
  }
}

void CardCatalogTests::explicitNullForNonNullableStringFieldRejected() {
  for (const QString &fieldName :
       {QStringLiteral("encounterSet"), QStringLiteral("errata")}) {
    QJsonObject obj = minimalCardObject();
    obj.insert(fieldName, QJsonValue(QJsonValue::Null));
    const auto result = CardDef::fromJson(obj, u"card");
    QVERIFY2(
        !result.has_value(),
        qPrintable(
            QStringLiteral("%1 unexpectedly accepted null").arg(fieldName)));
    QVERIFY2(result.error().contains(fieldName), qPrintable(result.error()));
  }
}

void CardCatalogTests::absentNonNullableScalarFieldsDecodeToNullopt() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1"
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(!result->level.has_value());
  QVERIFY(!result->victoryPoints.has_value());
  QVERIFY(!result->vengeancePoints.has_value());
  QVERIFY(!result->overrideActionPlayableIfCriteriaMet.has_value());
  QVERIFY(!result->permanent.has_value());
  QVERIFY(!result->encounterSet.has_value());
  QVERIFY(!result->encounterSetQuantity.has_value());
  QVERIFY(!result->unique.has_value());
  QVERIFY(!result->doubleSided.has_value());
  QVERIFY(!result->exceptional.has_value());
  QVERIFY(!result->playableFromDiscard.has_value());
  QVERIFY(!result->stage.has_value());
  QVERIFY(!result->grantedXp.has_value());
  QVERIFY(!result->canReplace.has_value());
  QVERIFY(!result->skipPlayWindows.has_value());
  QVERIFY(!result->beforeEffect.has_value());
  QVERIFY(!result->canCommitWhenNoIcons.has_value());
  QVERIFY(!result->commitTrigger.has_value());
  QVERIFY(!result->errata.has_value());
  // Round trip stays byte-faithful: none of these absent fields reappear.
  const QJsonObject encoded = result->toJson();
  QVERIFY(!encoded.contains(QStringLiteral("level")));
  QVERIFY(!encoded.contains(QStringLiteral("errata")));
}

void CardCatalogTests::wrongOuterTypeForArrayFieldRejected_data() {
  QTest::addColumn<QString>("fieldName");
  QTest::newRow("keywords") << QStringLiteral("keywords");
  QTest::newRow("commitRestrictions") << QStringLiteral("commitRestrictions");
  QTest::newRow("attackOfOpportunityModifiers")
      << QStringLiteral("attackOfOpportunityModifiers");
  QTest::newRow("limits") << QStringLiteral("limits");
  QTest::newRow("locationConnections") << QStringLiteral("locationConnections");
  QTest::newRow("locationRevealedConnections")
      << QStringLiteral("locationRevealedConnections");
  QTest::newRow("deckRestrictions") << QStringLiteral("deckRestrictions");
}

void CardCatalogTests::wrongOuterTypeForArrayFieldRejected() {
  QFETCH(QString, fieldName);
  QJsonObject obj{
      {QStringLiteral("cardCode"), QStringLiteral("c00001")},
      {QStringLiteral("name"),
       QJsonObject{{QStringLiteral("title"), QStringLiteral("X")},
                   {QStringLiteral("subtitle"), QJsonValue()}}},
      {QStringLiteral("cardType"), QStringLiteral("AssetType")},
      {QStringLiteral("art"), QStringLiteral("1")},
  };
  // catalog.schema.json types this field's outer shape as "array"; a
  // string value matches neither "array" nor "null" and must fail rather
  // than being silently preserved as opaque raw JSON.
  obj.insert(fieldName, QStringLiteral("not an array"));
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(fieldName));
}

void CardCatalogTests::explicitNullForArrayFieldRejected() {
  // An explicit JSON null matches neither "array" nor "object", so it must
  // be rejected for these outer-typed fields exactly like any other wrong
  // type -- unlike optionalString/Int/Bool's absent-or-null collapse used
  // for genuinely backend-nullable fields elsewhere.
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "keywords": null
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("keywords")));
}

void CardCatalogTests::wrongOuterTypeForObjectFieldRejected() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "meta": [1, 2, 3]
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("meta")));
}

void CardCatalogTests::
    arrayAndObjectFieldsPreservedVerbatimWhenOuterTypeValid() {
  const QJsonArray rawKeywords{QStringLiteral("surge"), 1, QJsonValue()};
  const QJsonArray rawCommitRestrictions{
      QJsonObject{
          {QStringLiteral("matcher"), QStringLiteral("attacksOfOpportunity")}},
      QJsonArray{true, QStringLiteral("freeform")}};
  const QJsonObject rawMeta{{QStringLiteral("adaptable"), true}};
  const QJsonObject obj{
      {QStringLiteral("cardCode"), QStringLiteral("c00001")},
      {QStringLiteral("name"),
       QJsonObject{{QStringLiteral("title"), QStringLiteral("X")},
                   {QStringLiteral("subtitle"), QJsonValue()}}},
      {QStringLiteral("cardType"), QStringLiteral("AssetType")},
      {QStringLiteral("art"), QStringLiteral("1")},
      {QStringLiteral("keywords"), rawKeywords},
      {QStringLiteral("commitRestrictions"), rawCommitRestrictions},
      {QStringLiteral("meta"), rawMeta},
  };
  const auto result = CardDef::fromJson(obj, u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->keywords == toRawJson(QJsonValue(rawKeywords)));
  QVERIFY(result->commitRestrictions ==
          toRawJson(QJsonValue(rawCommitRestrictions)));
  QVERIFY(result->meta == toRawJson(QJsonValue(rawMeta)));
  QCOMPARE(result->toJson(), obj);
}

void CardCatalogTests::duplicateClassSymbolsRejected() {
  // classSymbols is schema-typed "uniqueItems":true; a repeated decoded
  // value must fail rather than silently collapsing.
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "classSymbols": ["Guardian", "Guardian"]
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("classSymbols")));
}

void CardCatalogTests::duplicateCardTraitsRejected() {
  // cardTraits $refs the shared stringSet def ("uniqueItems":true).
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "cardTraits": ["Tactic", "Tactic"]
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("cardTraits")));
}

void CardCatalogTests::duplicateRevealedCardTraitsRejected() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "revealedCardTraits": ["Omen", "Omen"]
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("revealedCardTraits")));
}

void CardCatalogTests::duplicateTagsAllowedNoUniquenessConstraint() {
  // Unlike classSymbols/cardTraits/revealedCardTraits, "tags" is a plain
  // string array with no "uniqueItems" constraint in the schema, so
  // repeated values are valid and must round-trip unchanged.
  const QJsonObject obj{
      {QStringLiteral("cardCode"), QStringLiteral("c00001")},
      {QStringLiteral("name"),
       QJsonObject{{QStringLiteral("title"), QStringLiteral("X")},
                   {QStringLiteral("subtitle"), QJsonValue()}}},
      {QStringLiteral("cardType"), QStringLiteral("AssetType")},
      {QStringLiteral("art"), QStringLiteral("1")},
      {QStringLiteral("tags"),
       QJsonArray{QStringLiteral("dup"), QStringLiteral("dup")}},
  };
  const auto result = CardDef::fromJson(obj, u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->tags.size(), 2);
  QCOMPARE(result->toJson(), obj);
}

void CardCatalogTests::rawBytesPreserveNumericPrecisionInUnconstrainedFields() {
  // A number nested inside an unconstrained field (any depth) must survive
  // CardDef::fromRawBytes() byte-exact: this is impossible for a decode
  // path that collapses to QJsonValue first, since QJsonValue stores every
  // number as a C++ double. None of these three literals is representable
  // as an exact double / fits qint64, so this test fails outright if
  // fromRawBytes() ever converts the tree to QJsonValue before extracting
  // these fields.
  const QByteArray bytes = R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "meta": {
      "hugeInt": 99999999999999999999999999,
      "longFraction": 1.234567890123456789012345678901234567890,
      "hugeExponent": 1e400
    },
    "criteria": [{"nested": {"alsoHuge": 90071992547409931234567}}],
    "keywords": ["surge", 99999999999999999999999999]
  })";
  const auto result = CardDef::fromRawBytes(bytes, u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));

  QCOMPARE(result->meta.kind(), Json::Value::Kind::Object);
  const auto hugeInt = result->meta.value("hugeInt"_L1).toRawNumber();
  QCOMPARE(hugeInt.literal(), QStringLiteral("99999999999999999999999999"));
  QVERIFY(!hugeInt.toExactInt64().has_value());

  const auto longFraction = result->meta.value("longFraction"_L1).toRawNumber();
  QCOMPARE(longFraction.literal(),
           QStringLiteral("1.234567890123456789012345678901234567890"));

  const auto hugeExponent = result->meta.value("hugeExponent"_L1).toRawNumber();
  QCOMPARE(hugeExponent.literal(), QStringLiteral("1e400"));

  QCOMPARE(result->criteria.kind(), Json::Value::Kind::Array);
  const auto nestedHuge = result->criteria.toArray()
                              .at(0)
                              .value("nested"_L1)
                              .value("alsoHuge"_L1)
                              .toRawNumber();
  QCOMPARE(nestedHuge.literal(), QStringLiteral("90071992547409931234567"));

  QCOMPARE(result->keywords.kind(), Json::Value::Kind::Array);
  QCOMPARE(result->keywords.toArray().at(1).toRawNumber().literal(),
           QStringLiteral("99999999999999999999999999"));
}

void CardCatalogTests::rawBytesRejectDuplicateKeyNestedInUnconstrainedField() {
  // The duplicate-key rejection Value::parse() performs at every nesting
  // depth must apply inside an unconstrained field too -- fromRawBytes()
  // must never fall back to a lenient QJsonDocument-style "last one wins"
  // parse for the parts of the tree this client does not itself model.
  const QByteArray bytes = R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "meta": {"dup": 1, "dup": 2}
  })";
  const auto result = CardDef::fromRawBytes(bytes, u"card");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("duplicate")),
           qPrintable(result.error()));
}

void CardCatalogTests::decodeCatalogFromRawBytesRoundTripsArray() {
  const QByteArray bytes = R"([
    {
      "cardCode": "c00001",
      "name": {"title": "First", "subtitle": null},
      "cardType": "AssetType",
      "art": "1",
      "meta": {"bigId": 9007199254740993}
    },
    {
      "cardCode": "c00002",
      "name": {"title": "Second", "subtitle": null},
      "cardType": "EventType",
      "art": "2"
    }
  ])";
  const auto result = decodeCatalogFromRawBytes(bytes, u"catalog");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->size(), 2);
  QCOMPARE((*result)[0].cardCode.value(), QStringLiteral("c00001"));
  QCOMPARE((*result)[0].meta.value("bigId"_L1).toRawNumber().literal(),
           QStringLiteral("9007199254740993"));
  QCOMPARE((*result)[1].cardCode.value(), QStringLiteral("c00002"));

  // Not a JSON array at all: a top-level object is rejected, not silently
  // treated as a single-element catalog.
  const auto notArray =
      decodeCatalogFromRawBytes(QByteArrayLiteral("{}"), u"catalog");
  QVERIFY(!notArray.has_value());
}

QTEST_APPLESS_MAIN(CardCatalogTests)

#include "CardCatalogTests.moc"
