#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

namespace Arkham {

// A server-wide maintenance window. The fork server answers every
// non-allow-listed route with HTTP 503, `Retry-After: 120`, and the body
// {"maintenance": true, "since": <UTCTime>, "message": <Text>} (backend
// Foundation.hs maintenanceMiddleware).
//
// Parsed with the Apple client's rules (ServerMaintenance.swift): only a 503
// whose body is a JSON object with "maintenance": true maps here, so any
// other 503 (for example from a proxy) stays a generic HTTP error. `since`
// and `message` are kept only when they are strings; the message is trimmed,
// dropped when blank, and capped at maximumMessageLength characters.
struct ServerMaintenance {
  static constexpr int maximumRetryAfterSeconds = 7 * 24 * 60 * 60;
  static constexpr qsizetype maximumMessageLength = 1000;

  std::optional<QString> message;
  std::optional<QString> since;
  // Seconds from Retry-After when it is 1-9 ASCII digits of at most a week.
  // An HTTP-date or any other value counts as absent.
  std::optional<int> retryAfterSeconds;

  // nullopt unless status is 503 and body is the maintenance object.
  // retryAfterHeader is the raw Retry-After value (empty when absent).
  [[nodiscard]] static std::optional<ServerMaintenance>
  fromResponse(int status, const QByteArray &body,
               const QByteArray &retryAfterHeader);

  [[nodiscard]] static std::optional<int>
  parseRetryAfter(const QByteArray &header);

  // User-facing notice: the server's message (or a generic maintenance
  // line) followed by "Try again in about N minutes." or "Try again later.".
  [[nodiscard]] QString notice() const;

  bool operator==(const ServerMaintenance &) const = default;
};

} // namespace Arkham
