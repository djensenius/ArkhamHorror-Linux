#pragma once

#include "ContractRevision.h"

#include <QList>
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

// One vendored contract file (fixture, schema, or manifest) and the SHA-256
// digest of its exact bytes at currentPin().backendCommit. Recorded together
// so that editing a vendored file without updating its digest -- or vice
// versa -- fails ContractDriftTests instead of silently drifting from the
// pinned backend commit.
struct GovernedFixtureDigest {
  // Path relative to the repository's contracts/ directory, e.g.
  // "fixtures/catalog.json".
  QString relativePath;
  // Lowercase hex SHA-256 of the file's exact bytes.
  QString sha256Hex;
};

// Every contracts/ file this client's decoders are bound to, pinned to
// currentPin().backendCommit. See ContractDriftTests for the verification
// that re-hashes each file and compares it against this table.
[[nodiscard]] const QList<GovernedFixtureDigest> &governedFixtureDigests();

} // namespace Arkham
