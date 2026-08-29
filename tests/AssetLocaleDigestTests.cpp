#include "AssetLocaleDigestTests.h"

#include "AssetLocaleDigest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTest>

using namespace Arkham;

namespace {

// The exact, closed locale set djensenius/ArkhamHorror-Linux#17's
// provenance covers -- see contracts/asset-locale-digest.json's own
// "provenance" note. Any future addition or removal must be a conscious
// change to BOTH the manifest and this test, not something that could
// silently slip in via a Python regeneration alone.
QSet<QString> expectedWebLocales() {
  return {QStringLiteral("ita"), QStringLiteral("fr"), QStringLiteral("es"),
          QStringLiteral("ko"), QStringLiteral("zh")};
}

QByteArray readContractsFile(const QString &relativePath) {
  QFile file(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + u'/' + relativePath);
  // Copilot review: Q_ASSERT compiles out in release builds, which would
  // silently turn a missing/unreadable fixture file into a confusing
  // downstream test failure instead of a clear, immediate diagnosis.
  // qFatal() is enforced in every build configuration.
  if (!file.open(QIODevice::ReadOnly)) {
    qFatal("readContractsFile(%s) failed: %s", qPrintable(relativePath),
           qPrintable(file.errorString()));
  }
  return file.readAll();
}

QJsonObject readManifest() {
  const QByteArray bytes =
      readContractsFile(QStringLiteral("asset-locale-digest.json"));
  const QJsonDocument doc = QJsonDocument::fromJson(bytes);
  if (!doc.isObject()) {
    qFatal("asset-locale-digest.json did not parse as a JSON object");
  }
  return doc.object();
}

} // namespace

void AssetLocaleDigestTests::generatedHeaderMatchesPinnedManifestHash() {
  // Independent, C++-only drift check: recompute the raw-byte SHA-256 of
  // the checked-in contracts/asset-locale-digest.json manifest and compare
  // it against the hash tools/generate_asset_locale_digest.py embedded in
  // the generated header. This must never depend on Python being
  // available at test time.
  const QByteArray bytes =
      readContractsFile(QStringLiteral("asset-locale-digest.json"));
  const QString recomputed = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());

  QCOMPARE(recomputed, AssetLocaleDigest::pinnedManifestJsonSha256());
}

void AssetLocaleDigestTests::generatedHeaderMatchesEveryPinnedSourceFileHash() {
  // Same independent drift check as above, but for EVERY individual
  // pinned per-locale source file, not just the manifest that references
  // them -- catching an edit/tamper to e.g. contracts/asset-locale-digest-
  // sources/fr.json that the manifest's own hash alone would not detect.
  const QHash<QString, QString> pinnedHashes =
      AssetLocaleDigest::pinnedSourceFileSha256();
  QCOMPARE(QSet<QString>(pinnedHashes.keyBegin(), pinnedHashes.keyEnd()),
           expectedWebLocales());

  for (auto it = pinnedHashes.constBegin(); it != pinnedHashes.constEnd();
       ++it) {
    const QByteArray bytes =
        readContractsFile(QStringLiteral("asset-locale-digest-sources/") +
                          it.key() + QStringLiteral(".json"));
    const QString recomputed = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    QCOMPARE(recomputed, it.value());
  }
}

void AssetLocaleDigestTests::manifestDeclaresExactlyTheExpectedLocaleSet() {
  // Detects an "unregistered source/output addition": the manifest's
  // localeMap values, its provenance.sourceFiles keys, and the actual set
  // of files physically present in contracts/asset-locale-digest-sources/
  // must all independently agree with the exact expected set -- a stray
  // extra source file dropped in without updating the manifest (or vice
  // versa) fails here even before the generator itself would catch it.
  const QJsonObject manifest = readManifest();

  const QJsonObject localeMap =
      manifest.value(QStringLiteral("localeMap")).toObject();
  QSet<QString> localeMapValues;
  for (const auto &value : localeMap) {
    localeMapValues.insert(value.toString());
  }
  QCOMPARE(localeMapValues, expectedWebLocales());

  const QJsonObject provenance =
      manifest.value(QStringLiteral("provenance")).toObject();
  const QJsonObject sourceFiles =
      provenance.value(QStringLiteral("sourceFiles")).toObject();
  QSet<QString> sourceFileKeys;
  for (auto it = sourceFiles.constBegin(); it != sourceFiles.constEnd(); ++it) {
    sourceFileKeys.insert(it.key());
  }
  QCOMPARE(sourceFileKeys, expectedWebLocales());

  const QDir sourcesDir(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) +
                        QStringLiteral("/asset-locale-digest-sources"));
  QSet<QString> onDiskLocales;
  for (const QString &fileName : sourcesDir.entryList(
           QStringList{QStringLiteral("*.json")}, QDir::Files)) {
    onDiskLocales.insert(fileName.chopped(QStringLiteral(".json").size()));
  }
  QCOMPARE(onDiskLocales, expectedWebLocales());
}

void AssetLocaleDigestTests::webLocaleForKnownMappings() {
  QCOMPARE(AssetLocaleDigest::webLocaleFor(QStringLiteral("it")),
           QStringLiteral("ita"));
  QCOMPARE(AssetLocaleDigest::webLocaleFor(QStringLiteral("fr")),
           QStringLiteral("fr"));
  QCOMPARE(AssetLocaleDigest::webLocaleFor(QStringLiteral("es")),
           QStringLiteral("es"));
  QCOMPARE(AssetLocaleDigest::webLocaleFor(QStringLiteral("ko")),
           QStringLiteral("ko"));
  QCOMPARE(AssetLocaleDigest::webLocaleFor(QStringLiteral("zh")),
           QStringLiteral("zh"));
}

void AssetLocaleDigestTests::webLocaleForUnmappedReturnsEmpty() {
  QVERIFY(AssetLocaleDigest::webLocaleFor(QStringLiteral("de")).isEmpty());
  QVERIFY(AssetLocaleDigest::webLocaleFor(QStringLiteral("en")).isEmpty());
  QVERIFY(AssetLocaleDigest::webLocaleFor(QString()).isEmpty());
  // Case-sensitive: the mapping is defined only for lowercase ISO codes.
  QVERIFY(AssetLocaleDigest::webLocaleFor(QStringLiteral("IT")).isEmpty());
}

// Every row below is a real entry sampled directly from the pinned
// upstream source files (see contracts/asset-locale-digest-sources/), not
// invented -- e.g. "01001b" (the Back side of card 01001) is genuinely
// present for fr/zh but genuinely ABSENT for ita/es despite "01001"
// (Front) being present in all four, proving the digest is keyed
// precisely by the exact resolved art code and not merely by a coarser
// per-identifier or per-locale existence check. Every non-empty locale
// (ita/fr/es/zh) gets at least one present AND one absent sample; ko is
// sampled as always-absent (it is genuinely empty upstream), directly
// exercising the "sample present/absent across every locale" requirement.
void AssetLocaleDigestTests::
    hasLocalizedVariantSampledAcrossEveryLocale_data() {
  QTest::addColumn<QString>("webLocale");
  QTest::addColumn<QString>("artCode");
  QTest::addColumn<bool>("expectPresent");

  QTest::newRow("ita-01001-front-present")
      << QStringLiteral("ita") << QStringLiteral("01001") << true;
  QTest::newRow("ita-01001b-back-absent")
      << QStringLiteral("ita") << QStringLiteral("01001b") << false;
  QTest::newRow("ita-03276ab-resolved-override-present")
      << QStringLiteral("ita") << QStringLiteral("03276ab") << true;

  QTest::newRow("fr-01001-front-present")
      << QStringLiteral("fr") << QStringLiteral("01001") << true;
  QTest::newRow("fr-01001b-back-present")
      << QStringLiteral("fr") << QStringLiteral("01001b") << true;
  QTest::newRow("fr-01514-mutated19-mutated-present")
      << QStringLiteral("fr") << QStringLiteral("01514_Mutated19") << true;
  QTest::newRow("fr-99999-unknown-absent")
      << QStringLiteral("fr") << QStringLiteral("99999") << false;

  QTest::newRow("es-01001-front-present")
      << QStringLiteral("es") << QStringLiteral("01001") << true;
  QTest::newRow("es-01001b-back-absent")
      << QStringLiteral("es") << QStringLiteral("01001b") << false;

  QTest::newRow("zh-01001-front-present")
      << QStringLiteral("zh") << QStringLiteral("01001") << true;
  QTest::newRow("zh-01001b-back-present")
      << QStringLiteral("zh") << QStringLiteral("01001b") << true;
  QTest::newRow("zh-03276ab-resolved-override-absent")
      << QStringLiteral("zh") << QStringLiteral("03276ab") << false;

  QTest::newRow("ko-01001-front-absent-empty-digest")
      << QStringLiteral("ko") << QStringLiteral("01001") << false;
  QTest::newRow("ko-99999-unknown-absent")
      << QStringLiteral("ko") << QStringLiteral("99999") << false;
}

void AssetLocaleDigestTests::hasLocalizedVariantSampledAcrossEveryLocale() {
  QFETCH(QString, webLocale);
  QFETCH(QString, artCode);
  QFETCH(bool, expectPresent);

  QCOMPARE(AssetLocaleDigest::hasLocalizedVariant(webLocale,
                                                  AssetCategory::Card, artCode),
           expectPresent);
}

void AssetLocaleDigestTests::hasLocalizedVariantUnknownEntriesAreFalse() {
  QVERIFY(!AssetLocaleDigest::hasLocalizedVariant(
      QString(), AssetCategory::Card, QStringLiteral("01001")));
  // Right webLocale/artCode, wrong category: the digest only ever
  // contains Card entries (see contracts/asset-locale-digest.json's
  // acceptedCategoryRoots), so any other category is always absent.
  QVERIFY(!AssetLocaleDigest::hasLocalizedVariant(QStringLiteral("ita"),
                                                  AssetCategory::HomebrewCard,
                                                  QStringLiteral("01001")));
  QVERIFY(!AssetLocaleDigest::hasLocalizedVariant(
      QStringLiteral("ita"), AssetCategory::InvestigatorPortrait,
      QStringLiteral("01001")));
  // An unmapped webLocale string (not one of ita/fr/es/ko/zh) is always
  // absent, never accidentally matched against some other locale's data.
  QVERIFY(!AssetLocaleDigest::hasLocalizedVariant(
      QStringLiteral("de"), AssetCategory::Card, QStringLiteral("01001")));
}
