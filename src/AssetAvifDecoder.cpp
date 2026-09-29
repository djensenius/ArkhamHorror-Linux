#include "AssetAvifDecoder.h"

#include "AssetTypes.h"

#include <avif/avif.h>

#include <cstdint>
#include <cstring>

namespace Arkham {

namespace {

enum class AvifPrimaryItemMetadataState {
  Present,
  Missing,
  Malformed,
};

quint32 readBigEndian32(const char *bytes) {
  const auto *data = reinterpret_cast<const unsigned char *>(bytes);
  return (static_cast<quint32>(data[0]) << 24) |
         (static_cast<quint32>(data[1]) << 16) |
         (static_cast<quint32>(data[2]) << 8) | static_cast<quint32>(data[3]);
}

quint64 readBigEndian64(const char *bytes) {
  const auto *data = reinterpret_cast<const unsigned char *>(bytes);
  quint64 value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<quint64>(data[i]);
  }
  return value;
}

AvifPrimaryItemMetadataState topLevelMetaBoxState(const QByteArray &bytes) {
  qsizetype offset = 0;
  while (offset + 8 <= bytes.size()) {
    quint64 boxSize = readBigEndian32(bytes.constData() + offset);
    qsizetype headerSize = 8;
    if (boxSize == 1) {
      if (offset + 16 > bytes.size()) {
        return AvifPrimaryItemMetadataState::Malformed;
      }
      boxSize = readBigEndian64(bytes.constData() + offset + 8);
      headerSize = 16;
    } else if (boxSize == 0) {
      boxSize = static_cast<quint64>(bytes.size() - offset);
    }

    if (boxSize < static_cast<quint64>(headerSize) ||
        boxSize > static_cast<quint64>(bytes.size() - offset)) {
      return AvifPrimaryItemMetadataState::Malformed;
    }

    if (std::memcmp(bytes.constData() + offset + 4, "meta", 4) == 0) {
      return AvifPrimaryItemMetadataState::Present;
    }
    offset += static_cast<qsizetype>(boxSize);
  }

  if (offset != bytes.size()) {
    return AvifPrimaryItemMetadataState::Malformed;
  }
  return AvifPrimaryItemMetadataState::Missing;
}

AssetDecodeErrorCode errorCodeForAvifResult(avifResult result) {
  switch (result) {
  case AVIF_RESULT_NO_CODEC_AVAILABLE:
  case AVIF_RESULT_NOT_IMPLEMENTED:
  case AVIF_RESULT_UNSUPPORTED_DEPTH:
#if AVIF_VERSION >= 1000000
  case AVIF_RESULT_MISSING_IMAGE_ITEM:
#else
  case AVIF_RESULT_NO_AV1_ITEMS_FOUND:
#endif
  case AVIF_RESULT_NO_IMAGES_REMAINING:
  case AVIF_RESULT_NO_CONTENT:
    return AssetDecodeErrorCode::UnsupportedCodec;
  default:
    return AssetDecodeErrorCode::MalformedImage;
  }
}

AssetDecodeOutcome<qint64>
validateAvifDimensions(uint32_t width, uint32_t height,
                       const AssetDecodeLimits &limits) {
  if (width == 0 || height == 0) {
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("AVIF declares a non-positive dimension")};
  }
  if (width > static_cast<uint32_t>(limits.maxWidth) ||
      height > static_cast<uint32_t>(limits.maxHeight)) {
    return AssetDecodeError{
        AssetDecodeErrorCode::DimensionTooLarge,
        QStringLiteral(
            "AVIF dimension %1x%2 exceeds the configured cap of %3x%4")
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
            "AVIF totals %1 pixels, exceeding the configured cap of %2")
            .arg(totalPixels)
            .arg(limits.maxPixelCount),
    };
  }
  return totalPixels;
}

} // namespace

AssetDecodeOutcome<QImage> decodeAvifImage(const QByteArray &encodedBytes,
                                           const AssetDecodeLimits &limits) {
  const AvifPrimaryItemMetadataState metadataState =
      topLevelMetaBoxState(encodedBytes);
  if (metadataState == AvifPrimaryItemMetadataState::Missing) {
    return AssetDecodeError{
        AssetDecodeErrorCode::UnsupportedCodec,
        QStringLiteral(
            "AVIF payload does not contain a primary item metadata box")};
  }

  avifDecoder *decoder = avifDecoderCreate();
  if (!decoder) {
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("libavif decoder allocation failed")};
  }

  decoder->maxThreads = 1;
  decoder->requestedSource = AVIF_DECODER_SOURCE_PRIMARY_ITEM;

  avifResult result = avifDecoderSetIOMemory(
      decoder, reinterpret_cast<const uint8_t *>(encodedBytes.constData()),
      static_cast<size_t>(encodedBytes.size()));
  if (result != AVIF_RESULT_OK) {
    const AssetDecodeErrorCode code = errorCodeForAvifResult(result);
    avifDecoderDestroy(decoder);
    return AssetDecodeError{
        code,
        QStringLiteral("libavif failed to accept AVIF bytes: %1")
            .arg(QString::fromLatin1(avifResultToString(result))),
    };
  }

  result = avifDecoderParse(decoder);
  if (result != AVIF_RESULT_OK) {
    const AssetDecodeErrorCode code = errorCodeForAvifResult(result);
    avifDecoderDestroy(decoder);
    return AssetDecodeError{
        code,
        QStringLiteral("libavif failed to parse primary AVIF item: %1")
            .arg(QString::fromLatin1(avifResultToString(result))),
    };
  }

  if (!decoder->image) {
    avifDecoderDestroy(decoder);
    return AssetDecodeError{
        AssetDecodeErrorCode::UnsupportedCodec,
        QStringLiteral("AVIF payload does not contain a primary still image")};
  }

  const auto preDecodeValidation = validateAvifDimensions(
      decoder->image->width, decoder->image->height, limits);
  if (!preDecodeValidation) {
    const AssetDecodeError error = preDecodeValidation.error();
    avifDecoderDestroy(decoder);
    return error;
  }

  result = avifDecoderNextImage(decoder);
  if (result != AVIF_RESULT_OK) {
    const AssetDecodeErrorCode code = errorCodeForAvifResult(result);
    avifDecoderDestroy(decoder);
    return AssetDecodeError{
        code,
        QStringLiteral("libavif failed to decode AVIF primary item: %1")
            .arg(QString::fromLatin1(avifResultToString(result))),
    };
  }

  const auto postDecodeValidation = validateAvifDimensions(
      decoder->image->width, decoder->image->height, limits);
  if (!postDecodeValidation) {
    const AssetDecodeError error = postDecodeValidation.error();
    avifDecoderDestroy(decoder);
    return error;
  }

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, decoder->image);
  rgb.depth = 8;
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  (void)avifRGBImageAllocatePixels(&rgb);
  if (!rgb.pixels) {
    avifDecoderDestroy(decoder);
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("failed to allocate AVIF RGB buffer")};
  }

  result = avifImageYUVToRGB(decoder->image, &rgb);
  if (result != AVIF_RESULT_OK) {
    const AssetDecodeErrorCode code = errorCodeForAvifResult(result);
    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(decoder);
    return AssetDecodeError{
        code,
        QStringLiteral("libavif failed to convert AVIF pixels to RGB: %1")
            .arg(QString::fromLatin1(avifResultToString(result))),
    };
  }

  const QImage view(rgb.pixels, static_cast<int>(rgb.width),
                    static_cast<int>(rgb.height),
                    static_cast<int>(rgb.rowBytes), QImage::Format_RGBA8888);
  QImage owned = view.copy();

  avifRGBImageFreePixels(&rgb);
  avifDecoderDestroy(decoder);

  if (owned.isNull()) {
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("AVIF decode produced an empty image")};
  }
  return owned;
}

} // namespace Arkham
