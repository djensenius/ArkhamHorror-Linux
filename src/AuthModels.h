#pragma once

#include "ValueOrError.h"

#include <QJsonObject>
#include <QString>
#include <optional>

namespace Arkham {

// ── Request models ──────────────────────────────────────────────────────
//
// These types deliberately have no QDebug stream operator or toString(): a
// password must never be reachable through an accidental qDebug() <<
// request or similar default string conversion. Do not add one; if a
// description is ever genuinely needed, it must redact the password field
// explicitly (e.g. print only email, and a fixed "<redacted>" marker).

// Request body for POST /authenticate.
struct AuthenticateRequest {
  QString email;
  QString password;

  [[nodiscard]] QJsonObject toJson() const;
};

// Request body for POST /register.
struct RegisterRequest {
  QString email;
  QString username;
  QString password;

  [[nodiscard]] QJsonObject toJson() const;
};

// ── Response / domain models ────────────────────────────────────────────

// Response body for POST /authenticate and POST /register: {"token": "..."}.
// No QDebug/toString: the token is a bearer secret and must never be logged.
struct AuthToken {
  QString token;

  [[nodiscard]] static ValueOrError<AuthToken> fromJson(const QJsonObject &obj);
};

// Response body for GET /whoami.
struct CurrentUser {
  QString username;
  QString email;
  bool beta{false};
  bool admin{false};

  [[nodiscard]] static ValueOrError<CurrentUser>
  fromJson(const QJsonObject &obj);
};

// ── Typed, secret-free client outcomes ──────────────────────────────────

// Discriminated outcome of a single AuthenticationClient request.
enum class AuthOutcome {
  Success,
  InvalidInput,     ///< Invalid ServerProfile or request encoding failure;
                    ///< rejected before any request was issued.
  Transport,        ///< Reply carried an explicit transport-level error
                    ///< (e.g. connection refused, DNS failure, timeout).
  NonHttpResponse,  ///< No HTTP status was received even though no
                    ///< transport error was reported (unexpected).
  Unauthorized,     ///< HTTP 401.
  MalformedPayload, ///< 2xx but the JSON body did not match the expected
                    ///< shape.
  UnexpectedStatus, ///< Any other status, including every 3xx redirect
                    ///< (redirects are never followed).
  Cancelled,        ///< The request was explicitly cancelled.
};

// Full, secret-free result of a single AuthenticationClient request.
//
// diagnostic is always a static, human-readable string. It never contains
// the response body, the request body, a password, a token, an
// Authorization header, or any low-level exception/error text that might
// echo request details.
template <typename T> struct AuthResult {
  AuthOutcome outcome{AuthOutcome::Transport};
  QString diagnostic;
  // HTTP status code when one was received (including 3xx and 401); 0 when
  // no HTTP response was received (InvalidInput, Transport, NonHttpResponse,
  // Cancelled).
  int httpStatus{0};
  // Populated only when outcome == Success.
  std::optional<T> value;
};

} // namespace Arkham
