#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QtTest>

#include <algorithm>
#include <limits>
#include <utility>

#include "CardCatalog.h"
#include "RawJson.h"

#include "DomainJsonTestAdapter.h"

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
  // Round 7 item 8: byPlayerCount()'s four parameters are exact positional
  // values for 1/2/3/4 players (per the pinned backend's `fromGameValue`
  // case-match on `1|2|3|4`), not an "oneOrTwo/three/four/fiveOrMore"
  // grouping.
  void gameValueByPlayerCountFactoryUsesPositionalPlayerCountSemantics();
  void allSkillIconVariantsRoundTrip();
  void unrecognizedCardCostTagPreservedNotRejected();
  void unrecognizedGameValueTagPreservedNotRejected();
  void unrecognizedSkillIconTagPreservedNotRejected();
  void missingContentsRejectedForRawPayloadCardCostTags();
  void rawPayloadCardCostFactoriesValidateContents();
  void nullaryCardCostTagWithContentsRejected();
  void nullaryGameValueTagWithContentsRejected();
  void nullarySkillIconTagWithContentsRejected();
  // Round-8 item 7 / round-9 item 5: additionalProperties:false on a known
  // tagged-union branch's exact {"tag","contents"}/{"tag"} shape means an
  // extra sibling key is malformed input, not a forward-compat additive
  // field. GameListRow's success shape and its nested summary objects are
  // ALSO exact-key-enforced (round-9 item 5), since that closed shape is
  // what positively disambiguates a success row from a failure row; see
  // gameListRowSuccessRejectsAdditiveTopLevelField in GamesTests.cpp. As of
  // round-10-cumulative-review item 4, CardDef/CardName are exact-key-
  // enforced too (see extraTopLevelFieldOnCardDefRejected below),
  // superseding this type's own earlier additive-tolerant policy.
  void extraKeyOnKnownCardCostBranchRejected();
  void extraKeyOnKnownGameValueBranchRejected();
  void extraKeyOnKnownSkillIconBranchRejected();

  // Forward compatibility / exact-key enforcement ─────────────────────────────
  void extraTopLevelFieldOnCardDefRejected();
  void extraTopLevelFieldOnCardNameRejected();
  // Round-10-cumulative-review item 4's own wording: prove the check runs
  // on the canonical raw-byte decode path with an extra-key value that
  // cannot survive a QJson-collapsed representation intact (an exact
  // integer outside qint64's positive range), and that CardDef's required
  // "art" key participates in the same escape-equivalent-duplicate-key
  // protection every other aggregate gets for free from Json::Value::parse().
  void extraTopLevelFieldWithExactNumberRejectedThroughRawBytes();
  void escapeEquivalentDuplicateTopLevelKeyRejectedForCardDef();
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
  void largeUniqueCardTraitsDecodesSubQuadratically();

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
  // Round 7 item 4: the ENCODE side must be equally lossless -- decoding a
  // fixture with a huge/long-fraction number nested inside an
  // unconstrained field, then re-encoding via toJsonBytes() (never
  // through the QJsonObject-based toJson()), must reproduce the identical
  // literal on reparse.
  void toJsonBytesPreservesNumericPrecisionInUnconstrainedFieldsOnEncode();
  void toJsonBytesPreservesUnknownTaggedUnionPrecisionOnEncode();
  // "Stop whack-a-mole" cumulative review item 1: SkillIcon/CardCost/
  // GameValue/CardDef::toJson() now compose a complete toRawJson() AST
  // and convert exactly ONCE via Value::toExactQJsonObject() (never the
  // old lossy Value::toQJson(), now renamed/privatized
  // toLossyQJsonForTestingOnly()), so a non-qint64-exact number, a
  // duplicate key, or a nested Undefined anywhere inside an unknown tag's
  // raw contents (or CardDef's unconstrained meta field) must now make
  // the QJsonObject-typed toJson() itself fail -- not merely toJsonBytes()
  // above, which was already proven lossless. Only SkillIcon gets the
  // duplicate-key/Undefined pair directly (CardCost/GameValue/CardDef
  // share the exact same Value::toExactQJsonObject() primitive, already
  // exercised for those two shapes by RawJsonTests.cpp).
  void skillIconToJsonRejectsHugeExponentInUnknownTagContents();
  void skillIconToJsonRejectsDuplicateKeyInUnknownTagContents();
  void skillIconToJsonRejectsNestedUndefinedInUnknownTagContents();
  void cardCostToJsonRejectsHugeExponentInUnknownTagContents();
  void gameValueToJsonRejectsHugeExponentInUnknownTagContents();
  void cardDefToJsonRejectsHugeExponentInMetaField();

  // Round-9 item 6: encodeClosedEnum() now returns a typed failure rather
  // than Q_UNREACHABLE_RETURN when given an enum value fabricated via
  // static_cast from outside its real range -- this is reachable through
  // any public field/factory of a closed-enum-carrying type, never a
  // JSON-decode-only concern. skillType() validates at construction time
  // (the class has no other way to reach a populated m_skill), while
  // CardDef's enum fields are public struct members a caller can mutate
  // directly after a valid decode, so toJson()/toRawJson() must be the
  // ones to catch it.
  void skillTypeFactoryRejectsOutOfRangeEnumValue();
  void cardDefToJsonRejectsOutOfRangeCardType();
  void cardDefToJsonRejectsOutOfRangeCardSubType();
  void cardDefToJsonRejectsOutOfRangeClassSymbolInArray();
  void cardDefToJsonRejectsOutOfRangeRevelation();
  void cardDefToJsonRejectsOutOfRangeCardSlotInArray();
  void cardDefToJsonRejectsOutOfRangeOutOfPlayEffectInArray();
  void cardDefToJsonRejectsOutOfRangeWhenDiscarded();

  // Round-19-cumulative-review item 2: a stored Kind::Undefined member
  // (constructible only via Json::Value::makeObject(), never a real
  // parse -- see Value::find()'s doc comment) previously satisfied
  // fieldPresence(obj, key) != Absent as if the key were genuinely
  // present with a real value, so the reviewer's literal repro
  // (makeObject({{"revealedName", Value{}}})) silently decoded as if
  // "revealedName" were simply omitted. Each covers a different
  // field-presence-sensitive helper family so none of them can silently
  // regress to the old absent-vs-undefined conflation independently.
  void cardDefRawDecodeRejectsOptionalRevealedNamePresentButStoredUndefined();
  void cardDefRawDecodeRejectsRequiredCardCodePresentButStoredUndefined();
  void cardDefRawDecodeRejectsNestedNullableSubtitlePresentButStoredUndefined();
  void cardDefRawDecodeStillTreatsGenuinelyAbsentRevealedNameAsAbsent();

  // Round-19-cumulative-review item 3: GameValue/SkillIcon/CardCost had
  // no user-declared copy/move, so a compiler-generated move constructor/
  // assignment would (for GameValue specifically) really move
  // m_contents (QList<qint64>, leaving it empty) while the discriminating
  // tag stayed ByPlayerCount on the moved-from source -- the exact
  // element-count loss the reviewer flagged. All three now explicitly
  // declare a copy constructor/assignment (see CardCatalog.h), so
  // std::move() falls back to a full (cheap, implicit-sharing) copy
  // instead, and this also transitively protects each type's Unknown-tag
  // Json::Value raw payload (itself now copy-only, see RawJson.h).
  void gameValueByPlayerCountMoveConstructLeavesSourceContentsIntact();
  void gameValueByPlayerCountMoveAssignLeavesSourceContentsIntact();
  void skillIconUnknownTagMoveConstructLeavesSourceRawIntact();
  void cardCostUnknownTagMoveConstructLeavesSourceRawIntact();
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

// Round-19-cumulative-review item 2: a directly-constructed Json::Value
// object -- built via makeObject(), never Value::parse() -- can hold a
// member whose value is a genuine Kind::Undefined (no valid JSON spelling
// produces that; see Value::find()'s doc comment). replaceMember lets a
// test overwrite (or newly insert) exactly one top-level member of the
// otherwise-minimal raw card object with such a value, or with any other
// Value, while every other member keeps decoding successfully, isolating
// the field under test.
Json::Value minimalCardRawObject(QLatin1StringView memberToReplace = {},
                                 Json::Value replacement = {}) {
  Json::Value base = toRawJson(minimalCardObject());
  QList<std::pair<QString, Json::Value>> members = base.members();
  if (memberToReplace.isEmpty())
    return Json::Value::makeObject(members);
  bool replaced = false;
  for (auto &member : members) {
    if (member.first == memberToReplace) {
      member.second = replacement;
      replaced = true;
      break;
    }
  }
  if (!replaced)
    members.append({QString(memberToReplace), replacement});
  return Json::Value::makeObject(members);
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
  auto encoded = Arkham::TestOnly::objectJson(*result);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, cards.at(0).toObject());
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

  auto encoded = Arkham::TestOnly::objectJson(*result);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, homebrew.at(0).toObject());
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

  auto encoded = Arkham::TestOnly::objectJson(*result);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, card.toObject());
}

void CardCatalogTests::missingCardCodeRejected() {
  const QJsonObject obj = parseJson(R"({
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1"
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  // Asserts the exact, clearer "missing required field" phrasing (see
  // Json::requireField, JsonDecode.h) rather than the less specific
  // "expected string, got missing" a bare value-decoder call would
  // produce for an absent key.
  QCOMPARE(
      result.error(),
      QStringLiteral("card.cardCode: missing required field \"cardCode\""));
}

void CardCatalogTests::missingNameRejected() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "cardType": "AssetType",
    "art": "1"
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QCOMPARE(result.error(),
           QStringLiteral("card.name: missing required field \"name\""));
}

void CardCatalogTests::missingCardTypeRejected() {
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "art": "1"
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QCOMPARE(
      result.error(),
      QStringLiteral("card.cardType: missing required field \"cardType\""));
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
    const auto encoded = Arkham::TestOnly::objectJson(*result);
    if (!encoded)
      QFAIL(qPrintable(encoded.error()));
    QCOMPARE(*encoded, obj);
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
    const auto encoded = Arkham::TestOnly::objectJson(*result);
    if (!encoded)
      QFAIL(qPrintable(encoded.error()));
    QCOMPARE(*encoded, obj);
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
  const auto matchingEnemyFieldEncoded =
      Arkham::TestOnly::objectJson(*matchingEnemyFieldResult);
  if (!matchingEnemyFieldEncoded)
    QFAIL(qPrintable(matchingEnemyFieldEncoded.error()));
  QCOMPARE(*matchingEnemyFieldEncoded, matchingEnemyFieldObj);

  const QJsonObject staticObj{
      {QStringLiteral("tag"), QStringLiteral("StaticCost")},
      {QStringLiteral("contents"), 5}};
  const auto staticResult = CardCost::fromJson(staticObj, u"cost");
  if (!staticResult)
    QFAIL(qPrintable(staticResult.error()));
  QCOMPARE(*staticResult->staticAmount(), qint64(5));
  const auto staticEncoded = Arkham::TestOnly::objectJson(*staticResult);
  if (!staticEncoded)
    QFAIL(qPrintable(staticEncoded.error()));
  QCOMPARE(*staticEncoded, staticObj);
}

void CardCatalogTests::allGameValueVariantsRoundTrip() {
  const QJsonObject staticObj{{QStringLiteral("tag"), QStringLiteral("Static")},
                              {QStringLiteral("contents"), 4}};
  auto r1 = GameValue::fromJson(staticObj, u"gv");
  if (!r1)
    QFAIL(qPrintable(r1.error()));
  auto r1Encoded = Arkham::TestOnly::objectJson(*r1);
  if (!r1Encoded)
    QFAIL(qPrintable(r1Encoded.error()));
  QCOMPARE(*r1Encoded, staticObj);

  const QJsonObject perPlayerObj{
      {QStringLiteral("tag"), QStringLiteral("PerPlayer")},
      {QStringLiteral("contents"), 2}};
  auto r2 = GameValue::fromJson(perPlayerObj, u"gv");
  if (!r2)
    QFAIL(qPrintable(r2.error()));
  auto r2Encoded = Arkham::TestOnly::objectJson(*r2);
  if (!r2Encoded)
    QFAIL(qPrintable(r2Encoded.error()));
  QCOMPARE(*r2Encoded, perPlayerObj);

  const QJsonObject staticWithPerPlayerObj{
      {QStringLiteral("tag"), QStringLiteral("StaticWithPerPlayer")},
      {QStringLiteral("contents"), QJsonArray{3, 1}}};
  auto r3 = GameValue::fromJson(staticWithPerPlayerObj, u"gv");
  if (!r3)
    QFAIL(qPrintable(r3.error()));
  QCOMPARE(r3->contents(), (QList<qint64>{qint64(3), qint64(1)}));
  auto r3Encoded = Arkham::TestOnly::objectJson(*r3);
  if (!r3Encoded)
    QFAIL(qPrintable(r3Encoded.error()));
  QCOMPARE(*r3Encoded, staticWithPerPlayerObj);

  const QJsonObject byPlayerCountObj{
      {QStringLiteral("tag"), QStringLiteral("ByPlayerCount")},
      {QStringLiteral("contents"), QJsonArray{2, 3, 4, 5}}};
  auto r4 = GameValue::fromJson(byPlayerCountObj, u"gv");
  if (!r4)
    QFAIL(qPrintable(r4.error()));
  QCOMPARE(r4->contents(),
           (QList<qint64>{qint64(2), qint64(3), qint64(4), qint64(5)}));
  auto r4Encoded = Arkham::TestOnly::objectJson(*r4);
  if (!r4Encoded)
    QFAIL(qPrintable(r4Encoded.error()));
  QCOMPARE(*r4Encoded, byPlayerCountObj);

  for (const auto &tag : {"ValueX"_L1, "ValueStar"_L1, "ValueUnknown"_L1}) {
    const QJsonObject obj{{QStringLiteral("tag"), QString(tag)}};
    auto r = GameValue::fromJson(obj, u"gv");
    if (!r)
      QFAIL(qPrintable(r.error()));
    auto encoded = Arkham::TestOnly::objectJson(*r);
    if (!encoded)
      QFAIL(qPrintable(encoded.error()));
    QCOMPARE(*encoded, obj);
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

void CardCatalogTests::
    gameValueByPlayerCountFactoryUsesPositionalPlayerCountSemantics() {
  // Distinct values in each of the four positions prove contents()
  // preserves onePlayer/twoPlayers/threePlayers/fourPlayers in that exact
  // order -- matching the pinned backend's Arkham.GameValue
  // `fromGameValue (ByPlayerCount n1 n2 n3 n4) pc = case pc of 1 -> n1;
  // 2 -> n2; 3 -> n3; 4 -> n4`, not a grouped "1-or-2 / 3 / 4 / 5-or-more"
  // scheme.
  const GameValue value = GameValue::byPlayerCount(10, 20, 30, 40);
  QCOMPARE(value.tag(), GameValueTag::ByPlayerCount);
  QCOMPARE(value.contents(),
           (QList<qint64>{qint64(10), qint64(20), qint64(30), qint64(40)}));
  const QJsonObject expected{
      {QStringLiteral("tag"), QStringLiteral("ByPlayerCount")},
      {QStringLiteral("contents"), QJsonArray{10, 20, 30, 40}}};
  auto encoded = Arkham::TestOnly::objectJson(value);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, expected);
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
    auto encoded = Arkham::TestOnly::objectJson(*r);
    if (!encoded)
      QFAIL(qPrintable(encoded.error()));
    QCOMPARE(*encoded, obj);
  }

  for (const auto &tag : {"WildIcon"_L1, "WildMinusIcon"_L1}) {
    const QJsonObject obj{{QStringLiteral("tag"), QString(tag)}};
    auto r = SkillIcon::fromJson(obj, u"skill");
    if (!r)
      QFAIL(qPrintable(r.error()));
    QVERIFY(!r->skill().has_value());
    auto encoded = Arkham::TestOnly::objectJson(*r);
    if (!encoded)
      QFAIL(qPrintable(encoded.error()));
    QCOMPARE(*encoded, obj);
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
  const auto withContentsEncoded = Arkham::TestOnly::objectJson(*result);
  if (!withContentsEncoded)
    QFAIL(qPrintable(withContentsEncoded.error()));
  QCOMPARE(*withContentsEncoded, withContents);

  const QJsonObject withoutContents{
      {QStringLiteral("tag"), QStringLiteral("AnotherFutureCostTag")}};
  const auto result2 = CardCost::fromJson(withoutContents, u"cost");
  if (!result2)
    QFAIL(qPrintable(result2.error()));
  QCOMPARE(result2->tag(), CardCostTag::Unknown);
  QVERIFY(result2->unknownRaw() == toRawJson(withoutContents));
  const auto withoutContentsEncoded = Arkham::TestOnly::objectJson(*result2);
  if (!withoutContentsEncoded)
    QFAIL(qPrintable(withoutContentsEncoded.error()));
  QCOMPARE(*withoutContentsEncoded, withoutContents);

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
  const auto encoded = Arkham::TestOnly::objectJson(*result);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, obj);
}

void CardCatalogTests::unrecognizedSkillIconTagPreservedNotRejected() {
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("SomeFutureIcon")}};
  const auto result = SkillIcon::fromJson(obj, u"skill");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->tag(), SkillIconTag::Unknown);
  QVERIFY(result->unknownRaw() == toRawJson(obj));
  auto encoded = Arkham::TestOnly::objectJson(*result);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, obj);
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

void CardCatalogTests::extraKeyOnKnownCardCostBranchRejected() {
  // A sibling key beside "tag"/"contents" on a known, fixed-shape branch
  // is malformed -- not a forward-compat additive field this client
  // should silently ignore (that policy applies only to named-field
  // response summary objects like CardDef, never a 1-2 key tagged
  // union branch).
  const QJsonObject staticCostExtra{
      {QStringLiteral("tag"), QStringLiteral("StaticCost")},
      {QStringLiteral("contents"), 3},
      {QStringLiteral("unexpected"), true}};
  const auto staticResult = CardCost::fromJson(staticCostExtra, u"cost");
  QVERIFY(!staticResult.has_value());
  QVERIFY2(staticResult.error().contains(QStringLiteral("unexpected")),
           qPrintable(staticResult.error()));

  const QJsonObject dynamicCostExtra{
      {QStringLiteral("tag"), QStringLiteral("DynamicCost")},
      {QStringLiteral("extra"), 1}};
  const auto dynamicResult = CardCost::fromJson(dynamicCostExtra, u"cost");
  QVERIFY(!dynamicResult.has_value());
  QVERIFY2(dynamicResult.error().contains(QStringLiteral("extra")),
           qPrintable(dynamicResult.error()));

  const QJsonObject matchingEnemyFieldExtra{
      {QStringLiteral("tag"), QStringLiteral("MatchingEnemyFieldCost")},
      {QStringLiteral("contents"), QJsonArray{QStringLiteral("a"), 1}},
      {QStringLiteral("extra"), QJsonValue(QJsonValue::Null)}};
  const auto matchingResult =
      CardCost::fromJson(matchingEnemyFieldExtra, u"cost");
  QVERIFY(!matchingResult.has_value());
  QVERIFY2(matchingResult.error().contains(QStringLiteral("extra")),
           qPrintable(matchingResult.error()));
}

void CardCatalogTests::extraKeyOnKnownGameValueBranchRejected() {
  const QJsonObject staticExtra{
      {QStringLiteral("tag"), QStringLiteral("Static")},
      {QStringLiteral("contents"), 2},
      {QStringLiteral("unexpected"), true}};
  const auto staticResult = GameValue::fromJson(staticExtra, u"gv");
  QVERIFY(!staticResult.has_value());
  QVERIFY2(staticResult.error().contains(QStringLiteral("unexpected")),
           qPrintable(staticResult.error()));

  const QJsonObject byPlayerCountExtra{
      {QStringLiteral("tag"), QStringLiteral("ByPlayerCount")},
      {QStringLiteral("contents"), QJsonArray{1, 2, 3, 4}},
      {QStringLiteral("extra"), 1}};
  const auto byPlayerCountResult =
      GameValue::fromJson(byPlayerCountExtra, u"gv");
  QVERIFY(!byPlayerCountResult.has_value());
  QVERIFY2(byPlayerCountResult.error().contains(QStringLiteral("extra")),
           qPrintable(byPlayerCountResult.error()));

  const QJsonObject valueXExtra{
      {QStringLiteral("tag"), QStringLiteral("ValueX")},
      {QStringLiteral("extra"), 1}};
  const auto valueXResult = GameValue::fromJson(valueXExtra, u"gv");
  QVERIFY(!valueXResult.has_value());
  QVERIFY2(valueXResult.error().contains(QStringLiteral("extra")),
           qPrintable(valueXResult.error()));
}

void CardCatalogTests::extraKeyOnKnownSkillIconBranchRejected() {
  const QJsonObject skillIconExtra{
      {QStringLiteral("tag"), QStringLiteral("SkillIcon")},
      {QStringLiteral("contents"), QStringLiteral("Willpower")},
      {QStringLiteral("unexpected"), true}};
  const auto skillIconResult = SkillIcon::fromJson(skillIconExtra, u"skill");
  QVERIFY(!skillIconResult.has_value());
  QVERIFY2(skillIconResult.error().contains(QStringLiteral("unexpected")),
           qPrintable(skillIconResult.error()));

  const QJsonObject wildExtra{
      {QStringLiteral("tag"), QStringLiteral("WildIcon")},
      {QStringLiteral("extra"), 1}};
  const auto wildResult = SkillIcon::fromJson(wildExtra, u"skill");
  QVERIFY(!wildResult.has_value());
  QVERIFY2(wildResult.error().contains(QStringLiteral("extra")),
           qPrintable(wildResult.error()));
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
    const auto nullEncoded = Arkham::TestOnly::objectJson(*nullResult);
    if (!nullEncoded)
      QFAIL(qPrintable(nullEncoded.error()));
    QCOMPARE(*nullEncoded, nullContents);
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
  const auto matchingEnemyFieldCostEncoded =
      Arkham::TestOnly::objectJson(*matchingEnemyFieldCost);
  if (!matchingEnemyFieldCostEncoded)
    QFAIL(qPrintable(matchingEnemyFieldCostEncoded.error()));
  QCOMPARE(
      *matchingEnemyFieldCostEncoded,
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

void CardCatalogTests::extraTopLevelFieldOnCardDefRejected() {
  // Round-10-cumulative-review item 4: catalog.schema.json's cardDef
  // definition is additionalProperties:false; an unrecognized top-level
  // key is now a hard decode failure rather than silently ignored (see
  // CardCatalog.h's updated doc comment) -- this inverts the previous
  // "ignored" acceptance test of the same name.
  const QJsonObject obj = parseJson(R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "aFutureFieldThisClientHasNeverHeardOf": {"anything": [1, 2, 3]}
  })"_L1);
  const auto result = CardDef::fromJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(
               QStringLiteral("aFutureFieldThisClientHasNeverHeardOf")),
           qPrintable(result.error()));
}

void CardCatalogTests::extraTopLevelFieldOnCardNameRejected() {
  // catalog.schema.json's `name` definition is additionalProperties:false
  // with exactly {"title","subtitle"} -- reused verbatim wherever this
  // client decodes a card name, including CardDef's own name/revealedName
  // fields and (per the round-10 finding's own text) game-list's
  // scenario/investigator summaries; asserted directly on
  // CardName::fromJson() here so the check is proven at its single source
  // of truth rather than only transitively through CardDef.
  const QJsonObject obj{
      {QStringLiteral("title"), QStringLiteral("X")},
      {QStringLiteral("subtitle"), QJsonValue()},
      {QStringLiteral("aFutureFieldThisClientHasNeverHeardOf"),
       QStringLiteral("anything")},
  };
  const auto result = CardName::fromJson(obj, u"name");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(
               QStringLiteral("aFutureFieldThisClientHasNeverHeardOf")),
           qPrintable(result.error()));
}

void CardCatalogTests::
    extraTopLevelFieldWithExactNumberRejectedThroughRawBytes() {
  // Proves the exact-key check runs on the canonical raw-byte decode path
  // (never a QJsonValue-collapsed copy first): the extra key's value,
  // 9223372036854775809, is one past qint64::max and therefore cannot be
  // represented exactly as either a QJsonValue::Double or a qint64. If
  // this rejection somehow ran against a QJson-collapsed representation
  // instead of the lossless raw AST, the conversion itself would already
  // have failed or silently rounded before this check ever saw the key.
  const QByteArray bytes = R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "aFutureFieldThisClientHasNeverHeardOf": 9223372036854775809
  })";
  const auto result = CardDef::fromRawBytes(bytes, u"card");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(
               QStringLiteral("aFutureFieldThisClientHasNeverHeardOf")),
           qPrintable(result.error()));
}

void CardCatalogTests::
    escapeEquivalentDuplicateTopLevelKeyRejectedForCardDef() {
  // "\u0061rt" decodes to the same text as the required "art" key;
  // Json::Value::parse() itself rejects this as a duplicate key (see
  // RawJsonTests::rejectsEscapeEquivalentDuplicateObjectKeys()) before any
  // CardDef-specific decoding runs, proving this generic protection
  // extends through CardDef::fromRawBytes()'s aggregate boundary, not
  // merely a standalone RawJson unit test.
  const QByteArray bytes = R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "\u0061rt": "2"
  })";
  const auto result = CardDef::fromRawBytes(bytes, u"card");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(u"duplicate"_s), qPrintable(result.error()));
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
  auto encoded = Arkham::TestOnly::objectJson(*result);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(encoded->value(QStringLiteral("meta")).toObject(), rawMeta);
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
  auto encoded = Arkham::TestOnly::objectJson(*result);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, obj);
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
  auto encoded = Arkham::TestOnly::objectJson(*result);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, obj);
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
  auto encoded = Arkham::TestOnly::objectJson(*result);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QVERIFY(!encoded->contains(QStringLiteral("level")));
  QVERIFY(!encoded->contains(QStringLiteral("errata")));
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
  auto encoded = Arkham::TestOnly::objectJson(*result);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, obj);
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
  auto encoded = Arkham::TestOnly::objectJson(*result);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, obj);
}

void CardCatalogTests::largeUniqueCardTraitsDecodesSubQuadratically() {
  // Regression for a reviewer-identified quadratic-time bug: decodeStringSet()
  // (backing cardTraits/revealedCardTraits, both schema "uniqueItems":true)
  // previously tracked "already seen" values via repeated
  // QStringList::contains() scans -- O(n) per insertion, O(n^2) overall --
  // additionally worst-cased by a long *shared* prefix (each comparison
  // must scan almost the entire string before finding the differing
  // suffix, denying operator==() any early-mismatch/differing-length
  // short-circuit). Fixed to track membership via QSet<QString> (amortized
  // O(1) per insertion) while still returning results in first-seen order.
  //
  // This proves the fix's *asymptotic* behavior rather than depending on
  // any one machine's absolute timing (which would be flaky across CI
  // runners of different speeds): decode a small (N) and 4x-larger (4N)
  // batch of unique, equal-length, long-common-prefix trait strings, and
  // assert the measured time ratio lands close to the ~4x linear growth
  // an O(n) implementation exhibits -- nowhere near the ~16x an O(n^2)
  // implementation would show for the same 4x size increase. The minimum
  // of several repeated measurements is used per size (standard technique
  // for suppressing OS-scheduling/cache-warmup noise), and an untimed
  // warm-up run precedes measurement.
  constexpr int prefixLength = 400;
  constexpr int smallCount = 800;
  constexpr int largeCount = smallCount * 4;
  constexpr int suffixWidth = 7;

  const QByteArray prefix(prefixLength, 'a');
  auto traitFor = [&](int index) {
    return prefix + QByteArray::number(index).rightJustified(suffixWidth, '0');
  };

  auto makePayload = [&](int count) {
    QByteArray json =
        "{\"cardCode\":\"c00001\",\"name\":{\"title\":\"X\",\"subtitle\":null},"
        "\"cardType\":\"AssetType\",\"art\":\"1\",\"cardTraits\":[";
    for (int i = 0; i < count; ++i) {
      if (i > 0)
        json += ',';
      json += '"';
      json += traitFor(i);
      json += '"';
    }
    json += "]}";
    return json;
  };

  const QByteArray smallPayload = makePayload(smallCount);
  const QByteArray largePayload = makePayload(largeCount);

  auto decodeOnce = [](const QByteArray &payload) -> CardDef {
    // qFatal (not QFAIL) here follows this file's existing convention (see
    // the toRawJson() helper above) for a lambda that must unconditionally
    // return a value: this is test-fixture construction that must not
    // fail, never production code, and QFAIL's implicit `return;` cannot
    // coexist with a non-void lambda return type.
    auto result = CardDef::fromRawBytes(payload, u"card");
    if (!result)
      qFatal("card decode must not fail in this performance regression "
             "fixture: %s",
             qPrintable(result.error()));
    return *result;
  };

  // Untimed warm-up: absorbs first-touch page faults, allocator growth,
  // and branch-prediction warmup so the timed runs below measure steady-
  // state decode cost, not one-time setup overhead.
  decodeOnce(largePayload);

  constexpr int repeats = 5;
  auto measureMinNanos = [&](const QByteArray &payload) {
    qint64 best = std::numeric_limits<qint64>::max();
    for (int i = 0; i < repeats; ++i) {
      QElapsedTimer timer;
      timer.start();
      decodeOnce(payload);
      best = std::min(best, timer.nsecsElapsed());
    }
    return best;
  };

  const qint64 smallNanos = measureMinNanos(smallPayload);
  const qint64 largeNanos = measureMinNanos(largePayload);

  QVERIFY2(smallNanos > 0 && largeNanos > 0,
           "timer resolution too coarse to measure a nonzero duration");

  const double ratio = double(largeNanos) / double(smallNanos);

  // Linear growth predicts ~4x; quadratic predicts ~16x. 8x sits at the
  // geometric midpoint, comfortably separating the two while tolerating
  // ordinary measurement noise.
  QVERIFY2(
      ratio < 8.0,
      qPrintable(
          QStringLiteral(
              "decodeStringSet() appears superlinear again: a 4x cardTraits "
              "size increase (%1 -> %2) produced a %3x time increase "
              "(small=%4ns, large=%5ns); expected close to linear ~4x, "
              "would be ~16x for the old O(n^2) QStringList::contains() "
              "implementation")
              .arg(smallCount)
              .arg(largeCount)
              .arg(ratio, 0, 'f', 2)
              .arg(smallNanos)
              .arg(largeNanos)));

  // Correctness alongside the performance assertion: every trait must
  // still decode losslessly with no coalescing/truncation of the large
  // unique set.
  const auto largeResult = decodeOnce(largePayload);
  QCOMPARE(largeResult.cardTraits.size(), largeCount);
  QCOMPARE(largeResult.cardTraits.first(), QString::fromLatin1(traitFor(0)));
  QCOMPARE(largeResult.cardTraits.last(),
           QString::fromLatin1(traitFor(largeCount - 1)));
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

void CardCatalogTests::
    toJsonBytesPreservesNumericPrecisionInUnconstrainedFieldsOnEncode() {
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
    "criteria": [{"nested": {"alsoHuge": 90071992547409931234567}}]
  })";
  const auto decoded = CardDef::fromRawBytes(bytes, u"card");
  if (!decoded)
    QFAIL(qPrintable(decoded.error()));

  const auto encoded = decoded->toJsonBytes();
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));

  const auto reparsed = CardDef::fromRawBytes(*encoded, u"card");
  if (!reparsed)
    QFAIL(qPrintable(reparsed.error()));

  QCOMPARE(reparsed->meta.value("hugeInt"_L1).toRawNumber().literal(),
           QStringLiteral("99999999999999999999999999"));
  QCOMPARE(reparsed->meta.value("longFraction"_L1).toRawNumber().literal(),
           QStringLiteral("1.234567890123456789012345678901234567890"));
  QCOMPARE(reparsed->meta.value("hugeExponent"_L1).toRawNumber().literal(),
           QStringLiteral("1e400"));
  QCOMPARE(reparsed->criteria.toArray()
               .at(0)
               .value("nested"_L1)
               .value("alsoHuge"_L1)
               .toRawNumber()
               .literal(),
           QStringLiteral("90071992547409931234567"));
}

void CardCatalogTests::
    toJsonBytesPreservesUnknownTaggedUnionPrecisionOnEncode() {
  // An unknown SkillIcon/GameValue/CardCost tag's raw payload -- with a
  // numeric literal beyond double precision -- must survive toJsonBytes(),
  // not merely toRawJson() in isolation, since toJsonBytes() (a
  // ValueOrError<QByteArray>-returning encoder, not the plain
  // QJsonObject-returning toJson()) is the canonical byte-exact path.
  const QByteArray costBytes = QByteArrayLiteral(
      "{\"tag\":\"FutureCost\",\"contents\":{\"n\":9007199254740993}}");
  const auto cost =
      CardCost::fromRawJson(*Json::Value::parse(costBytes, u"cost"), u"cost");
  if (!cost)
    QFAIL(qPrintable(cost.error()));
  const auto costEncoded = cost->toJsonBytes();
  if (!costEncoded)
    QFAIL(qPrintable(costEncoded.error()));
  const auto costReparsed = Json::Value::parse(*costEncoded, u"cost");
  if (!costReparsed)
    QFAIL(qPrintable(costReparsed.error()));
  QCOMPARE(
      costReparsed->value("contents"_L1).value("n"_L1).toRawNumber().literal(),
      QStringLiteral("9007199254740993"));

  const QByteArray gvBytes = QByteArrayLiteral(
      "{\"tag\":\"FutureValue\",\"contents\":[9007199254740993]}");
  const auto gv =
      GameValue::fromRawJson(*Json::Value::parse(gvBytes, u"gv"), u"gv");
  if (!gv)
    QFAIL(qPrintable(gv.error()));
  const auto gvEncoded = gv->toJsonBytes();
  if (!gvEncoded)
    QFAIL(qPrintable(gvEncoded.error()));
  const auto gvReparsed = Json::Value::parse(*gvEncoded, u"gv");
  if (!gvReparsed)
    QFAIL(qPrintable(gvReparsed.error()));
  QCOMPARE(
      gvReparsed->value("contents"_L1).toArray().at(0).toRawNumber().literal(),
      QStringLiteral("9007199254740993"));
}

void CardCatalogTests::
    skillIconToJsonRejectsHugeExponentInUnknownTagContents() {
  // "Stop whack-a-mole" cumulative review item 1: SkillIcon::toJson() now
  // composes toRawJson() and converts exactly once via
  // Value::toExactQJsonObject(), so a huge-exponent literal (no exact
  // double representation) nested inside an unrecognized tag's contents
  // must now make the QJsonObject-typed toJson() fail outright -- the old
  // Value::toQJson() (now private/test-only toLossyQJsonForTestingOnly())
  // would have silently rounded it instead.
  const QByteArray bytes = QByteArrayLiteral(
      "{\"tag\":\"SomeFutureIcon\",\"contents\":{\"scale\":1e300}}");
  const auto decoded =
      SkillIcon::fromRawJson(*Json::Value::parse(bytes, u"skill"), u"skill");
  if (!decoded)
    QFAIL(qPrintable(decoded.error()));
  QCOMPARE(decoded->tag(), SkillIconTag::Unknown);
  const auto encoded = Arkham::TestOnly::objectJson(*decoded);
  QVERIFY(!encoded.has_value());
}

void CardCatalogTests::
    skillIconToJsonRejectsDuplicateKeyInUnknownTagContents() {
  // Value::makeObject() (unlike Value::parse()) can transiently hold a
  // duplicate key, so a hand-built unknown-tag payload -- never reachable
  // via fromRawBytes()'s parse-first entry point, but reachable through
  // fromRawJson() given an already-constructed AST -- must still be
  // rejected by toJson()'s exact conversion, not silently collapsed to
  // whichever occurrence QJsonObject::insert() happens to keep.
  const Json::Value raw = Json::Value::makeObject(
      {{QStringLiteral("tag"),
        Json::Value::makeString(QStringLiteral("SomeFutureIcon"))},
       {QStringLiteral("contents"),
        Json::Value::makeObject(
            {{QStringLiteral("dup"), Json::Value::makeBool(true)},
             {QStringLiteral("dup"), Json::Value::makeBool(false)}})}});
  const auto decoded = SkillIcon::fromRawJson(raw, u"skill");
  if (!decoded)
    QFAIL(qPrintable(decoded.error()));
  const auto encoded = Arkham::TestOnly::objectJson(*decoded);
  QVERIFY(!encoded.has_value());
}

void CardCatalogTests::
    skillIconToJsonRejectsNestedUndefinedInUnknownTagContents() {
  // Same rationale as the duplicate-key case immediately above, for a
  // nested Kind::Undefined member (decode never produces one, but a
  // caller can build this AST directly via makeObject()/the default
  // Value{} constructor).
  const Json::Value raw = Json::Value::makeObject(
      {{QStringLiteral("tag"),
        Json::Value::makeString(QStringLiteral("SomeFutureIcon"))},
       {QStringLiteral("contents"),
        Json::Value::makeObject(
            {{QStringLiteral("present"), Json::Value::makeBool(true)},
             {QStringLiteral("vanishes"), Json::Value{}}})}});
  const auto decoded = SkillIcon::fromRawJson(raw, u"skill");
  if (!decoded)
    QFAIL(qPrintable(decoded.error()));
  const auto encoded = Arkham::TestOnly::objectJson(*decoded);
  QVERIFY(!encoded.has_value());
}

void CardCatalogTests::cardCostToJsonRejectsHugeExponentInUnknownTagContents() {
  const QByteArray bytes = QByteArrayLiteral(
      "{\"tag\":\"FutureCost\",\"contents\":{\"scale\":1e300}}");
  const auto decoded =
      CardCost::fromRawJson(*Json::Value::parse(bytes, u"cost"), u"cost");
  if (!decoded)
    QFAIL(qPrintable(decoded.error()));
  const auto encoded = Arkham::TestOnly::objectJson(*decoded);
  QVERIFY(!encoded.has_value());
}

void CardCatalogTests::
    gameValueToJsonRejectsHugeExponentInUnknownTagContents() {
  const QByteArray bytes =
      QByteArrayLiteral("{\"tag\":\"FutureValue\",\"contents\":[1e300]}");
  const auto decoded =
      GameValue::fromRawJson(*Json::Value::parse(bytes, u"gv"), u"gv");
  if (!decoded)
    QFAIL(qPrintable(decoded.error()));
  const auto encoded = Arkham::TestOnly::objectJson(*decoded);
  QVERIFY(!encoded.has_value());
}

void CardCatalogTests::cardDefToJsonRejectsHugeExponentInMetaField() {
  // Companion to rawBytesPreserveNumericPrecisionInUnconstrainedFields()
  // above, which only proves the DECODE side keeps this literal exact:
  // CardDef::toJson() now composes toRawJson() and converts exactly once
  // via Value::toExactQJsonObject(), so re-encoding the very same
  // decoded CardDef must now fail outright rather than silently round
  // meta.hugeExponent through the old lossy adapter.
  const QByteArray bytes = R"({
    "cardCode": "c00001",
    "name": {"title": "X", "subtitle": null},
    "cardType": "AssetType",
    "art": "1",
    "meta": {"hugeExponent": 1e300}
  })";
  const auto result = CardDef::fromRawBytes(bytes, u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  const auto encoded = Arkham::TestOnly::objectJson(*result);
  QVERIFY(!encoded.has_value());
}

void CardCatalogTests::skillTypeFactoryRejectsOutOfRangeEnumValue() {
  // SkillIcon has a private constructor: skillType() is the only way to
  // populate m_skill, so it alone must reject a static_cast fabricated
  // outside SkillType's real range -- typed failure, not
  // Q_UNREACHABLE_RETURN inside the later toJson()/toRawJson() encode.
  const auto result = SkillIcon::skillType(static_cast<SkillType>(999));
  QVERIFY(!result.has_value());
}

void CardCatalogTests::cardDefToJsonRejectsOutOfRangeCardType() {
  auto result = CardDef::fromJson(minimalCardObject(), u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  result->cardType = static_cast<CardType>(999);
  const auto encoded = Arkham::TestOnly::objectJson(*result);
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("cardType")),
           qPrintable(encoded.error()));
}

void CardCatalogTests::cardDefToJsonRejectsOutOfRangeCardSubType() {
  auto result = CardDef::fromJson(minimalCardObject(), u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  result->cardSubType = static_cast<CardSubType>(999);
  const auto encoded = Arkham::TestOnly::objectJson(*result);
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("cardSubType")),
           qPrintable(encoded.error()));
}

void CardCatalogTests::cardDefToJsonRejectsOutOfRangeClassSymbolInArray() {
  auto result = CardDef::fromJson(minimalCardObject(), u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  result->classSymbols = {static_cast<ClassSymbol>(999)};
  const auto encoded = Arkham::TestOnly::objectJson(*result);
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("classSymbols")),
           qPrintable(encoded.error()));
}

void CardCatalogTests::cardDefToJsonRejectsOutOfRangeRevelation() {
  auto result = CardDef::fromJson(minimalCardObject(), u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  result->revelation = static_cast<Revelation>(999);
  const auto encoded = Arkham::TestOnly::objectJson(*result);
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("revelation")),
           qPrintable(encoded.error()));
}

void CardCatalogTests::cardDefToJsonRejectsOutOfRangeCardSlotInArray() {
  auto result = CardDef::fromJson(minimalCardObject(), u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  result->cardSlots = {static_cast<SlotType>(999)};
  const auto encoded = Arkham::TestOnly::objectJson(*result);
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("slots")),
           qPrintable(encoded.error()));
}

void CardCatalogTests::cardDefToJsonRejectsOutOfRangeOutOfPlayEffectInArray() {
  auto result = CardDef::fromJson(minimalCardObject(), u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  result->outOfPlayEffects = {static_cast<OutOfPlayEffect>(999)};
  const auto encoded = Arkham::TestOnly::objectJson(*result);
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("outOfPlayEffects")),
           qPrintable(encoded.error()));
}

void CardCatalogTests::cardDefToJsonRejectsOutOfRangeWhenDiscarded() {
  auto result = CardDef::fromJson(minimalCardObject(), u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  result->whenDiscarded = static_cast<WhenDiscarded>(999);
  const auto encoded = Arkham::TestOnly::objectJson(*result);
  QVERIFY(!encoded.has_value());
  QVERIFY2(encoded.error().contains(QStringLiteral("whenDiscarded")),
           qPrintable(encoded.error()));
}

void CardCatalogTests::
    cardDefRawDecodeRejectsOptionalRevealedNamePresentButStoredUndefined() {
  // The reviewer's literal repro: "revealedName" is an OPTIONAL field
  // (see decodeCardDef's fieldPresence-gated decode in CardCatalog.cpp),
  // decoded only when Json::fieldPresence() reports something other than
  // Absent. Before this fix, a stored Kind::Undefined member satisfied
  // that check exactly like a real value, so the field was then read via
  // obj.value("revealedName") -- which also returns an indistinguishable
  // Undefined -- and decodeCardNameValue() rejected it, but through
  // fieldPresence()'s OLD behavior (reporting Absent) the field would
  // instead have been silently skipped entirely, letting the whole
  // CardDef decode SUCCEED as if "revealedName" were simply omitted. This
  // must now fail instead.
  const Json::Value obj = minimalCardRawObject(
      "revealedName"_L1, Json::Value{} /* stored Undefined */);
  const auto result = CardDef::fromRawJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("revealedName")),
           qPrintable(result.error()));
}

void CardCatalogTests::
    cardDefRawDecodeRejectsRequiredCardCodePresentButStoredUndefined() {
  // "cardCode" is a REQUIRED field decoded via Json::requireField(), which
  // now routes through the presence-distinguishing Json::detail::
  // findField() rather than a plain value() lookup. A stored-Undefined
  // member here was never a silent-success risk (requireField() always
  // failed either way), but must keep failing with an honest message
  // once find() forwards the found-but-Undefined value on to
  // decodeCardCodeValue() for its own type check, rather than reporting
  // "missing required field" for a key that is not actually missing.
  const Json::Value obj =
      minimalCardRawObject("cardCode"_L1, Json::Value{} /* stored Undefined */);
  const auto result = CardDef::fromRawJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("cardCode")),
           qPrintable(result.error()));
}

void CardCatalogTests::
    cardDefRawDecodeRejectsNestedNullableSubtitlePresentButStoredUndefined() {
  // "subtitle" is a REQUIRED-but-NULLABLE field (Json::requireNullableString,
  // routed through findOptionalField() in JsonDecode.cpp) nested two
  // levels deep inside CardDef's top-level "name" object -- covering both
  // the "nullable" and "nested" categories in one repro. A stored
  // Undefined here is distinct from both an absent "subtitle" key (which
  // requireNullableString would reject as missing, since it is not itself
  // optional -- CardName's exact-keys check demands exactly {title,
  // subtitle}) and an explicit JSON null (a legitimate empty subtitle);
  // it must fail with its own distinct message, not be silently treated
  // as either.
  const Json::Value base = minimalCardRawObject();
  QList<std::pair<QString, Json::Value>> nameMembers =
      base.value("name"_L1).members();
  bool replaced = false;
  for (auto &member : nameMembers) {
    if (member.first == QStringLiteral("subtitle")) {
      member.second = Json::Value{}; // stored Undefined, not Kind::Null
      replaced = true;
      break;
    }
  }
  QVERIFY(replaced);
  const Json::Value obj =
      minimalCardRawObject("name"_L1, Json::Value::makeObject(nameMembers));

  const auto result = CardDef::fromRawJson(obj, u"card");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("subtitle")),
           qPrintable(result.error()));
}

void CardCatalogTests::
    cardDefRawDecodeStillTreatsGenuinelyAbsentRevealedNameAsAbsent() {
  // Guards against an overcorrection: a genuinely absent "revealedName"
  // key (never inserted at all, the ordinary/common case for this
  // optional field) must still decode successfully with no revealedName
  // populated -- fieldPresence()'s fix must distinguish stored-Undefined
  // from absence in both directions, not merely start rejecting the
  // field unconditionally.
  const Json::Value obj = minimalCardRawObject();
  QVERIFY(!obj.contains("revealedName"_L1));
  const auto result = CardDef::fromRawJson(obj, u"card");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(!result->revealedName.has_value());
}

void CardCatalogTests::
    gameValueByPlayerCountMoveConstructLeavesSourceContentsIntact() {
  GameValue source = GameValue::byPlayerCount(10, 20, 30, 40);
  QCOMPARE(source.tag(), GameValueTag::ByPlayerCount);
  QCOMPARE(source.contents(),
           (QList<qint64>{qint64(10), qint64(20), qint64(30), qint64(40)}));

  // std::move() on a GameValue now binds the explicitly-declared copy
  // constructor (see CardCatalog.h) rather than an implicit move, so
  // `source` below must remain fully intact -- still tag()==
  // ByPlayerCount with all 4 entries, never silently collapsed to a
  // 0-element contents() while the tag stays unchanged.
  GameValue moved(std::move(source));
  QCOMPARE(moved.tag(), GameValueTag::ByPlayerCount);
  QCOMPARE(moved.contents(),
           (QList<qint64>{qint64(10), qint64(20), qint64(30), qint64(40)}));

  QCOMPARE(source.tag(), GameValueTag::ByPlayerCount);
  QCOMPARE(source.contents(),
           (QList<qint64>{qint64(10), qint64(20), qint64(30), qint64(40)}));

  // Reuse the moved-from source end-to-end through its own encoder.
  const QJsonObject expected{
      {QStringLiteral("tag"), QStringLiteral("ByPlayerCount")},
      {QStringLiteral("contents"), QJsonArray{10, 20, 30, 40}}};
  auto sourceEncoded = Arkham::TestOnly::objectJson(source);
  if (!sourceEncoded)
    QFAIL(qPrintable(sourceEncoded.error()));
  QCOMPARE(*sourceEncoded, expected);
}

void CardCatalogTests::
    gameValueByPlayerCountMoveAssignLeavesSourceContentsIntact() {
  GameValue source = GameValue::byPlayerCount(1, 2, 3, 4);
  GameValue destination = GameValue::valueX();
  destination = std::move(source);

  QCOMPARE(destination.tag(), GameValueTag::ByPlayerCount);
  QCOMPARE(destination.contents(),
           (QList<qint64>{qint64(1), qint64(2), qint64(3), qint64(4)}));
  // Move-assignment (falling back to copy-assignment) must leave
  // `source` just as valid/reusable as move-construction does above.
  QCOMPARE(source.tag(), GameValueTag::ByPlayerCount);
  QCOMPARE(source.contents(),
           (QList<qint64>{qint64(1), qint64(2), qint64(3), qint64(4)}));
}

void CardCatalogTests::skillIconUnknownTagMoveConstructLeavesSourceRawIntact() {
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("SomeFutureIcon")},
      {QStringLiteral("contents"), QJsonObject{{QStringLiteral("x"), 1}}}};
  auto decoded = SkillIcon::fromJson(obj, u"skill");
  if (!decoded)
    QFAIL(qPrintable(decoded.error()));
  SkillIcon source = *decoded;
  QCOMPARE(source.tag(), SkillIconTag::Unknown);

  // std::move() on a SkillIcon now binds the explicitly-declared copy
  // constructor (see CardCatalog.h) rather than an implicit move, so
  // `source` below must remain fully intact -- unknownRaw() must still
  // be the complete original object, not emptied/defaulted to
  // Kind::Undefined while tag() stays Unknown.
  SkillIcon moved(std::move(source));
  QCOMPARE(moved.tag(), SkillIconTag::Unknown);
  QVERIFY(moved.unknownRaw() == toRawJson(obj));

  QCOMPARE(source.tag(), SkillIconTag::Unknown);
  QVERIFY(source.unknownRaw() == toRawJson(obj));
  auto encoded = Arkham::TestOnly::objectJson(source);
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, obj);
}

void CardCatalogTests::cardCostUnknownTagMoveConstructLeavesSourceRawIntact() {
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("SomeFutureCostTag")},
      {QStringLiteral("contents"), QJsonObject{{QStringLiteral("y"), 2}}}};
  auto decoded = CardCost::fromJson(obj, u"cost");
  if (!decoded)
    QFAIL(qPrintable(decoded.error()));
  CardCost source = *decoded;
  QCOMPARE(source.tag(), CardCostTag::Unknown);

  CardCost moved(std::move(source));
  QCOMPARE(moved.tag(), CardCostTag::Unknown);
  QVERIFY(moved.unknownRaw() == toRawJson(obj));

  // The moved-from source must still independently report the complete
  // original raw object.
  QCOMPARE(source.tag(), CardCostTag::Unknown);
  QVERIFY(source.unknownRaw() == toRawJson(obj));
  auto sourceEncoded = Arkham::TestOnly::objectJson(source);
  if (!sourceEncoded)
    QFAIL(qPrintable(sourceEncoded.error()));
  QCOMPARE(*sourceEncoded, obj);
}

QTEST_APPLESS_MAIN(CardCatalogTests)

#include "CardCatalogTests.moc"
