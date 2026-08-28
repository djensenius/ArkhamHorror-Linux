#pragma once

#include "CompatibilityEvaluator.h"

#include <QMetaType>
#include <QString>
#include <optional>

namespace Arkham {

// Discriminated outcome of a single capability probe.
enum class ProbeOutcome {
  Compatible,     ///< Server is compatible; the client may connect normally.
  LegacyFallback, ///< Capabilities endpoint returned 404; server predates the
                  ///< endpoint.  No modern capabilities are assumed.
  Incompatible,   ///< Version or API-base mismatch; see compatibility field.
  NetworkError,   ///< Transport failure; no HTTP response was received.  Also
                  ///< used when a 2xx reply carries a transport-level error
                  ///< (e.g. connection reset mid-transfer).
  MalformedJson,  ///< 2xx received but body is not valid ServerCapabilities.
  HttpError,      ///< Non-2xx and non-404 HTTP status.
  InvalidProfile, ///< The supplied ServerProfile failed isValid() before any
                  ///< request was issued.
};

// Full result of a single capability probe.
struct ProbeResult {
  ProbeOutcome outcome{ProbeOutcome::NetworkError};

  // Human-readable diagnostic.  Always non-empty except when outcome is
  // Compatible.
  QString diagnostic;

  // Populated when CompatibilityEvaluator ran (outcome is Compatible,
  // LegacyFallback, or Incompatible).
  std::optional<CompatibilityResult> compatibility;

  // HTTP status code.  Non-zero for HttpError and for probes that received an
  // HTTP response (including 2xx and 404).  Zero when no HTTP response was
  // received (NetworkError, InvalidProfile).
  int httpStatus{0};
};

} // namespace Arkham

Q_DECLARE_METATYPE(Arkham::ProbeResult)
