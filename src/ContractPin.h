#pragma once

#include "ContractRevision.h"

#include <QString>

namespace Arkham {

// Immutable metadata that pins this client build to a specific backend
// contract. Produced once at build time and embedded in the binary.
struct ContractPin {
  // Exact git commit SHA of the backend that published this contract.
  QString backendCommit;
  // Backend PR / issue that introduced the pinned revision.
  QString sourceRef;
  // Contract schema revision this client can decode.
  ContractRevision supportedSchemaRevision;
  // Minimum modern server schema revision this client requires.
  ContractRevision minimumServerSchemaRevision;
  // Minimum native-client revision declared by the canonical source fixture.
  // Retained only to detect drift between the fixture and this pin.
  ContractRevision sourceNativeClientMinimumRevision;
  // Expected API base path (e.g. "/api/v1").
  QString expectedApiBasePath;
};

// Returns the single ContractPin embedded in this build.
[[nodiscard]] const ContractPin &currentPin();

} // namespace Arkham
