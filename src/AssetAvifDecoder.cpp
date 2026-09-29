#include "AssetAvifDecoder.h"

#include "AssetTypes.h"

#include <avif/avif.h>

#include <cstdint>

namespace Arkham {

namespace {

AssetDecodeErrorCode errorCodeForAvifResult(avifResult result) {
  switch (result) {
  case AVIF_RESULT_NO_CODEC_AVAILABLE:
  case AVIF_RESULT_NOT_IMPLEMENTED:
  case AVIF_RESULT_UNSUPPORTED_DEPTH:
#if AVIF_VERSION >= 1000000
  case AVIF_RESULT_MISSING_IMAGE_ITEM:
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
