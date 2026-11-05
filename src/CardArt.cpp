#include "CardArt.h"

#include "AssetLocator.h"

#include <QStringList>

using namespace Qt::StringLiterals;

namespace Arkham::CardArt {

namespace {

// Investigator.vue's classVariant: Lola Hayes (c03006, c90087) shows the art
// for her current role, `meta`, unless it is Neutral. Only the five role art
// files exist, so any other class symbol shows the plain art.
QString classVariant(const Investigator &investigator) {
  const QString &code = investigator.cardCode().value();
  const auto &role = investigator.metaClassSymbol();
  if ((code != "c03006"_L1 && code != "c90087"_L1) || !role ||
      *role == ClassSymbol::Neutral)
    return {};
  for (const auto &[wire, symbol] : kClassSymbolTable) {
    if (symbol == *role && AssetLocator::isClassVariantSuffix(u"_"_s + wire))
      return QString(wire);
  }
  return {};
}

// JavaScript's text.replace(/^c/, '').
QString withoutLeadingC(const QString &text) {
  return text.startsWith(u'c') ? text.mid(1) : text;
}

// JavaScript's text.replace(/<c>$/, '').
void chopTrailing(QString &text, QChar c) {
  if (text.endsWith(c))
    text.chop(1);
}

// JavaScript's text.replace(/[<chars>]/, with): replaces the first character
// of text that is one of chars.
QString replaceFirstOf(QString text, QStringView chars, const QString &with) {
  for (qsizetype i = 0; i < text.size(); ++i) {
    if (chars.contains(text.at(i))) {
      text.replace(i, 1, with);
      break;
    }
  }
  return text;
}

// JavaScript's parseInt(text) for a card number: the leading decimal digits,
// or -1 when there are none (NaN) or too many to be a card number.
qint64 leadingNumber(const QString &text) {
  qsizetype digits = 0;
  while (digits < text.size() && text.at(digits) >= u'0' &&
         text.at(digits) <= u'9')
    ++digits;
  bool ok = false;
  const qint64 number = QStringView(text).left(digits).toLongLong(&ok);
  return ok ? number : -1;
}

} // namespace

QString investigatorImageId(const Investigator &investigator) {
  switch (investigator.form()) {
  case InvestigatorForm::Yithian:
    return u"c04244"_s;
  case InvestigatorForm::Homunculus:
    return u"c11068b"_s;
  case InvestigatorForm::Shattered:
    return u"c10661"_s;
  case InvestigatorForm::Transfigured:
    if (const auto &card = investigator.transfiguredCardCode();
        card && AssetLocator::isCardArtId(card->value()))
      return card->value();
    break;
  case InvestigatorForm::Regular:
    break;
  }

  const QString art = AssetLocator::isCardArtId(investigator.art())
                          ? investigator.art()
                          : investigator.id().value();
  QStringList query;
  if (const QString variant = classVariant(investigator); !variant.isEmpty())
    query << u"variant="_s + variant;
  if (investigator.mutated())
    query << u"mutation="_s + *investigator.mutated();
  return query.isEmpty() ? art : art + u'?' + query.join(u'&');
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

QString actImageId(const Act &act) {
  static_assert(static_cast<int>(ActSide::H) == 7);
  const QString &id = act.id().value();
  const QString side =
      act.sequenceSide() == ActSide::A
          ? QString()
          : QString(QChar(u'a' + static_cast<int>(act.sequenceSide())));
  const QString sidePart =
      side.isEmpty() || id.endsWith(side) ? QString() : side;

  QString newId =
      side == "b"_L1 && id == "c10607a"_L1 ? u"10607"_s : withoutLeadingC(id);
  if (sidePart == "d"_L1)
    chopTrailing(newId, u'c');
  if (sidePart == "f"_L1)
    chopTrailing(newId, u'e');
  if (sidePart == "h"_L1)
    chopTrailing(newId, u'g');

  // Threads of Fate and Return to Threads of Fate, hard-coded on the web.
  // Each result is "c" + the web's file stem; the locator strips the 'c'.
  const qint64 number = leadingNumber(newId);
  if (number >= 4117 && number <= 4140)
    return u"c"_s + newId +
           replaceFirstOf(replaceFirstOf(sidePart, u"ace", QString()), u"df",
                          u"b"_s);
  if (number >= 53029 && number <= 53036)
    return u"c"_s + newId +
           replaceFirstOf(replaceFirstOf(sidePart, u"g", QString()), u"h",
                          u"b"_s);
  return u"c"_s + newId + sidePart;
}

QString imageSource(const QString &imageId) {
  return u"image://arkham-card/"_s + imageId;
}

} // namespace Arkham::CardArt
