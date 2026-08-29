#pragma once

#include <QObject>

// Tests for AssetLocaleDigest: the pinned locale map, digest lookup
// behaviour, and drift detection between contracts/asset-locale-digest.json
// and the generated header (src/AssetLocaleDigestData.generated.h)
// produced from it by tools/generate_asset_locale_digest.py.
class AssetLocaleDigestTests final : public QObject {
  Q_OBJECT

private slots:
  void generatedHeaderMatchesPinnedSourceJsonHash();
  void webLocaleForKnownMappings();
  void webLocaleForUnmappedReturnsEmpty();
  void hasLocalizedVariantKnownEntries();
  void hasLocalizedVariantUnknownEntriesAreFalse();
};
