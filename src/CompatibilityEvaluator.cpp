#include "CompatibilityEvaluator.h"

namespace Arkham {

CompatibilityResult
CompatibilityEvaluator::evaluate(const ServerCapabilities &server,
                                 const ContractPin &pin) {
  // 1. Legacy fallback: server predates the capabilities endpoint.
  //    Return an explicit degraded-mode outcome rather than running version
  //    checks against the artificially low revisions in the fallback struct.
  if (server.isLegacyFallback) {
    return {
        CompatibilityOutcome::LegacyFallback,
        IncompatibilityCode::None,
        QStringLiteral("server predates the capabilities endpoint; "
                       "operating in legacy fallback mode without modern "
                       "feature assumptions"),
    };
  }

  // 2. Client too old: this client's supported schema revision is below the
  //    minimum the server will accept.
  if (pin.supportedSchemaRevision < server.nativeClientMinimumRevision) {
    return {
        CompatibilityOutcome::Incompatible,
        IncompatibilityCode::ClientTooOld,
        QStringLiteral(
            "client schema %1 is below server minimum %2; update the client")
            .arg(pin.supportedSchemaRevision.toString(),
                 server.nativeClientMinimumRevision.toString()),
    };
  }

  // 3. Server too old: server schema is below the minimum this client requires.
  if (server.schemaRevision < pin.minimumServerSchemaRevision) {
    return {
        CompatibilityOutcome::Incompatible,
        IncompatibilityCode::ServerTooOld,
        QStringLiteral(
            "server schema %1 is below client minimum %2; update the server")
            .arg(server.schemaRevision.toString(),
                 pin.minimumServerSchemaRevision.toString()),
    };
  }

  // 4. API base path mismatch: wrong server or misconfiguration.
  if (server.apiBasePath != pin.expectedApiBasePath) {
    return {
        CompatibilityOutcome::Incompatible,
        IncompatibilityCode::ApiBaseMismatch,
        QStringLiteral(
            "server apiBasePath \"%1\" does not match expected \"%2\"")
            .arg(server.apiBasePath, pin.expectedApiBasePath),
    };
  }

  // Unknown capabilities are silently allowed (forward-compatible).
  return {CompatibilityOutcome::Compatible, IncompatibilityCode::None, {}};
}

} // namespace Arkham
