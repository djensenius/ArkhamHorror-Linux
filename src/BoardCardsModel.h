#pragma once

#include "Board.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <optional>

namespace Arkham {

// A flat list model of the card art on a decoded PublicGame snapshot, for
// QML (see qml/BoardCardsView.qml). Rows are in board order: agendas, acts,
// locations, then investigators, each in wire order. Every row's
// imageSource comes from CardArt (an `image://arkham-card/...` source).
//
// Agenda, act and location labels are catalog card names (see
// setCardNames()); a card the catalog has no name for is labelled with its
// card code. Investigator labels are the snapshot's own name.
class BoardCardsModel final : public QAbstractListModel {
  Q_OBJECT

public:
  enum Role {
    KindRole = Qt::UserRole + 1,
    LabelRole,
    ImageSourceRole,
  };
  Q_ENUM(Role)

  // One catalog card's printed names (CardDef.name and revealedName).
  struct CardNames {
    CardName name;
    std::optional<CardName> revealedName;
  };
  // Catalog names keyed by card code, e.g. "c01111".
  using CardNameTable = QHash<QString, CardNames>;

  explicit BoardCardsModel(QObject *parent = nullptr);

  // The names of every card in |cards| (a GET /arkham/cards response).
  [[nodiscard]] static CardNameTable
  cardNamesFromCatalog(const QList<CardDef> &cards);

  // Replaces every row with the cards of `game`.
  void setGame(const PublicGame &game);
  // Removes every row (the card names are kept).
  void clear();
  // Relabels the agenda, act and location rows from |names|, now and for
  // every later setGame(): agendas and acts by their id, locations by
  // their card code (the revealed name once revealed, if the card has
  // one). A card missing from |names| is labelled with its code.
  void setCardNames(CardNameTable names);

  [[nodiscard]] int
  rowCount(const QModelIndex &parent = QModelIndex()) const override;
  [[nodiscard]] QVariant data(const QModelIndex &index,
                              int role = Qt::DisplayRole) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

private:
  struct Row {
    // "agenda", "act", "location" or "investigator".
    QString kind;
    // The card code catalog names are looked up by; empty for
    // investigators, whose label is fixed.
    QString code;
    bool revealed{false};
    QString label;
    QString imageSource;
  };

  [[nodiscard]] QString labelFor(const Row &row) const;

  QList<Row> m_rows;
  CardNameTable m_names;
};

} // namespace Arkham
