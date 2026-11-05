#include <QtTest>

#include "AssetLocator.h"
#include "Board.h"
#include "CardArt.h"

using namespace Arkham;
using namespace Qt::StringLiterals;

namespace {

// Small valid board entities; each test row edits one field.
constexpr auto kInvestigator =
    R"({"id":"%1","cardCode":"%1",%2)"
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
    QTest::addColumn<QString>("cardCode");
    QTest::addColumn<QString>("fields");
    QTest::addColumn<QString>("imageId");
    const auto roland = u"c01001"_s;
    const auto lola = u"c03006"_s;
    QTest::newRow("plain art")
        << roland << uR"("art":"c01001","mutated":null,)"_s << u"c01001"_s;
    QTest::newRow("art absent falls back to id")
        << roland << QString() << u"c01001"_s;
    QTest::newRow("mutated")
        << roland << uR"("art":"c01001","mutated":"Mutated21",)"_s
        << u"c01001?mutation=Mutated21"_s;
    QTest::newRow("art differs from id")
        << roland << uR"("art":"c98001","mutated":"Mutated3",)"_s
        << u"c98001?mutation=Mutated3"_s;
    QTest::newRow("homebrew art")
        << roland << uR"("art":":fan:12345",)"_s << u":fan:12345"_s;
    QTest::newRow("malformed mutation shows plain art")
        << roland << uR"("art":"c01001","mutated":"Mutated",)"_s << u"c01001"_s;

    // Art that is not a valid asset id would change the provider id's
    // path, query or fragment, so the id is shown instead.
    QTest::newRow("art with '/' falls back to id")
        << roland << uR"("art":"c01001/../c02002",)"_s << u"c01001"_s;
    QTest::newRow("art with '?' falls back to id")
        << roland << uR"("art":"c02002?face=back",)"_s << u"c01001"_s;
    QTest::newRow("art with '#' falls back to id")
        << roland << uR"("art":"c02002#x",)"_s << u"c01001"_s;
    QTest::newRow("art with '%' falls back to id")
        << roland << uR"("art":"c02002%2Fx",)"_s << u"c01001"_s;
    QTest::newRow("invalid art keeps the mutation")
        << roland << uR"("art":"c02002/x","mutated":"Mutated21",)"_s
        << u"c01001?mutation=Mutated21"_s;

    // The form override comes first and ignores the mutation and variant.
    QTest::newRow("regular form")
        << roland << uR"("form":{"tag":"RegularForm"},)"_s << u"c01001"_s;
    QTest::newRow("Yithian form")
        << roland << uR"("form":{"tag":"YithianForm"},"mutated":"Mutated2",)"_s
        << u"c04244"_s;
    QTest::newRow("Homunculus form")
        << roland << uR"("form":{"tag":"HomunculusForm"},)"_s << u"c11068b"_s;
    QTest::newRow("Shattered form")
        << roland << uR"("form":{"tag":"ShatteredForm"},)"_s << u"c10661"_s;
    QTest::newRow("Transfigured form")
        << roland
        << uR"("form":{"tag":"TransfiguredForm","contents":"c10016a"},)"
           R"("mutated":"Mutated2",)"_s
        << u"c10016a"_s;
    QTest::newRow("Transfigured into an invalid asset id shows the art")
        << roland
        << uR"("form":{"tag":"TransfiguredForm","contents":"c10016.a"},)"_s
        << u"c01001"_s;
    QTest::newRow("unknown form shows the art")
        << roland << uR"("form":{"tag":"MoonForm"},)"_s << u"c01001"_s;
    QTest::newRow("form beats the class variant")
        << lola << uR"("form":{"tag":"ShatteredForm"},"meta":"Seeker",)"_s
        << u"c10661"_s;

    // Lola Hayes' role art, keyed on cardCode.
    QTest::newRow("c03006 Seeker")
        << lola << uR"("art":"c03006","meta":"Seeker",)"_s
        << u"c03006?variant=Seeker"_s;
    QTest::newRow("c90087 Mystic, mutated")
        << u"c90087"_s << uR"("meta":"Mystic","mutated":"Mutated20",)"_s
        << u"c90087?variant=Mystic&mutation=Mutated20"_s;
    QTest::newRow("c03006 Neutral has no variant")
        << lola << uR"("meta":"Neutral",)"_s << u"c03006"_s;
    QTest::newRow("c03006 Mythos has no variant")
        << lola << uR"("meta":"Mythos",)"_s << u"c03006"_s;
    QTest::newRow("c03006 without meta has no variant")
        << lola << uR"("meta":null,)"_s << u"c03006"_s;
    QTest::newRow("other investigators ignore meta")
        << roland << uR"("meta":"Seeker",)"_s << u"c01001"_s;
  }

  void investigator() {
    QFETCH(QString, cardCode);
    QFETCH(QString, fields);
    QFETCH(QString, imageId);
    const auto investigator = decode<Investigator>(
        QString::fromUtf8(kInvestigator).arg(cardCode, fields));
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

  // Act.vue's sequence-side art; the act's flip state does not matter.
  void act_data() {
    QTest::addColumn<QString>("id");
    QTest::addColumn<QString>("side");
    QTest::addColumn<bool>("flipped");
    QTest::addColumn<QString>("imageId");
    const auto row = [](const char *name, const QString &id,
                        const QString &side, const QString &imageId,
                        bool flipped = false) {
      QTest::newRow(name) << id << side << flipped << imageId;
    };
    row("A", u"c01108"_s, u"A"_s, u"c01108"_s);
    row("A flipped", u"c01108"_s, u"A"_s, u"c01108"_s, true);
    row("B", u"c01108"_s, u"B"_s, u"c01108b"_s);
    row("B flipped", u"c01108"_s, u"B"_s, u"c01108b"_s, true);
    row("id already ends with the side", u"c50000c"_s, u"C"_s, u"c50000c"_s);
    row("c10607a on side A", u"c10607a"_s, u"A"_s, u"c10607a"_s);
    row("c10607a on side B", u"c10607a"_s, u"B"_s, u"c10607b"_s);
    row("D strips a trailing c", u"c50000c"_s, u"D"_s, u"c50000d"_s);
    row("D keeps another letter", u"c50000a"_s, u"D"_s, u"c50000ad"_s);
    row("F strips a trailing e", u"c50000e"_s, u"F"_s, u"c50000f"_s);
    row("H strips a trailing g", u"c50000g"_s, u"H"_s, u"c50000h"_s);
    row("homebrew B", u"c:fan:123"_s, u"B"_s, u"c:fan:123b"_s);
    // Threads of Fate (4117-4140): C and E show the front, D and F the back.
    row("ToF B", u"c04117"_s, u"B"_s, u"c04117b"_s);
    row("ToF C", u"c04117"_s, u"C"_s, u"c04117"_s);
    row("ToF D", u"c04118"_s, u"D"_s, u"c04118b"_s);
    row("ToF D strips c first", u"c04122c"_s, u"D"_s, u"c04122b"_s);
    row("ToF E", u"c04133"_s, u"E"_s, u"c04133"_s);
    row("ToF F", u"c04140"_s, u"F"_s, u"c04140b"_s);
    row("below ToF", u"c04116"_s, u"D"_s, u"c04116d"_s);
    row("above ToF", u"c04141"_s, u"D"_s, u"c04141d"_s);
    // Return to Threads of Fate (53029-53036): G front, H back.
    row("RtToF G", u"c53029"_s, u"G"_s, u"c53029"_s);
    row("RtToF H", u"c53036"_s, u"H"_s, u"c53036b"_s);
    row("RtToF B", u"c53030"_s, u"B"_s, u"c53030b"_s);
    row("below RtToF", u"c53028"_s, u"H"_s, u"c53028h"_s);
    row("above RtToF", u"c53037"_s, u"H"_s, u"c53037h"_s);
  }

  void act() {
    QFETCH(QString, id);
    QFETCH(QString, side);
    QFETCH(bool, flipped);
    QFETCH(QString, imageId);
    const auto act = decode<Act>(QString::fromUtf8(kAct).arg(
        id, side, flipped ? "true"_L1 : "false"_L1));
    QCOMPARE(CardArt::actImageId(act), imageId);
  }

  void imageSource() {
    QCOMPARE(CardArt::imageSource(u"c01001?mutation=Mutated21"_s),
             u"image://arkham-card/c01001?mutation=Mutated21"_s);
    QCOMPARE(CardArt::imageSource(u"back/c01111"_s),
             u"image://arkham-card/back/c01111"_s);
  }

  // artVariants.ts's reprintedArt: Revised Core 01604-01682 reads the
  // original's image, 500 lower, keeping any lowercase face suffix.
  void reprintedArt_data() {
    QTest::addColumn<QString>("art");
    QTest::addColumn<QString>("expected");
    const auto row = [](const char *name, const QString &art,
                        const QString &expected) {
      QTest::newRow(name) << art << expected;
    };
    row("first reprint", u"01604"_s, u"01104"_s);
    row("last reprint", u"01682"_s, u"01182"_s);
    row("back of a reprint", u"01615b"_s, u"01115b"_s);
    row("lettered reprint", u"01645ab"_s, u"01145ab"_s);
    row("below the range", u"01603"_s, u"01603"_s);
    row("above the range", u"01683"_s, u"01683"_s);
    row("original card", u"01115"_s, u"01115"_s);
    row("other cycle", u"02604"_s, u"02604"_s);
    row("unpadded number", u"1650"_s, u"01150"_s);
    row("suffixed art", u"01650_Mutated1"_s, u"01650_Mutated1"_s);
    row("leading letter", u"x01650"_s, u"x01650"_s);
    row("legacy x art", u"xpolyp"_s, u"xpolyp"_s);
    row("empty", QString(), QString());
  }

  void reprintedArt() {
    QFETCH(QString, art);
    QFETCH(QString, expected);
    QCOMPARE(CardArt::reprintedArt(art), expected);
  }

  // helpers.ts's cardImgPath: the Ages Unwound past-selves acts 164-167 use
  // C/D in game but front/"b" file names.
  void homebrewArt_data() {
    QTest::addColumn<QString>("campaign");
    QTest::addColumn<QString>("art");
    QTest::addColumn<QString>("expected");
    const auto row = [](const char *name, const QString &campaign,
                        const QString &art, const QString &expected) {
      QTest::newRow(name) << campaign << art << expected;
    };
    const auto ages = u"ages-unwound"_s;
    row("164c", ages, u"164c"_s, u"164"_s);
    row("164d", ages, u"164d"_s, u"164b"_s);
    row("167c", ages, u"167c"_s, u"167"_s);
    row("167d", ages, u"167d"_s, u"167b"_s);
    row("163c", ages, u"163c"_s, u"163c"_s);
    row("168d", ages, u"168d"_s, u"168d"_s);
    row("164a", ages, u"164a"_s, u"164a"_s);
    row("164b", ages, u"164b"_s, u"164b"_s);
    row("164 front", ages, u"164"_s, u"164"_s);
    row("back of 164c", ages, u"164cb"_s, u"164cb"_s);
    row("other homebrew", u"dark-matter"_s, u"164c"_s, u"164c"_s);
    row("named card", ages, u"xsoldier"_s, u"xsoldier"_s);
    row("homebrew is not reprinted", u"fan"_s, u"01650"_s, u"01650"_s);
  }

  void homebrewArt() {
    QFETCH(QString, campaign);
    QFETCH(QString, art);
    QFETCH(QString, expected);
    QCOMPARE(CardArt::homebrewArt(campaign, art), expected);
  }

  // The rules reach the asset URL the image provider fetches, as on the web.
  void assetUrl_data() {
    QTest::addColumn<QString>("cardCode");
    QTest::addColumn<int>("face");
    QTest::addColumn<QString>("mutation");
    QTest::addColumn<QString>("path");
    const auto row = [](const char *name, const QString &cardCode,
                        AssetLocator::CardFace face, const QString &mutation,
                        const QString &path) {
      QTest::newRow(name) << cardCode << static_cast<int>(face) << mutation
                          << path;
    };
    const auto front = AssetLocator::CardFace::Front;
    const auto back = AssetLocator::CardFace::Back;
    row("reprint", u"c01604"_s, front, QString(),
        u"/img/arkham/cards/01104.avif"_s);
    row("reprint back", u"c01682"_s, back, QString(),
        u"/img/arkham/cards/01182b.avif"_s);
    row("before the reprints", u"c01603"_s, front, QString(),
        u"/img/arkham/cards/01603.avif"_s);
    row("after the reprints", u"c01683"_s, back, QString(),
        u"/img/arkham/cards/01683b.avif"_s);
    row("mutated reprint number", u"c01650"_s, front, u"_Mutated1"_s,
        u"/img/arkham/cards/01650_Mutated1.avif"_s);
    row("Ages Unwound C", u"c:ages-unwound:165c"_s, front, QString(),
        u"/img/arkham/homebrew/ages-unwound/cards/165.avif"_s);
    row("Ages Unwound D", u"c:ages-unwound:166d"_s, front, QString(),
        u"/img/arkham/homebrew/ages-unwound/cards/166b.avif"_s);
    row("Ages Unwound other act", u"c:ages-unwound:168c"_s, front, QString(),
        u"/img/arkham/homebrew/ages-unwound/cards/168c.avif"_s);
    row("homebrew reprint number", u"c:fan:01650"_s, front, QString(),
        u"/img/arkham/homebrew/fan/cards/01650.avif"_s);
  }

  void assetUrl() {
    QFETCH(QString, cardCode);
    QFETCH(int, face);
    QFETCH(QString, mutation);
    QFETCH(QString, path);
    const auto url = AssetLocator::buildCardImageUrl(
        AssetLocator::defaultAssetBaseUrl(),
        {cardCode, static_cast<AssetLocator::CardFace>(face), mutation});
    QVERIFY(url.has_value());
    QCOMPARE(url->toString(), u"https://assets.arkhamhorror.app"_s + path);
  }

  // Act.vue's side C/D ids for the past-selves acts resolve to the files
  // cardImgPath names.
  void agesUnwoundActUrl_data() {
    QTest::addColumn<QString>("id");
    QTest::addColumn<QString>("side");
    QTest::addColumn<QString>("path");
    QTest::newRow("C") << u"c:ages-unwound:164c"_s << u"C"_s
                       << u"/img/arkham/homebrew/ages-unwound/cards/164.avif"_s;
    QTest::newRow("D")
        << u"c:ages-unwound:164c"_s << u"D"_s
        << u"/img/arkham/homebrew/ages-unwound/cards/164b.avif"_s;
  }

  void agesUnwoundActUrl() {
    QFETCH(QString, id);
    QFETCH(QString, side);
    QFETCH(QString, path);
    const auto act =
        decode<Act>(QString::fromUtf8(kAct).arg(id, side, "false"_L1));
    const auto url = AssetLocator::buildCardImageUrl(
        AssetLocator::defaultAssetBaseUrl(), {CardArt::actImageId(act)});
    QVERIFY(url.has_value());
    QCOMPARE(url->toString(), u"https://assets.arkhamhorror.app"_s + path);
  }
};

QTEST_GUILESS_MAIN(CardArtTests)

#include "CardArtTests.moc"
