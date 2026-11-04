#include <QFile>
#include <QtTest>

#include <initializer_list>
#include <utility>

#include "Board.h"
#include "RawJson.h"

using namespace Arkham;
using namespace Qt::StringLiterals;

namespace {

QByteArray loadFixtureBytes(const QString &fileName) {
  QFile f(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + u"/fixtures/" + fileName);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return f.readAll();
}

// Parses a vendored fixture with the production parse limits. Fails via
// qFatal only for an unreadable/malformed *test fixture*, per this
// project's convention that qFatal is permitted solely for
// impossible-to-reach test setup.
Json::Value loadFixture(const QString &fileName) {
  const QByteArray bytes = loadFixtureBytes(fileName);
  if (bytes.isEmpty())
    qFatal("missing fixture: %s", qPrintable(fileName));
  auto parsed = Json::Value::parse(bytes, fileName);
  if (!parsed)
    qFatal("fixture must parse: %s", qPrintable(parsed.error()));
  return *parsed;
}

// get-game.json's PublicGame, i.e. the envelope's `game` member.
Json::Value getGamePublicGame() {
  return loadFixture(u"get-game.json"_s).value("game"_L1);
}

// game-update.json's PublicGame, i.e. the GameUpdate message's `contents`.
Json::Value gameUpdatePublicGame() {
  return loadFixture(u"game-update.json"_s).value("contents"_L1);
}

Json::Value chaosBagOf(const Json::Value &game) {
  return game.value("mode"_L1).value("That"_L1).value("chaosBag"_L1);
}

// The fixtures' only location (the Study), keyed by its UUID.
constexpr auto kStudyId = "782de537-8f5d-4eda-b849-2362e28402e5";

Json::Value studyOf(const Json::Value &game) {
  return game.value("locations"_L1).value(QLatin1StringView(kStudyId));
}

// One valid document per decoder; the data-driven cases below mutate it.
constexpr auto kToken =
    R"({"chaosTokenId":"22e6cf42-cc5c-4a9c-b9d1-f71e594e0d60",)"
    R"("chaosTokenFace":"MinusTwo","chaosTokenRevealedBy":null,)"
    R"("chaosTokenCancelled":false,"chaosTokenSealed":false})";
constexpr auto kAct =
    R"({"id":"c01108","cardId":"419015cc-abb7-41d0-9795-7ec1d981955f",)"
    R"("sequence":[1,"A"],"deckId":1,"flipped":false,"advanceCost":null})";
constexpr auto kAgenda =
    R"({"id":"c01105","cardId":"d96ae83d-bf74-48e1-8233-78b565f30716",)"
    R"("sequence":{"agendaSequenceSide":"A","agendaSequenceStep":1},)"
    R"("deckId":1,"doom":0,"doomThreshold":{"tag":"Static","contents":3},)"
    R"("flipped":false})";
constexpr auto kTokens = R"([["Clue",2],["Doom",1],["custom:sigil",3]])";
// get-game.json's Study, trimmed to the modeled keys, with one connection.
constexpr auto kLocation =
    R"({"id":"782de537-8f5d-4eda-b849-2362e28402e5","cardCode":"c01111",)"
    R"("cardId":"4db426b4-641e-4812-8d10-27fc6470ef6e","label":"study",)"
    R"("revealed":true,"tokens":[["Clue",2]],)"
    R"("shroud":{"tag":"Static","contents":2},)"
    R"("revealClues":{"tag":"PerPlayer","contents":2},)"
    R"("symbol":"Circle","revealedSymbol":"Circle",)"
    R"("connectedLocations":["00000000-0000-0000-0000-000000000032"],)"
    R"("investigators":["c01001"]})";
// The fork's location-enemy-view.json fixture at the pinned commit.
constexpr auto kEnemyLocation =
    R"({"assets":[],"brazier":null,"breaches":null,"cardCode":"c10547",)"
    R"("cardId":"00000000-0000-0000-0000-000000000033",)"
    R"("cardsUnderneath":[],"concealedCards":[],"connectedLocations":[],)"
    R"("enemies":[],"enemyLocation":true,"events":[],"exhausted":false,)"
    R"("floodLevel":null,"grantedConnections":[],"group":null,)"
    R"("id":"00000000-0000-0000-0000-000000000032","investigators":[],)"
    R"("keys":[],"label":"shapeless_cellar","modifiers":[],)"
    R"("placement":null,"revealed":true,"scarletKeys":[],)"
    R"("sealedChaosTokens":[],"seals":[],)"
    R"("shroud":{"contents":4,"tag":"Static"},"tokens":[],"treacheries":[]})";

QByteArray validJson(const QString &type) {
  if (type == "token"_L1)
    return kToken;
  if (type == "bag"_L1)
    return QByteArray(R"({"chaosTokens":[)") + kToken +
           R"(],"revealedChaosTokens":[]})";
  if (type == "step"_L1)
    return R"({"tag":"UpkeepPhaseStep","contents":"CheckHandSizeStep"})";
  if (type == "act"_L1)
    return kAct;
  if (type == "agenda"_L1)
    return kAgenda;
  if (type == "phase"_L1)
    return R"("InvestigationPhase")";
  if (type == "tokens"_L1)
    return kTokens;
  if (type == "location"_L1)
    return kLocation;
  if (type == "enemyLocation"_L1)
    return kEnemyLocation;
  qFatal("unknown test type: %s", qPrintable(type));
}

// Replaces the first occurrence of `from` in `type`'s valid document.
// Fails via qFatal if `from` is absent, so a stale row can never silently
// test the unmutated document.
QByteArray mutated(const QString &type, const QByteArray &from,
                   const QByteArray &to) {
  QByteArray json = validJson(type);
  const qsizetype at = json.indexOf(from);
  if (at < 0)
    qFatal("mutation target not found: %s", from.constData());
  return json.replace(at, from.size(), to);
}

// Decodes `json` as `type` through its raw-bytes entry point (path "x")
// and returns the error, or an empty string on success.
QString decodeError(const QString &type, const QByteArray &json) {
  const auto errorOf = [](const auto &result) {
    return result ? QString() : result.error();
  };
  if (type == "token"_L1)
    return errorOf(ChaosToken::fromRawBytes(json, u"x"));
  if (type == "bag"_L1)
    return errorOf(ChaosBag::fromRawBytes(json, u"x"));
  if (type == "step"_L1)
    return errorOf(PhaseStep::fromRawBytes(json, u"x"));
  if (type == "act"_L1)
    return errorOf(Act::fromRawBytes(json, u"x"));
  if (type == "agenda"_L1)
    return errorOf(Agenda::fromRawBytes(json, u"x"));
  if (type == "tokens"_L1)
    return errorOf(TokenCounts::fromRawBytes(json, u"x"));
  if (type == "location"_L1 || type == "enemyLocation"_L1)
    return errorOf(Location::fromRawBytes(json, u"x"));
  auto parsed = Json::Value::parse(json, u"x");
  if (!parsed)
    return parsed.error();
  return errorOf(decodePhase(*parsed, u"x"));
}

} // namespace

class BoardTests : public QObject {
  Q_OBJECT

private slots:
  void getGameFixtureEnvelopeShape() {
    const auto envelope = loadFixture(u"get-game.json"_s);
    QVERIFY(envelope.isObject());
    QVERIFY(envelope.value("eventId"_L1).isNull());
    QCOMPARE(envelope.value("multiplayerMode"_L1).toString(), u"Solo"_s);
    QCOMPARE(envelope.value("playerId"_L1).toString(),
             u"00000000-0000-0000-0000-000000000001"_s);
    QVERIFY(envelope.value("game"_L1).isObject());
  }

  void getGameFixtureChaosBag() {
    const auto bag =
        ChaosBag::fromRawJson(chaosBagOf(getGamePublicGame()), u"chaosBag");
    QVERIFY2(bag, qPrintable(bag ? QString() : bag.error()));

    const QStringList expectedFaces{
        u"MinusTwo"_s, u"PlusOne"_s,  u"Zero"_s,   u"MinusOne"_s,
        u"PlusOne"_s,  u"MinusOne"_s, u"Skull"_s,  u"ElderSign"_s,
        u"MinusTwo"_s, u"Zero"_s,     u"Tablet"_s, u"AutoFail"_s,
        u"Skull"_s,    u"MinusOne"_s, u"Zero"_s,   u"Cultist"_s,
    };
    QStringList faces;
    for (const auto &token : bag->chaosTokens())
      faces.append(token.face());
    QCOMPARE(faces, expectedFaces);

    const ChaosToken &first = bag->chaosTokens().first();
    QCOMPARE(first.id().value(), u"22e6cf42-cc5c-4a9c-b9d1-f71e594e0d60"_s);
    QCOMPARE(first.face(), u"MinusTwo"_s);
    QVERIFY(!first.revealedBy().has_value());
    QCOMPARE(first.cancelled(), false);
    QCOMPARE(first.sealed(), false);

    const ChaosToken &last = bag->chaosTokens().last();
    QCOMPARE(last.id().value(), u"fe1ebefe-cb12-4529-84e3-5dcc2b4f7258"_s);
    QCOMPARE(last.face(), u"Cultist"_s);

    QVERIFY(bag->revealedChaosTokens().isEmpty());
  }

  void getGameFixturePhase() {
    const auto game = getGamePublicGame();
    const auto phase = decodePhase(game.value("phase"_L1), u"phase");
    QVERIFY2(phase, qPrintable(phase ? QString() : phase.error()));
    QCOMPARE(*phase, Phase::InvestigationPhase);

    const auto step =
        PhaseStep::fromRawJson(game.value("phaseStep"_L1), u"phaseStep");
    QVERIFY2(step, qPrintable(step ? QString() : step.error()));
    QCOMPARE(step->kind(), PhaseStep::Kind::InvestigationPhaseStep);
    QCOMPARE(step->step(), PhaseStep::Step::InvestigatorTakesActionStep);
  }

  void getGameFixtureAct() {
    const auto acts = getGamePublicGame().value("acts"_L1);
    QVERIFY(acts.isObject());
    const auto act = Act::fromRawJson(acts.value("c01108"_L1), u"act");
    QVERIFY2(act, qPrintable(act ? QString() : act.error()));
    QCOMPARE(act->id().value(), u"c01108"_s);
    QCOMPARE(act->cardId().value(), u"419015cc-abb7-41d0-9795-7ec1d981955f"_s);
    QCOMPARE(act->sequenceStep(), 1);
    QCOMPARE(act->sequenceSide(), ActSide::A);
    QCOMPARE(act->deckId(), 1);
    QCOMPARE(act->flipped(), false);
    QVERIFY(act->advanceCost().isObject());
    QCOMPARE(act->advanceCost().value("tag"_L1).toString(), u"GroupClueCost"_s);
  }

  void getGameFixtureAgenda() {
    const auto agendas = getGamePublicGame().value("agendas"_L1);
    QVERIFY(agendas.isObject());
    const auto agenda =
        Agenda::fromRawJson(agendas.value("c01105"_L1), u"agenda");
    QVERIFY2(agenda, qPrintable(agenda ? QString() : agenda.error()));
    QCOMPARE(agenda->id().value(), u"c01105"_s);
    QCOMPARE(agenda->cardId().value(),
             u"d96ae83d-bf74-48e1-8233-78b565f30716"_s);
    QCOMPARE(agenda->sequenceStep(), 1);
    QCOMPARE(agenda->sequenceSide(), AgendaSide::A);
    QCOMPARE(agenda->deckId(), 1);
    QCOMPARE(agenda->doom(), 0);
    QCOMPARE(agenda->flipped(), false);
    QVERIFY(agenda->doomThreshold().has_value());
    QCOMPARE(agenda->doomThreshold()->tag(), GameValueTag::Static);
    QCOMPARE(agenda->doomThreshold()->singleAmount(), std::optional<qint64>(3));
    QVERIFY(*agenda->doomThreshold() == GameValue::staticValue(3));
  }

  void getGameFixtureLocation() {
    const auto game = getGamePublicGame();
    QCOMPARE(game.value("locations"_L1).keys(),
             QStringList{QLatin1StringView(kStudyId)});
    const auto location = Location::fromRawJson(studyOf(game), u"location");
    QVERIFY2(location, qPrintable(location ? QString() : location.error()));
    QCOMPARE(location->kind(), Location::Kind::Ordinary);
    QCOMPARE(location->id().value(), QLatin1StringView(kStudyId));
    QCOMPARE(location->cardCode().value(), u"c01111"_s);
    QCOMPARE(location->cardId().value(),
             u"4db426b4-641e-4812-8d10-27fc6470ef6e"_s);
    QCOMPARE(location->label(), u"study"_s);
    QCOMPARE(location->revealed(), true);
    const QList<std::pair<QString, qint64>> expectedTokens{{u"Clue"_s, 2}};
    QCOMPARE(location->tokens().entries(), expectedTokens);
    QCOMPARE(location->clues(), 2);
    QCOMPARE(location->doom(), 0);
    QCOMPARE(location->damage(), 0);
    QCOMPARE(location->horror(), 0);
    QCOMPARE(location->resources(), 0);
    QVERIFY(location->shroud().has_value());
    QVERIFY(*location->shroud() == GameValue::staticValue(2));
    QVERIFY(location->revealClues().has_value());
    QCOMPARE(location->revealClues()->tag(), GameValueTag::PerPlayer);
    QCOMPARE(location->revealClues()->singleAmount(), std::optional<qint64>(2));
    QCOMPARE(location->symbol(),
             std::optional<LocationSymbol>(LocationSymbol::Circle));
    QCOMPARE(location->revealedSymbol(),
             std::optional<LocationSymbol>(LocationSymbol::Circle));
    QVERIFY(!location->exhausted().has_value());
    QVERIFY(location->connectedLocations().isEmpty());
    QCOMPARE(location->investigators().size(), 1);
    QCOMPARE(location->investigators().first().value(), u"c01001"_s);
  }

  // The enemy-spawned pseudo-location shape (location.schema.json's
  // enemyLocation branch), from the fork's location-enemy-view.json.
  void enemyLocationDecodes() {
    const auto location =
        Location::fromRawBytes(validJson(u"enemyLocation"_s), u"location");
    QVERIFY2(location, qPrintable(location ? QString() : location.error()));
    QCOMPARE(location->kind(), Location::Kind::EnemyLocation);
    QCOMPARE(location->id().value(), u"00000000-0000-0000-0000-000000000032"_s);
    QCOMPARE(location->cardCode().value(), u"c10547"_s);
    QCOMPARE(location->cardId().value(),
             u"00000000-0000-0000-0000-000000000033"_s);
    QCOMPARE(location->label(), u"shapeless_cellar"_s);
    QCOMPARE(location->revealed(), true);
    QCOMPARE(location->exhausted(), std::optional<bool>(false));
    QVERIFY(location->shroud().has_value());
    QVERIFY(*location->shroud() == GameValue::staticValue(4));
    QVERIFY(!location->revealClues().has_value());
    QVERIFY(!location->symbol().has_value());
    QVERIFY(!location->revealedSymbol().has_value());
    QVERIFY(location->tokens().entries().isEmpty());
    QCOMPARE(location->clues(), 0);
    QVERIFY(location->connectedLocations().isEmpty());
    QVERIFY(location->investigators().isEmpty());

    const auto exhausted = Location::fromRawBytes(
        mutated(u"enemyLocation"_s, R"("exhausted":false)",
                R"("exhausted":true)"),
        u"location");
    QVERIFY2(exhausted, qPrintable(exhausted ? QString() : exhausted.error()));
    QCOMPARE(exhausted->exhausted(), std::optional<bool>(true));
  }

  void locationListsKeepWireOrder() {
    const auto location = Location::fromRawBytes(
        mutated(u"location"_s, R"("00000000-0000-0000-0000-000000000032"])",
                R"("00000000-0000-0000-0000-000000000032",)"
                R"("0c3cfd0c-1b5e-4f43-9d9a-0b1c2d3e4f50"])")
            .replace(R"(["c01001"])", R"(["c01002","c01001"])"),
        u"location");
    QVERIFY2(location, qPrintable(location ? QString() : location.error()));
    QCOMPARE(location->connectedLocations().size(), 2);
    QCOMPARE(location->connectedLocations()[0].value(),
             u"00000000-0000-0000-0000-000000000032"_s);
    QCOMPARE(location->connectedLocations()[1].value(),
             u"0c3cfd0c-1b5e-4f43-9d9a-0b1c2d3e4f50"_s);
    QCOMPARE(location->investigators().size(), 2);
    QCOMPARE(location->investigators()[0].value(), u"c01002"_s);
    QCOMPARE(location->investigators()[1].value(), u"c01001"_s);
  }

  // Each count accessor reads its own Arkham.Token core name.
  void locationTokenCountAccessors() {
    const auto location = Location::fromRawBytes(
        mutated(u"location"_s, R"([["Clue",2]])",
                R"([["Resource",6],["Horror",5],["Damage",4],)"
                R"(["Doom",3],["Clue",2],["custom:sigil",9]])"),
        u"location");
    QVERIFY2(location, qPrintable(location ? QString() : location.error()));
    QCOMPARE(location->clues(), 2);
    QCOMPARE(location->doom(), 3);
    QCOMPARE(location->damage(), 4);
    QCOMPARE(location->horror(), 5);
    QCOMPARE(location->resources(), 6);
    QCOMPARE(location->tokens().count(u"custom:sigil"), 9);
    QCOMPARE(location->tokens().entries().size(), 6);
  }

  void tokenCountsDecode() {
    const auto tokens = TokenCounts::fromRawBytes(kTokens, u"tokens");
    QVERIFY2(tokens, qPrintable(tokens ? QString() : tokens.error()));
    const QList<std::pair<QString, qint64>> expected{
        {u"Clue"_s, 2}, {u"Doom"_s, 1}, {u"custom:sigil"_s, 3}};
    QCOMPARE(tokens->entries(), expected);
    QCOMPARE(tokens->count(u"Clue"), 2);
    QCOMPARE(tokens->count(u"Doom"), 1);
    QCOMPARE(tokens->count(u"custom:sigil"), 3);
    // Absent names read as 0; names are case-sensitive.
    QCOMPARE(tokens->count(u"Horror"), 0);
    QCOMPARE(tokens->count(u"clue"), 0);

    const auto empty = TokenCounts::fromRawBytes("[]", u"tokens");
    QVERIFY(empty);
    QVERIFY(empty->entries().isEmpty());
    QCOMPARE(empty->count(u"Clue"), 0);

    const auto zero = TokenCounts::fromRawBytes(R"([["Clue",0]])", u"tokens");
    QVERIFY2(zero, qPrintable(zero ? QString() : zero.error()));
    QCOMPARE(zero->count(u"Clue"), 0);
  }

  // The GameUpdate WebSocket message carries the same PublicGame shape:
  // every leaf decodes, and to exactly what get-game.json decodes to.
  void gameUpdateFixtureMatchesGetGame() {
    const auto getGame = getGamePublicGame();
    const auto update = gameUpdatePublicGame();
    QVERIFY(update.isObject());

    const auto bagA = ChaosBag::fromRawJson(chaosBagOf(getGame), u"a");
    const auto bagB = ChaosBag::fromRawJson(chaosBagOf(update), u"b");
    QVERIFY(bagA && bagB);
    QVERIFY(*bagA == *bagB);

    const auto phaseB = decodePhase(update.value("phase"_L1), u"b");
    QVERIFY(phaseB);
    QCOMPARE(*phaseB, Phase::InvestigationPhase);

    const auto stepA =
        PhaseStep::fromRawJson(getGame.value("phaseStep"_L1), u"a");
    const auto stepB =
        PhaseStep::fromRawJson(update.value("phaseStep"_L1), u"b");
    QVERIFY(stepA && stepB);
    QVERIFY(*stepA == *stepB);

    const auto actA =
        Act::fromRawJson(getGame.value("acts"_L1).value("c01108"_L1), u"a");
    const auto actB =
        Act::fromRawJson(update.value("acts"_L1).value("c01108"_L1), u"b");
    QVERIFY(actA && actB);
    QVERIFY(*actA == *actB);

    const auto agendaA = Agenda::fromRawJson(
        getGame.value("agendas"_L1).value("c01105"_L1), u"a");
    const auto agendaB = Agenda::fromRawJson(
        update.value("agendas"_L1).value("c01105"_L1), u"b");
    QVERIFY(agendaA && agendaB);
    QVERIFY(*agendaA == *agendaB);

    const auto locationA = Location::fromRawJson(studyOf(getGame), u"a");
    const auto locationB = Location::fromRawJson(studyOf(update), u"b");
    QVERIFY(locationA && locationB);
    QVERIFY(*locationA == *locationB);
    QCOMPARE(locationB->clues(), 2);
  }

  void chaosTokenOpenFaceAndRevealedBy() {
    const auto token = ChaosToken::fromRawBytes(
        R"({"chaosTokenId":"22e6cf42-cc5c-4a9c-b9d1-f71e594e0d60",)"
        R"("chaosTokenFace":"z_homebrew_face",)"
        R"("chaosTokenRevealedBy":"c01001",)"
        R"("chaosTokenCancelled":true,"chaosTokenSealed":true})",
        u"token");
    QVERIFY2(token, qPrintable(token ? QString() : token.error()));
    QCOMPARE(token->face(), u"z_homebrew_face"_s);
    QVERIFY(token->revealedBy().has_value());
    QCOMPARE(token->revealedBy()->value(), u"c01001"_s);
    QCOMPARE(token->cancelled(), true);
    QCOMPARE(token->sealed(), true);
  }

  void nullableFieldsDecodeToNullopt() {
    const auto agenda = Agenda::fromRawBytes(
        R"({"id":"c01105","cardId":"d96ae83d-bf74-48e1-8233-78b565f30716",)"
        R"("sequence":{"agendaSequenceSide":"B","agendaSequenceStep":2},)"
        R"("deckId":1,"doom":4,"doomThreshold":null,"flipped":true})",
        u"agenda");
    QVERIFY2(agenda, qPrintable(agenda ? QString() : agenda.error()));
    QVERIFY(!agenda->doomThreshold().has_value());
    QCOMPARE(agenda->sequenceSide(), AgendaSide::B);
    QCOMPARE(agenda->sequenceStep(), 2);
    QCOMPARE(agenda->doom(), 4);
    QCOMPARE(agenda->flipped(), true);

    const auto act = Act::fromRawBytes(
        R"({"id":"c01108","cardId":"419015cc-abb7-41d0-9795-7ec1d981955f",)"
        R"("sequence":[3,"H"],"deckId":2,"flipped":true,"advanceCost":null})",
        u"act");
    QVERIFY2(act, qPrintable(act ? QString() : act.error()));
    QVERIFY(act->advanceCost().isNull());
    QCOMPARE(act->sequenceStep(), 3);
    QCOMPARE(act->sequenceSide(), ActSide::H);

    const auto location = Location::fromRawBytes(
        mutated(u"location"_s, R"("shroud":{"tag":"Static","contents":2})",
                R"("shroud":null)"),
        u"location");
    QVERIFY2(location, qPrintable(location ? QString() : location.error()));
    QVERIFY(!location->shroud().has_value());
  }

  // Every hand-written wire string decodes to its own value. The rows are
  // copied from the pinned schemas (public-game, phase-step, act, agenda),
  // never generated from Board.cpp's tables, so a typo on either side fails.
  void phaseDecodesEveryValue_data() {
    QTest::addColumn<QByteArray>("wire");
    QTest::addColumn<Phase>("expected");
    const std::initializer_list<std::pair<const char *, Phase>> rows{
        {"MythosPhase", Phase::MythosPhase},
        {"InvestigationPhase", Phase::InvestigationPhase},
        {"EnemyPhase", Phase::EnemyPhase},
        {"UpkeepPhase", Phase::UpkeepPhase},
        {"ResolutionPhase", Phase::ResolutionPhase},
        {"CampaignPhase", Phase::CampaignPhase},
    };
    for (const auto &[wire, expected] : rows)
      QTest::newRow(wire) << QByteArray(wire) << expected;
  }

  void phaseDecodesEveryValue() {
    QFETCH(QByteArray, wire);
    QFETCH(Phase, expected);
    const auto parsed =
        Json::Value::parse(QByteArray('"' + wire + '"'), u"phase");
    QVERIFY(parsed);
    const auto phase = decodePhase(*parsed, u"phase");
    QVERIFY2(phase, qPrintable(phase ? QString() : phase.error()));
    QCOMPARE(*phase, expected);
  }

  void phaseStepDecodesEveryValue_data() {
    using K = PhaseStep::Kind;
    using S = PhaseStep::Step;
    QTest::addColumn<QByteArray>("tag");
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<K>("kind");
    QTest::addColumn<S>("step");
    const auto rows =
        [](const char *tag, K kind,
           std::initializer_list<std::pair<const char *, S>> steps) {
          for (const auto &[wire, step] : steps)
            QTest::addRow("%s/%s", tag, wire)
                << QByteArray(tag) << QByteArray(wire) << kind << step;
        };
    rows("MythosPhaseStep", K::MythosPhaseStep,
         {
             {"MythosPhaseBeginsStep", S::MythosPhaseBeginsStep},
             {"PlaceDoomOnAgendaStep", S::PlaceDoomOnAgendaStep},
             {"CheckDoomThresholdStep", S::CheckDoomThresholdStep},
             {"EachInvestigatorDrawsEncounterCardStep",
              S::EachInvestigatorDrawsEncounterCardStep},
             {"MythosPhaseWindow", S::MythosPhaseWindow},
             {"MythosPhaseEndsStep", S::MythosPhaseEndsStep},
         });
    rows("InvestigationPhaseStep", K::InvestigationPhaseStep,
         {
             {"InvestigationPhaseBeginsStep", S::InvestigationPhaseBeginsStep},
             {"InvestigationPhaseBeginsWindow",
              S::InvestigationPhaseBeginsWindow},
             {"NextInvestigatorsTurnBeginsStep",
              S::NextInvestigatorsTurnBeginsStep},
             {"NextInvestigatorsTurnBeginsWindow",
              S::NextInvestigatorsTurnBeginsWindow},
             {"InvestigatorTakesActionStep", S::InvestigatorTakesActionStep},
             {"InvestigatorsTurnEndsStep", S::InvestigatorsTurnEndsStep},
             {"InvestigationPhaseEndsStep", S::InvestigationPhaseEndsStep},
         });
    rows("EnemyPhaseStep", K::EnemyPhaseStep,
         {
             {"EnemyPhaseBeginsStep", S::EnemyPhaseBeginsStep},
             {"HunterEnemiesMoveStep", S::HunterEnemiesMoveStep},
             {"ResolveAttacksWindow", S::ResolveAttacksWindow},
             {"ResolveAttacksStep", S::ResolveAttacksStep},
             {"AfterResolveAttacksWindow", S::AfterResolveAttacksWindow},
             {"EnemyPhaseEndsStep", S::EnemyPhaseEndsStep},
         });
    rows("UpkeepPhaseStep", K::UpkeepPhaseStep,
         {
             {"UpkeepPhaseBeginsStep", S::UpkeepPhaseBeginsStep},
             {"UpkeepPhaseBeginsWindow", S::UpkeepPhaseBeginsWindow},
             {"ResetActionsStep", S::ResetActionsStep},
             {"ReadyExhaustedStep", S::ReadyExhaustedStep},
             {"DrawCardAndGainResourceStep", S::DrawCardAndGainResourceStep},
             {"CheckHandSizeStep", S::CheckHandSizeStep},
             {"UpkeepPhaseEndsStep", S::UpkeepPhaseEndsStep},
         });
  }

  void phaseStepDecodesEveryValue() {
    QFETCH(QByteArray, tag);
    QFETCH(QByteArray, contents);
    QFETCH(PhaseStep::Kind, kind);
    QFETCH(PhaseStep::Step, step);
    const auto decoded = PhaseStep::fromRawBytes(
        "{\"tag\":\"" + tag + "\",\"contents\":\"" + contents + "\"}", u"step");
    QVERIFY2(decoded, qPrintable(decoded ? QString() : decoded.error()));
    QCOMPARE(decoded->kind(), kind);
    QCOMPARE(decoded->step(), step);
  }

  void actSideDecodesEveryValue_data() {
    QTest::addColumn<QByteArray>("wire");
    QTest::addColumn<ActSide>("expected");
    const std::initializer_list<std::pair<const char *, ActSide>> rows{
        {"A", ActSide::A}, {"B", ActSide::B}, {"C", ActSide::C},
        {"D", ActSide::D}, {"E", ActSide::E}, {"F", ActSide::F},
        {"G", ActSide::G}, {"H", ActSide::H},
    };
    for (const auto &[wire, expected] : rows)
      QTest::newRow(wire) << QByteArray(wire) << expected;
  }

  void actSideDecodesEveryValue() {
    QFETCH(QByteArray, wire);
    QFETCH(ActSide, expected);
    const auto act = Act::fromRawBytes(
        mutated(u"act"_s, R"([1,"A"])", "[1,\"" + wire + "\"]"), u"act");
    QVERIFY2(act, qPrintable(act ? QString() : act.error()));
    QCOMPARE(act->sequenceSide(), expected);
  }

  void agendaSideDecodesEveryValue_data() {
    QTest::addColumn<QByteArray>("wire");
    QTest::addColumn<AgendaSide>("expected");
    const std::initializer_list<std::pair<const char *, AgendaSide>> rows{
        {"A", AgendaSide::A},
        {"B", AgendaSide::B},
        {"C", AgendaSide::C},
        {"D", AgendaSide::D},
    };
    for (const auto &[wire, expected] : rows)
      QTest::newRow(wire) << QByteArray(wire) << expected;
  }

  void agendaSideDecodesEveryValue() {
    QFETCH(QByteArray, wire);
    QFETCH(AgendaSide, expected);
    const auto agenda =
        Agenda::fromRawBytes(mutated(u"agenda"_s, R"("agendaSequenceSide":"A")",
                                     "\"agendaSequenceSide\":\"" + wire + '"'),
                             u"agenda");
    QVERIFY2(agenda, qPrintable(agenda ? QString() : agenda.error()));
    QCOMPARE(agenda->sequenceSide(), expected);
  }

  void locationSymbolDecodesEveryValue_data() {
    QTest::addColumn<QByteArray>("wire");
    QTest::addColumn<LocationSymbol>("expected");
    using L = LocationSymbol;
    const std::initializer_list<std::pair<const char *, L>> rows{
        {"Circle", L::Circle},
        {"Square", L::Square},
        {"Triangle", L::Triangle},
        {"Plus", L::Plus},
        {"Diamond", L::Diamond},
        {"Squiggle", L::Squiggle},
        {"Moon", L::Moon},
        {"Hourglass", L::Hourglass},
        {"T", L::T},
        {"Equals", L::Equals},
        {"Heart", L::Heart},
        {"Star", L::Star},
        {"Droplet", L::Droplet},
        {"Trefoil", L::Trefoil},
        {"Spade", L::Spade},
        {"NoSymbol", L::NoSymbol},
    };
    for (const auto &[wire, expected] : rows)
      QTest::newRow(wire) << QByteArray(wire) << expected;
  }

  void locationSymbolDecodesEveryValue() {
    QFETCH(QByteArray, wire);
    QFETCH(LocationSymbol, expected);
    const auto location = Location::fromRawBytes(
        mutated(u"location"_s, R"("symbol":"Circle","revealedSymbol":"Circle")",
                "\"symbol\":\"" + wire + "\",\"revealedSymbol\":\"" + wire +
                    '"'),
        u"location");
    QVERIFY2(location, qPrintable(location ? QString() : location.error()));
    QCOMPARE(location->symbol(), std::optional<LocationSymbol>(expected));
    QCOMPARE(location->revealedSymbol(),
             std::optional<LocationSymbol>(expected));
  }

  void validDocumentsDecode_data() {
    QTest::addColumn<QString>("type");
    for (const auto *type : {"token", "bag", "step", "act", "agenda", "phase",
                             "tokens", "location", "enemyLocation"})
      QTest::newRow(type) << QString::fromLatin1(type);
  }

  void validDocumentsDecode() {
    QFETCH(QString, type);
    const QString error = decodeError(type, validJson(type));
    QVERIFY2(error.isEmpty(), qPrintable(error));
  }

  // Response decoding policy: an additive server key never breaks decoding.
  void unknownKeysAreIgnored_data() {
    QTest::addColumn<QString>("type");
    QTest::addColumn<QByteArray>("from");
    QTest::addColumn<QByteArray>("to");

    const QByteArray extra = R"({"futureField":[1,{"nested":null}],)";
    for (const auto *type :
         {"token", "bag", "step", "act", "agenda", "location", "enemyLocation"})
      QTest::newRow(type) << QString::fromLatin1(type) << QByteArray("{")
                          << extra;
    QTest::newRow("token nested in bag")
        << u"bag"_s << QByteArray(R"("chaosTokenSealed":false)")
        << QByteArray(R"("chaosTokenSealed":false,"futureField":1)");
    QTest::newRow("agenda sequence")
        << u"agenda"_s << QByteArray(R"({"agendaSequenceSide")")
        << QByteArray(R"({"futureField":true,"agendaSequenceSide")");
  }

  void unknownKeysAreIgnored() {
    QFETCH(QString, type);
    QFETCH(QByteArray, from);
    QFETCH(QByteArray, to);
    const QString error = decodeError(type, mutated(type, from, to));
    QVERIFY2(error.isEmpty(), qPrintable(error));
  }

  void malformedInputIsRejected_data() {
    QTest::addColumn<QString>("type");
    QTest::addColumn<QByteArray>("from");
    QTest::addColumn<QByteArray>("to");
    QTest::addColumn<QString>("expectedError");

    const auto row = [](const char *name, const QString &type,
                        const QByteArray &from, const QByteArray &to,
                        const QString &expected) {
      QTest::newRow(name) << type << from << to << expected;
    };
    row("token missing face", u"token"_s, R"("chaosTokenFace":"MinusTwo",)", "",
        uR"(x.chaosTokenFace: missing required field "chaosTokenFace")"_s);
    row("token sealed wrong type", u"token"_s, R"("chaosTokenSealed":false)",
        R"("chaosTokenSealed":"no")",
        u"x.chaosTokenSealed: expected bool, got string"_s);
    row("token bad uuid", u"token"_s, "22e6cf42-cc5c-4a9c-b9d1-f71e594e0d60",
        "not-a-uuid", u"x.chaosTokenId"_s);
    row("token revealedBy missing", u"token"_s,
        R"("chaosTokenRevealedBy":null,)", "",
        uR"(x.chaosTokenRevealedBy: missing required field "chaosTokenRevealedBy")"_s);
    row("token revealedBy unprefixed", u"token"_s,
        R"("chaosTokenRevealedBy":null)", R"("chaosTokenRevealedBy":"01001")",
        u"x.chaosTokenRevealedBy"_s);
    row("duplicate key", u"token"_s, R"("chaosTokenFace":"MinusTwo")",
        R"("chaosTokenFace":"MinusTwo","chaosTokenFace":"Zero")",
        u"duplicate object key"_s);
    row("bag missing revealed", u"bag"_s, R"(,"revealedChaosTokens":[])", "",
        uR"(x.revealedChaosTokens: missing required field "revealedChaosTokens")"_s);
    row("bag bad nested token", u"bag"_s,
        R"("chaosTokenId":"22e6cf42-cc5c-4a9c-b9d1-f71e594e0d60",)", "",
        uR"(x.chaosTokens[0].chaosTokenId: missing required field "chaosTokenId")"_s);
    row("phase unknown", u"phase"_s, "InvestigationPhase", "LunchPhase",
        uR"(x: unrecognized value "LunchPhase")"_s);
    row("phase wrong type", u"phase"_s, R"("InvestigationPhase")", "3",
        u"x: expected string, got number"_s);
    row("phase step unknown tag", u"step"_s, "UpkeepPhaseStep",
        "LunchPhaseStep", uR"(x.tag: unrecognized value "LunchPhaseStep")"_s);
    row("phase step from another phase", u"step"_s, "UpkeepPhaseStep",
        "MythosPhaseStep",
        uR"(x.contents: unrecognized value "CheckHandSizeStep")"_s);
    row("phase step null", u"step"_s, validJson(u"step"_s), "null",
        u"x: expected object, got null"_s);
    row("act id not a card code", u"act"_s, R"("id":"c01108")",
        R"("id":"01108")", u"x.id"_s);
    row("act sequence arity", u"act"_s, R"([1,"A"])", R"([1,"A",2])",
        u"x.sequence: expected a 2-element array, got 3 elements"_s);
    row("act side out of range", u"act"_s, R"([1,"A"])", R"([1,"Z"])",
        uR"(x.sequence[1]: unrecognized value "Z")"_s);
    row("act missing advanceCost", u"act"_s, R"(,"advanceCost":null)", "",
        uR"(x.advanceCost: missing required field "advanceCost")"_s);
    row("act advanceCost wrong type", u"act"_s, R"("advanceCost":null)",
        R"("advanceCost":"free")",
        u"x.advanceCost: expected object or null, got string"_s);
    row("agenda negative doom", u"agenda"_s, R"("doom":0)", R"("doom":-1)",
        u"x.doom: must not be negative, got -1"_s);
    row("agenda doom wrong type", u"agenda"_s, R"("doom":0)", R"("doom":"0")",
        u"x.doom: expected integer, got string"_s);
    row("agenda missing doomThreshold", u"agenda"_s,
        R"("doomThreshold":{"tag":"Static","contents":3},)", "",
        uR"(x.doomThreshold: missing required field "doomThreshold")"_s);
    // GameValue is a closed contract shape: it stays exact-keyed.
    row("agenda doomThreshold extra key", u"agenda"_s, R"("contents":3})",
        R"("contents":3,"extra":1})", u"x.doomThreshold"_s);
    row("agenda side out of range", u"agenda"_s, R"("agendaSequenceSide":"A")",
        R"("agendaSequenceSide":"E")",
        uR"(x.sequence.agendaSequenceSide: unrecognized value "E")"_s);

    row("tokens not an array", u"tokens"_s, kTokens, "{}",
        u"x: expected array, got object"_s);
    row("tokens entry not an array", u"tokens"_s, R"(["Clue",2])",
        R"({"Clue":2})", u"x[0]: expected array, got object"_s);
    row("tokens entry arity", u"tokens"_s, R"(["Clue",2])", R"(["Clue",2,3])",
        u"x[0]: expected a 2-element array, got 3 elements"_s);
    row("tokens entry short", u"tokens"_s, R"(["Clue",2])", R"(["Clue"])",
        u"x[0]: expected a 2-element array, got 1 elements"_s);
    row("tokens name wrong type", u"tokens"_s, R"(["Clue",2])", "[2,2]",
        u"x[0][0]: expected string, got number"_s);
    row("tokens count wrong type", u"tokens"_s, R"(["Clue",2])",
        R"(["Clue","2"])", u"x[0][1]: expected integer, got string"_s);
    row("tokens count fractional", u"tokens"_s, R"(["Clue",2])",
        R"(["Clue",2.5])", u"x[0][1]: expected integer"_s);
    row("tokens count negative", u"tokens"_s, R"(["Clue",2])", R"(["Clue",-1])",
        u"x[0][1]: must not be negative, got -1"_s);
    row("tokens duplicate name", u"tokens"_s, R"(["Doom",1])", R"(["Clue",1])",
        uR"(x[1][0]: duplicate token "Clue")"_s);

    row("location null", u"location"_s, kLocation, "null",
        u"x: expected object, got null"_s);
    row("location id not a uuid", u"location"_s,
        R"("id":"782de537-8f5d-4eda-b849-2362e28402e5")", R"("id":"c01111")",
        u"x.id"_s);
    row("location missing id", u"location"_s,
        R"("id":"782de537-8f5d-4eda-b849-2362e28402e5",)", "",
        uR"(x.id: missing required field "id")"_s);
    row("location cardCode not a card code", u"location"_s,
        R"("cardCode":"c01111")", R"("cardCode":"01111")",
        u"x.cardCode: card code must start with 'c'"_s);
    row("location cardId not a uuid", u"location"_s,
        R"("cardId":"4db426b4-641e-4812-8d10-27fc6470ef6e")",
        R"("cardId":"study")", u"x.cardId"_s);
    row("location missing label", u"location"_s, R"("label":"study",)", "",
        uR"(x.label: missing required field "label")"_s);
    row("location label wrong type", u"location"_s, R"("label":"study")",
        R"("label":null)", u"x.label: expected string, got null"_s);
    row("location revealed wrong type", u"location"_s, R"("revealed":true)",
        R"("revealed":"yes")", u"x.revealed: expected bool, got string"_s);
    row("location missing tokens", u"location"_s, R"("tokens":[["Clue",2]],)",
        "", uR"(x.tokens: missing required field "tokens")"_s);
    row("location negative clues", u"location"_s, R"([["Clue",2]])",
        R"([["Clue",-2]])", u"x.tokens[0][1]: must not be negative, got -2"_s);
    row("location missing shroud", u"location"_s,
        R"("shroud":{"tag":"Static","contents":2},)", "",
        uR"(x.shroud: missing required field "shroud")"_s);
    row("location shroud wrong type", u"location"_s,
        R"("shroud":{"tag":"Static","contents":2})", R"("shroud":2)",
        u"x.shroud"_s);
    row("location missing revealClues", u"location"_s,
        R"("revealClues":{"tag":"PerPlayer","contents":2},)", "",
        uR"(x.revealClues: missing required field "revealClues")"_s);
    row("location revealClues null", u"location"_s,
        R"("revealClues":{"tag":"PerPlayer","contents":2})",
        R"("revealClues":null)", u"x.revealClues"_s);
    row("location missing symbol", u"location"_s, R"("symbol":"Circle",)", "",
        uR"(x.symbol: missing required field "symbol")"_s);
    row("location symbol unknown", u"location"_s, R"("symbol":"Circle")",
        R"("symbol":"Pentagon")",
        uR"(x.symbol: unrecognized value "Pentagon")"_s);
    row("location revealedSymbol wrong case", u"location"_s,
        R"("revealedSymbol":"Circle")", R"("revealedSymbol":"circle")",
        uR"(x.revealedSymbol: unrecognized value "circle")"_s);
    row("location missing connectedLocations", u"location"_s,
        R"("connectedLocations":["00000000-0000-0000-0000-000000000032"],)", "",
        uR"(x.connectedLocations: missing required field "connectedLocations")"_s);
    row("location connectedLocations not a uuid", u"location"_s,
        R"(["00000000-0000-0000-0000-000000000032"])", R"(["study"])",
        u"x.connectedLocations[0]"_s);
    row("location connectedLocations nil uuid", u"location"_s,
        R"(["00000000-0000-0000-0000-000000000032"])",
        R"(["00000000-0000-0000-0000-000000000000"])",
        u"x.connectedLocations[0]"_s);
    row("location missing investigators", u"location"_s,
        R"(,"investigators":["c01001"])", "",
        uR"(x.investigators: missing required field "investigators")"_s);
    row("location investigators not a card code", u"location"_s,
        R"(["c01001"])", R"(["01001"])",
        u"x.investigators[0]: card code must start with 'c'"_s);
    row("location enemyLocation false", u"location"_s, R"({"id")",
        R"({"enemyLocation":false,"id")",
        u"x.enemyLocation: expected true or absent, got false"_s);
    row("location enemyLocation null", u"location"_s, R"({"id")",
        R"({"enemyLocation":null,"id")",
        u"x.enemyLocation: expected bool, got null"_s);
    row("enemy location missing exhausted", u"enemyLocation"_s,
        R"("exhausted":false,)", "",
        uR"(x.exhausted: missing required field "exhausted")"_s);
    row("enemy location exhausted wrong type", u"enemyLocation"_s,
        R"("exhausted":false)", R"("exhausted":0)",
        u"x.exhausted: expected bool, got number"_s);
    row("enemy location enemyLocation string", u"enemyLocation"_s,
        R"("enemyLocation":true)", R"("enemyLocation":"true")",
        u"x.enemyLocation: expected bool, got string"_s);
    row("enemy location missing shroud", u"enemyLocation"_s,
        R"("shroud":{"contents":4,"tag":"Static"},)", "",
        uR"(x.shroud: missing required field "shroud")"_s);
  }

  void malformedInputIsRejected() {
    QFETCH(QString, type);
    QFETCH(QByteArray, from);
    QFETCH(QByteArray, to);
    QFETCH(QString, expectedError);
    const QString error = decodeError(type, mutated(type, from, to));
    QVERIFY2(!error.isEmpty(), "malformed input must not decode");
    QVERIFY2(error.contains(expectedError), qPrintable(error));
  }

  // act/agenda `id` and a non-null chaosTokenRevealedBy follow the pinned
  // board schemas' `^c[0-9a-z:._-]+$` (act.schema.json, agenda.schema.json,
  // chaos-token.schema.json), not CardCode's broader catalog grammar.
  // chaosTokenRevealedBy: null stays accepted (see getGameFixtureChaosBag).
  void boardCardCodeGrammar_data() {
    QTest::addColumn<QString>("type");
    QTest::addColumn<QByteArray>("from");
    QTest::addColumn<QByteArray>("key");
    QTest::addColumn<QByteArray>("value");
    // Empty: the value must decode verbatim.
    QTest::addColumn<QString>("expectedError");

    struct Field {
      const char *type;
      const char *key;
      const char *from;
    };
    const Field fields[] = {
        {"act", "id", R"("id":"c01108")"},
        {"agenda", "id", R"("id":"c01105")"},
        {"token", "chaosTokenRevealedBy", R"("chaosTokenRevealedBy":null)"},
    };
    const char *const grammar = "card code must match ^c[0-9a-z:._-]+$";
    const char *const prefix = "card code must start with 'c'";
    const std::initializer_list<std::pair<const char *, const char *>> values{
        {"c01108", nullptr}, {"c01108:b", nullptr}, {"c:hb_1.2-x", nullptr},
        {"c bad", grammar},  {"cBAD", grammar},     {"c/01108", grammar},
        {"c", prefix},       {"", prefix},          {"01108", prefix},
    };
    for (const Field &field : fields) {
      for (const auto &[value, error] : values) {
        const QString expected = error
                                     ? u"x."_s + QLatin1StringView(field.key) +
                                           u": "_s + QLatin1StringView(error)
                                     : QString();
        QTest::addRow("%s.%s \"%s\"", field.type, field.key, value)
            << QString::fromLatin1(field.type) << QByteArray(field.from)
            << QByteArray(field.key) << QByteArray(value) << expected;
      }
    }
  }

  void boardCardCodeGrammar() {
    QFETCH(QString, type);
    QFETCH(QByteArray, from);
    QFETCH(QByteArray, key);
    QFETCH(QByteArray, value);
    QFETCH(QString, expectedError);
    const QByteArray json =
        mutated(type, from, '"' + key + "\":\"" + value + '"');

    if (!expectedError.isEmpty()) {
      const QString error = decodeError(type, json);
      QVERIFY2(!error.isEmpty(), "invalid board card code must not decode");
      QVERIFY2(error.contains(expectedError), qPrintable(error));
      return;
    }
    QString decoded;
    if (type == "act"_L1) {
      const auto act = Act::fromRawBytes(json, u"x");
      QVERIFY2(act, qPrintable(act ? QString() : act.error()));
      decoded = act->id().value();
    } else if (type == "agenda"_L1) {
      const auto agenda = Agenda::fromRawBytes(json, u"x");
      QVERIFY2(agenda, qPrintable(agenda ? QString() : agenda.error()));
      decoded = agenda->id().value();
    } else {
      const auto token = ChaosToken::fromRawBytes(json, u"x");
      QVERIFY2(token, qPrintable(token ? QString() : token.error()));
      QVERIFY(token->revealedBy().has_value());
      decoded = token->revealedBy()->value();
    }
    QCOMPARE(decoded, QString::fromLatin1(value));
  }
};

QTEST_APPLESS_MAIN(BoardTests)

#include "BoardTests.moc"
