#include "AssetTypes.h"

#include "AssetAvifDecoder.h"
#include "AssetJpegDecoder.h"
#include "AssetPngDecoder.h"

#include <cstring>

namespace Arkham {

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

AssetDecodeOutcome<bool> validateLimits(const AssetDecodeLimits &limits) {
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

} // namespace

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

AssetDecodeOutcome<QImage> decodeAssetImage(const QByteArray &encodedBytes,
                                            const AssetDecodeLimits &limits) {
  const auto limitsValidation = validateLimits(limits);
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

  switch (*format) {
  case AssetImageFormat::Png:
    return decodePngImage(encodedBytes, limits);
  case AssetImageFormat::Jpeg:
    return decodeJpegImage(encodedBytes, limits);
  case AssetImageFormat::Avif:
    return decodeAvifImage(encodedBytes, limits);
  }
  return AssetDecodeError{AssetDecodeErrorCode::UnrecognizedFormat,
                          QStringLiteral("unhandled asset image format")};
}

} // namespace Arkham
