#include "ContractPin.h"

namespace Arkham {

// Pinned to djensenius/ArkhamHorror#130 (commit
// f454ccb9fb789739c666a9cbd74d3fdc14a81fb9), which published
// schemaRevision 0.1.48 and nativeClientMinimumRevision 0.1.0. See
// contracts/contract-pin.json for the machine-readable record.
const ContractPin &currentPin() {
  static const ContractPin pin{
      .backendCommit =
          QStringLiteral("f454ccb9fb789739c666a9cbd74d3fdc14a81fb9"),
      .sourceRef = QStringLiteral("djensenius/ArkhamHorror#130"),
      .supportedSchemaRevision = {0, 1, 48},
      .minimumServerSchemaRevision = {0, 1, 48},
      .sourceNativeClientMinimumRevision = {0, 1, 0},
      .expectedApiBasePath = QStringLiteral("/api/v1"),
  };
  return pin;
}

// SHA-256 digests of every contracts/ file this client's decoders are bound
// to, captured from djensenius/ArkhamHorror commit
// f454ccb9fb789739c666a9cbd74d3fdc14a81fb9. Recomputed and compared against
// the vendored bytes by ContractDriftTests; see GovernedFixtureDigest.
const QList<GovernedFixtureDigest> &governedFixtureDigests() {
  static const QList<GovernedFixtureDigest> digests{
      {QStringLiteral("manifest.json"),
       QStringLiteral(
           "ce186e7c7a448bb411d6cb21fee6d5f8a6e46c7c05b34e6b556489981bebb34"
           "e")},
      {QStringLiteral("fixtures/capabilities.json"),
       QStringLiteral(
           "56a02493c3b3495797bac45f84d3b6de0926645f243a2a71fc66fa85b3ff6fa"
           "c")},
      {QStringLiteral("fixtures/answer-rejected.json"),
       QStringLiteral(
           "ad8069b02586f3cfe4677dc6fa25d2297d9f2f6cbd3366c6954845446a0440d"
           "c")},
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
