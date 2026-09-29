#include "AssetPngDecoder.h"

#include "AssetTypes.h"

#include <QBuffer>
#include <QImageReader>

namespace Arkham {

namespace {

AssetDecodeOutcome<qint64>
validateImageDimensions(int width, int height, const AssetDecodeLimits &limits,
                        QStringView formatName) {
  if (width <= 0 || height <= 0) {
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("%1 header declares a non-positive dimension")
            .arg(formatName),
    };
  }
  if (width > limits.maxWidth || height > limits.maxHeight) {
    return AssetDecodeError{
        AssetDecodeErrorCode::DimensionTooLarge,
        QStringLiteral("%1 dimension %2x%3 exceeds the configured cap of %4x%5")
            .arg(formatName)
            .arg(width)
            .arg(height)
            .arg(limits.maxWidth)
            .arg(limits.maxHeight),
    };
  }
  const qint64 totalPixels =
      static_cast<qint64>(width) * static_cast<qint64>(height);
  if (totalPixels > limits.maxPixelCount) {
    return AssetDecodeError{
        AssetDecodeErrorCode::PixelBudgetExceeded,
        QStringLiteral(
            "%1 totals %2 pixels, exceeding the configured cap of %3")
            .arg(formatName)
            .arg(totalPixels)
            .arg(limits.maxPixelCount),
    };
  }
  return totalPixels;
}

} // namespace

AssetDecodeOutcome<QImage> decodePngImage(const QByteArray &encodedBytes,
                                          const AssetDecodeLimits &limits) {
  QBuffer buffer;
  buffer.setData(encodedBytes);
  if (!buffer.open(QIODevice::ReadOnly)) {
    return AssetDecodeError{AssetDecodeErrorCode::MalformedImage,
                            QStringLiteral("failed to open PNG bytes")};
  }

  QImageReader reader(&buffer, "png");
  reader.setAutoTransform(false);

  const QSize declaredSize = reader.size();
  if (!declaredSize.isValid()) {
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("PNG header could not be read: %1")
            .arg(reader.errorString()),
    };
  }
  const auto headerValidation =
      validateImageDimensions(declaredSize.width(), declaredSize.height(),
                              limits, QStringLiteral("PNG"));
  if (!headerValidation) {
    return headerValidation.error();
  }

  QImage image;
  if (!reader.read(&image) || image.isNull()) {
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("PNG decode failed: %1").arg(reader.errorString()),
    };
  }

  const auto decodedValidation = validateImageDimensions(
      image.width(), image.height(), limits, QStringLiteral("PNG"));
  if (!decodedValidation) {
    return decodedValidation.error();
  }
  return image;
}

} // namespace Arkham
