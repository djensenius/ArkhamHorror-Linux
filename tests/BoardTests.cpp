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

  void validDocumentsDecode_data() {
    QTest::addColumn<QString>("type");
    for (const auto *type : {"token", "bag", "step", "act", "agenda", "phase"})
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
    for (const auto *type : {"token", "bag", "step", "act", "agenda"})
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
};

QTEST_APPLESS_MAIN(BoardTests)

#include "BoardTests.moc"
