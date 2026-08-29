#include <QFile>
#include <QJsonDocument>
#include <QtTest>

#include "CardCatalog.h"

using namespace Arkham;
using namespace Qt::StringLiterals;

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

  // Closed enums ─────────────────────────────────────────────────────────────
  void unrecognizedCardTypeRejected();
  void unrecognizedClassSymbolRejected();

  // Tagged variants (cardCost/gameValue/skillIcon) ──────────────────────────
  void allCardCostVariantsRoundTrip();
  void allGameValueVariantsRoundTrip();
  void allSkillIconVariantsRoundTrip();
  void unrecognizedCardCostTagRejected();
  void missingContentsRejectedForRawPayloadCardCostTags();

  // Forward compatibility ────────────────────────────────────────────────────
  void unknownAdditiveTopLevelFieldIgnored();
  void unconstrainedFieldsPreservedVerbatim();

  // Collections ──────────────────────────────────────────────────────────────
  void bondedWithRoundTrips();
  void alternateSkillsAndErrataRoundTrip();
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
  QCOMPARE(result->cost->tag, CardCostTag::StaticCost);
  QCOMPARE(*result->cost->staticAmount, 3);
  QCOMPARE(*result->level, 0);
  QCOMPARE(result->cardType, CardType::AssetType);
  QCOMPARE(result->classSymbols, (QList<ClassSymbol>{ClassSymbol::Guardian}));
  QCOMPARE(result->skills.size(), 1);
  QCOMPARE(result->skills.at(0).tag, SkillIconTag::SkillIcon);
  QCOMPARE(*result->skills.at(0).skill, SkillType::SkillCombat);
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
  QCOMPARE(*result->encounterSetQuantity, 3);
  QVERIFY(result->health.has_value());
  QCOMPARE(result->health->tag, GameValueTag::Static);
  QCOMPARE(*result->health->singleAmount, 1);
  QCOMPARE(result->fight->tag, GameValueTag::Static);
  QCOMPARE(*result->fight->singleAmount, 1);
  QCOMPARE(*result->evade->singleAmount, 3);
  QCOMPARE(*result->healthDamage->singleAmount, 1);
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
  QVERIFY(result.error().contains(QStringLiteral("art")));
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

  // Unconstrained-contents variants: preserved verbatim, whatever shape.
  const QList<QLatin1StringView> rawPayload{"MaxDynamicCost"_L1,
                                            "AnyMatchingCardCost"_L1,
                                            "MatchingEnemyFieldCost"_L1};
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

  const QJsonObject staticObj{
      {QStringLiteral("tag"), QStringLiteral("StaticCost")},
      {QStringLiteral("contents"), 5}};
  const auto staticResult = CardCost::fromJson(staticObj, u"cost");
  if (!staticResult)
    QFAIL(qPrintable(staticResult.error()));
  QCOMPARE(*staticResult->staticAmount, 5);
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
  QCOMPARE(r3->contents, (QList<int>{3, 1}));
  QCOMPARE(r3->toJson(), staticWithPerPlayerObj);

  const QJsonObject byPlayerCountObj{
      {QStringLiteral("tag"), QStringLiteral("ByPlayerCount")},
      {QStringLiteral("contents"), QJsonArray{2, 3, 4, 5}}};
  auto r4 = GameValue::fromJson(byPlayerCountObj, u"gv");
  if (!r4)
    QFAIL(qPrintable(r4.error()));
  QCOMPARE(r4->contents, (QList<int>{2, 3, 4, 5}));
  QCOMPARE(r4->toJson(), byPlayerCountObj);

  for (const auto &tag : {"ValueX"_L1, "ValueStar"_L1, "ValueUnknown"_L1}) {
    const QJsonObject obj{{QStringLiteral("tag"), QString(tag)}};
    auto r = GameValue::fromJson(obj, u"gv");
    if (!r)
      QFAIL(qPrintable(r.error()));
    QCOMPARE(r->toJson(), obj);
  }

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
    QCOMPARE(*r->skill, skill);
    QCOMPARE(r->toJson(), obj);
  }

  for (const auto &tag : {"WildIcon"_L1, "WildMinusIcon"_L1}) {
    const QJsonObject obj{{QStringLiteral("tag"), QString(tag)}};
    auto r = SkillIcon::fromJson(obj, u"skill");
    if (!r)
      QFAIL(qPrintable(r.error()));
    QVERIFY(!r->skill.has_value());
    QCOMPARE(r->toJson(), obj);
  }
}

void CardCatalogTests::unrecognizedCardCostTagRejected() {
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("SomeFutureCostTag")}};
  const auto result = CardCost::fromJson(obj, u"cost");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("cost")),
           qPrintable(result.error()));
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

  // An explicit JSON null still counts as present -- only an entirely
  // absent key is rejected.
  const QJsonObject nullContents{
      {QStringLiteral("tag"), QStringLiteral("MaxDynamicCost")},
      {QStringLiteral("contents"), QJsonValue(QJsonValue::Null)}};
  const auto nullResult = CardCost::fromJson(nullContents, u"cost");
  if (!nullResult)
    QFAIL(qPrintable(nullResult.error()));
  QVERIFY(nullResult->rawContents.isNull());
  QCOMPARE(nullResult->toJson(), nullContents);
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
  QCOMPARE(result->meta, QJsonValue(rawMeta));
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
  QCOMPARE(result->bondedWith.at(0).first, 2);
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

QTEST_APPLESS_MAIN(CardCatalogTests)

#include "CardCatalogTests.moc"
