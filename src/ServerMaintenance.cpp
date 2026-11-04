#include "ServerMaintenance.h"

#include "RawJson.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Arkham {

std::optional<ServerMaintenance>
ServerMaintenance::fromResponse(int status, const QByteArray &body,
                                const QByteArray &retryAfterHeader) {
  if (status != 503 || body.isEmpty()) {
    return std::nullopt;
  }
  // The lossless parser rejects duplicate keys, matching Apple's
  // LosslessJSONParser (Qt's parser would silently keep the last one).
  const auto parsed = Json::Value::parse(body, QStringLiteral("maintenance"));
  if (!parsed || !parsed->isObject()) {
    return std::nullopt;
  }
  const std::optional<Json::Value> flag = parsed->find("maintenance"_L1);
  if (!flag || !flag->isBool() || !flag->toBool()) {
    return std::nullopt;
  }

  ServerMaintenance result;
  if (const auto since = parsed->find("since"_L1); since && since->isString()) {
    result.since = since->toString();
  }
  if (const auto message = parsed->find("message"_L1);
      message && message->isString()) {
    QString trimmed = message->toString().trimmed();
    if (trimmed.size() > maximumMessageLength) {
      // The cap counts UTF-16 code units (Apple's prefix counts Characters),
      // so a long message may be cut slightly differently there. Stepping
      // back one unit when the cap would end on a high surrogate keeps a
      // surrogate pair from being split.
      qsizetype length = maximumMessageLength;
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
