#include <QtTest>

#include "Board.h"
#include "CardArt.h"

using namespace Arkham;
using namespace Qt::StringLiterals;

namespace {

// Small valid board entities; each test row edits one field.
constexpr auto kInvestigator =
    R"({"id":"c01001","cardCode":"c01001",%1)"
    R"("playerId":"00000000-0000-0000-0000-000000000001",)"
    R"("name":{"title":"Roland Banks","subtitle":"The Fed"},)"
    R"("class":"Guardian","health":9,"sanity":5,"willpower":3,)"
    R"("intellect":3,"combat":4,"agility":2,"tokens":[],)"
    R"("remainingActions":3,"unhealedHorrorThisRound":0,)"
    R"("defeated":false,"resigned":false,"eliminated":false,)"
    R"("placement":{"tag":"Unplaced"},"connectedLocations":[]})";
constexpr auto kLocation =
    R"({"id":"782de537-8f5d-4eda-b849-2362e28402e5","cardCode":"c01111",)"
    R"("cardId":"4db426b4-641e-4812-8d10-27fc6470ef6e","label":"study",)"
    R"("revealed":%1,"tokens":[],"shroud":null,)"
    R"("revealClues":{"tag":"Static","contents":1},)"
    R"("symbol":"Circle","revealedSymbol":"Circle",)"
    R"("connectedLocations":[],"investigators":[]})";
constexpr auto kEnemyLocation =
    R"({"cardCode":"c10547","cardId":"00000000-0000-0000-0000-000000000033",)"
    R"("connectedLocations":[],"enemyLocation":true,"exhausted":false,)"
    R"("id":"00000000-0000-0000-0000-000000000032","investigators":[],)"
    R"("label":"shapeless_cellar","revealed":%1,)"
    R"("shroud":{"contents":4,"tag":"Static"},"tokens":[]})";
constexpr auto kAgenda =
    R"({"id":"%1","cardId":"d96ae83d-bf74-48e1-8233-78b565f30716",)"
    R"("sequence":{"agendaSequenceSide":"A","agendaSequenceStep":1},)"
    R"("deckId":1,"doom":0,"doomThreshold":null,"flipped":%2})";
constexpr auto kAct =
    R"({"id":"%1","cardId":"419015cc-abb7-41d0-9795-7ec1d981955f",)"
    R"("sequence":[1,"%2"],"deckId":1,"flipped":%3,"advanceCost":null})";

template <typename T> T decode(const QString &json) {
  auto result = T::fromRawBytes(json.toUtf8(), u"x");
  if (!result)
    qFatal("test entity must decode: %s", qPrintable(result.error()));
  return *result;
}

} // namespace

class CardArtTests : public QObject {
  Q_OBJECT

private slots:
  void investigator_data() {
    QTest::addColumn<QString>("fields");
    QTest::addColumn<QString>("imageId");
    QTest::newRow("plain art")
        << uR"("art":"c01001","mutated":null,)"_s << u"c01001"_s;
    QTest::newRow("art absent falls back to id") << QString() << u"c01001"_s;
    QTest::newRow("mutated") << uR"("art":"c01001","mutated":"Mutated21",)"_s
                             << u"c01001?mutation=Mutated21"_s;
    QTest::newRow("art differs from id")
        << uR"("art":"c98001","mutated":"Mutated3",)"_s
        << u"c98001?mutation=Mutated3"_s;
    QTest::newRow("homebrew art")
        << uR"("art":":fan:12345",)"_s << u":fan:12345"_s;
    QTest::newRow("malformed mutation shows plain art")
        << uR"("art":"c01001","mutated":"Mutated",)"_s << u"c01001"_s;
  }

  void investigator() {
    QFETCH(QString, fields);
    QFETCH(QString, imageId);
    const auto investigator =
        decode<Investigator>(QString::fromUtf8(kInvestigator).arg(fields));
    QCOMPARE(CardArt::investigatorImageId(investigator), imageId);
  }

  void location_data() {
    QTest::addColumn<bool>("enemy");
    QTest::addColumn<bool>("revealed");
    QTest::addColumn<QString>("imageId");
    QTest::newRow("revealed shows front") << false << true << u"c01111"_s;
    QTest::newRow("unrevealed shows back")
        << false << false << u"back/c01111"_s;
    QTest::newRow("revealed enemy-location") << true << true << u"c10547"_s;
    QTest::newRow("unrevealed enemy-location still shows front")
        << true << false << u"c10547"_s;
  }

  void location() {
    QFETCH(bool, enemy);
    QFETCH(bool, revealed);
    QFETCH(QString, imageId);
    const auto location =
        decode<Location>(QString::fromUtf8(enemy ? kEnemyLocation : kLocation)
                             .arg(revealed ? "true"_L1 : "false"_L1));
    QCOMPARE(CardArt::locationImageId(location), imageId);
  }

  void agenda_data() {
    QTest::addColumn<QString>("id");
    QTest::addColumn<bool>("flipped");
    QTest::addColumn<QString>("imageId");
    QTest::newRow("front") << u"c01105"_s << false << u"c01105"_s;
    QTest::newRow("flipped") << u"c01105"_s << true << u"back/c01105"_s;
    QTest::newRow("front with trailing a")
        << u"c02119a"_s << false << u"c02119a"_s;
    QTest::newRow("flipped drops trailing a")
        << u"c02119a"_s << true << u"back/c02119"_s;
    QTest::newRow("c03276a keeps its a")
        << u"c03276a"_s << true << u"back/c03276a"_s;
    QTest::newRow("c03279a keeps its a")
        << u"c03279a"_s << true << u"back/c03279a"_s;
  }

  void agenda() {
    QFETCH(QString, id);
    QFETCH(bool, flipped);
    QFETCH(QString, imageId);
    const auto agenda = decode<Agenda>(
        QString::fromUtf8(kAgenda).arg(id, flipped ? "true"_L1 : "false"_L1));
    QCOMPARE(CardArt::agendaImageId(agenda), imageId);
  }

  // Acts show their front only, whatever their side or flip state.
  void act_data() {
    QTest::addColumn<QString>("side");
    QTest::addColumn<bool>("flipped");
    QTest::newRow("A") << u"A"_s << false;
    QTest::newRow("B flipped") << u"B"_s << true;
  }

  void act() {
    QFETCH(QString, side);
    QFETCH(bool, flipped);
    const auto act = decode<Act>(QString::fromUtf8(kAct).arg(
        u"c01108"_s, side, flipped ? "true"_L1 : "false"_L1));
    QCOMPARE(CardArt::actImageId(act), u"c01108"_s);
  }

  void imageSource() {
    QCOMPARE(CardArt::imageSource(u"c01001?mutation=Mutated21"_s),
             u"image://arkham-card/c01001?mutation=Mutated21"_s);
    QCOMPARE(CardArt::imageSource(u"back/c01111"_s),
             u"image://arkham-card/back/c01111"_s);
  }
};

QTEST_GUILESS_MAIN(CardArtTests)

#include "CardArtTests.moc"
