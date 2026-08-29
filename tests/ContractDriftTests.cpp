#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QtTest>

#include "ContractPin.h"

using namespace Arkham;

// Proves that every vendored contracts/ file this client's decoders are
// bound to still matches the exact bytes recorded at ContractPin's pinned
// backend commit -- so silently editing a vendored fixture/schema/manifest
// (or forgetting to update its digest after a legitimate re-vendor) fails a
// test instead of drifting unnoticed. See ContractPin.h's
// GovernedFixtureDigest and ContractPin.cpp's governedFixtureDigests().
//
// This is an *offline* proof: it only shows that the vendored bytes match
// the SHA-256 recorded beside them in this same repository/commit. It
// cannot by itself catch a change that edits a vendored file and updates
// its recorded digest together (internally consistent, but silently
// diverged from the real backend). packaging/verify_contract_provenance.py
// closes that gap by fetching the pinned commit directly from the
// backend's own git remote and byte-comparing against the real git blob --
// an independent, external proof this in-process test cannot provide (it
// has no network access and must not depend on any local developer
// checkout path).
class ContractDriftTests final : public QObject {
  Q_OBJECT

private slots:
  void everyGovernedFileMatchesItsPinnedDigest();
  void digestTableCoversAllPinnedContractFiles();
  void digestTableHasNoDuplicatePaths();
  void hashHelperDetectsASingleByteMutation();
  void pinnedDigestsAreLowercaseHex();
  void vendoredDirectoriesContainNoFilesMissingFromDigestTable();
};

namespace {
QString hexSha256(const QByteArray &bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
} // namespace

void ContractDriftTests::everyGovernedFileMatchesItsPinnedDigest() {
  const QList<GovernedFixtureDigest> &digests = governedFixtureDigests();
  QVERIFY(!digests.isEmpty());

  for (const GovernedFixtureDigest &entry : digests) {
    const QString path =
        QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + u'/' + entry.relativePath;
    QFile f(path);
    QVERIFY2(
        f.open(QIODevice::ReadOnly),
        qPrintable(
            QStringLiteral("cannot open %1: %2").arg(path, f.errorString())));
    const QByteArray bytes = f.readAll();
    const QString actual = hexSha256(bytes);
    QVERIFY2(actual == entry.sha256Hex,
             qPrintable(QStringLiteral("%1: recorded digest %2 does not "
                                       "match recomputed digest %3 -- "
                                       "vendored bytes have drifted from "
                                       "the pinned backend commit")
                            .arg(entry.relativePath, entry.sha256Hex, actual)));
  }
}

void ContractDriftTests::digestTableCoversAllPinnedContractFiles() {
  // Every file this client's decoders are bound to (manifest, capabilities,
  // and the four domain fixtures + their five schemas) must appear -- an
  // omission here would let that specific file drift completely
  // unverified.
  const QSet<QString> expected{
      QStringLiteral("manifest.json"),
      QStringLiteral("fixtures/capabilities.json"),
      QStringLiteral("fixtures/catalog.json"),
      QStringLiteral("fixtures/decks.json"),
      QStringLiteral("fixtures/game-lifecycle.json"),
      QStringLiteral("fixtures/game-list.json"),
      QStringLiteral("schemas/catalog.schema.json"),
      QStringLiteral("schemas/decks.schema.json"),
      QStringLiteral("schemas/game-lifecycle.schema.json"),
      QStringLiteral("schemas/game-list.schema.json"),
      QStringLiteral("schemas/game-state.schema.json"),
  };
  QSet<QString> actual;
  for (const GovernedFixtureDigest &entry : governedFixtureDigests())
    actual.insert(entry.relativePath);
  QCOMPARE(actual, expected);
}

void ContractDriftTests::digestTableHasNoDuplicatePaths() {
  const QList<GovernedFixtureDigest> &digests = governedFixtureDigests();
  QSet<QString> seen;
  for (const GovernedFixtureDigest &entry : digests) {
    QVERIFY2(
        !seen.contains(entry.relativePath),
        qPrintable(
            QStringLiteral("duplicate entry for %1").arg(entry.relativePath)));
    seen.insert(entry.relativePath);
  }
}

void ContractDriftTests::hashHelperDetectsASingleByteMutation() {
  // Proves the comparison itself is discriminating -- not a tautology that
  // would pass even if governedFixtureDigests() recorded the wrong value --
  // by mutating one real vendored file's bytes in memory (never touching
  // disk) and confirming the recomputed digest no longer matches the
  // recorded one.
  //
  // Looked up by relativePath rather than a fixed index: depending on
  // governedFixtureDigests()'s entry order would make this test brittle to
  // a harmless reordering of that table.
  const QList<GovernedFixtureDigest> &digests = governedFixtureDigests();
  const GovernedFixtureDigest *manifestEntry = nullptr;
  for (const GovernedFixtureDigest &entry : digests) {
    if (entry.relativePath == QStringLiteral("manifest.json")) {
      manifestEntry = &entry;
      break;
    }
  }
  QVERIFY(manifestEntry != nullptr);

  const QString path = QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + u'/' +
                       manifestEntry->relativePath;
  QFile f(path);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QByteArray bytes = f.readAll();
  QVERIFY(!bytes.isEmpty());

  // Sanity: the unmutated bytes still match (otherwise this test would
  // "pass" for the wrong reason if the fixture were already drifted).
  QCOMPARE(hexSha256(bytes), manifestEntry->sha256Hex);

  bytes[0] = bytes[0] ^ char(0x01);
  QVERIFY(hexSha256(bytes) != manifestEntry->sha256Hex);
}

void ContractDriftTests::pinnedDigestsAreLowercaseHex() {
  static const QRegularExpression lowercaseHex64(
      QStringLiteral("^[0-9a-f]{64}$"));
  for (const GovernedFixtureDigest &entry : governedFixtureDigests()) {
    QVERIFY2(lowercaseHex64.match(entry.sha256Hex).hasMatch(),
             qPrintable(QStringLiteral("%1: digest \"%2\" is not exactly 64 "
                                       "lowercase hex characters")
                            .arg(entry.relativePath, entry.sha256Hex)));
  }
}

void ContractDriftTests::
    vendoredDirectoriesContainNoFilesMissingFromDigestTable() {
  // digestTableCoversAllPinnedContractFiles() only checks that the digest
  // *table* contains every path this client's decoders are bound to; it
  // says nothing about whether the table also covers every file actually
  // sitting on disk. A vendored file added to contracts/fixtures/ or
  // contracts/schemas/ without a matching digest-table entry would pass
  // that test yet be verified by nothing at all -- neither
  // everyGovernedFileMatchesItsPinnedDigest() (which only walks the
  // table) nor packaging/verify_contract_provenance.py's git-blob proof
  // (which walks the same table). This test enumerates the real
  // directories on disk instead and requires an exact match against the
  // digest table's path set, so an addition, removal, or silent path
  // substitution is caught even if the table itself was never touched.
  QSet<QString> onDisk;
  const QString contractsRoot = QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR);
  QDirIterator it(contractsRoot, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    const QString relative =
        QDir(contractsRoot).relativeFilePath(it.filePath());
    // contract-pin.json is this client's own authored pin-metadata
    // sidecar (see ContractPin.h/.cpp), not a file vendored byte-for-byte
    // from the backend -- it is deliberately outside the byte-identity
    // proof this table exists to police.
    if (relative == QStringLiteral("contract-pin.json"))
      continue;
    onDisk.insert(relative);
  }
  QVERIFY(!onDisk.isEmpty());

  QSet<QString> tabled;
  for (const GovernedFixtureDigest &entry : governedFixtureDigests())
    tabled.insert(entry.relativePath);

  const QSet<QString> onDiskOnly = onDisk - tabled;
  QVERIFY2(onDiskOnly.isEmpty(),
           qPrintable(
               QStringLiteral("file(s) present on disk with no digest-table "
                              "entry (added without vendoring provenance?): %1")
                   .arg(QStringList(onDiskOnly.values())
                            .join(QStringLiteral(", ")))));

  const QSet<QString> tabledOnly = tabled - onDisk;
  QVERIFY2(
      tabledOnly.isEmpty(),
      qPrintable(QStringLiteral("digest-table entry(ies) with no file on disk "
                                "(removed without updating the table?): %1")
                     .arg(QStringList(tabledOnly.values())
                              .join(QStringLiteral(", ")))));
}

QTEST_APPLESS_MAIN(ContractDriftTests)

#include "ContractDriftTests.moc"
