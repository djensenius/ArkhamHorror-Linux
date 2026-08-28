#include "ContractRevision.h"

#include <QList>

namespace Arkham {

namespace {

// Validate that a component string contains only ASCII decimal digits [0-9]+.
// This rejects leading '+', whitespace, Unicode digits, and empty strings —
// matching the schema pattern exactly.
bool isAsciiDigits(QStringView s) {
  if (s.isEmpty())
    return false;
  for (const QChar c : s) {
    const unsigned short u = c.unicode();
    if (u < u'0' || u > u'9')
      return false;
  }
  return true;
}

} // namespace

ValueOrError<ContractRevision> ContractRevision::parse(QStringView str) {
  const QList<QStringView> parts = str.split(QLatin1Char('.'));
  if (parts.size() != 3) {
    return failure(
        QStringLiteral(
            "version must have exactly three dot-separated components: \"%1\"")
            .arg(str));
  }

  ContractRevision result;
  bool ok = false;

  if (!isAsciiDigits(parts[0])) {
    return failure(
        QStringLiteral("major component \"%1\" must match [0-9]+ in \"%2\"")
            .arg(parts[0])
            .arg(str));
  }
  result.major = parts[0].toInt(&ok);
  if (!ok) {
    return failure(
        QStringLiteral("major component \"%1\" overflows int in \"%2\"")
            .arg(parts[0])
            .arg(str));
  }

  if (!isAsciiDigits(parts[1])) {
    return failure(
        QStringLiteral("minor component \"%1\" must match [0-9]+ in \"%2\"")
            .arg(parts[1])
            .arg(str));
  }
  result.minor = parts[1].toInt(&ok);
  if (!ok) {
    return failure(
        QStringLiteral("minor component \"%1\" overflows int in \"%2\"")
            .arg(parts[1])
            .arg(str));
  }

  if (!isAsciiDigits(parts[2])) {
    return failure(
        QStringLiteral("patch component \"%1\" must match [0-9]+ in \"%2\"")
            .arg(parts[2])
            .arg(str));
  }
  result.patch = parts[2].toInt(&ok);
  if (!ok) {
    return failure(
        QStringLiteral("patch component \"%1\" overflows int in \"%2\"")
            .arg(parts[2])
            .arg(str));
  }

  return result;
}

QString ContractRevision::toString() const {
  return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

} // namespace Arkham
