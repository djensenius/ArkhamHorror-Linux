#include "AssetTypes.h"

#include <QtAssert>

namespace Arkham {

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
