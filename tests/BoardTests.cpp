#include <QFile>
#include <QtTest>

#include <initializer_list>
#include <utility>

#include "Board.h"
#include "Games.h"
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

// The fixtures' only investigator (Roland Banks), keyed by its card code.
Json::Value rolandOf(const Json::Value &game) {
  return game.value("investigators"_L1).value("c01001"_L1);
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
// get-game.json's Roland Banks, trimmed to the modeled keys, with tokens and
// a connection added.
constexpr auto kInvestigator =
    R"({"id":"c01001","cardCode":"c01001",)"
    R"("playerId":"00000000-0000-0000-0000-000000000001",)"
    R"("name":{"title":"Roland Banks","subtitle":"The Fed"},)"
    R"("class":"Guardian","health":9,"sanity":5,"willpower":3,)"
    R"("intellect":3,"combat":4,"agility":2,)"
    R"("tokens":[["Damage",1],["Horror",2],["Resource",5],["Clue",1]],)"
    R"("remainingActions":3,"unhealedHorrorThisRound":0,)"
    R"("defeated":false,"resigned":false,"eliminated":false,)"
    R"("placement":{"tag":"AtLocation",)"
    R"("contents":"782de537-8f5d-4eda-b849-2362e28402e5"},)"
    R"("connectedLocations":["00000000-0000-0000-0000-000000000032"]})";

// A standalone scenario at turn zero with an empty chaos bag.
constexpr auto kMode = R"({"That":{"id":"c01104","turn":0,)"
                       R"("chaosBag":{"chaosTokens":[],)"
                       R"("revealedChaosTokens":[]}}})";

// A minimal PublicGame: the mode above, no running phase step and one entry
// in each modeled map, keyed by that entry's own id.
QByteArray publicGameJson() {
  return QByteArray(R"({"tag":"PublicGame",)"
                    R"("id":"00000000-0000-0000-0000-000000000003",)"
                    R"("name":"Contract fixture game","mode":)") +
         kMode +
         R"(,"encounterDeckSize":26,"phase":"InvestigationPhase",)"
         R"("phaseStep":null,"locations":{")" +
         kStudyId + R"(":)" + kLocation + R"(},"investigators":{"c01001":)" +
         kInvestigator + R"(},"acts":{"c01108":)" + kAct +
         R"(},"agendas":{"c01105":)" + kAgenda + "}}";
}

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
  if (type == "investigator"_L1)
    return kInvestigator;
  if (type == "mode"_L1)
    return kMode;
  if (type == "game"_L1)
    return publicGameJson();
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
  if (type == "investigator"_L1)
    return errorOf(Investigator::fromRawBytes(json, u"x"));
  if (type == "mode"_L1)
    return errorOf(GameMode::fromRawBytes(json, u"x"));
  if (type == "game"_L1)
    return errorOf(PublicGame::fromRawBytes(json, u"x"));
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

  // A large list of distinct names decodes in wire order, and a duplicate
  // far from its first occurrence is still caught.
  void tokenCountsLargeDistinctList() {
    constexpr int kCount = 5'000;
    QByteArray json = "[";
    for (int i = 0; i < kCount; ++i) {
      if (i > 0)
        json += ',';
      json +=
          "[\"t" + QByteArray::number(i) + "\"," + QByteArray::number(i) + ']';
    }
    json += ']';

    const auto tokens = TokenCounts::fromRawBytes(json, u"tokens");
    QVERIFY2(tokens, qPrintable(tokens ? QString() : tokens.error()));
    QCOMPARE(tokens->entries().size(), kCount);
    QCOMPARE(tokens->entries().first(), std::pair(u"t0"_s, qint64{0}));
    QCOMPARE(tokens->entries().last(), std::pair(u"t4999"_s, qint64{4999}));
    QCOMPARE(tokens->count(u"t2500"), 2500);

    json.chop(1);
    json += R"(,["t0",1]])";
    QCOMPARE(decodeError(u"tokens"_s, json),
             uR"(x[5000][0]: duplicate token "t0")"_s);
  }

  void getGameFixtureInvestigator() {
    const auto game = getGamePublicGame();
    QCOMPARE(game.value("investigators"_L1).keys(), QStringList{u"c01001"_s});
    const auto investigator =
        Investigator::fromRawJson(rolandOf(game), u"investigator");
    QVERIFY2(investigator,
             qPrintable(investigator ? QString() : investigator.error()));
    QCOMPARE(investigator->id().value(), u"c01001"_s);
    QCOMPARE(investigator->cardCode().value(), u"c01001"_s);
    QVERIFY(investigator->playerId().has_value());
    QCOMPARE(investigator->playerId()->value(),
             u"00000000-0000-0000-0000-000000000001"_s);
    QCOMPARE(investigator->name().title, u"Roland Banks"_s);
    QCOMPARE(investigator->name().subtitle, std::optional(u"The Fed"_s));
    QCOMPARE(investigator->classSymbol(), ClassSymbol::Guardian);
    QCOMPARE(investigator->health(), 9);
    QCOMPARE(investigator->sanity(), 5);
    QCOMPARE(investigator->willpower(), 3);
    QCOMPARE(investigator->intellect(), 3);
    QCOMPARE(investigator->combat(), 4);
    QCOMPARE(investigator->agility(), 2);
    QVERIFY(investigator->tokens().entries().isEmpty());
    QCOMPARE(investigator->damage(), 0);
    QCOMPARE(investigator->horror(), 0);
    QCOMPARE(investigator->resources(), 0);
    QCOMPARE(investigator->clues(), 0);
    QCOMPARE(investigator->doom(), 0);
    QCOMPARE(investigator->remainingActions(), 3);
    QCOMPARE(investigator->unhealedHorrorThisRound(), 0);
    QCOMPARE(investigator->defeated(), false);
    QCOMPARE(investigator->resigned(), false);
    QCOMPARE(investigator->eliminated(), false);
    QCOMPARE(investigator->placement().value("tag"_L1).toString(),
             u"AtLocation"_s);
    QVERIFY(investigator->placementLocation().has_value());
    QCOMPARE(investigator->placementLocation()->value(),
             QLatin1StringView(kStudyId));
    QVERIFY(investigator->connectedLocations().isEmpty());
    QCOMPARE(investigator->art(), u"c01001"_s);
    QVERIFY(!investigator->mutated().has_value());

    // The Study lists Roland as its occupant.
    const auto study = Location::fromRawJson(studyOf(game), u"location");
    QVERIFY(study);
    QVERIFY(study->investigators().contains(investigator->id()));
    QVERIFY(study->id() == *investigator->placementLocation());
  }

  void investigatorDecodes() {
    const auto investigator =
        Investigator::fromRawBytes(validJson(u"investigator"_s), u"inv");
    QVERIFY2(investigator,
             qPrintable(investigator ? QString() : investigator.error()));
    QCOMPARE(investigator->damage(), 1);
    QCOMPARE(investigator->horror(), 2);
    QCOMPARE(investigator->resources(), 5);
    QCOMPARE(investigator->clues(), 1);
    QCOMPARE(investigator->doom(), 0);
    QCOMPARE(investigator->connectedLocations().size(), 1);
    QCOMPARE(investigator->connectedLocations().first().value(),
             u"00000000-0000-0000-0000-000000000032"_s);
  }

  // `art` and `mutated` are presentation data decoded tolerantly: a missing
  // or unusable value falls back (art to the id, mutated to no mutation)
  // and never fails the investigator.
  void investigatorArtAndMutation_data() {
    QTest::addColumn<QByteArray>("fields");
    QTest::addColumn<QString>("art");
    QTest::addColumn<QString>("mutation");
    QTest::newRow("both absent") << QByteArray() << u"c01001"_s << QString();
    QTest::newRow("present, mutated null")
        << QByteArray(R"("art":"c01001b","mutated":null,)") << u"c01001b"_s
        << QString();
    QTest::newRow("homebrew art")
        << QByteArray(R"("art":":fan:12345",)") << u":fan:12345"_s << QString();
    QTest::newRow("valid Mutated21")
        << QByteArray(R"("art":"c01001","mutated":"Mutated21",)") << u"c01001"_s
        << u"Mutated21"_s;
    QTest::newRow("malformed suffix")
        << QByteArray(R"("mutated":"Mutated2x",)") << u"c01001"_s << QString();
    QTest::newRow("prefix only")
        << QByteArray(R"("mutated":"Mutated",)") << u"c01001"_s << QString();
    QTest::newRow("leading underscore")
        << QByteArray(R"("mutated":"_Mutated21",)") << u"c01001"_s << QString();
    QTest::newRow("non-string mutated")
        << QByteArray(R"("mutated":21,)") << u"c01001"_s << QString();
    QTest::newRow("null art")
        << QByteArray(R"("art":null,)") << u"c01001"_s << QString();
    QTest::newRow("empty art")
        << QByteArray(R"("art":"",)") << u"c01001"_s << QString();
    QTest::newRow("non-string art")
        << QByteArray(R"("art":{"x":1},)") << u"c01001"_s << QString();
  }

  void investigatorArtAndMutation() {
    QFETCH(QByteArray, fields);
    QFETCH(QString, art);
    QFETCH(QString, mutation);
    const auto investigator = Investigator::fromRawBytes(
        mutated(u"investigator"_s, R"("cardCode":"c01001",)",
                QByteArray(R"("cardCode":"c01001",)") + fields),
        u"inv");
    QVERIFY2(investigator,
             qPrintable(investigator ? QString() : investigator.error()));
    QCOMPARE(investigator->art(), art);
    QCOMPARE(investigator->mutated().has_value(), !mutation.isNull());
    if (!mutation.isNull())
      QCOMPARE(*investigator->mutated(), mutation);
  }

  // Unbounded integers: the fork's investigator-unhealed-horror-negative.json
  // carries -3, and the base skills have no schema minimum.
  void investigatorAcceptsNegativeUnboundedIntegers() {
    const QByteArray json =
        mutated(u"investigator"_s, R"("unhealedHorrorThisRound":0)",
                R"("unhealedHorrorThisRound":-3)")
            .replace(R"("willpower":3)", R"("willpower":-1)")
            .replace(R"("remainingActions":3)", R"("remainingActions":0)");
    const auto investigator = Investigator::fromRawBytes(json, u"inv");
    QVERIFY2(investigator,
             qPrintable(investigator ? QString() : investigator.error()));
    QCOMPARE(investigator->unhealedHorrorThisRound(), -3);
    QCOMPARE(investigator->willpower(), -1);
    QCOMPARE(investigator->remainingActions(), 0);
  }

  // The backend publishes killed and other-campaign investigators with
  // `PlayerId nil` (Arkham.Game's lookupInvestigator iid (PlayerId nil)).
  void investigatorNilPlayerIdDecodesToNullopt() {
    const auto investigator = Investigator::fromRawBytes(
        mutated(u"investigator"_s,
                R"("playerId":"00000000-0000-0000-0000-000000000001")",
                R"("playerId":"00000000-0000-0000-0000-000000000000")"),
        u"inv");
    QVERIFY2(investigator,
             qPrintable(investigator ? QString() : investigator.error()));
    QVERIFY(!investigator->playerId().has_value());
    QCOMPARE(investigator->id().value(), u"c01001"_s);
  }

  void investigatorStatusFlags() {
    const QByteArray json =
        mutated(u"investigator"_s,
                R"("defeated":false,"resigned":false,"eliminated":false)",
                R"("defeated":true,"resigned":false,"eliminated":true)")
            .replace(R"("subtitle":"The Fed")", R"("subtitle":null)");
    const auto investigator = Investigator::fromRawBytes(json, u"inv");
    QVERIFY2(investigator,
             qPrintable(investigator ? QString() : investigator.error()));
    QCOMPARE(investigator->defeated(), true);
    QCOMPARE(investigator->resigned(), false);
    QCOMPARE(investigator->eliminated(), true);
    QVERIFY(!investigator->name().subtitle.has_value());
  }

  // Only "AtLocation" yields a placementLocation. Any other string tag is
  // accepted and kept raw, including backend tags placement.schema.json's
  // enum does not list (AtLocations, BetweenLocations).
  void investigatorPlacementTags_data() {
    QTest::addColumn<QByteArray>("placement");
    QTest::addColumn<QString>("tag");
    QTest::newRow("InVehicle")
        << QByteArray(R"({"tag":"InVehicle","contents":)"
                      R"("0c3cfd0c-1b5e-4f43-9d9a-0b1c2d3e4f50"})")
        << u"InVehicle"_s;
    QTest::newRow("Unplaced")
        << QByteArray(R"({"tag":"Unplaced"})") << u"Unplaced"_s;
    QTest::newRow("Limbo") << QByteArray(R"({"tag":"Limbo"})") << u"Limbo"_s;
    QTest::newRow("AtLocations")
        << QByteArray(R"({"tag":"AtLocations","contents":[)"
                      R"("782de537-8f5d-4eda-b849-2362e28402e5"]})")
        << u"AtLocations"_s;
    QTest::newRow("BetweenLocations")
        << QByteArray(R"({"tag":"BetweenLocations","contents":{"x":1}})")
        << u"BetweenLocations"_s;
  }

  void investigatorPlacementTags() {
    QFETCH(QByteArray, placement);
    QFETCH(QString, tag);
    const auto investigator = Investigator::fromRawBytes(
        mutated(u"investigator"_s,
                R"({"tag":"AtLocation",)"
                R"("contents":"782de537-8f5d-4eda-b849-2362e28402e5"})",
                placement),
        u"inv");
    QVERIFY2(investigator,
             qPrintable(investigator ? QString() : investigator.error()));
    QCOMPARE(investigator->placement().value("tag"_L1).toString(), tag);
    QVERIFY(!investigator->placementLocation().has_value());
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

    const auto investigatorA =
        Investigator::fromRawJson(rolandOf(getGame), u"a");
    const auto investigatorB =
        Investigator::fromRawJson(rolandOf(update), u"b");
    QVERIFY(investigatorA && investigatorB);
    QVERIFY(*investigatorA == *investigatorB);
    QCOMPARE(investigatorB->health(), 9);
  }

  // The whole CORE snapshot through PublicGame, with exact fixture values.
  void publicGameDecodesGetGameFixture() {
    const auto game = PublicGame::fromRawJson(getGamePublicGame(), u"game");
    QVERIFY2(game, qPrintable(game ? QString() : game.error()));

    QCOMPARE(game->id().value(), u"00000000-0000-0000-0000-000000000003"_s);
    QCOMPARE(game->name(), u"Contract fixture game"_s);
    QCOMPARE(game->encounterDeckSize(), 26);
    QCOMPARE(game->phase(), Phase::InvestigationPhase);
    QVERIFY(game->phaseStep().has_value());
    QCOMPARE(game->phaseStep()->kind(),
             PhaseStep::Kind::InvestigationPhaseStep);
    QCOMPARE(game->phaseStep()->step(),
             PhaseStep::Step::InvestigatorTakesActionStep);

    QCOMPARE(game->mode().kind(), GameMode::Kind::ScenarioOnly);
    QVERIFY(!game->mode().campaign().has_value());
    QVERIFY(game->mode().scenario().has_value());
    QCOMPARE(game->mode().scenario()->id().value(), u"c01104"_s);
    QCOMPARE(game->mode().scenario()->turn(), 1);

    const ChaosBag *bag = game->chaosBag();
    QVERIFY(bag != nullptr);
    QCOMPARE(bag, &game->mode().scenario()->chaosBag());
    QCOMPARE(bag->chaosTokens().size(), 16);
    QCOMPARE(bag->chaosTokens().first().face(), u"MinusTwo"_s);
    QCOMPARE(bag->chaosTokens().first().id().value(),
             u"22e6cf42-cc5c-4a9c-b9d1-f71e594e0d60"_s);
    QCOMPARE(bag->chaosTokens().last().face(), u"Cultist"_s);
    QVERIFY(bag->revealedChaosTokens().isEmpty());

    QCOMPARE(game->locations().size(), 1);
    const auto studyId = LocationId::parse(QString::fromLatin1(kStudyId));
    QVERIFY(studyId);
    const Location *study = game->location(*studyId);
    QVERIFY(study != nullptr);
    QCOMPARE(study, &game->locations().first());
    QCOMPARE(study->label(), u"study"_s);
    QCOMPARE(study->cardCode().value(), u"c01111"_s);
    QCOMPARE(study->clues(), 2);
    const auto otherId =
        LocationId::parse(u"00000000-0000-0000-0000-000000000032"_s);
    QVERIFY(otherId);
    QCOMPARE(game->location(*otherId), nullptr);

    const auto rolandCode = CardCode::parse(u"c01001"_s);
    QVERIFY(rolandCode);
    QCOMPARE(game->investigators().size(), 1);
    const Investigator *roland = game->investigator(*rolandCode);
    QVERIFY(roland != nullptr);
    QCOMPARE(roland->name().title, u"Roland Banks"_s);
    QCOMPARE(roland->health(), 9);
    QCOMPARE(roland->sanity(), 5);
    QCOMPARE(roland->placementLocation(), std::optional<LocationId>(*studyId));
    QCOMPARE(study->investigators(), QList<CardCode>{*rolandCode});

    const auto actCode = CardCode::parse(u"c01108"_s);
    QVERIFY(actCode);
    QCOMPARE(game->acts().size(), 1);
    const Act *act = game->act(*actCode);
    QVERIFY(act != nullptr);
    QCOMPARE(act->sequenceStep(), 1);
    QCOMPARE(act->sequenceSide(), ActSide::A);
    QCOMPARE(act->advanceCost().value("tag"_L1).toString(), u"GroupClueCost"_s);
    QCOMPARE(game->act(*rolandCode), nullptr);

    const auto agendaCode = CardCode::parse(u"c01105"_s);
    QVERIFY(agendaCode);
    QCOMPARE(game->agendas().size(), 1);
    const Agenda *agenda = game->agenda(*agendaCode);
    QVERIFY(agenda != nullptr);
    QCOMPARE(agenda->doom(), 0);
    QVERIFY(agenda->doomThreshold().has_value());
    QVERIFY(*agenda->doomThreshold() == GameValue::staticValue(3));
  }

  // The GetGame REST envelope around that PublicGame.
  void getGameEnvelopeDecodes() {
    const auto getGame =
        GetGame::fromRawBytes(loadFixtureBytes(u"get-game.json"_s), u"getGame");
    QVERIFY2(getGame, qPrintable(getGame ? QString() : getGame.error()));
    QVERIFY(getGame->playerId().has_value());
    QCOMPARE(getGame->playerId()->value(),
             u"00000000-0000-0000-0000-000000000001"_s);
    QCOMPARE(getGame->multiplayerMode(), MultiplayerVariant::Solo);
    QVERIFY(!getGame->eventId().has_value());

    const auto game = PublicGame::fromRawJson(getGamePublicGame(), u"game");
    QVERIFY(game);
    QVERIFY(getGame->game() == *game);
    QCOMPARE(getGame->game().encounterDeckSize(), 26);
  }

  void getGameEnvelopeNullableAndRejected_data() {
    QTest::addColumn<QByteArray>("from");
    QTest::addColumn<QByteArray>("to");
    // Empty: the mutated envelope must decode.
    QTest::addColumn<QString>("expectedError");
    QTest::newRow("spectator playerId null") << QByteArray(R"("Solo",
  "playerId": "00000000-0000-0000-0000-000000000001")")
                                             << QByteArray(R"("Solo",
  "playerId": null)") << QString();
    QTest::newRow("eventId uuid")
        << QByteArray(R"("eventId": null)")
        << QByteArray(R"("eventId": "00000000-0000-0000-0000-000000000005")")
        << QString();
    QTest::newRow("WithFriends")
        << QByteArray(R"("multiplayerMode": "Solo")")
        << QByteArray(R"("multiplayerMode": "WithFriends")") << QString();
    QTest::newRow("unknown multiplayerMode")
        << QByteArray(R"("multiplayerMode": "Solo")")
        << QByteArray(R"("multiplayerMode": "Party")")
        << uR"(getGame.multiplayerMode: unrecognized value "Party")"_s;
    QTest::newRow("missing eventId")
        << QByteArray(R"("eventId": null,)") << QByteArray()
        << uR"(getGame.eventId: missing required field "eventId")"_s;
    QTest::newRow("bad playerId") << QByteArray(R"("Solo",
  "playerId": "00000000-0000-0000-0000-000000000001")")
                                  << QByteArray(R"("Solo",
  "playerId": 1)") << u"getGame.playerId"_s;
    QTest::newRow("game error path")
        << QByteArray(R"("encounterDeckSize": 26)")
        << QByteArray(R"("encounterDeckSize": -26)")
        << u"getGame.game.encounterDeckSize: must not be negative, got -26"_s;
    QTest::newRow("missing playerId") << QByteArray(R"("Solo",
  "playerId")") << QByteArray(R"("Solo",
  "playerIdX")") << uR"(getGame.playerId: missing required field "playerId")"_s;
    QTest::newRow("missing game")
        << QByteArray(R"("game": {)") << QByteArray(R"("gameX": {)")
        << uR"(getGame.game: missing required field "game")"_s;
    QTest::newRow("missing multiplayerMode")
        << QByteArray(R"("multiplayerMode": "Solo")")
        << QByteArray(R"("multiplayerModeX": "Solo")")
        << uR"(getGame.multiplayerMode: missing required field "multiplayerMode")"_s;
  }

  void getGameEnvelopeNullableAndRejected() {
    QFETCH(QByteArray, from);
    QFETCH(QByteArray, to);
    QFETCH(QString, expectedError);
    QByteArray json = loadFixtureBytes(u"get-game.json"_s);
    const qsizetype at = json.indexOf(from);
    QVERIFY2(at >= 0, from.constData());
    json.replace(at, from.size(), to);
    const auto getGame = GetGame::fromRawBytes(json, u"getGame");
    if (expectedError.isEmpty()) {
      QVERIFY2(getGame, qPrintable(getGame ? QString() : getGame.error()));
      return;
    }
    QVERIFY2(!getGame, "mutated envelope must not decode");
    QVERIFY2(getGame.error().contains(expectedError),
             qPrintable(getGame.error()));
  }

  // REST and WebSocket decode to the same snapshot.
  void gameUpdateServerMessageMatchesGetGame() {
    const auto message = ServerMessage::fromRawBytes(
        loadFixtureBytes(u"game-update.json"_s), u"message");
    QVERIFY2(message, qPrintable(message ? QString() : message.error()));
    QCOMPARE(message->kind(), ServerMessage::Kind::GameUpdate);
    QCOMPARE(message->tag(), u"GameUpdate"_s);
    QVERIFY(message->displayText().isEmpty());
    QVERIFY(message->game().has_value());

    const auto getGame =
        GetGame::fromRawBytes(loadFixtureBytes(u"get-game.json"_s), u"getGame");
    QVERIFY(getGame);
    QVERIFY(*message->game() == getGame->game());
    QCOMPARE(message->game()->encounterDeckSize(), 26);
    QCOMPARE(message->game()->chaosBag()->chaosTokens().first().face(),
             u"MinusTwo"_s);
  }

  void gameUpdateWithBadContentsFails() {
    const auto missing =
        ServerMessage::fromRawBytes(R"({"tag":"GameUpdate"})", u"message");
    QVERIFY(!missing);
    QCOMPARE(missing.error(),
             uR"(message.contents: missing required field "contents")"_s);

    const auto unmodeled = ServerMessage::fromRawBytes(
        R"({"tag":"GameUpdate","contents":{"unmodeled":"public-game"}})",
        u"message");
    QVERIFY(!unmodeled);
    QCOMPARE(unmodeled.error(),
             uR"(message.contents.tag: missing required field "tag")"_s);
  }

  // Every message kind other than GameUpdate carries no snapshot.
  void nonGameUpdateMessagesHaveNoGame() {
    const auto message = ServerMessage::fromRawBytes(
        R"({"tag":"GameMessage","contents":"hi"})", u"message");
    QVERIFY(message);
    QVERIFY(!message->game().has_value());
  }

  // mode.schema.json's three sibling-key branches.
  void gameModeVariants() {
    const QByteArray scenario =
        R"({"id":"c01104","turn":0,"chaosBag":{"chaosTokens":[],)"
        R"("revealedChaosTokens":[]}})";
    const QByteArray campaign = R"({"continueOptions":[],"other":1})";

    const auto campaignOnly =
        GameMode::fromRawBytes("{\"This\":" + campaign + "}", u"mode");
    QVERIFY2(campaignOnly,
             qPrintable(campaignOnly ? QString() : campaignOnly.error()));
    QCOMPARE(campaignOnly->kind(), GameMode::Kind::CampaignOnly);
    QVERIFY(campaignOnly->campaign().has_value());
    QCOMPARE(campaignOnly->campaign()->keys(),
             (QStringList{u"continueOptions"_s, u"other"_s}));
    QVERIFY(!campaignOnly->scenario().has_value());

    const auto both = GameMode::fromRawBytes(
        "{\"This\":" + campaign + ",\"That\":" + scenario + "}", u"mode");
    QVERIFY2(both, qPrintable(both ? QString() : both.error()));
    QCOMPARE(both->kind(), GameMode::Kind::CampaignAndScenario);
    QVERIFY(both->campaign().has_value());
    QVERIFY(both->scenario().has_value());
    QCOMPARE(both->scenario()->turn(), 0);
    QCOMPARE(both->scenario()->id().value(), u"c01104"_s);

    const auto scenarioOnly =
        GameMode::fromRawBytes("{\"That\":" + scenario + "}", u"mode");
    QVERIFY(scenarioOnly);
    QCOMPARE(scenarioOnly->kind(), GameMode::Kind::ScenarioOnly);
    QVERIFY(!scenarioOnly->campaign().has_value());
    QVERIFY(scenarioOnly->scenario()->chaosBag().chaosTokens().isEmpty());

    // A between-scenario snapshot has no chaos bag.
    const QByteArray game =
        mutated(u"game"_s, validJson(u"mode"_s), "{\"This\":" + campaign + "}");
    const auto campaignGame = PublicGame::fromRawBytes(game, u"game");
    QVERIFY2(campaignGame,
             qPrintable(campaignGame ? QString() : campaignGame.error()));
    QCOMPARE(campaignGame->chaosBag(), nullptr);
  }

  // Map entries keep wire order even when their keys are in descending
  // order, and each lookup returns that entry's own element.
  void publicGameMapsKeepWireOrder() {
    const QByteArray secondLocation =
        QByteArray(kLocation)
            .replace(R"("id":"782de537-8f5d-4eda-b849-2362e28402e5")",
                     R"("id":"00000000-0000-0000-0000-000000000032")")
            .replace(R"("label":"study")", R"("label":"hallway")");
    const QByteArray secondInvestigator =
        QByteArray(kInvestigator)
            .replace(R"("id":"c01001","cardCode":"c01001")",
                     R"("id":"c00002","cardCode":"c00002")")
            .replace(R"("title":"Roland Banks")", R"("title":"Second")");
    QByteArray json = mutated(u"game"_s, R"(}},"investigators":{)",
                              R"(},"00000000-0000-0000-0000-000000000032":)" +
                                  secondLocation + R"(},"investigators":{)");
    const QByteArray actsFrom = R"(}},"acts":{)";
    const qsizetype at = json.indexOf(actsFrom);
    QVERIFY(at >= 0);
    json.replace(at, actsFrom.size(),
                 R"(},"c00002":)" + secondInvestigator + R"(},"acts":{)");

    const auto game = PublicGame::fromRawBytes(json, u"game");
    QVERIFY2(game, qPrintable(game ? QString() : game.error()));

    QCOMPARE(game->locations().size(), 2);
    QCOMPARE(game->locations()[0].id().value(),
             u"782de537-8f5d-4eda-b849-2362e28402e5"_s);
    QCOMPARE(game->locations()[1].id().value(),
             u"00000000-0000-0000-0000-000000000032"_s);
    for (const Location &location : game->locations()) {
      const Location *found = game->location(location.id());
      QCOMPARE(found, &location);
    }
    QCOMPARE(game->location(game->locations()[0].id())->label(), u"study"_s);
    QCOMPARE(game->location(game->locations()[1].id())->label(), u"hallway"_s);

    QCOMPARE(game->investigators().size(), 2);
    QCOMPARE(game->investigators()[0].id().value(), u"c01001"_s);
    QCOMPARE(game->investigators()[1].id().value(), u"c00002"_s);
    for (const Investigator &investigator : game->investigators()) {
      const Investigator *found = game->investigator(investigator.id());
      QCOMPARE(found, &investigator);
    }
    QCOMPARE(game->investigator(game->investigators()[0].id())->name().title,
             u"Roland Banks"_s);
    QCOMPARE(game->investigator(game->investigators()[1].id())->name().title,
             u"Second"_s);
  }

  // Tolerated mode shapes: an extra "These" key beside "That" is an ignored
  // additive key, so the mode is still ScenarioOnly.
  void gameModeToleratesExtraTheseKey() {
    const auto mode = GameMode::fromRawBytes(
        mutated(u"mode"_s, R"({"That":)", R"({"These":{"x":1},"That":)"),
        u"mode");
    QVERIFY2(mode, qPrintable(mode ? QString() : mode.error()));
    QCOMPARE(mode->kind(), GameMode::Kind::ScenarioOnly);
    QVERIFY(!mode->campaign().has_value());
    QVERIFY(mode->scenario().has_value());
    QCOMPARE(mode->scenario()->id().value(), u"c01104"_s);
  }

  // The minimal document: turn zero, a null phase step, and an uppercase
  // UUID key that canonicalizes to the location's own id.
  void publicGameMinimalDocument() {
    const auto game = PublicGame::fromRawBytes(validJson(u"game"_s), u"game");
    QVERIFY2(game, qPrintable(game ? QString() : game.error()));
    QVERIFY(!game->phaseStep().has_value());
    QCOMPARE(game->mode().scenario()->turn(), 0);
    QVERIFY(game->chaosBag()->chaosTokens().isEmpty());

    const auto upper = PublicGame::fromRawBytes(
        mutated(u"game"_s,
                R"("locations":{"782de537-8f5d-4eda-b849-2362e28402e5")",
                R"("locations":{"782DE537-8F5D-4EDA-B849-2362E28402E5")"),
        u"game");
    QVERIFY2(upper, qPrintable(upper ? QString() : upper.error()));
    QVERIFY(*upper == *game);

    const auto empty = PublicGame::fromRawBytes(
        mutated(u"game"_s,
                R"("agendas":{"c01105":)" + QByteArray(kAgenda) + "}",
                R"("agendas":{})"),
        u"game");
    QVERIFY2(empty, qPrintable(empty ? QString() : empty.error()));
    QVERIFY(empty->agendas().isEmpty());
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

  void classSymbolDecodesEveryValue_data() {
    QTest::addColumn<QByteArray>("wire");
    QTest::addColumn<ClassSymbol>("expected");
    const std::initializer_list<std::pair<const char *, ClassSymbol>> rows{
        {"Guardian", ClassSymbol::Guardian}, {"Seeker", ClassSymbol::Seeker},
        {"Survivor", ClassSymbol::Survivor}, {"Rogue", ClassSymbol::Rogue},
        {"Mystic", ClassSymbol::Mystic},     {"Neutral", ClassSymbol::Neutral},
        {"Mythos", ClassSymbol::Mythos},
    };
    for (const auto &[wire, expected] : rows)
      QTest::newRow(wire) << QByteArray(wire) << expected;
  }

  void classSymbolDecodesEveryValue() {
    QFETCH(QByteArray, wire);
    QFETCH(ClassSymbol, expected);
    const auto investigator = Investigator::fromRawBytes(
        mutated(u"investigator"_s, R"("class":"Guardian")",
                "\"class\":\"" + wire + '"'),
        u"inv");
    QVERIFY2(investigator,
             qPrintable(investigator ? QString() : investigator.error()));
    QCOMPARE(investigator->classSymbol(), expected);
  }

  void validDocumentsDecode_data() {
    QTest::addColumn<QString>("type");
    for (const auto *type :
         {"token", "bag", "step", "act", "agenda", "phase", "tokens",
          "location", "enemyLocation", "investigator", "mode", "game"})
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
         {"token", "bag", "step", "act", "agenda", "location", "enemyLocation",
          "investigator", "mode", "game"})
      QTest::newRow(type) << QString::fromLatin1(type) << QByteArray("{")
                          << extra;
    QTest::newRow("token nested in bag")
        << u"bag"_s << QByteArray(R"("chaosTokenSealed":false)")
        << QByteArray(R"("chaosTokenSealed":false,"futureField":1)");
    QTest::newRow("agenda sequence")
        << u"agenda"_s << QByteArray(R"({"agendaSequenceSide")")
        << QByteArray(R"({"futureField":true,"agendaSequenceSide")");
    QTest::newRow("investigator name")
        << u"investigator"_s << QByteArray(R"({"title")")
        << QByteArray(R"({"futureField":true,"title")");
    QTest::newRow("investigator placement")
        << u"investigator"_s << QByteArray(R"({"tag":"AtLocation")")
        << QByteArray(R"({"futureField":true,"tag":"AtLocation")");
    QTest::newRow("mode scenario")
        << u"mode"_s << QByteArray(R"({"id":"c01104")")
        << QByteArray(R"({"futureField":true,"id":"c01104")");
    QTest::newRow("game entity nested in map")
        << u"game"_s << QByteArray(R"({"id":"c01108")")
        << QByteArray(R"({"futureField":true,"id":"c01108")");
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
    row("location tokens not an array", u"location"_s,
        R"("tokens":[["Clue",2]])", R"("tokens":{"Clue":2})",
        u"x.tokens: expected array, got object"_s);
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
    row("enemy location missing cardId", u"enemyLocation"_s,
        R"("cardId":"00000000-0000-0000-0000-000000000033",)", "",
        uR"(x.cardId: missing required field "cardId")"_s);
    row("enemy location missing label", u"enemyLocation"_s,
        R"("label":"shapeless_cellar",)", "",
        uR"(x.label: missing required field "label")"_s);

    row("investigator null", u"investigator"_s, kInvestigator, "null",
        u"x: expected object, got null"_s);
    row("investigator missing playerId", u"investigator"_s,
        R"("playerId":"00000000-0000-0000-0000-000000000001",)", "",
        uR"(x.playerId: missing required field "playerId")"_s);
    row("investigator playerId not a uuid", u"investigator"_s,
        R"("playerId":"00000000-0000-0000-0000-000000000001")",
        R"("playerId":"player-1")", u"x.playerId: not a valid non-null uuid"_s);
    // The nil exemption is exact: a short all-zero string is still malformed.
    row("investigator playerId short zeros", u"investigator"_s,
        R"("playerId":"00000000-0000-0000-0000-000000000001")",
        R"("playerId":"00000000-0000-0000-0000-00000000000")",
        u"x.playerId: not a valid non-null uuid"_s);
    row("investigator playerId null", u"investigator"_s,
        R"("playerId":"00000000-0000-0000-0000-000000000001")",
        R"("playerId":null)", u"x.playerId: expected uuid string, got null"_s);
    row("investigator playerId wrong type", u"investigator"_s,
        R"("playerId":"00000000-0000-0000-0000-000000000001")",
        R"("playerId":0)", u"x.playerId: expected uuid string, got number"_s);
    row("investigator missing name", u"investigator"_s,
        R"("name":{"title":"Roland Banks","subtitle":"The Fed"},)", "",
        uR"(x.name: missing required field "name")"_s);
    row("investigator name not an object", u"investigator"_s,
        R"({"title":"Roland Banks","subtitle":"The Fed"})", R"("Roland Banks")",
        u"x.name: expected object, got string"_s);
    row("investigator name missing subtitle", u"investigator"_s,
        R"(,"subtitle":"The Fed")", "",
        uR"(x.name.subtitle: missing required field "subtitle")"_s);
    row("investigator name subtitle wrong type", u"investigator"_s,
        R"("subtitle":"The Fed")", R"("subtitle":7)", u"x.name.subtitle"_s);
    row("investigator name title not a string", u"investigator"_s,
        R"("title":"Roland Banks")", R"("title":42)",
        u"x.name.title: expected string, got number"_s);
    row("investigator class unknown", u"investigator"_s,
        R"("class":"Guardian")", R"("class":"Wizard")",
        uR"(x.class: unrecognized value "Wizard")"_s);
    row("investigator class wrong case", u"investigator"_s,
        R"("class":"Guardian")", R"("class":"guardian")",
        uR"(x.class: unrecognized value "guardian")"_s);
    row("investigator negative health", u"investigator"_s, R"("health":9)",
        R"("health":-1)", u"x.health: must not be negative, got -1"_s);
    row("investigator negative sanity", u"investigator"_s, R"("sanity":5)",
        R"("sanity":-2)", u"x.sanity: must not be negative, got -2"_s);
    row("investigator sanity wrong type", u"investigator"_s, R"("sanity":5)",
        R"("sanity":"5")", u"x.sanity: expected integer, got string"_s);
    row("investigator missing combat", u"investigator"_s, R"("combat":4,)", "",
        uR"(x.combat: missing required field "combat")"_s);
    row("investigator missing tokens", u"investigator"_s,
        R"("tokens":[["Damage",1],["Horror",2],["Resource",5],["Clue",1]],)",
        "", uR"(x.tokens: missing required field "tokens")"_s);
    row("investigator duplicate token", u"investigator"_s, R"(["Clue",1])",
        R"(["Damage",3])", uR"(x.tokens[3][0]: duplicate token "Damage")"_s);
    row("investigator remainingActions wrong type", u"investigator"_s,
        R"("remainingActions":3)", R"("remainingActions":null)",
        u"x.remainingActions: expected integer, got null"_s);
    row("investigator unhealedHorror fractional", u"investigator"_s,
        R"("unhealedHorrorThisRound":0)", R"("unhealedHorrorThisRound":0.5)",
        u"x.unhealedHorrorThisRound: expected integer"_s);
    row("investigator defeated wrong type", u"investigator"_s,
        R"("defeated":false)", R"("defeated":0)",
        u"x.defeated: expected bool, got number"_s);
    row("investigator missing eliminated", u"investigator"_s,
        R"(,"eliminated":false)", "",
        uR"(x.eliminated: missing required field "eliminated")"_s);
    row("investigator missing placement", u"investigator"_s,
        R"("placement":{"tag":"AtLocation",)"
        R"("contents":"782de537-8f5d-4eda-b849-2362e28402e5"},)",
        "", uR"(x.placement: missing required field "placement")"_s);
    row("investigator placement null", u"investigator"_s,
        R"({"tag":"AtLocation",)"
        R"("contents":"782de537-8f5d-4eda-b849-2362e28402e5"})",
        "null", u"x.placement: expected object, got null"_s);
    row("investigator placement missing tag", u"investigator"_s,
        R"("tag":"AtLocation",)", "",
        uR"(x.placement.tag: missing required field "tag")"_s);
    row("investigator placement tag wrong type", u"investigator"_s,
        R"("tag":"AtLocation")", R"("tag":1)",
        u"x.placement.tag: expected string, got number"_s);
    row("investigator AtLocation missing contents", u"investigator"_s,
        R"(,"contents":"782de537-8f5d-4eda-b849-2362e28402e5")", "",
        uR"(x.placement.contents: missing required field "contents")"_s);
    row("investigator AtLocation contents not a uuid", u"investigator"_s,
        R"("contents":"782de537-8f5d-4eda-b849-2362e28402e5")",
        R"("contents":"study")", u"x.placement.contents"_s);
    row("investigator AtLocation nil uuid", u"investigator"_s,
        R"("contents":"782de537-8f5d-4eda-b849-2362e28402e5")",
        R"("contents":"00000000-0000-0000-0000-000000000000")",
        u"x.placement.contents"_s);
    row("investigator connectedLocations not a uuid", u"investigator"_s,
        R"(["00000000-0000-0000-0000-000000000032"])", R"([1])",
        u"x.connectedLocations[0]"_s);
    row("investigator missing connectedLocations", u"investigator"_s,
        R"(,"connectedLocations":["00000000-0000-0000-0000-000000000032"])", "",
        uR"(x.connectedLocations: missing required field "connectedLocations")"_s);

    // GameMode: an object with neither "This" nor "That" (including a
    // wrapping "These" key) is an unsupported variant.
    row("mode wrapping These key", u"mode"_s, R"({"That":)", R"({"These":)",
        uR"(x: unsupported mode: expected a "This" and/or "That" key)"_s);
    row("mode empty", u"mode"_s, validJson(u"mode"_s), "{}",
        uR"(x: unsupported mode: expected a "This" and/or "That" key)"_s);
    row("mode not an object", u"mode"_s, validJson(u"mode"_s), "[]",
        u"x: expected object, got array"_s);
    row("mode This null", u"mode"_s, R"({"That":)", R"({"This":null,"That":)",
        u"x.This: expected object, got null"_s);
    row("mode That null", u"mode"_s, R"({"That":{"id":"c01104",)",
        R"({"That":null,"x":{"id":"c01104",)",
        u"x.That: expected object, got null"_s);
    row("scenario negative turn", u"mode"_s, R"("turn":0)", R"("turn":-1)",
        u"x.That.turn: must not be negative, got -1"_s);
    row("scenario missing turn", u"mode"_s, R"("turn":0,)", "",
        uR"(x.That.turn: missing required field "turn")"_s);
    row("scenario bad id", u"mode"_s, R"("id":"c01104")", R"("id":"cBAD")",
        u"x.That.id: card code must match"_s);
    row("scenario missing chaosBag", u"mode"_s, R"("chaosBag":)",
        R"("chaosBagX":)",
        uR"(x.That.chaosBag: missing required field "chaosBag")"_s);

    // PublicGame.
    row("game wrong tag", u"game"_s, R"("tag":"PublicGame")",
        R"("tag":"PrivateGame")",
        uR"(x.tag: expected "PublicGame", got "PrivateGame")"_s);
    row("game nil id", u"game"_s, "00000000-0000-0000-0000-000000000003",
        "00000000-0000-0000-0000-000000000000",
        u"x.id: not a valid non-null uuid"_s);
    row("game missing mode", u"game"_s, R"("mode":)", R"("modeX":)",
        uR"(x.mode: missing required field "mode")"_s);
    row("game mode wrapping These key", u"game"_s, R"({"That":)",
        R"({"These":)", u"x.mode: unsupported mode"_s);
    row("game negative encounterDeckSize", u"game"_s,
        R"("encounterDeckSize":26)", R"("encounterDeckSize":-1)",
        u"x.encounterDeckSize: must not be negative, got -1"_s);
    row("game unknown phase", u"game"_s, R"("phase":"InvestigationPhase")",
        R"("phase":"LunchPhase")",
        uR"(x.phase: unrecognized value "LunchPhase")"_s);
    row("game missing phaseStep", u"game"_s, R"("phaseStep":null,)", "",
        uR"(x.phaseStep: missing required field "phaseStep")"_s);
    row("game phaseStep unknown tag", u"game"_s, R"("phaseStep":null)",
        R"("phaseStep":{"tag":"LunchPhaseStep","contents":"X"})",
        uR"(x.phaseStep.tag: unrecognized value "LunchPhaseStep")"_s);
    row("game locations not an object", u"game"_s, R"("locations":{)",
        R"("locations":[],"x":{)",
        u"x.locations: expected object, got array"_s);
    row("game location key not a uuid", u"game"_s,
        R"("locations":{"782de537-8f5d-4eda-b849-2362e28402e5")",
        R"("locations":{"not-a-uuid")",
        u"x.locations.not-a-uuid: not a valid non-null uuid"_s);
    row("game location key nil uuid", u"game"_s,
        R"("locations":{"782de537-8f5d-4eda-b849-2362e28402e5")",
        R"("locations":{"00000000-0000-0000-0000-000000000000")",
        u"x.locations.00000000-0000-0000-0000-000000000000: not a valid "
        u"non-null uuid"_s);
    row("game location key does not match id", u"game"_s,
        R"("locations":{"782de537-8f5d-4eda-b849-2362e28402e5")",
        R"("locations":{"00000000-0000-0000-0000-000000000099")",
        uR"(x.locations.00000000-0000-0000-0000-000000000099: map key )"
        uR"("00000000-0000-0000-0000-000000000099" does not match the entry's )"
        uR"(id "782de537-8f5d-4eda-b849-2362e28402e5")"_s);
    row("game location key duplicate spelling", u"game"_s,
        R"(}},"investigators":{)",
        QByteArray(R"(},"782DE537-8F5D-4EDA-B849-2362E28402E5":)") + kLocation +
            R"(},"investigators":{)",
        uR"(x.locations.782DE537-8F5D-4EDA-B849-2362E28402E5: duplicate key "782de537-8f5d-4eda-b849-2362e28402e5")"_s);
    row("game location literal duplicate key", u"game"_s,
        R"(}},"investigators":{)",
        QByteArray(R"(},"782de537-8f5d-4eda-b849-2362e28402e5":)") + kLocation +
            R"(},"investigators":{)",
        u"duplicate object key"_s);
    row("game location entry error path", u"game"_s, R"("label":"study")",
        R"("label":7)",
        u"x.locations.782de537-8f5d-4eda-b849-2362e28402e5.label: expected string, got number"_s);
    row("game investigator key not a board card code", u"game"_s,
        R"("investigators":{"c01001":)", R"("investigators":{"01001":)",
        u"x.investigators.01001: card code must start with 'c'"_s);
    row("game investigator key grammar", u"game"_s,
        R"("investigators":{"c01001":)", R"("investigators":{"cBAD":)",
        u"x.investigators.cBAD: card code must match ^c[0-9a-z:._-]+$"_s);
    row("game investigator key does not match id", u"game"_s,
        R"("investigators":{"c01001":)", R"("investigators":{"c01002":)",
        uR"(x.investigators.c01002: map key "c01002" does not match the )"
        uR"(entry's id "c01001")"_s);
    row("game investigator entry error path", u"game"_s, R"("health":9)",
        R"("health":-9)",
        u"x.investigators.c01001.health: must not be negative, got -9"_s);
    row("game act key does not match id", u"game"_s, R"("acts":{"c01108":)",
        R"("acts":{"c01109":)",
        uR"(x.acts.c01109: map key "c01109" does not match the entry's id )"
        uR"("c01108")"_s);
    row("game act entry error path", u"game"_s, R"("sequence":[1,"A"])",
        R"("sequence":[1,"Z"])",
        uR"(x.acts.c01108.sequence[1]: unrecognized value "Z")"_s);
    row("game agenda key does not match id", u"game"_s,
        R"("agendas":{"c01105":)", R"("agendas":{"c01106":)",
        uR"(x.agendas.c01106: map key "c01106" does not match the entry's id )"
        uR"("c01105")"_s);
    row("game agenda entry error path", u"game"_s, R"("doom":0)",
        R"("doom":-1)",
        u"x.agendas.c01105.doom: must not be negative, got -1"_s);
    row("game missing agendas", u"game"_s, R"("agendas":)", R"("agendasX":)",
        uR"(x.agendas: missing required field "agendas")"_s);
    row("game missing tag", u"game"_s, R"("tag":"PublicGame",)", "",
        uR"(x.tag: missing required field "tag")"_s);
    row("game non-string tag", u"game"_s, R"("tag":"PublicGame")", R"("tag":1)",
        u"x.tag: expected string, got number"_s);
    row("game FailedToLoadGame tag", u"game"_s, R"("tag":"PublicGame")",
        R"("tag":"FailedToLoadGame")",
        uR"(x.tag: expected "PublicGame", got "FailedToLoadGame")"_s);
    row("game missing id", u"game"_s,
        R"("id":"00000000-0000-0000-0000-000000000003",)", "",
        uR"(x.id: missing required field "id")"_s);
    row("game missing name", u"game"_s, R"("name":"Contract fixture game",)",
        "", uR"(x.name: missing required field "name")"_s);
    row("game missing locations", u"game"_s, R"("locations":{)",
        R"("locationsX":{)",
        uR"(x.locations: missing required field "locations")"_s);
    row("game missing investigators", u"game"_s,
        R"("investigators":{"c01001":)", R"("investigatorsX":{"c01001":)",
        uR"(x.investigators: missing required field "investigators")"_s);
    row("game missing acts", u"game"_s, R"("acts":{)", R"("actsX":{)",
        uR"(x.acts: missing required field "acts")"_s);
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

  // act/agenda/investigator `id`, location/investigator `cardCode` and a
  // non-null chaosTokenRevealedBy follow the pinned board schemas'
  // `^c[0-9a-z:._-]+$` (act, agenda, location, investigator and chaos-token
  // schemas), not CardCode's broader catalog grammar.
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
        {"location", "cardCode", R"("cardCode":"c01111")"},
        {"investigator", "id", R"("id":"c01001")"},
        {"investigator", "cardCode", R"("cardCode":"c01001")"},
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
    } else if (type == "location"_L1) {
      const auto location = Location::fromRawBytes(json, u"x");
      QVERIFY2(location, qPrintable(location ? QString() : location.error()));
      decoded = location->cardCode().value();
    } else if (type == "investigator"_L1) {
      const auto investigator = Investigator::fromRawBytes(json, u"x");
      QVERIFY2(investigator,
               qPrintable(investigator ? QString() : investigator.error()));
      decoded = key == "id" ? investigator->id().value()
                            : investigator->cardCode().value();
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
