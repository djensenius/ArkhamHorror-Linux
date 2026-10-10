#include "ContractPin.h"

namespace Arkham {

// Pinned to djensenius/ArkhamHorror#152 (commit
// 25a3eb8072d0b3a309a7090912c2a9c2b858c730), which published
// schemaRevision 0.1.52 and nativeClientMinimumRevision 0.1.50. See
// contracts/contract-pin.json for the machine-readable record.
const ContractPin &currentPin() {
  static const ContractPin pin{
      .backendCommit =
          QStringLiteral("25a3eb8072d0b3a309a7090912c2a9c2b858c730"),
      .sourceRef = QStringLiteral("djensenius/ArkhamHorror#152"),
      .supportedSchemaRevision = {0, 1, 52},
      .minimumServerSchemaRevision = {0, 1, 48},
      .sourceNativeClientMinimumRevision = {0, 1, 50},
      .expectedApiBasePath = QStringLiteral("/api/v1"),
  };
  return pin;
}

// SHA-256 digests of every contracts/ file this client's decoders are bound
// to, captured from djensenius/ArkhamHorror commit
// 25a3eb8072d0b3a309a7090912c2a9c2b858c730. Recomputed and compared against
// the vendored bytes by ContractDriftTests; see GovernedFixtureDigest.
const QList<GovernedFixtureDigest> &governedFixtureDigests() {
  static const QList<GovernedFixtureDigest> digests{
      {QStringLiteral("manifest.json"),
       QStringLiteral(
           "a91257aefcab09ab3c5614edcbe0e4631a28d40f286cb8a55ea2c4d8d4a735f"
           "2")},
      {QStringLiteral("fixtures/capabilities.json"),
       QStringLiteral(
           "38effc498e81e1cd5b9138476e4e8b1032663b4d9af4ed6fd9b19834308c7f2"
           "5")},
      {QStringLiteral("fixtures/answer-rejected.json"),
       QStringLiteral(
           "ad8069b02586f3cfe4677dc6fa25d2297d9f2f6cbd3366c6954845446a0440d"
           "c")},
      {QStringLiteral("fixtures/campaign-catalog.json"),
       QStringLiteral(
           "1319f85f8d709c2e7134586621da42cb376048154cc5498e3f24a395c365c25"
           "0")},
      {QStringLiteral("fixtures/catalog.json"),
       QStringLiteral(
           "481f42cbac1fcb208cdb1b626a3cc6951c35531383e7439dc3c0d7c02a9044a"
           "c")},
      {QStringLiteral("fixtures/decks.json"),
       QStringLiteral(
           "be1b19529d95386c6c2ed0b5c665aa25ae64a450c8a3f412d10b8523b966aa"
           "ff")},
      {QStringLiteral("fixtures/game-lifecycle.json"),
       QStringLiteral(
           "436fa9aea0e0e256b68b7f6038c15692e66af2677293b41bca25c691ab60120"
           "4")},
      {QStringLiteral("fixtures/game-list.json"),
       QStringLiteral(
           "5e89ffcf2cba73da7df12cd2f0a6fe6ccd951a2f1d7b5b404454abf2055785f"
           "f")},
      {QStringLiteral("schemas/campaign-catalog.schema.json"),
       QStringLiteral(
           "e49e5000658288b4324280fe4164359b419a0c91fbbf7c16c045385060c23dc"
           "1")},
      {QStringLiteral("schemas/catalog.schema.json"),
       QStringLiteral(
           "7b4c692f0e151701b2588e18c51a1ce608fa5c36a77e4d695be4f383f39cecc"
           "9")},
      {QStringLiteral("schemas/decks.schema.json"),
       QStringLiteral(
           "c9350787341834c68f7c5bb4d5f1bcaa1880474049bdc12c772ea3fd76b7c44"
           "0")},
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
