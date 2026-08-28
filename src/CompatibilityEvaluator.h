#pragma once

#include "ContractPin.h"
#include "ServerCapabilities.h"

#include <QString>

namespace Arkham {

enum class CompatibilityOutcome {
  Compatible,     ///< Server and client are mutually acceptable.
  LegacyFallback, ///< Server predates the capabilities endpoint (404 observed).
                  ///< No modern capabilities are assumed.  Caller decides mode.
  Incompatible,   ///< Hard rejection; see IncompatibilityCode for the reason.
};

enum class IncompatibilityCode {
  None,            ///< Not set (outcome is not Incompatible).
  ClientTooOld,    ///< Client support is below the server's client floor.
  ServerTooOld,    ///< Server schema is below the client's server floor.
  ApiBaseMismatch, ///< Server and client API base paths differ.
};

struct CompatibilityResult {
  CompatibilityOutcome outcome{CompatibilityOutcome::Incompatible};
  IncompatibilityCode code{IncompatibilityCode::None};
  /// Human-readable explanation.  Empty when outcome == Compatible.
  QString diagnostic;

  [[nodiscard]] bool isCompatible() const {
    return outcome == CompatibilityOutcome::Compatible;
  }
  /// True when the client can operate, possibly in a degraded mode.
  [[nodiscard]] bool isUsable() const {
    return outcome == CompatibilityOutcome::Compatible ||
           outcome == CompatibilityOutcome::LegacyFallback;
  }
};

class CompatibilityEvaluator {
public:
  // Evaluate compatibility between this client (pin) and the server.
  //
  // The client's capability-negotiation identity is
  // pin.supportedSchemaRevision (not the native-client software version
  // string). Evaluation order:
  //
  //   1. LegacyFallback  — server.isLegacyFallback (404 on endpoint).
  //   2. ClientTooOld    — pin.supportedSchemaRevision <
  //   server.nativeClientMinimumRevision.
  //   3. ServerTooOld    — server.schemaRevision <
  //   pin.minimumServerSchemaRevision.
  //   4. ApiBaseMismatch — server.apiBasePath != pin.expectedApiBasePath.
  //   5. Compatible      — all checks passed; unknown capabilities are ignored.
  [[nodiscard]] static CompatibilityResult
  evaluate(const ServerCapabilities &server, const ContractPin &pin);
};

} // namespace Arkham
