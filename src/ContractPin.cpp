#include "ContractPin.h"

namespace Arkham {

// Pinned to djensenius/ArkhamHorror#24 (commit
// 6a1befbd7b01b4a0f763e41260ae4dd1a5d14c27), which published schemaRevision
// 0.1.12 and nativeClientMinimumRevision 0.1.0, building on the catalog (#20)
// and deck (#22) contract PRs. See contracts/contract-pin.json for the
// machine-readable record.
const ContractPin &currentPin() {
  static const ContractPin pin{
      .backendCommit =
          QStringLiteral("6a1befbd7b01b4a0f763e41260ae4dd1a5d14c27"),
      .sourceRef = QStringLiteral("djensenius/ArkhamHorror#24"),
      .supportedSchemaRevision = {0, 1, 12},
      .minimumServerSchemaRevision = {0, 1, 12},
      .sourceNativeClientMinimumRevision = {0, 1, 0},
      .expectedApiBasePath = QStringLiteral("/api/v1"),
  };
  return pin;
}

// SHA-256 digests of every contracts/ file this client's decoders are bound
// to, captured from djensenius/ArkhamHorror commit
// 6a1befbd7b01b4a0f763e41260ae4dd1a5d14c27. Recomputed and compared against
// the vendored bytes by ContractDriftTests; see GovernedFixtureDigest.
const QList<GovernedFixtureDigest> &governedFixtureDigests() {
  static const QList<GovernedFixtureDigest> digests{
      {QStringLiteral("manifest.json"),
       QStringLiteral(
           "1c5b41c75766a2e94575f6b88b95d703dc125874280bfdde1611a7a8c100db5"
           "e")},
      {QStringLiteral("fixtures/capabilities.json"),
       QStringLiteral(
           "eef5172ea810103ccde4b3182a14a3b50bfee727b2b92335804287a596fd3e1"
           "d")},
      {QStringLiteral("fixtures/catalog.json"),
       QStringLiteral(
           "653e00824e6834b1a21b803ef01b8a1a4abe4987410830f70890f3accb71ad8"
           "2")},
      {QStringLiteral("fixtures/decks.json"),
       QStringLiteral(
           "037153d7c611b2b67e101a6eb847f138e4c2b433a06f567d7d8e05857e21165"
           "d")},
      {QStringLiteral("fixtures/game-lifecycle.json"),
       QStringLiteral(
           "436fa9aea0e0e256b68b7f6038c15692e66af2677293b41bca25c691ab60120"
           "4")},
      {QStringLiteral("fixtures/game-list.json"),
       QStringLiteral(
           "5e89ffcf2cba73da7df12cd2f0a6fe6ccd951a2f1d7b5b404454abf2055785f"
           "f")},
      {QStringLiteral("schemas/catalog.schema.json"),
       QStringLiteral(
           "7b4c692f0e151701b2588e18c51a1ce608fa5c36a77e4d695be4f383f39cecc"
           "9")},
      {QStringLiteral("schemas/decks.schema.json"),
       QStringLiteral(
           "6e1e4bd7d5245c63d38a1f78b0c72541d20d36aa571cd3b5b060be6f8d9354c"
           "e")},
      {QStringLiteral("schemas/game-lifecycle.schema.json"),
       QStringLiteral(
           "894ce38d078fe0857e824033578972bacab4744595ca1741250c2813f2fd682"
           "d")},
      {QStringLiteral("schemas/game-list.schema.json"),
       QStringLiteral(
           "f34c3b12198bf2d3d7744d8793cb476b90b30bfec60a3ba5d415221b341e75d"
           "2")},
      {QStringLiteral("schemas/game-state.schema.json"),
       QStringLiteral(
           "b193923d9d272df08adad5d2a3845b04171756edb1edb304f21fc774996a306"
           "9")},
  };
  return digests;
}

} // namespace Arkham
