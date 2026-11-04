#include "ServerMaintenance.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <algorithm>

namespace Arkham {

std::optional<ServerMaintenance>
ServerMaintenance::fromResponse(int status, const QByteArray &body,
                                const QByteArray &retryAfterHeader) {
  if (status != 503 || body.isEmpty()) {
    return std::nullopt;
  }
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    return std::nullopt;
  }
  const QJsonObject object = doc.object();
  const QJsonValue flag = object.value(QLatin1String("maintenance"));
  if (!flag.isBool() || !flag.toBool()) {
    return std::nullopt;
  }

  ServerMaintenance result;
  const QJsonValue since = object.value(QLatin1String("since"));
  if (since.isString()) {
    result.since = since.toString();
  }
  const QJsonValue message = object.value(QLatin1String("message"));
  if (message.isString()) {
    QString trimmed = message.toString().trimmed();
    if (trimmed.size() > maximumMessageLength) {
      qsizetype length = maximumMessageLength;
      // Never split a surrogate pair at the cap.
      if (trimmed.at(length - 1).isHighSurrogate()) {
        --length;
      }
      trimmed.truncate(length);
    }
    if (!trimmed.isEmpty()) {
      result.message = trimmed;
    }
  }
  result.retryAfterSeconds = parseRetryAfter(retryAfterHeader);
  return result;
}

std::optional<int>
ServerMaintenance::parseRetryAfter(const QByteArray &header) {
  const QByteArray value = header.trimmed();
  if (value.isEmpty() || value.size() > 9 ||
      !std::all_of(value.cbegin(), value.cend(),
                   [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  const int seconds = value.toInt();
  if (seconds > maximumRetryAfterSeconds) {
    return std::nullopt;
  }
  return seconds;
}

QString ServerMaintenance::notice() const {
  const QString reason = message.value_or(QStringLiteral(
      "This server is temporarily down, most likely for maintenance."));
  if (!retryAfterSeconds) {
    return QStringLiteral("%1 Try again later.").arg(reason);
  }
  const int minutes = std::max(1, (*retryAfterSeconds + 59) / 60);
  const QString wait = minutes == 1
                           ? QStringLiteral("about a minute")
                           : QStringLiteral("about %1 minutes").arg(minutes);
  return QStringLiteral("%1 Try again in %2.").arg(reason, wait);
}

} // namespace Arkham
