#include "AssetTypes.h"

#include "AssetAvifDecoder.h"
#include "AssetJpegDecoder.h"
#include "AssetPngDecoder.h"

#include <cstring>

namespace Arkham {

AssetDecodeOutcome<QImage> decodePngImage(const QByteArray &encodedBytes,
                                          const AssetDecodeLimits &limits);
AssetDecodeOutcome<QImage> decodeJpegImage(const QByteArray &encodedBytes,
                                           const AssetDecodeLimits &limits,
                                           const QSize &bound);
AssetDecodeOutcome<QImage> decodeAvifImage(const QByteArray &encodedBytes,
                                           const AssetDecodeLimits &limits);

namespace {

bool hasPrefix(const QByteArray &bytes, const unsigned char *prefix,
               qsizetype prefixSize) {
  return bytes.size() >= prefixSize &&
         std::memcmp(bytes.constData(), prefix,
                     static_cast<size_t>(prefixSize)) == 0;
}

bool hasAvifBrand(const QByteArray &bytes) {
  if (bytes.size() < 16 || std::memcmp(bytes.constData() + 4, "ftyp", 4) != 0) {
    return false;
  }

  const auto *data = reinterpret_cast<const unsigned char *>(bytes.constData());
  const quint32 boxSize = (static_cast<quint32>(data[0]) << 24) |
                          (static_cast<quint32>(data[1]) << 16) |
                          (static_cast<quint32>(data[2]) << 8) |
                          static_cast<quint32>(data[3]);
  if (boxSize < 16 || boxSize > static_cast<quint32>(bytes.size())) {
    return false;
  }

  auto isAvifBrand = [](const char *brand) {
    return std::memcmp(brand, "avif", 4) == 0 ||
           std::memcmp(brand, "avis", 4) == 0;
  };
  if (isAvifBrand(bytes.constData() + 8)) {
    return true;
  }
  for (qsizetype offset = 16; offset + 4 <= static_cast<qsizetype>(boxSize);
       offset += 4) {
    if (isAvifBrand(bytes.constData() + offset)) {
      return true;
    }
  }
  return false;
}

} // namespace

AssetDecodeOutcome<bool>
validateAssetDecodeLimits(const AssetDecodeLimits &limits) {
  if (limits.maxEncodedBytes <= 0 || limits.maxWidth <= 0 ||
      limits.maxHeight <= 0 || limits.maxPixelCount <= 0 ||
      limits.maxProgressiveJpegScans <= 0) {
    return AssetDecodeError{
        AssetDecodeErrorCode::InvalidLimits,
        QStringLiteral("asset decode limits must all be positive"),
    };
  }
  return true;
}

AssetDecodeOutcome<AssetImageFormat>
detectAssetImageFormat(const QByteArray &encodedBytes) {
  static constexpr unsigned char pngSignature[] = {0x89, 'P',  'N',  'G',
                                                   '\r', '\n', 0x1A, '\n'};
  static constexpr unsigned char jpegSignature[] = {0xFF, 0xD8, 0xFF};

  if (hasPrefix(encodedBytes, pngSignature, sizeof(pngSignature))) {
    return AssetImageFormat::Png;
  }
  if (hasPrefix(encodedBytes, jpegSignature, sizeof(jpegSignature))) {
    return AssetImageFormat::Jpeg;
  }
  if (hasAvifBrand(encodedBytes)) {
    return AssetImageFormat::Avif;
  }
  return AssetDecodeError{
      AssetDecodeErrorCode::UnrecognizedFormat,
      QStringLiteral("asset bytes do not match PNG, JPEG, or AVIF magic")};
}

QSize normalizedAssetDecodeBound(const QSize &bound) {
  if (!bound.isValid() || bound.isEmpty()) {
    return {};
  }
  return bound;
}

QSize fittedAssetDecodeSize(const QSize &sourceSize, const QSize &bound) {
  const QSize normalized = normalizedAssetDecodeBound(bound);
  if (!normalized.isValid() || sourceSize.isEmpty() ||
      (sourceSize.width() <= normalized.width() &&
       sourceSize.height() <= normalized.height())) {
    return sourceSize;
  }
  return sourceSize.scaled(normalized, Qt::KeepAspectRatio)
      .expandedTo(QSize(1, 1));
}

namespace {

AssetDecodeOutcome<QImage> decodeFullOrHinted(const QByteArray &encodedBytes,
                                              AssetImageFormat format,
                                              const AssetDecodeLimits &limits,
                                              const QSize &bound) {
  switch (format) {
  case AssetImageFormat::Png:
    return decodePngImage(encodedBytes, limits);
  case AssetImageFormat::Jpeg:
    return decodeJpegImage(encodedBytes, limits, bound);
  case AssetImageFormat::Avif:
    return decodeAvifImage(encodedBytes, limits);
  }
  return AssetDecodeError{AssetDecodeErrorCode::UnrecognizedFormat,
                          QStringLiteral("unhandled asset image format")};
}

} // namespace

AssetDecodeOutcome<QImage> decodeAssetImage(const QByteArray &encodedBytes,
                                            const AssetDecodeLimits &limits,
                                            const QSize &bound) {
  const auto limitsValidation = validateAssetDecodeLimits(limits);
  if (!limitsValidation) {
    return limitsValidation.error();
  }
  if (encodedBytes.size() > limits.maxEncodedBytes) {
    return AssetDecodeError{
        AssetDecodeErrorCode::EncodedBytesTooLarge,
        QStringLiteral(
            "encoded image has %1 bytes, exceeding the configured cap of %2")
            .arg(encodedBytes.size())
            .arg(limits.maxEncodedBytes),
    };
  }

  const auto format = detectAssetImageFormat(encodedBytes);
  if (!format) {
    return format.error();
  }

  auto decoded = decodeFullOrHinted(encodedBytes, *format, limits, bound);
  if (!decoded) {
    return decoded;
  }
  const QSize target = fittedAssetDecodeSize(decoded->size(), bound);
  if (target == decoded->size()) {
    return decoded;
  }
  // Replace the decoded image so only the scaled result stays alive.
  *decoded =
      decoded->scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  if (decoded->isNull()) {
    return AssetDecodeError{
        AssetDecodeErrorCode::AllocationFailed,
        QStringLiteral("failed to allocate the downscaled asset image")};
  }
  return decoded;
}

} // namespace Arkham
