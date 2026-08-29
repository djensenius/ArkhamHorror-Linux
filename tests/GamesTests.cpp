#include <QFile>
#include <QJsonDocument>
#include <QtTest>

#include "Games.h"
#include "RawJson.h"

using namespace Arkham;
using namespace Qt::StringLiterals;

namespace {
// Test-only convenience conversion standing in for a real byte-parsed
// Json::Value (see RawJson.h): GameState/CampaignOption's unknown-tag raw
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

class GamesTests final : public QObject {
  Q_OBJECT

private slots:
  // game-list.json: all 4 GameState kinds + the failure row ────────────────
  void decodesPendingRowFromFixture();
  void decodesChooseDecksRowFromFixture();
  void decodesActiveRowFromFixture();
  void decodesOverRowFromFixture();
  void decodesFailureRowFromFixture();
  void decodeGameListRoundTripsWholeFixtureByteExact();

  // GameState forward compatibility ─────────────────────────────────────────
  void unknownGameStateTagPreservedAndRoundTrips();
  void unknownGameStateTagWithContentsPreservedAndRoundTrips();
  void gameStatePendingRejectsNullUuidInPlayerIds();
  void gameStateChooseDecksRejectsNullUuidInPlayerIds();
  void gameStateIsActiveWithContentsRejected();
  void gameStateIsOverWithContentsRejected();
  void gameStateIsActiveWithNullContentsRejected();
  void gameStateRawBytesPreserveAdditiveFieldBesideTagAndContents();
  void gameStateRawBytesPreserveHugeNestedNumericLiteralInUnknownContents();

  // GameListRow success/error ambiguity ──────────────────────────────────────
  void rowWithBareErrorKeyIsFailure();
  void rowCombiningErrorWithAnyOtherKeyIsRejectedNotFailure();
  void rowWithoutErrorKeyAttemptsFullDecode();

  // game-lifecycle.json fixture entries ──────────────────────────────────────
  void decodesCreateGameFromFixture();
  void decodesCreateGameDefaultsFromFixture();
  void createGameSupports64BitPlayerCount();
  void createGameDefaultsAndNullDefaultsDecodeIdentically();
  void decodesChooseDeckFromFixture();
  void decodesContinueWithoutUpgradeFromFixture();
  void decodesClaimSeatFromFixture();
  void chooseDeckMissingInvestigatorIdRejected();
  void claimSeatMissingInvestigatorIdRejected();
  void decodesOpenSeatsFromFixture();

  // CampaignOption forward compatibility ────────────────────────────────────
  void unknownCampaignOptionWithContentsPreservedAndRoundTrips();
  void unknownCampaignOptionWithoutContentsPreservedAndRoundTrips();
  void knownCampaignOptionRoundTrips();
  void campaignVariantOptionRoundTrips();
  void unknownCampaignOptionCannotBeMistakenForKnownOption();
  void unknownCampaignOptionCannotBeSubmittedAsRequestOption();
  void knownCampaignOptionNarrowsToRequestOption();
  void campaignOptionRequestRejectsUnrecognizedTag();
  void campaignOptionRawBytesPreserveAdditiveFieldBesideTagAndContents();
  void
  campaignOptionRawBytesPreserveHugeNestedNumericLiteralInUnknownContents();

  // campaignId/scenarioId invariant ──────────────────────────────────────────
  void campaignWithStartingScenarioAccepted();
  void neitherCampaignNorScenarioIdRejected();
  void neitherCampaignNorScenarioIdRejectedWhenKeysAbsent();
  void campaignOnlyAccepted();
  void scenarioOnlyAccepted();

  // Null-vs-omitted defaultable fields ───────────────────────────────────────
  void strictAsIfAtOmittedWhenUnset();
  void asIfRulingOmittedWhenUnset();
  void asIfRulingLegacySpellingDecodesToCanonicalValue();
  void ultimatumsAndBoonsAbsentAndExplicitNullResolveIdentically();
  void achievementsEnabledAbsentAndExplicitNullResolveIdentically();
  void strictAsIfAtAbsentAndExplicitNullResolveIdentically();
  void asIfRulingAbsentAndExplicitNullResolveIdentically();
  void chooseDeckUrlAbsentAndExplicitNullResolveIdentically();
  void deckListInputTabooIdAbsentAndExplicitNullResolveIdentically();

  // Malformed / wrong-type / missing ─────────────────────────────────────────
  void malformedUuidInDeckIdsRejected();
  void malformedCardCodeInInvestigatorSummaryRejected();
  void missingRequiredCampaignKeyRejected();
  void wrongTypeForPlayerCountRejected();
  void wholeValueNotObjectRejected();
  void unrecognizedUltimatumOrBoonRejected();
  void unrecognizedClassSymbolInInvestigatorSummaryRejected();

  // Additive unknown fields ignored ──────────────────────────────────────────
  void additiveUnknownFieldsIgnoredInChooseDeck();
  void additiveUnknownFieldsIgnoredInClaimSeat();

  // ClaimSeatRequest / ChooseDeckRequest ─────────────────────────────────────
  void chooseDeckWithDeckListRoundTrips();
  void chooseDeckRawBytesPreserveExactNumericDeckIdAndSideSlots();
  void chooseDeckRawBytesDistinguishAbsentAndNullDeckId();
  void claimSeatRoundTrips();
};

namespace {

QByteArray loadFixtureBytes(const QString &fileName) {
  QFile f(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + u"/fixtures/" + fileName);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return f.readAll();
}

QJsonObject loadFixtureObject(const QString &fileName) {
  return QJsonDocument::fromJson(loadFixtureBytes(fileName)).object();
}

QJsonArray loadFixtureArray(const QString &fileName) {
  return QJsonDocument::fromJson(loadFixtureBytes(fileName)).array();
}

QJsonObject lifecycleFixture() {
  return loadFixtureObject(QStringLiteral("game-lifecycle.json"));
}

QJsonObject withoutKey(QJsonObject obj, QLatin1StringView key) {
  obj.remove(key);
  return obj;
}

} // namespace

void GamesTests::decodesPendingRowFromFixture() {
  const QJsonArray rows = loadFixtureArray(QStringLiteral("game-list.json"));
  QCOMPARE(rows.size(), 5);
  const auto result = GameListRow::fromJson(rows.at(0), u"rows[0]");
  if (!result)
    QFAIL(qPrintable(result.error()));

  QCOMPARE(result->kind(), GameListRow::Kind::Success);
  QCOMPARE(result->id()->value(),
           QStringLiteral("00000000-0000-0000-0000-000000000003"));
  QVERIFY(result->scenario().has_value());
  QCOMPARE(result->scenario()->id.value(), QStringLiteral("c01104"));
  QCOMPARE(result->scenario()->difficulty, Difficulty::Easy);
  QVERIFY(!result->campaign().has_value());
  QVERIFY(result->gameState().has_value());
  QCOMPARE(result->gameState()->kind(), GameState::Kind::Pending);
  QVERIFY(result->gameState()->playerIds().isEmpty());
  QCOMPARE(result->name(), QStringLiteral("Contract fixture game"));
  QVERIFY(result->investigators().isEmpty());
  QCOMPARE(*result->multiplayerVariant(), MultiplayerVariant::Solo);
  QVERIFY(!result->hasOpenSeats());

  QCOMPARE(result->toJson(), rows.at(0).toObject());
}

void GamesTests::decodesChooseDecksRowFromFixture() {
  const QJsonArray rows = loadFixtureArray(QStringLiteral("game-list.json"));
  const auto result = GameListRow::fromJson(rows.at(1), u"rows[1]");
  if (!result)
    QFAIL(qPrintable(result.error()));

  QVERIFY(!result->scenario().has_value());
  QVERIFY(result->campaign().has_value());
  QCOMPARE(result->campaign()->id.value(), QStringLiteral("06"));
  QCOMPARE(result->campaign()->difficulty, Difficulty::Easy);
  QCOMPARE(*result->campaign()->currentCampaignMode,
           CampaignPart::TheDreamQuest);
  QCOMPARE(result->gameState()->kind(), GameState::Kind::ChooseDecks);
  QCOMPARE(result->gameState()->playerIds().size(), 1);
  QCOMPARE(
      result->gameState()->playerIds().at(0).toString(QUuid::WithoutBraces),
      QStringLiteral("00000000-0000-0000-0000-000000000001"));
  QCOMPARE(result->investigators().size(), 2);
  QCOMPARE(result->investigators().at(0).id.value(), QStringLiteral("c06001"));
  QCOMPARE(result->investigators().at(0).classSymbol, ClassSymbol::Guardian);
  QCOMPARE(result->otherInvestigators().size(), 1);
  QCOMPARE(*result->multiplayerVariant(), MultiplayerVariant::WithFriends);
  QVERIFY(result->hasOpenSeats());

  QCOMPARE(result->toJson(), rows.at(1).toObject());
}

void GamesTests::decodesActiveRowFromFixture() {
  const QJsonArray rows = loadFixtureArray(QStringLiteral("game-list.json"));
  const auto result = GameListRow::fromJson(rows.at(2), u"rows[2]");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->gameState()->kind(), GameState::Kind::Active);
  QCOMPARE(result->toJson(), rows.at(2).toObject());
}

void GamesTests::decodesOverRowFromFixture() {
  const QJsonArray rows = loadFixtureArray(QStringLiteral("game-list.json"));
  const auto result = GameListRow::fromJson(rows.at(3), u"rows[3]");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->gameState()->kind(), GameState::Kind::Over);
  QCOMPARE(result->toJson(), rows.at(3).toObject());
}

void GamesTests::decodesFailureRowFromFixture() {
  const QJsonArray rows = loadFixtureArray(QStringLiteral("game-list.json"));
  const auto result = GameListRow::fromJson(rows.at(4), u"rows[4]");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->kind(), GameListRow::Kind::Failure);
  QCOMPARE(result->error(), QStringLiteral("Contract fixture failed to load."));
  QCOMPARE(result->toJson(), rows.at(4).toObject());
}

void GamesTests::decodeGameListRoundTripsWholeFixtureByteExact() {
  const QJsonArray rows = loadFixtureArray(QStringLiteral("game-list.json"));
  const auto result = decodeGameList(rows, u"rows");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->size(), 5);
  QCOMPARE(encodeGameList(*result), rows);
}

void GamesTests::unknownGameStateTagPreservedAndRoundTrips() {
  const QJsonObject obj{{QStringLiteral("tag"), QStringLiteral("IsSuspended")}};
  const auto result = GameState::fromJson(obj, u"gameState");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->kind(), GameState::Kind::Unknown);
  QCOMPARE(result->unknownTag(), QStringLiteral("IsSuspended"));
  QVERIFY(result->unknownContents().isUndefined());
  QCOMPARE(result->toJson(), obj);
}

void GamesTests::unknownGameStateTagWithContentsPreservedAndRoundTrips() {
  // A future GameState variant may carry a "contents" payload of any shape
  // (matching the tagged-object encoding Pending/ChooseDecks already use);
  // it must round-trip losslessly rather than being silently dropped.
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("IsSuspended")},
      {QStringLiteral("contents"),
       QJsonObject{{QStringLiteral("reason"), QStringLiteral("maintenance")}}},
  };
  const auto result = GameState::fromJson(obj, u"gameState");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->kind(), GameState::Kind::Unknown);
  QCOMPARE(result->unknownTag(), QStringLiteral("IsSuspended"));
  QVERIFY(!result->unknownContents().isUndefined());
  QCOMPARE(
      result->unknownContents().value(QLatin1StringView("reason")).toString(),
      QStringLiteral("maintenance"));
  QCOMPARE(result->toJson(), obj);
}

void GamesTests::gameStatePendingRejectsNullUuidInPlayerIds() {
  // A seat can never genuinely be owed to "no one": GameState::pending()'s
  // validated factory must reject the all-zero uuid even when called
  // directly (i.e. not just via fromJson's own per-element decodeUuid
  // check), since that factory is the single source of truth for this
  // invariant.
  const auto result =
      GameState::pending(QList<QUuid>{QUuid(), QUuid::createUuid()});
  QVERIFY(!result);
}

void GamesTests::gameStateChooseDecksRejectsNullUuidInPlayerIds() {
  const auto result =
      GameState::chooseDecks(QList<QUuid>{QUuid::createUuid(), QUuid()});
  QVERIFY(!result);
}

void GamesTests::gameStateIsActiveWithContentsRejected() {
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("IsActive")},
      {QStringLiteral("contents"), QJsonArray{}},
  };
  const auto result = GameState::fromJson(obj, u"gameState");
  QVERIFY(!result);
}

void GamesTests::gameStateIsOverWithContentsRejected() {
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("IsOver")},
      {QStringLiteral("contents"), QJsonArray{}},
  };
  const auto result = GameState::fromJson(obj, u"gameState");
  QVERIFY(!result);
}

void GamesTests::gameStateIsActiveWithNullContentsRejected() {
  // Even an explicit JSON null for "contents" must be rejected on a known
  // nullary tag -- presence is what matters, not nullness.
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("IsActive")},
      {QStringLiteral("contents"), QJsonValue::Null},
  };
  const auto result = GameState::fromJson(obj, u"gameState");
  QVERIFY(!result);
}

void GamesTests::gameStateRawBytesPreserveAdditiveFieldBesideTagAndContents() {
  // A future backend release may add a sibling key alongside "tag"/
  // "contents" on an unrecognized GameState tag (e.g. a "since" timestamp
  // on a hypothetical IsSuspended variant); the *complete* decoded object
  // must survive -- not merely the two keys this client currently knows to
  // look for -- so toJson() below must re-emit "since" too, aggregately
  // proven through the canonical byte parser rather than fromJson() alone.
  const QByteArray bytes = QByteArrayLiteral(
      "{\"tag\":\"IsSuspended\",\"contents\":{\"reason\":\"maintenance\"},"
      "\"since\":123456}");
  const auto result = GameState::fromRawBytes(bytes, u"gameState");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->kind(), GameState::Kind::Unknown);
  QCOMPARE(result->unknownTag(), QStringLiteral("IsSuspended"));
  QVERIFY(result->unknownRaw().isObject());
  QCOMPARE(result->unknownRaw()
               .value(QLatin1StringView("since"))
               .toRawNumber()
               .literal(),
           QStringLiteral("123456"));
  const auto encoded = result->toJson();
  QCOMPARE(encoded.value(QStringLiteral("since")).toInt(), 123456);
  QCOMPARE(QJsonDocument(encoded).toJson(QJsonDocument::Compact),
           QJsonDocument::fromJson(bytes).toJson(QJsonDocument::Compact));
}

void GamesTests::
    gameStateRawBytesPreserveHugeNestedNumericLiteralInUnknownContents() {
  // 9007199254740993 == 2^53 + 1, the smallest positive integer a double
  // cannot represent exactly; nested inside an unrecognized tag's
  // "contents", it must survive the canonical byte parser boundary
  // byte-exact rather than being rounded by any QJsonValue-mediated path.
  const QByteArray bytes =
      QByteArrayLiteral("{\"tag\":\"IsSuspended\",\"contents\":{\"nested\":"
                        "[9007199254740993,1e300]}}");
  const auto result = GameState::fromRawBytes(bytes, u"gameState");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->kind(), GameState::Kind::Unknown);
  const Json::Value nested =
      result->unknownContents().value(QLatin1StringView("nested"));
  QVERIFY(nested.isArray());
  QCOMPARE(nested.toArray().size(), 2);
  QCOMPARE(nested.toArray().at(0).toRawNumber().literal(),
           QStringLiteral("9007199254740993"));
  QCOMPARE(nested.toArray().at(1).toRawNumber().literal(),
           QStringLiteral("1e300"));

  // toJson() re-emits the complete raw object; parsing it back through the
  // canonical byte decoder must yield the identical numeric literals --
  // proof the QJsonObject-typed toJson() convenience encoder still carries
  // the exact digits forward for any caller that re-parses it byte-exact,
  // even though QJsonObject's own in-memory storage is double-backed.
  const QByteArray reencoded =
      QJsonDocument(result->toJson()).toJson(QJsonDocument::Compact);
  const auto reparsed = GameState::fromRawBytes(reencoded, u"gameState");
  if (!reparsed)
    QFAIL(qPrintable(reparsed.error()));
  const Json::Value reparsedNested =
      reparsed->unknownContents().value(QLatin1StringView("nested"));
  QCOMPARE(reparsedNested.toArray().at(0).toRawNumber().literal(),
           QStringLiteral("9007199254740993"));
}

void GamesTests::rowWithBareErrorKeyIsFailure() {
  const QJsonObject obj{{QStringLiteral("error"), QStringLiteral("boom")}};
  const auto result = GameListRow::fromJson(obj, u"row");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->kind(), GameListRow::Kind::Failure);
  QCOMPARE(result->error(), QStringLiteral("boom"));
}

void GamesTests::rowCombiningErrorWithAnyOtherKeyIsRejectedNotFailure() {
  // failedGameDetails is additionalProperties:false with "error" as its
  // only allowed key, so an object carrying "error" alongside any other
  // key -- even one that isn't a recognized success field -- matches no
  // valid shape at all. It must be a decode error, not a Kind::Failure row
  // that silently discards the extra key(s).
  const QJsonObject obj{{QStringLiteral("error"), QStringLiteral("boom")},
                        {QStringLiteral("name"), QStringLiteral("ignored")}};
  const auto result = GameListRow::fromJson(obj, u"row");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("error")),
           qPrintable(result.error()));
}

void GamesTests::rowWithoutErrorKeyAttemptsFullDecode() {
  const QJsonObject obj{{QStringLiteral("name"), QStringLiteral("incomplete")}};
  const auto result = GameListRow::fromJson(obj, u"row");
  QVERIFY(!result.has_value());
  // Missing "id" (the first field decoded after the shape check), not
  // mistaken for a Failure row.
  QVERIFY2(result.error().contains(QStringLiteral("id")),
           qPrintable(result.error()));
}

void GamesTests::decodesCreateGameFromFixture() {
  const QJsonObject fixture = lifecycleFixture();
  const QJsonValue createGame = fixture.value("createGame"_L1);
  const auto result = CreateGameRequest::fromJson(createGame, u"createGame");
  if (!result)
    QFAIL(qPrintable(result.error()));

  QCOMPARE(result->deckIds.size(), 2);
  QVERIFY(result->deckIds.at(0).has_value());
  QCOMPARE(result->deckIds.at(0)->toString(QUuid::WithoutBraces),
           QStringLiteral("00000000-0000-0000-0000-000000000017"));
  QVERIFY(!result->deckIds.at(1).has_value());
  QCOMPARE(result->playerCount, 2);
  QVERIFY(result->campaignOrScenario.isCampaign());
  QCOMPARE(result->campaignOrScenario.campaignId()->value(),
           QStringLiteral("01"));
  QVERIFY(!result->campaignOrScenario.scenarioId().has_value());
  QCOMPARE(result->difficulty, Difficulty::Standard);
  QCOMPARE(result->campaignName, QStringLiteral("Contract campaign"));
  QCOMPARE(result->multiplayerVariant, MultiplayerVariant::WithFriends);
  QVERIFY(result->includeTarotReadings);
  QCOMPARE(result->options.size(), 2);
  QCOMPARE(result->options.at(0).kind(), CampaignOptionRequest::Kind::Known);
  QCOMPARE(*result->options.at(0).known(), KnownCampaignOption::PerformIntro);
  QCOMPARE(result->options.at(1).kind(), CampaignOptionRequest::Kind::Variant);
  QCOMPARE(result->options.at(1).text(), QStringLiteral("return-to"));
  QVERIFY(result->strictAsIfAt.has_value());
  QVERIFY(!*result->strictAsIfAt);
  QVERIFY(result->asIfRuling.has_value());
  QCOMPARE(*result->asIfRuling, AsIfRulingValue::Chapter1AsIfRuling);
  QCOMPARE(result->ultimatumsAndBoons.size(), 2);
  QCOMPARE(result->ultimatumsAndBoons.at(0), UltimatumOrBoon::BoonOfHades);
  QCOMPARE(result->ultimatumsAndBoons.at(1), UltimatumOrBoon::UltimatumOfChaos);
  QVERIFY(!result->achievementsEnabled);

  // Fixture carries an additive "unknownField" this client does not model;
  // re-encoding correctly drops it.
  QCOMPARE(result->toJson(),
           withoutKey(createGame.toObject(), "unknownField"_L1));
}

void GamesTests::decodesCreateGameDefaultsFromFixture() {
  const QJsonObject fixture = lifecycleFixture();
  const QJsonValue v = fixture.value("createGameDefaults"_L1);
  const auto result = CreateGameRequest::fromJson(v, u"createGameDefaults");
  if (!result)
    QFAIL(qPrintable(result.error()));

  QVERIFY(result->deckIds.isEmpty());
  QCOMPARE(result->playerCount, 1);
  QVERIFY(!result->campaignOrScenario.isCampaign());
  QCOMPARE(result->campaignOrScenario.scenarioId()->value(),
           QStringLiteral("01104"));
  QCOMPARE(result->difficulty, Difficulty::Easy);
  QCOMPARE(result->multiplayerVariant, MultiplayerVariant::Solo);
  QVERIFY(!result->includeTarotReadings);
  QVERIFY(result->options.isEmpty());
  QVERIFY(!result->strictAsIfAt.has_value());
  QVERIFY(!result->asIfRuling.has_value());
  QVERIFY(result->ultimatumsAndBoons.isEmpty());
  QVERIFY(result->achievementsEnabled); // default true.

  // strictAsIfAt/asIfRuling are omitted (not merely null) on re-encode, but
  // ultimatumsAndBoons/achievementsEnabled are always emitted (already
  // resolved to their default) even though the fixture itself omits them.
  const QJsonObject reencoded = result->toJson();
  QVERIFY(!reencoded.contains(QStringLiteral("strictAsIfAt")));
  QVERIFY(!reencoded.contains(QStringLiteral("asIfRuling")));
  QJsonObject expected = v.toObject();
  expected.insert(QStringLiteral("ultimatumsAndBoons"), QJsonArray{});
  expected.insert(QStringLiteral("achievementsEnabled"), true);
  QCOMPARE(reencoded, expected);
}

void GamesTests::createGameSupports64BitPlayerCount() {
  const qint64 largePlayerCount = qint64{3000000000LL};
  const QJsonObject obj{
      {QStringLiteral("deckIds"), QJsonArray{}},
      {QStringLiteral("playerCount"), QJsonValue(largePlayerCount)},
      {QStringLiteral("campaignId"), QJsonValue()},
      {QStringLiteral("scenarioId"), QStringLiteral("01104")},
      {QStringLiteral("difficulty"), QStringLiteral("Easy")},
      {QStringLiteral("campaignName"), QStringLiteral("X")},
      {QStringLiteral("multiplayerVariant"), QStringLiteral("Solo")},
      {QStringLiteral("includeTarotReadings"), false},
      {QStringLiteral("options"), QJsonArray{}},
  };

  const auto result = CreateGameRequest::fromJson(obj, u"request");
  if (!result)
    QFAIL(qPrintable(result.error()));

  QCOMPARE(result->playerCount, largePlayerCount);
  QCOMPARE(result->toJson().value(QStringLiteral("playerCount")).toInteger(),
           largePlayerCount);
}

void GamesTests::createGameDefaultsAndNullDefaultsDecodeIdentically() {
  // createGameDefaults omits strictAsIfAt/asIfRuling/ultimatumsAndBoons/
  // achievementsEnabled entirely; createGameNullDefaults sets each
  // explicitly to JSON null. Both collapse to the identical resolved
  // value, matching the backend's `.:? ... .!= <default>` parse.
  const QJsonObject fixture = lifecycleFixture();
  const auto defaults = CreateGameRequest::fromJson(
      fixture.value("createGameDefaults"_L1), u"createGameDefaults");
  const auto nullDefaults = CreateGameRequest::fromJson(
      fixture.value("createGameNullDefaults"_L1), u"createGameNullDefaults");
  if (!defaults)
    QFAIL(qPrintable(defaults.error()));
  if (!nullDefaults)
    QFAIL(qPrintable(nullDefaults.error()));
  QVERIFY(*defaults == *nullDefaults);

  // Re-encoding the explicit-null fixture also omits the keys (there is
  // nothing left to distinguish once decoded).
  const QJsonObject reencoded = nullDefaults->toJson();
  QVERIFY(!reencoded.contains(QStringLiteral("strictAsIfAt")));
  QVERIFY(!reencoded.contains(QStringLiteral("asIfRuling")));
}

void GamesTests::decodesChooseDeckFromFixture() {
  const QJsonObject fixture = lifecycleFixture();
  const QJsonValue v = fixture.value("chooseDeck"_L1);
  const auto result = ChooseDeckRequest::fromJson(v, u"chooseDeck");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->investigatorId.value(), QStringLiteral("01001"));
  QCOMPARE(*result->deckUrl,
           QStringLiteral("https://arkhamdb.com/decklist/view/4242"));
  QVERIFY(result->deckList.has_value());
  QCOMPARE(result->deckList->investigatorCode.value(), QStringLiteral("01001"));

  // The top-level object carries an additive "unknownField" this client
  // does not model, and the nested deckList's "taboo_id" is present as an
  // explicit JSON null; DeckListInput collapses absent/null identically and
  // omits the key once unset, so both must be stripped from the expected
  // shape before an otherwise byte-exact comparison.
  QJsonObject expected = withoutKey(v.toObject(), "unknownField"_L1);
  QJsonObject expectedDeckList =
      withoutKey(expected.value("deckList"_L1).toObject(), "taboo_id"_L1);
  expected.insert(QStringLiteral("deckList"), expectedDeckList);
  QCOMPARE(result->toJson(), expected);
}

void GamesTests::decodesContinueWithoutUpgradeFromFixture() {
  const QJsonObject fixture = lifecycleFixture();
  const QJsonValue v = fixture.value("continueWithoutUpgrade"_L1);
  const auto result = ChooseDeckRequest::fromJson(v, u"continueWithoutUpgrade");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->investigatorId.value(), QStringLiteral("c01001"));
  QVERIFY(!result->deckUrl.has_value());
  QVERIFY(!result->deckList.has_value());
  QCOMPARE(result->toJson(), v.toObject());
}

void GamesTests::decodesClaimSeatFromFixture() {
  const QJsonObject fixture = lifecycleFixture();
  const QJsonValue v = fixture.value("claimSeat"_L1);
  const auto result = ClaimSeatRequest::fromJson(v, u"claimSeat");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->investigatorId.value(), QStringLiteral("01001"));
  QCOMPARE(result->toJson(), withoutKey(v.toObject(), "unknownField"_L1));
}

void GamesTests::chooseDeckMissingInvestigatorIdRejected() {
  const QJsonObject obj{
      {QStringLiteral("deckUrl"),
       QStringLiteral("https://arkhamdb.com/decklist/view/4242")},
  };
  const auto result = ChooseDeckRequest::fromJson(obj, u"chooseDeck");
  QVERIFY(!result.has_value());
  // Asserts the exact, clearer "missing required field" phrasing rather
  // than the less specific "expected string, got missing" a bare
  // value-decoder call would produce for an absent key -- see
  // Json::requireField (JsonDecode.h).
  QCOMPARE(result.error(),
           QStringLiteral("chooseDeck.investigatorId: missing required field "
                          "\"investigatorId\""));
}

void GamesTests::claimSeatMissingInvestigatorIdRejected() {
  const QJsonObject obj{};
  const auto result = ClaimSeatRequest::fromJson(obj, u"claimSeat");
  QVERIFY(!result.has_value());
  QCOMPARE(result.error(),
           QStringLiteral("claimSeat.investigatorId: missing required field "
                          "\"investigatorId\""));
}

void GamesTests::decodesOpenSeatsFromFixture() {
  const QJsonObject fixture = lifecycleFixture();
  const QJsonValue v = fixture.value("openSeats"_L1);
  const auto result = decodeOpenSeats(v, u"openSeats");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->size(), 2);
  QCOMPARE(result->at(0).value(), QStringLiteral("c01001"));
  QCOMPARE(encodeOpenSeats(*result), v.toArray());
}

void GamesTests::unknownCampaignOptionWithContentsPreservedAndRoundTrips() {
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("SomeFutureOption")},
      {QStringLiteral("contents"), QStringLiteral("payload")}};
  const auto result = CampaignOption::fromJson(obj, u"option");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->kind(), CampaignOption::Kind::Unknown);
  QCOMPARE(result->text(), QStringLiteral("SomeFutureOption"));
  QCOMPARE(result->unknownContents(),
           toRawJson(QJsonValue(QStringLiteral("payload"))));
  QCOMPARE(result->toJson(), obj);
}

void GamesTests::unknownCampaignOptionWithoutContentsPreservedAndRoundTrips() {
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("SomeFutureFlag")}};
  const auto result = CampaignOption::fromJson(obj, u"option");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->kind(), CampaignOption::Kind::Unknown);
  QVERIFY(result->unknownContents().isUndefined());
  QCOMPARE(result->toJson(), obj);
}

void GamesTests::knownCampaignOptionRoundTrips() {
  const CampaignOption option =
      CampaignOption::knownOption(KnownCampaignOption::TakeTheNecronomicon);
  QCOMPARE(option.toJson(),
           (QJsonObject{{QStringLiteral("tag"),
                         QStringLiteral("TakeTheNecronomicon")}}));
  const auto decoded = CampaignOption::fromJson(option.toJson(), u"option");
  if (!decoded)
    QFAIL(qPrintable(decoded.error()));
  QVERIFY(*decoded == option);
}

void GamesTests::campaignVariantOptionRoundTrips() {
  const CampaignOption option =
      CampaignOption::variantOption(QStringLiteral("return-to"));
  const QJsonObject expected{
      {QStringLiteral("tag"), QStringLiteral("CampaignVariant")},
      {QStringLiteral("contents"), QStringLiteral("return-to")}};
  QCOMPARE(option.toJson(), expected);
}

void GamesTests::unknownCampaignOptionCannotBeMistakenForKnownOption() {
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("NotARealOption")}};
  const auto decoded = CampaignOption::fromJson(obj, u"option");
  if (!decoded)
    QFAIL(qPrintable(decoded.error()));
  QCOMPARE(decoded->kind(), CampaignOption::Kind::Unknown);
  QVERIFY(!decoded->known().has_value());
  // There is no public factory that lets calling code fabricate an
  // Unknown-kind option directly -- knownOption()/variantOption() are the
  // only constructors besides fromJson.
  QVERIFY(*decoded !=
          CampaignOption::knownOption(KnownCampaignOption::Cheated));
}

void GamesTests::unknownCampaignOptionCannotBeSubmittedAsRequestOption() {
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("NotARealOption")}};
  const auto decoded = CampaignOption::fromJson(obj, u"option");
  if (!decoded)
    QFAIL(qPrintable(decoded.error()));
  // An option this client decoded but could not interpret must never be
  // resubmittable into a request as if the server already understood it.
  const auto narrowed = decoded->toRequestOption(u"option");
  QVERIFY(!narrowed);
}

void GamesTests::knownCampaignOptionNarrowsToRequestOption() {
  const CampaignOption known =
      CampaignOption::knownOption(KnownCampaignOption::TakeTheNecronomicon);
  const auto narrowed = known.toRequestOption(u"option");
  if (!narrowed)
    QFAIL(qPrintable(narrowed.error()));
  QCOMPARE(narrowed->kind(), CampaignOptionRequest::Kind::Known);
  QCOMPARE(*narrowed->known(), KnownCampaignOption::TakeTheNecronomicon);
  QCOMPARE(narrowed->toJson(), known.toJson());

  const CampaignOption variant =
      CampaignOption::variantOption(QStringLiteral("return-to"));
  const auto narrowedVariant = variant.toRequestOption(u"option");
  if (!narrowedVariant)
    QFAIL(qPrintable(narrowedVariant.error()));
  QCOMPARE(narrowedVariant->kind(), CampaignOptionRequest::Kind::Variant);
  QCOMPARE(narrowedVariant->text(), QStringLiteral("return-to"));
  QCOMPARE(narrowedVariant->toJson(), variant.toJson());
}

void GamesTests::campaignOptionRequestRejectsUnrecognizedTag() {
  // Unlike CampaignOption, CampaignOptionRequest has no forward-compatible
  // fallback: a request-bound value can only ever contain an option this
  // client understands, so an unrecognized tag is a hard decode failure.
  const QJsonObject obj{
      {QStringLiteral("tag"), QStringLiteral("NotARealOption")}};
  const auto result = CampaignOptionRequest::fromJson(obj, u"option");
  QVERIFY(!result);
}

void GamesTests::
    campaignOptionRawBytesPreserveAdditiveFieldBesideTagAndContents() {
  // A future backend release may add a sibling key alongside "tag"/
  // "contents" on an unrecognized CampaignOption tag; the *complete*
  // decoded object must survive -- not merely the two keys this client
  // currently knows to look for -- proven through the canonical byte
  // parser rather than fromJson() alone.
  const QByteArray bytes = QByteArrayLiteral(
      "{\"tag\":\"SomeFutureOption\",\"contents\":\"payload\","
      "\"isSuspended\":true}");
  const auto result = CampaignOption::fromRawBytes(bytes, u"option");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->kind(), CampaignOption::Kind::Unknown);
  QCOMPARE(result->text(), QStringLiteral("SomeFutureOption"));
  QVERIFY(result->unknownRaw().isObject());
  QVERIFY(
      result->unknownRaw().value(QLatin1StringView("isSuspended")).toBool());
  QCOMPARE(QJsonDocument(result->toJson()).toJson(QJsonDocument::Compact),
           QJsonDocument::fromJson(bytes).toJson(QJsonDocument::Compact));
}

void GamesTests::
    campaignOptionRawBytesPreserveHugeNestedNumericLiteralInUnknownContents() {
  // 9007199254740993 == 2^53 + 1, the smallest positive integer a double
  // cannot represent exactly; nested inside an unrecognized tag's
  // "contents" metadata, it must survive the canonical byte parser
  // boundary byte-exact.
  const QByteArray bytes =
      QByteArrayLiteral("{\"tag\":\"FutureOption\",\"contents\":{\"limit\":"
                        "9007199254740993}}");
  const auto result = CampaignOption::fromRawBytes(bytes, u"option");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->kind(), CampaignOption::Kind::Unknown);
  const Json::Value limit =
      result->unknownContents().value(QLatin1StringView("limit"));
  QCOMPARE(limit.toRawNumber().literal(), QStringLiteral("9007199254740993"));

  // An unknown option can never be submitted as a request, regardless of
  // what metadata it carries.
  const auto asRequest = result->toRequestOption(u"option");
  QVERIFY(!asRequest);
}

void GamesTests::campaignWithStartingScenarioAccepted() {
  // The backend dispatches on campaignId alone: `newCampaign cid
  // scenarioId ...` runs whether or not scenarioId is also set, so a
  // campaign carrying a starting scenario is a valid (not rejected)
  // combination -- see CampaignOrScenario's doc comment in Games.h.
  const QJsonObject obj{
      {QStringLiteral("campaignId"), QStringLiteral("01")},
      {QStringLiteral("scenarioId"), QStringLiteral("01104")}};
  const auto result = CampaignOrScenario::fromJson(obj, u"request");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->isCampaign());
  QCOMPARE(result->campaignId()->value(), QStringLiteral("01"));
  QVERIFY(result->scenarioId().has_value());
  QCOMPARE(result->scenarioId()->value(), QStringLiteral("01104"));
  QJsonObject encoded;
  result->insertInto(encoded);
  QCOMPARE(encoded, obj);

  // The factory-based construction path produces the same encoded shape.
  const CampaignOrScenario viaFactory =
      CampaignOrScenario::campaignWithStartingScenario(
          *CampaignId::parse(u"01"_s), *ScenarioId::parse(u"01104"_s));
  QJsonObject encodedViaFactory;
  viaFactory.insertInto(encodedViaFactory);
  QCOMPARE(encodedViaFactory, obj);
}

void GamesTests::neitherCampaignNorScenarioIdRejected() {
  const QJsonObject obj{{QStringLiteral("campaignId"), QJsonValue()},
                        {QStringLiteral("scenarioId"), QJsonValue()}};
  const auto result = CampaignOrScenario::fromJson(obj, u"request");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("must be set")),
           qPrintable(result.error()));
}

void GamesTests::neitherCampaignNorScenarioIdRejectedWhenKeysAbsent() {
  // Absent and explicit-null collapse identically (matching the backend's
  // `.:?` parse for both keys) -- the invariant must reject this
  // combination the same way whether the keys are omitted entirely or
  // present-but-null.
  const QJsonObject obj{};
  const auto result = CampaignOrScenario::fromJson(obj, u"request");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("must be set")),
           qPrintable(result.error()));
}

void GamesTests::campaignOnlyAccepted() {
  const QJsonObject obj{{QStringLiteral("campaignId"), QStringLiteral("01")},
                        {QStringLiteral("scenarioId"), QJsonValue()}};
  const auto result = CampaignOrScenario::fromJson(obj, u"request");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->isCampaign());
  QCOMPARE(result->campaignId()->value(), QStringLiteral("01"));
  QJsonObject encoded;
  result->insertInto(encoded);
  QCOMPARE(encoded, obj);
}

void GamesTests::scenarioOnlyAccepted() {
  const QJsonObject obj{
      {QStringLiteral("campaignId"), QJsonValue()},
      {QStringLiteral("scenarioId"), QStringLiteral("01104")}};
  const auto result = CampaignOrScenario::fromJson(obj, u"request");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(!result->isCampaign());
  QCOMPARE(result->scenarioId()->value(), QStringLiteral("01104"));
  QJsonObject encoded;
  result->insertInto(encoded);
  QCOMPARE(encoded, obj);

  // The factory-based construction path (not just fromJson) produces the
  // same encoded shape.
  const CampaignOrScenario viaFactory =
      CampaignOrScenario::scenario(*ScenarioId::parse(u"01104"_s));
  QJsonObject encodedViaFactory;
  viaFactory.insertInto(encodedViaFactory);
  QCOMPARE(encodedViaFactory, obj);
}

void GamesTests::strictAsIfAtOmittedWhenUnset() {
  const QJsonObject fixture = lifecycleFixture();
  const auto result = CreateGameRequest::fromJson(
      fixture.value("createGameDefaults"_L1), u"createGameDefaults");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(!result->toJson().contains(QStringLiteral("strictAsIfAt")));
}

void GamesTests::asIfRulingOmittedWhenUnset() {
  const QJsonObject fixture = lifecycleFixture();
  const auto result = CreateGameRequest::fromJson(
      fixture.value("createGameDefaults"_L1), u"createGameDefaults");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(!result->toJson().contains(QStringLiteral("asIfRuling")));
}

void GamesTests::asIfRulingLegacySpellingDecodesToCanonicalValue() {
  // AsIfRuling is exercised through CreateGameRequest, since it has no
  // standalone free function; build a minimal valid request and vary only
  // asIfRuling.
  auto buildRequest = [](const QString &asIfRuling) {
    return QJsonObject{
        {QStringLiteral("deckIds"), QJsonArray{}},
        {QStringLiteral("playerCount"), 1},
        {QStringLiteral("campaignId"), QJsonValue()},
        {QStringLiteral("scenarioId"), QStringLiteral("01104")},
        {QStringLiteral("difficulty"), QStringLiteral("Easy")},
        {QStringLiteral("campaignName"), QStringLiteral("X")},
        {QStringLiteral("multiplayerVariant"), QStringLiteral("Solo")},
        {QStringLiteral("includeTarotReadings"), false},
        {QStringLiteral("options"), QJsonArray{}},
        {QStringLiteral("asIfRuling"), asIfRuling},
    };
  };

  const auto legacyResult = CreateGameRequest::fromJson(
      buildRequest(QStringLiteral("Chapter1AsIfRuling")), u"request");
  if (!legacyResult)
    QFAIL(qPrintable(legacyResult.error()));
  QCOMPARE(*legacyResult->asIfRuling, AsIfRulingValue::Chapter1AsIfRuling);

  const auto shortResult = CreateGameRequest::fromJson(
      buildRequest(QStringLiteral("chapter1")), u"request");
  if (!shortResult)
    QFAIL(qPrintable(shortResult.error()));
  QCOMPARE(*shortResult->asIfRuling, AsIfRulingValue::Chapter1AsIfRuling);

  QVERIFY(*legacyResult == *shortResult);
  // Re-encode always uses the canonical short form, even when decoded from
  // the legacy spelling.
  QCOMPARE(
      legacyResult->toJson().value(QStringLiteral("asIfRuling")).toString(),
      QStringLiteral("chapter1"));

  const auto legacy2 = CreateGameRequest::fromJson(
      buildRequest(QStringLiteral("Chapter2AsIfRuling")), u"request");
  if (!legacy2)
    QFAIL(qPrintable(legacy2.error()));
  QCOMPARE(legacy2->toJson().value(QStringLiteral("asIfRuling")).toString(),
           QStringLiteral("chapter2"));
}

void GamesTests::ultimatumsAndBoonsAbsentAndExplicitNullResolveIdentically() {
  const QJsonObject fixture = lifecycleFixture();
  const auto defaults = CreateGameRequest::fromJson(
      fixture.value("createGameDefaults"_L1), u"createGameDefaults");
  const auto nullDefaults = CreateGameRequest::fromJson(
      fixture.value("createGameNullDefaults"_L1), u"createGameNullDefaults");
  if (!defaults)
    QFAIL(qPrintable(defaults.error()));
  if (!nullDefaults)
    QFAIL(qPrintable(nullDefaults.error()));
  QCOMPARE(defaults->ultimatumsAndBoons, nullDefaults->ultimatumsAndBoons);
  QVERIFY(defaults->ultimatumsAndBoons.isEmpty());
}

void GamesTests::achievementsEnabledAbsentAndExplicitNullResolveIdentically() {
  const QJsonObject fixture = lifecycleFixture();
  const auto defaults = CreateGameRequest::fromJson(
      fixture.value("createGameDefaults"_L1), u"createGameDefaults");
  const auto nullDefaults = CreateGameRequest::fromJson(
      fixture.value("createGameNullDefaults"_L1), u"createGameNullDefaults");
  if (!defaults)
    QFAIL(qPrintable(defaults.error()));
  if (!nullDefaults)
    QFAIL(qPrintable(nullDefaults.error()));
  QCOMPARE(defaults->achievementsEnabled, nullDefaults->achievementsEnabled);
  QVERIFY(defaults->achievementsEnabled);
}

void GamesTests::strictAsIfAtAbsentAndExplicitNullResolveIdentically() {
  // createGameDefaults omits strictAsIfAt entirely; createGameNullDefaults
  // carries an explicit JSON null for it. Per CreateGameRequest's doc
  // comment, strictAsIfAt has no backend default to resolve to (unlike
  // ultimatumsAndBoons/achievementsEnabled) and stays std::optional, but
  // the backend's own `.:?` parse still collapses absent and null to the
  // exact same std::nullopt -- this proves that collapse holds using the
  // same vendored fixture pair as the default-bearing fields above.
  const QJsonObject fixture = lifecycleFixture();
  const auto defaults = CreateGameRequest::fromJson(
      fixture.value("createGameDefaults"_L1), u"createGameDefaults");
  const auto nullDefaults = CreateGameRequest::fromJson(
      fixture.value("createGameNullDefaults"_L1), u"createGameNullDefaults");
  if (!defaults)
    QFAIL(qPrintable(defaults.error()));
  if (!nullDefaults)
    QFAIL(qPrintable(nullDefaults.error()));
  QCOMPARE(defaults->strictAsIfAt, nullDefaults->strictAsIfAt);
  QVERIFY(!defaults->strictAsIfAt.has_value());
}

void GamesTests::asIfRulingAbsentAndExplicitNullResolveIdentically() {
  const QJsonObject fixture = lifecycleFixture();
  const auto defaults = CreateGameRequest::fromJson(
      fixture.value("createGameDefaults"_L1), u"createGameDefaults");
  const auto nullDefaults = CreateGameRequest::fromJson(
      fixture.value("createGameNullDefaults"_L1), u"createGameNullDefaults");
  if (!defaults)
    QFAIL(qPrintable(defaults.error()));
  if (!nullDefaults)
    QFAIL(qPrintable(nullDefaults.error()));
  QCOMPARE(defaults->asIfRuling, nullDefaults->asIfRuling);
  QVERIFY(!defaults->asIfRuling.has_value());
}

void GamesTests::chooseDeckUrlAbsentAndExplicitNullResolveIdentically() {
  // Not backed by a vendored null-default fixture (only createGame has
  // one), so this uses a minimal test-authored object varying only
  // deckUrl. Proves ChooseDeckRequest's std::optional<QString> deckUrl
  // collapses absent and explicit null identically, matching the
  // backend's `.:?` parse for this key.
  const QJsonObject absentObj{
      {QStringLiteral("investigatorId"), QStringLiteral("01001")},
  };
  const QJsonObject explicitNullObj{
      {QStringLiteral("investigatorId"), QStringLiteral("01001")},
      {QStringLiteral("deckUrl"), QJsonValue()},
  };
  const auto absentResult =
      ChooseDeckRequest::fromJson(absentObj, u"chooseDeck");
  const auto nullResult =
      ChooseDeckRequest::fromJson(explicitNullObj, u"chooseDeck");
  if (!absentResult)
    QFAIL(qPrintable(absentResult.error()));
  if (!nullResult)
    QFAIL(qPrintable(nullResult.error()));
  QCOMPARE(absentResult->deckUrl, nullResult->deckUrl);
  QVERIFY(!absentResult->deckUrl.has_value());
  QVERIFY(!absentResult->toJson().contains(QStringLiteral("deckUrl")));
}

void GamesTests::deckListInputTabooIdAbsentAndExplicitNullResolveIdentically() {
  // Same rationale as chooseDeckUrlAbsentAndExplicitNullResolveIdentically,
  // but for DeckListInput::tabooId (std::optional<int>).
  const QJsonObject absentObj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
  };
  const QJsonObject explicitNullObj{
      {QStringLiteral("slots"), QJsonObject{}},
      {QStringLiteral("investigator_code"), QStringLiteral("01001")},
      {QStringLiteral("taboo_id"), QJsonValue()},
  };
  const auto absentResult = DeckListInput::fromJson(absentObj, u"deckList");
  const auto nullResult = DeckListInput::fromJson(explicitNullObj, u"deckList");
  if (!absentResult)
    QFAIL(qPrintable(absentResult.error()));
  if (!nullResult)
    QFAIL(qPrintable(nullResult.error()));
  QCOMPARE(absentResult->tabooId, nullResult->tabooId);
  QVERIFY(!absentResult->tabooId.has_value());
  QVERIFY(!absentResult->toJson().contains(QStringLiteral("taboo_id")));
}

void GamesTests::malformedUuidInDeckIdsRejected() {
  const QJsonObject obj{
      {QStringLiteral("deckIds"), QJsonArray{QStringLiteral("not-a-uuid")}},
      {QStringLiteral("playerCount"), 1},
      {QStringLiteral("campaignId"), QJsonValue()},
      {QStringLiteral("scenarioId"), QStringLiteral("01104")},
      {QStringLiteral("difficulty"), QStringLiteral("Easy")},
      {QStringLiteral("campaignName"), QStringLiteral("X")},
      {QStringLiteral("multiplayerVariant"), QStringLiteral("Solo")},
      {QStringLiteral("includeTarotReadings"), false},
      {QStringLiteral("options"), QJsonArray{}},
  };
  const auto result = CreateGameRequest::fromJson(obj, u"request");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("deckIds")),
           qPrintable(result.error()));
}

void GamesTests::malformedCardCodeInInvestigatorSummaryRejected() {
  const QJsonObject obj{
      {QStringLiteral("id"), QStringLiteral("01001")},
      {QStringLiteral("classSymbol"), QStringLiteral("Guardian")}};
  const auto result = InvestigatorSummary::fromJson(obj, u"investigator");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("id")),
           qPrintable(result.error()));
}

void GamesTests::missingRequiredCampaignKeyRejected() {
  const QJsonObject obj{
      {QStringLiteral("id"),
       QStringLiteral("00000000-0000-0000-0000-000000000003")},
      {QStringLiteral("scenario"), QJsonValue()},
      // "campaign" key entirely absent.
      {QStringLiteral("gameState"),
       QJsonObject{{QStringLiteral("tag"), QStringLiteral("IsActive")}}},
      {QStringLiteral("name"), QStringLiteral("X")},
      {QStringLiteral("investigators"), QJsonArray{}},
      {QStringLiteral("otherInvestigators"), QJsonArray{}},
      {QStringLiteral("multiplayerVariant"), QStringLiteral("Solo")},
      {QStringLiteral("hasOpenSeats"), false},
  };
  const auto result = GameListRow::fromJson(obj, u"row");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("campaign")),
           qPrintable(result.error()));
}

void GamesTests::wrongTypeForPlayerCountRejected() {
  const QJsonObject obj{
      {QStringLiteral("deckIds"), QJsonArray{}},
      {QStringLiteral("playerCount"), QStringLiteral("two")},
      {QStringLiteral("campaignId"), QJsonValue()},
      {QStringLiteral("scenarioId"), QStringLiteral("01104")},
      {QStringLiteral("difficulty"), QStringLiteral("Easy")},
      {QStringLiteral("campaignName"), QStringLiteral("X")},
      {QStringLiteral("multiplayerVariant"), QStringLiteral("Solo")},
      {QStringLiteral("includeTarotReadings"), false},
      {QStringLiteral("options"), QJsonArray{}},
  };
  const auto result = CreateGameRequest::fromJson(obj, u"request");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("playerCount")),
           qPrintable(result.error()));
}

void GamesTests::wholeValueNotObjectRejected() {
  const auto result = CreateGameRequest::fromJson(
      QJsonValue(QStringLiteral("nope")), u"request");
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("object")));
}

void GamesTests::unrecognizedUltimatumOrBoonRejected() {
  const QJsonObject obj{
      {QStringLiteral("deckIds"), QJsonArray{}},
      {QStringLiteral("playerCount"), 1},
      {QStringLiteral("campaignId"), QJsonValue()},
      {QStringLiteral("scenarioId"), QStringLiteral("01104")},
      {QStringLiteral("difficulty"), QStringLiteral("Easy")},
      {QStringLiteral("campaignName"), QStringLiteral("X")},
      {QStringLiteral("multiplayerVariant"), QStringLiteral("Solo")},
      {QStringLiteral("includeTarotReadings"), false},
      {QStringLiteral("options"), QJsonArray{}},
      {QStringLiteral("ultimatumsAndBoons"),
       QJsonArray{QStringLiteral("BoonOfSomeFutureThing")}},
  };
  const auto result = CreateGameRequest::fromJson(obj, u"request");
  QVERIFY(!result.has_value());
  // UltimatumOrBoon is a closed enum -- unlike GameState/CampaignOption, an
  // unrecognized value is a hard decode error, not preserved.
  QVERIFY2(result.error().contains(QStringLiteral("ultimatumsAndBoons")),
           qPrintable(result.error()));
}

void GamesTests::unrecognizedClassSymbolInInvestigatorSummaryRejected() {
  const QJsonObject obj{
      {QStringLiteral("id"), QStringLiteral("c01001")},
      {QStringLiteral("classSymbol"), QStringLiteral("SomeFutureClass")}};
  const auto result = InvestigatorSummary::fromJson(obj, u"investigator");
  QVERIFY(!result.has_value());
  QVERIFY2(result.error().contains(QStringLiteral("classSymbol")),
           qPrintable(result.error()));
}

void GamesTests::additiveUnknownFieldsIgnoredInChooseDeck() {
  const QJsonObject obj{
      {QStringLiteral("investigatorId"), QStringLiteral("01001")},
      {QStringLiteral("aFutureField"), 42},
  };
  const auto result = ChooseDeckRequest::fromJson(obj, u"chooseDeck");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(!result->toJson().contains(QStringLiteral("aFutureField")));
}

void GamesTests::additiveUnknownFieldsIgnoredInClaimSeat() {
  const QJsonObject obj{
      {QStringLiteral("investigatorId"), QStringLiteral("01001")},
      {QStringLiteral("aFutureField"), 42},
  };
  const auto result = ClaimSeatRequest::fromJson(obj, u"claimSeat");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(!result->toJson().contains(QStringLiteral("aFutureField")));
}

void GamesTests::chooseDeckWithDeckListRoundTrips() {
  const QJsonObject fixture = lifecycleFixture();
  const QJsonValue v = fixture.value("chooseDeck"_L1);
  const auto result = ChooseDeckRequest::fromJson(v, u"chooseDeck");
  if (!result)
    QFAIL(qPrintable(result.error()));
  const QJsonObject reencoded = result->toJson();
  QVERIFY(reencoded.contains(QStringLiteral("deckList")));
  QVERIFY(reencoded.contains(QStringLiteral("deckUrl")));
}

void GamesTests::chooseDeckRawBytesPreserveExactNumericDeckIdAndSideSlots() {
  const QByteArray bytes = QByteArrayLiteral(
      "{\"investigatorId\":\"01001\",\"deckList\":{\"slots\":{\"01016\":2},"
      "\"sideSlots\":[1e100],\"investigator_code\":\"01001\","
      "\"id\":9007199254740993,\"name\":\"Huge deck\"}}");

  const auto result = ChooseDeckRequest::fromRawBytes(bytes, u"chooseDeck");
  if (!result)
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->deckList.has_value());

  const ExternalDeckId &id = result->deckList->id;
  QCOMPARE(id.kind(), ExternalDeckId::Kind::Number);
  QCOMPARE(id.number().literal(), QStringLiteral("9007199254740993"));
  const auto exactId = id.number().toExactInt64();
  QVERIFY(exactId.has_value());
  QCOMPARE(*exactId, qint64{9007199254740993LL});

  QVERIFY(result->deckList->sideSlots.isArray());
  QCOMPARE(result->deckList->sideSlots.toArray().size(), 1);
  QCOMPARE(result->deckList->sideSlots.toArray().at(0).toRawNumber().literal(),
           QStringLiteral("1e100"));

  const auto encoded = result->toJsonBytes();
  if (!encoded)
    QFAIL(qPrintable(encoded.error()));
  QCOMPARE(*encoded, bytes);
}

void GamesTests::chooseDeckRawBytesDistinguishAbsentAndNullDeckId() {
  const QByteArray nullIdBytes = QByteArrayLiteral(
      "{\"investigatorId\":\"01001\",\"deckList\":{\"slots\":{},"
      "\"investigator_code\":\"01001\",\"id\":null}}");
  const QByteArray absentIdBytes = QByteArrayLiteral(
      "{\"investigatorId\":\"01001\",\"deckList\":{\"slots\":{},"
      "\"investigator_code\":\"01001\"}}");

  const auto nullIdResult =
      ChooseDeckRequest::fromRawBytes(nullIdBytes, u"chooseDeck");
  const auto absentIdResult =
      ChooseDeckRequest::fromRawBytes(absentIdBytes, u"chooseDeck");
  if (!nullIdResult)
    QFAIL(qPrintable(nullIdResult.error()));
  if (!absentIdResult)
    QFAIL(qPrintable(absentIdResult.error()));

  QVERIFY(nullIdResult->deckList.has_value());
  QVERIFY(absentIdResult->deckList.has_value());
  QCOMPARE(nullIdResult->deckList->id.kind(), ExternalDeckId::Kind::Null);
  QCOMPARE(absentIdResult->deckList->id.kind(), ExternalDeckId::Kind::Absent);

  const auto encodedNullId = nullIdResult->toJsonBytes();
  const auto encodedAbsentId = absentIdResult->toJsonBytes();
  if (!encodedNullId)
    QFAIL(qPrintable(encodedNullId.error()));
  if (!encodedAbsentId)
    QFAIL(qPrintable(encodedAbsentId.error()));
  QCOMPARE(*encodedNullId, nullIdBytes);
  QCOMPARE(*encodedAbsentId, absentIdBytes);
}

void GamesTests::claimSeatRoundTrips() {
  const ClaimSeatRequest request{
      .investigatorId = *InvestigatorRef::parse(QStringLiteral("c01001"))};
  QCOMPARE(request.toJson(), (QJsonObject{{QStringLiteral("investigatorId"),
                                           QStringLiteral("c01001")}}));
}

QTEST_APPLESS_MAIN(GamesTests)

#include "GamesTests.moc"
