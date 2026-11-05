#include <QFile>
#include <QSignalSpy>
#include <QtTest>

#include "Board.h"
#include "BoardCardsModel.h"

using namespace Arkham;
using namespace Qt::StringLiterals;

namespace {

// get-game.json's raw bytes, with each `edits` pair's first occurrence
// replaced. Fails via qFatal if the fixture or an edit target is missing,
// so a stale edit can never silently test the unedited fixture.
QByteArray getGameBytes(
    std::initializer_list<std::pair<QByteArray, QByteArray>> edits = {}) {
  QFile f(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) +
          u"/fixtures/get-game.json");
  if (!f.open(QIODevice::ReadOnly))
    qFatal("missing fixture: get-game.json");
  QByteArray bytes = f.readAll();
  for (const auto &[from, to] : edits) {
    const qsizetype at = bytes.indexOf(from);
    if (at < 0)
      qFatal("fixture edit target not found: %s", from.constData());
    bytes.replace(at, from.size(), to);
  }
  return bytes;
}

PublicGame decodeGame(const QByteArray &bytes) {
  auto envelope = Json::Value::parse(bytes, u"get-game.json");
  if (!envelope)
    qFatal("fixture must parse: %s", qPrintable(envelope.error()));
  auto game = PublicGame::fromRawJson(envelope->value("game"_L1), u"game");
  if (!game)
    qFatal("fixture must decode: %s", qPrintable(game.error()));
  return *game;
}

BoardCardsModel::CardNames names(const QString &title,
                                 std::optional<QString> revealed = {}) {
  BoardCardsModel::CardNames result{CardName{title, std::nullopt},
                                    std::nullopt};
  if (revealed)
    result.revealedName = CardName{*revealed, std::nullopt};
  return result;
}

// Agenda, act and location names (the location with a distinct revealed
// name), keyed as GET /arkham/cards keys them.
BoardCardsModel::CardNameTable coreNames() {
  return {
      {u"c01105"_s, names(u"What's Going On?!"_s)},
      {u"c01108"_s, names(u"Trapped"_s)},
      {u"c01111"_s, names(u"Study (unrevealed)"_s, u"Study"_s)},
  };
}

QStringList labelsOf(const BoardCardsModel &model) {
  QStringList labels;
  for (int row = 0; row < model.rowCount(); ++row)
    labels.append(
        model.data(model.index(row), BoardCardsModel::LabelRole).toString());
  return labels;
}

struct RowValues {
  QString kind;
  QString label;
  QString imageSource;
  friend bool operator==(const RowValues &, const RowValues &) = default;
};

QList<RowValues> rowsOf(const BoardCardsModel &model) {
  QList<RowValues> rows;
  for (int row = 0; row < model.rowCount(); ++row) {
    const QModelIndex index = model.index(row);
    rows.append(
        {model.data(index, BoardCardsModel::KindRole).toString(),
         model.data(index, BoardCardsModel::LabelRole).toString(),
         model.data(index, BoardCardsModel::ImageSourceRole).toString()});
  }
  return rows;
}

} // namespace

namespace QTest {
template <> char *toString(const RowValues &row) {
  return qstrdup(
      qPrintable(row.kind + u" | " + row.label + u" | " + row.imageSource));
}
} // namespace QTest

class BoardCardsModelTests : public QObject {
  Q_OBJECT

private slots:
  void roleNames() {
    const BoardCardsModel model;
    const auto names = model.roleNames();
    QCOMPARE(names.size(), 3);
    QCOMPARE(names.value(BoardCardsModel::KindRole), "kind"_ba);
    QCOMPARE(names.value(BoardCardsModel::LabelRole), "label"_ba);
    QCOMPARE(names.value(BoardCardsModel::ImageSourceRole), "imageSource"_ba);
  }

  void emptyUntilAGameIsSet() {
    const BoardCardsModel model;
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(!model.data(model.index(0), BoardCardsModel::KindRole).isValid());
  }

  // The vendored snapshot as published: plain art, revealed Study.
  void fixtureSnapshot() {
    BoardCardsModel model;
    model.setGame(decodeGame(getGameBytes()));
    const QList<RowValues> expected{
        {u"agenda"_s, u"c01105"_s, u"image://arkham-card/c01105"_s},
        {u"act"_s, u"c01108"_s, u"image://arkham-card/c01108"_s},
        {u"location"_s, u"c01111"_s, u"image://arkham-card/c01111"_s},
        {u"investigator"_s, u"Roland Banks"_s, u"image://arkham-card/c01001"_s},
    };
    QCOMPARE(rowsOf(model), expected);
  }

  // A mutated investigator, an unrevealed location and a flipped agenda.
  void mutatedArtUnrevealedLocationAndFlippedAgenda() {
    const PublicGame game = decodeGame(getGameBytes({
        {R"("mutated": null)", R"("mutated": "Mutated21")"},
        {R"("revealed": true)", R"("revealed": false)"},
        {R"("flipped": false,
        "id": "c01105")",
         R"("flipped": true,
        "id": "c01105")"},
    }));
    QCOMPARE(game.investigators().first().mutated(),
             std::optional(u"Mutated21"_s));

    BoardCardsModel model;
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    model.setGame(game);
    QCOMPARE(reset.count(), 1);
    const QList<RowValues> expected{
        {u"agenda"_s, u"c01105"_s, u"image://arkham-card/back/c01105"_s},
        {u"act"_s, u"c01108"_s, u"image://arkham-card/c01108"_s},
        {u"location"_s, u"c01111"_s, u"image://arkham-card/back/c01111"_s},
        {u"investigator"_s, u"Roland Banks"_s,
         u"image://arkham-card/c01001?mutation=Mutated21"_s},
    };
    QCOMPARE(rowsOf(model), expected);
    QCOMPARE(model.data(model.index(3), Qt::DisplayRole).toString(),
             u"Roland Banks"_s);
  }

  // Catalog names label agendas, acts and locations; a revealed location
  // uses its revealed name; investigators keep their own name.
  void cardNamesLabelBoardCards() {
    BoardCardsModel model;
    model.setCardNames(coreNames());
    model.setGame(decodeGame(getGameBytes()));
    QCOMPARE(labelsOf(model), QStringList({u"What's Going On?!"_s, u"Trapped"_s,
                                           u"Study"_s, u"Roland Banks"_s}));
    QCOMPARE(model.data(model.index(2), Qt::DisplayRole).toString(),
             u"Study"_s);
  }

  void unrevealedLocationUsesItsName() {
    BoardCardsModel model;
    model.setCardNames(coreNames());
    model.setGame(decodeGame(
        getGameBytes({{R"("revealed": true)", R"("revealed": false)"}})));
    QCOMPARE(labelsOf(model).at(2), u"Study (unrevealed)"_s);
  }

  // A revealed location without a revealed name keeps its name.
  void revealedLocationWithoutRevealedNameUsesItsName() {
    BoardCardsModel model;
    model.setCardNames({{u"c01111"_s, names(u"Study"_s)}});
    model.setGame(decodeGame(getGameBytes()));
    QCOMPARE(labelsOf(model).at(2), u"Study"_s);
  }

  // A card the catalog has no entry (or an empty title) for keeps its code.
  void missingCatalogEntryFallsBackToTheCode() {
    BoardCardsModel model;
    model.setCardNames({{u"c01108"_s, names(u"Trapped"_s)},
                        {u"c01111"_s, names(QString())},
                        {u"c01001"_s, names(u"Not Roland"_s)}});
    model.setGame(decodeGame(getGameBytes()));
    QCOMPARE(labelsOf(model), QStringList({u"c01105"_s, u"Trapped"_s,
                                           u"c01111"_s, u"Roland Banks"_s}));
  }

  // Names that arrive after the board relabel it in place (no reset), and
  // clearing them restores the codes.
  void lateCardNamesRelabelInPlace() {
    BoardCardsModel model;
    model.setGame(decodeGame(getGameBytes()));
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.setCardNames(coreNames());
    QCOMPARE(reset.count(), 0);
    QCOMPARE(changed.count(), 3);
    const auto roles = changed.first().at(2).value<QList<int>>();
    QVERIFY(roles.contains(BoardCardsModel::LabelRole));
    QVERIFY(roles.contains(Qt::DisplayRole));
    QCOMPARE(labelsOf(model), QStringList({u"What's Going On?!"_s, u"Trapped"_s,
                                           u"Study"_s, u"Roland Banks"_s}));

    // Names survive clear() and apply to the next game.
    model.clear();
    model.setGame(decodeGame(getGameBytes()));
    QCOMPARE(labelsOf(model).at(1), u"Trapped"_s);

    model.setCardNames({});
    QCOMPARE(labelsOf(model), QStringList({u"c01105"_s, u"c01108"_s,
                                           u"c01111"_s, u"Roland Banks"_s}));
  }

  // The vendored catalog fixture's card, keyed by its code.
  void cardNamesFromCatalogFixture() {
    QFile f(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) +
            u"/fixtures/catalog.json");
    QVERIFY(f.open(QIODevice::ReadOnly));
    auto bundle = Json::Value::parse(f.readAll(), u"catalog.json");
    QVERIFY2(bundle.has_value(), qPrintable(bundle.error()));
    const auto cardsBytes = bundle->value("cards"_L1).toJsonBytes();
    QVERIFY2(cardsBytes.has_value(), qPrintable(cardsBytes.error()));
    auto catalog = decodeCatalogNamesFromRawBytes(*cardsBytes, u"cards");
    QVERIFY2(catalog.has_value(), qPrintable(catalog.error()));
    QCOMPARE(catalog->skippedCount, 0);
    const auto table = BoardCardsModel::cardNamesFromCatalog(*catalog);
    QCOMPARE(table.size(), 1);
    QCOMPARE(table.value(u"c01020"_s).name.title, u"Machete"_s);
    QVERIFY(!table.value(u"c01020"_s).revealedName.has_value());
  }

  // A malformed mutation is dropped, never failing the snapshot.
  void malformedMutationShowsPlainArt() {
    BoardCardsModel model;
    model.setGame(decodeGame(
        getGameBytes({{R"("mutated": null)", R"("mutated": "Mutated-1")"}})));
    QCOMPARE(
        model.data(model.index(3), BoardCardsModel::ImageSourceRole).toString(),
        u"image://arkham-card/c01001"_s);
  }
};

QTEST_GUILESS_MAIN(BoardCardsModelTests)

#include "BoardCardsModelTests.moc"
