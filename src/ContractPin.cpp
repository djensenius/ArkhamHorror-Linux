#include "ContractPin.h"

namespace Arkham {

// Pinned to djensenius/ArkhamHorror#23 (commit
// 2bf2935cde121498435744a06fcf63502a80ae43) which published schemaRevision
// 0.1.11 and nativeClientMinimumRevision 0.1.0. See contracts/contract-pin.json
// for the machine-readable record.
const ContractPin &currentPin() {
  static const ContractPin pin{
      .backendCommit =
          QStringLiteral("2bf2935cde121498435744a06fcf63502a80ae43"),
      .sourceRef = QStringLiteral("djensenius/ArkhamHorror#23"),
      .supportedSchemaRevision = {0, 1, 11},
      .minimumServerSchemaRevision = {0, 1, 11},
      .sourceNativeClientMinimumRevision = {0, 1, 0},
      .expectedApiBasePath = QStringLiteral("/api/v1"),
  };
  return pin;
}

} // namespace Arkham
