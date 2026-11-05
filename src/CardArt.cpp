#include "CardArt.h"

using namespace Qt::StringLiterals;

namespace Arkham::CardArt {

QString investigatorImageId(const Investigator &investigator) {
  if (!investigator.mutated())
    return investigator.art();
  return investigator.art() + u"?mutation="_s + *investigator.mutated();
}

QString locationImageId(const Location &location) {
  if (location.kind() == Location::Kind::EnemyLocation || location.revealed())
    return location.cardCode().value();
  return u"back/"_s + location.cardCode().value();
}

QString agendaImageId(const Agenda &agenda) {
  const QString &id = agenda.id().value();
  if (!agenda.flipped())
    return id;
  if (id == "c03276a"_L1 || id == "c03279a"_L1)
    return u"back/"_s + id;
  QString code = id;
  if (code.endsWith(u'a'))
    code.chop(1);
  return u"back/"_s + code;
}

QString actImageId(const Act &act) { return act.id().value(); }

QString imageSource(const QString &imageId) {
  return u"image://arkham-card/"_s + imageId;
}

} // namespace Arkham::CardArt
