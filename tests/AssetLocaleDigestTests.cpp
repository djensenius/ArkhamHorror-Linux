#include "AssetLocaleDigestTests.h"

#include "AssetLocaleDigest.h"

#include <QCryptographicHash>
#include <QFile>
#include <QTest>

using namespace Arkham;

void AssetLocaleDigestTests::generatedHeaderMatchesPinnedSourceJsonHash() {
  // Independent, C++-only drift check: recompute the raw-byte SHA-256 of
  // the checked-in contracts/asset-locale-digest.json and compare it
  // against the hash tools/generate_asset_locale_digest.py embedded in the
  // generated header. This must never depend on Python being available at
  // test time.
  QFile file(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) +
             QStringLiteral("/asset-locale-digest.json"));
  QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
  const QByteArray bytes = file.readAll();
  const QString recomputed = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());

  QCOMPARE(recomputed, AssetLocaleDigest::pinnedSourceJsonSha256());
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

void AssetLocaleDigestTests::hasLocalizedVariantKnownEntries() {
  QVERIFY(AssetLocaleDigest::hasLocalizedVariant(
      QStringLiteral("ita"), AssetCategory::Card, QStringLiteral("01001"),
      AssetSide::Front));
  QVERIFY(AssetLocaleDigest::hasLocalizedVariant(
      QStringLiteral("ita"), AssetCategory::Card, QStringLiteral("01001"),
      AssetSide::Back));
  QVERIFY(AssetLocaleDigest::hasLocalizedVariant(
      QStringLiteral("fr"), AssetCategory::Card, QStringLiteral("01001"),
      AssetSide::Front));
  QVERIFY(AssetLocaleDigest::hasLocalizedVariant(
      QStringLiteral("ita"), AssetCategory::Card, QStringLiteral("01002"),
      AssetSide::ResolvedFront));
  QVERIFY(AssetLocaleDigest::hasLocalizedVariant(
      QStringLiteral("ita"), AssetCategory::Card, QStringLiteral("01003"),
      AssetSide::MutatedFront));
}

void AssetLocaleDigestTests::hasLocalizedVariantUnknownEntriesAreFalse() {
  // Right locale/category/identifier, wrong side.
  QVERIFY(!AssetLocaleDigest::hasLocalizedVariant(
      QStringLiteral("ita"), AssetCategory::Card, QStringLiteral("01002"),
      AssetSide::Back));
  // Right locale/category/side, unknown identifier entirely.
  QVERIFY(!AssetLocaleDigest::hasLocalizedVariant(
      QStringLiteral("ita"), AssetCategory::Card, QStringLiteral("99999"),
      AssetSide::Front));
  // "01001" has no ko/zh digest entry for HomebrewCard.
  QVERIFY(!AssetLocaleDigest::hasLocalizedVariant(
      QStringLiteral("ko"), AssetCategory::HomebrewCard,
      QStringLiteral("01001"), AssetSide::Front));
  // "01001" back has no zh entry (only ita/fr have a back entry for it).
  QVERIFY(!AssetLocaleDigest::hasLocalizedVariant(
      QStringLiteral("zh"), AssetCategory::Card, QStringLiteral("01001"),
      AssetSide::Back));
}
