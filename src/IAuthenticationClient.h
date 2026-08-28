#pragma once

#include "AuthModels.h"
#include "ServerProfile.h"

#include <QtGlobal>
#include <functional>

namespace Arkham {

// Opaque handle to an in-flight AuthenticationClient request, returned so a
// caller can optionally cancel() before completion. A default-constructed
// handle (id 0) never refers to a real request. Handles are only valid
// until their completion callback has fired or the client that issued them
// is destroyed; passing a stale handle to cancel() is a safe no-op.
struct AuthRequestHandle {
  quint64 id{0};
};

// Injectable, asynchronous authentication client for the backend contract's
// /authenticate, /register, and /whoami endpoints.
//
// Contract common to every method:
//   - Each call issues at most one HTTP request and invokes |callback|
//     exactly once, asynchronously, while the client is alive.
//   - Automatic redirects are never followed; every 3xx response is reported
//     as AuthOutcome::UnexpectedStatus without replaying the request body or
//     any Authorization header to another origin.
//   - Public requests (authenticate, registerAccount) never carry an
//     Authorization header. whoAmI carries exactly
//     "Authorization: Token <token>".
//   - Destroying the client aborts and disconnects every outstanding
//     request; no callback is invoked after destruction.
class IAuthenticationClient {
public:
  using AuthTokenCallback = std::function<void(AuthResult<AuthToken>)>;
  using CurrentUserCallback = std::function<void(AuthResult<CurrentUser>)>;

  virtual ~IAuthenticationClient() = default;

  // POST <profile apiUrl>/authenticate with |request| as the JSON body.
  // Carries no Authorization header.
  virtual AuthRequestHandle authenticate(const ServerProfile &profile,
                                         const AuthenticateRequest &request,
                                         AuthTokenCallback callback) = 0;

  // POST <profile apiUrl>/register with |request| as the JSON body. Carries
  // no Authorization header.
  virtual AuthRequestHandle registerAccount(const ServerProfile &profile,
                                            const RegisterRequest &request,
                                            AuthTokenCallback callback) = 0;

  // GET <profile apiUrl>/whoami with header
  // "Authorization: Token <token>".
  virtual AuthRequestHandle whoAmI(const ServerProfile &profile,
                                   const QString &token,
                                   CurrentUserCallback callback) = 0;

  // Aborts the outstanding request identified by |handle|, if any. A stale
  // or already-completed handle is a safe no-op.
  virtual void cancel(AuthRequestHandle handle) = 0;
};

} // namespace Arkham
