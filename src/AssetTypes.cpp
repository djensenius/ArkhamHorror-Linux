#include "AssetTypes.h"

#include "UrlValidator.h"

#include <QtAssert>

namespace Arkham {

AssetOutcome<ValidatedAssetSource>
ValidatedAssetSource::fromRaw(const QString &rawInput) {
  // Deliberately calls validateCustomUrl() with the caller's ORIGINAL raw
  // string -- never a QUrl the caller may have already constructed -- so
  // the raw-authority-only checks inside it (control characters before
  // trimming, ambiguous loopback spellings, Unicode homoglyphs) still have
  // the evidence they need. This is the exact same policy
  // ServerProfile/NetworkAuthenticationClient use; asset bases never fork
  // a weaker interpretation of it.
  const UrlValidationResult validated = validateCustomUrl(rawInput);
  if (!validated) {
    return AssetError{
        AssetErrorCode::InvalidAssetBase,
        validated.error().message,
    };
  }
  return ValidatedAssetSource(*validated);
}

QString assetFormatExtension(AssetFormat format) {
  switch (format) {
  case AssetFormat::Avif:
    return QStringLiteral("avif");
  case AssetFormat::Jpeg:
    return QStringLiteral("jpg");
  case AssetFormat::Png:
    return QStringLiteral("png");
  }
  Q_UNREACHABLE_RETURN(QString());
}

QString assetFormatMimeType(AssetFormat format) {
  switch (format) {
  case AssetFormat::Avif:
    return QStringLiteral("image/avif");
  case AssetFormat::Jpeg:
    return QStringLiteral("image/jpeg");
  case AssetFormat::Png:
    return QStringLiteral("image/png");
  }
  Q_UNREACHABLE_RETURN(QString());
}

} // namespace Arkham
