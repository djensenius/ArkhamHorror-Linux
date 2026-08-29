#include <QCryptographicHash>
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
class ContractDriftTests final : public QObject {
  Q_OBJECT

private slots:
  void everyGovernedFileMatchesItsPinnedDigest();
  void digestTableCoversAllPinnedContractFiles();
  void digestTableHasNoDuplicatePaths();
  void hashHelperDetectsASingleByteMutation();
  void pinnedDigestsAreLowercaseHex();
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
  const GovernedFixtureDigest &manifestEntry = governedFixtureDigests().at(0);
  QCOMPARE(manifestEntry.relativePath, QStringLiteral("manifest.json"));

  const QString path = QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + u'/' +
                       manifestEntry.relativePath;
  QFile f(path);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QByteArray bytes = f.readAll();
  QVERIFY(!bytes.isEmpty());

  // Sanity: the unmutated bytes still match (otherwise this test would
  // "pass" for the wrong reason if the fixture were already drifted).
  QCOMPARE(hexSha256(bytes), manifestEntry.sha256Hex);

  bytes[0] = bytes[0] ^ char(0x01);
  QVERIFY(hexSha256(bytes) != manifestEntry.sha256Hex);
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

QTEST_APPLESS_MAIN(ContractDriftTests)

#include "ContractDriftTests.moc"
