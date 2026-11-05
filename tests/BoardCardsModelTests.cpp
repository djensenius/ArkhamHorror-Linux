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
        {u"location"_s, u"study"_s, u"image://arkham-card/c01111"_s},
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
        {u"location"_s, u"study"_s, u"image://arkham-card/back/c01111"_s},
        {u"investigator"_s, u"Roland Banks"_s,
         u"image://arkham-card/c01001?mutation=Mutated21"_s},
    };
    QCOMPARE(rowsOf(model), expected);
    QCOMPARE(model.data(model.index(3), Qt::DisplayRole).toString(),
             u"Roland Banks"_s);
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
