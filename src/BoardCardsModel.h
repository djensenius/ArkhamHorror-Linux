#pragma once

#include "Board.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

namespace Arkham {

// A flat list model of the card art on a decoded PublicGame snapshot, for
// QML (see qml/BoardCardsView.qml). Rows are in board order: agendas, acts,
// locations, then investigators, each in wire order. Every row's
// imageSource comes from CardArt (an `image://arkham-card/...` source).
class BoardCardsModel final : public QAbstractListModel {
  Q_OBJECT

public:
  enum Role {
    KindRole = Qt::UserRole + 1,
    LabelRole,
    ImageSourceRole,
  };
  Q_ENUM(Role)

  explicit BoardCardsModel(QObject *parent = nullptr);

  // Replaces every row with the cards of `game`.
  void setGame(const PublicGame &game);
  // Removes every row.
  void clear();

  [[nodiscard]] int
  rowCount(const QModelIndex &parent = QModelIndex()) const override;
  [[nodiscard]] QVariant data(const QModelIndex &index,
                              int role = Qt::DisplayRole) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

private:
  struct Row {
    // "agenda", "act", "location" or "investigator".
    QString kind;
    QString label;
    QString imageSource;
  };

  QList<Row> m_rows;
};

} // namespace Arkham
