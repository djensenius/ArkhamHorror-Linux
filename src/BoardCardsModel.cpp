#include "BoardCardsModel.h"

#include "CardArt.h"

using namespace Qt::StringLiterals;

namespace Arkham {

BoardCardsModel::BoardCardsModel(QObject *parent)
    : QAbstractListModel(parent) {}

BoardCardsModel::CardNameTable
BoardCardsModel::cardNamesFromCatalog(const CatalogNames &catalog) {
  CardNameTable names;
  names.reserve(catalog.cards.size());
  for (const CatalogCardNames &card : catalog.cards)
    names.insert(card.cardCode.value(), {card.name, card.revealedName});
  return names;
}

void BoardCardsModel::setGame(const PublicGame &game) {
  QList<Row> rows;
  for (const Agenda &agenda : game.agendas())
    rows.append({u"agenda"_s, agenda.id().value(), false, QString(),
                 CardArt::imageSource(CardArt::agendaImageId(agenda))});
  for (const Act &act : game.acts())
    rows.append({u"act"_s, act.id().value(), false, QString(),
                 CardArt::imageSource(CardArt::actImageId(act))});
  for (const Location &location : game.locations())
    rows.append({u"location"_s, location.cardCode().value(),
                 location.revealed(), QString(),
                 CardArt::imageSource(CardArt::locationImageId(location))});
  for (const Investigator &investigator : game.investigators())
    rows.append(
        {u"investigator"_s, QString(), false, investigator.name().title,
         CardArt::imageSource(CardArt::investigatorImageId(investigator))});
  for (Row &row : rows) {
    if (!row.code.isEmpty())
      row.label = labelFor(row);
  }

  beginResetModel();
  m_rows = std::move(rows);
  endResetModel();
}

void BoardCardsModel::setCardNames(CardNameTable names) {
  m_names = std::move(names);
  for (qsizetype i = 0; i < m_rows.size(); ++i) {
    Row &row = m_rows[i];
    if (row.code.isEmpty())
      continue;
    QString label = labelFor(row);
    if (label == row.label)
      continue;
    row.label = std::move(label);
    const QModelIndex at = index(static_cast<int>(i));
    emit dataChanged(at, at, {Qt::DisplayRole, LabelRole});
  }
}

QString BoardCardsModel::labelFor(const Row &row) const {
  const auto it = m_names.constFind(row.code);
  if (it == m_names.cend())
    return row.code;
  const CardName &name =
      row.revealed && it->revealedName ? *it->revealedName : it->name;
  return name.title.isEmpty() ? row.code : name.title;
}

void BoardCardsModel::clear() {
  if (m_rows.isEmpty())
    return;
  beginResetModel();
  m_rows.clear();
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
