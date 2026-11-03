#include "ContractPin.h"

namespace Arkham {

// Pinned to djensenius/ArkhamHorror#247 (commit
// 4db9946179ef8b7a242bd7e40c0a2d74990cf3e9), which published
// schemaRevision 0.1.57 and nativeClientMinimumRevision 0.1.57. See
// contracts/contract-pin.json for the machine-readable record.
const ContractPin &currentPin() {
  static const ContractPin pin{
      .backendCommit =
          QStringLiteral("4db9946179ef8b7a242bd7e40c0a2d74990cf3e9"),
      .sourceRef = QStringLiteral("djensenius/ArkhamHorror#247"),
      .supportedSchemaRevision = {0, 1, 57},
      .minimumServerSchemaRevision = {0, 1, 48},
      .sourceNativeClientMinimumRevision = {0, 1, 57},
      .expectedApiBasePath = QStringLiteral("/api/v1"),
  };
  return pin;
}

// SHA-256 digests of every contracts/ file this client's decoders are bound
// to, captured from djensenius/ArkhamHorror commit
// 4db9946179ef8b7a242bd7e40c0a2d74990cf3e9. Recomputed and compared against
// the vendored bytes by ContractDriftTests; see GovernedFixtureDigest.
const QList<GovernedFixtureDigest> &governedFixtureDigests() {
  static const QList<GovernedFixtureDigest> digests{
      {QStringLiteral("manifest.json"),
       QStringLiteral(
           "75e58dcb69d945ecbc8983092b9a6fb47dd4bcfdbea3213588384502c62c0b4"
           "c")},
      {QStringLiteral("fixtures/capabilities.json"),
       QStringLiteral(
           "7843a3d789287d6bdec92bca841fc05a479c4e0c4dd6451d264f3a8ddfca8ee"
           "4")},
      {QStringLiteral("fixtures/answer-rejected.json"),
       QStringLiteral(
           "ad8069b02586f3cfe4677dc6fa25d2297d9f2f6cbd3366c6954845446a0440d"
           "c")},
      {QStringLiteral("fixtures/room-closed.json"),
       QStringLiteral(
           "fabfe52fd38ab86c7c210c681205d6aa2c992200a798063b982ba0a0aefcfae"
           "6")},
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
           "f813d04108cd223575c76a75c2b0ab822adbbb7302dd1655711f960cdf4af3f"
           "3")},
      {QStringLiteral("fixtures/game-list.json"),
       QStringLiteral(
           "15a68bfc25bdb5e73b4876ef4e7bbf97c66666bf2e185a6fbbb117ab6fe6ca8"
           "0")},
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
           "e8e42f6e1d32d1da310da0aa65bfa9187ef2dba42f217e157fa93b3e03fc0b4"
           "2")},
      {QStringLiteral("schemas/game-list.schema.json"),
       QStringLiteral(
           "fe412ca6e7efe8f23445582b1e4611cfecfe480051bdaf8e887588e8f766cda"
           "3")},
      {QStringLiteral("schemas/game-state.schema.json"),
       QStringLiteral(
           "b193923d9d272df08adad5d2a3845b04171756edb1edb304f21fc774996a306"
           "9")},
  };
  return digests;
}

} // namespace Arkham
