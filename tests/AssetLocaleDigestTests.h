#pragma once

#include <QObject>

// Tests for AssetLocaleDigest: the pinned locale map, digest lookup
// behaviour (keyed by resolved art code, not decomposed identifier+side),
// and drift detection between contracts/asset-locale-digest.json plus its
// pinned per-locale source files (contracts/asset-locale-digest-sources/)
// and the generated header (src/AssetLocaleDigestData.generated.h)
// produced from them by tools/generate_asset_locale_digest.py.
class AssetLocaleDigestTests final : public QObject {
  Q_OBJECT

private slots:
  void generatedHeaderMatchesPinnedManifestHash();
  void generatedHeaderMatchesEveryPinnedSourceFileHash();
  void manifestDeclaresExactlyTheExpectedLocaleSet();

  void webLocaleForKnownMappings();
  void webLocaleForUnmappedReturnsEmpty();

  void hasLocalizedVariantSampledAcrossEveryLocale_data();
  void hasLocalizedVariantSampledAcrossEveryLocale();

  void hasLocalizedVariantUnknownEntriesAreFalse();
};
