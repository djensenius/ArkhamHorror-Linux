#pragma once

#include <QUrl>

namespace Arkham {

// Returns true if it is safe to send authentication traffic (a request or
// response that may carry a password or bearer token) to |url| without
// encryption.
//
// Permitted:
//   - https, for any host.
//   - http, but only to a loopback host: exactly "localhost"
//     (case-insensitive), or a host QHostAddress recognises as a loopback
//     IPv4 (127.0.0.0/8, in any numeric encoding QHostAddress accepts --
//     e.g. "127.1", octal, hex, and decimal all denote the real loopback
//     address, not a distinct host) or IPv6 (::1) literal. This narrow
//     exception exists solely to preserve local development/self-hosting
//     without ever allowing a LAN or public host to receive a password or
//     bearer token in cleartext.
// Rejected:
//   - Any other scheme.
//   - http to any non-loopback host, including a hostname that merely
//     resembles a loopback address without being one (e.g.
//     "localhost.evil.com", "127.0.0.1.evil.com" -- these are different DNS
//     names, not the local machine).
//   - Any URL with a userinfo component (credentials embedded in the URL),
//     regardless of scheme or host: userinfo must never influence which
//     host is treated as "the" host for this decision.
//
// This function has no knowledge of redirects: callers that may follow
// redirects must not rely on this check alone to keep a redirected request
// secure. NetworkAuthenticationClient never follows redirects (every 3xx is
// a typed failure), so this exception can never be leveraged to smuggle a
// loopback request to an insecure remote origin.
[[nodiscard]] bool isSecureOrLoopbackAuthTransport(const QUrl &url);

} // namespace Arkham
