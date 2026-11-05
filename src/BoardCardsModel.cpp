#include "BoardCardsModel.h"

#include "CardArt.h"

using namespace Qt::StringLiterals;

namespace Arkham {

BoardCardsModel::BoardCardsModel(QObject *parent)
    : QAbstractListModel(parent) {}

void BoardCardsModel::setGame(const PublicGame &game) {
  QList<Row> rows;
  for (const Agenda &agenda : game.agendas())
    rows.append({u"agenda"_s, agenda.id().value(),
                 CardArt::imageSource(CardArt::agendaImageId(agenda))});
  for (const Act &act : game.acts())
    rows.append({u"act"_s, act.id().value(),
                 CardArt::imageSource(CardArt::actImageId(act))});
  for (const Location &location : game.locations())
    rows.append({u"location"_s, location.label(),
                 CardArt::imageSource(CardArt::locationImageId(location))});
  for (const Investigator &investigator : game.investigators())
    rows.append(
        {u"investigator"_s, investigator.name().title,
         CardArt::imageSource(CardArt::investigatorImageId(investigator))});

  beginResetModel();
  m_rows = std::move(rows);
  endResetModel();
}

int BoardCardsModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant BoardCardsModel::data(const QModelIndex &index, int role) const {
  if (!checkIndex(index, CheckIndexOption::IndexIsValid |
                             CheckIndexOption::ParentIsInvalid))
    return {};
  const Row &row = m_rows.at(index.row());
  switch (role) {
  case KindRole:
    return row.kind;
  case Qt::DisplayRole:
  case LabelRole:
    return row.label;
  case ImageSourceRole:
    return row.imageSource;
  default:
    return {};
  }
}

QHash<int, QByteArray> BoardCardsModel::roleNames() const {
  return {
      {KindRole, "kind"_ba},
      {LabelRole, "label"_ba},
      {ImageSourceRole, "imageSource"_ba},
  };
}

} // namespace Arkham
