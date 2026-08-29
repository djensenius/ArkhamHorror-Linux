#include "AssetAvifDecoder.h"

#include <avif/avif.h>

namespace Arkham {

namespace {

// Every avifResult this project can plausibly see, mapped to one of the two
// AssetErrorCode values decodeAndValidate() already surfaces for the
// JPEG/PNG path: UnsupportedCodec for a structural "this build's decode
// backend cannot do this" outcome, MalformedImage for anything that
// indicates corrupt/truncated/invalid data. There is no finer-grained
// third bucket here deliberately -- every caller of AssetNetworkFetcher
// only ever branches on UnsupportedCodec vs "any other failure" (see
// AssetTypes.h's AssetErrorCode comment and AssetRequestCoordinator's
// quarantine handling), so a finer split would have no observable effect.
AssetErrorCode errorCodeForAvifResult(avifResult result) {
  switch (result) {
  case AVIF_RESULT_NO_CODEC_AVAILABLE:
  case AVIF_RESULT_NOT_IMPLEMENTED:
  case AVIF_RESULT_UNSUPPORTED_DEPTH:
    return AssetErrorCode::UnsupportedCodec;
  default:
    return AssetErrorCode::MalformedImage;
  }
}

} // namespace

AssetOutcome<QImage> decodeAvifImage(const QByteArray &encodedBytes,
                                     int maxDimensionPixels,
                                     qint64 maxTotalPixels) {
  avifDecoder *decoder = avifDecoderCreate();
  if (!decoder) {
    return AssetOutcome<QImage>(
        AssetError{AssetErrorCode::MalformedImage,
                   QStringLiteral("libavif decoder allocation failed")});
  }

  // Never spin up worker threads to decode a single small card-art image --
  // a resource-exhaustion bound independent of the dimension/pixel-count
  // checks below.
  decoder->maxThreads = 1;
  // Deliberately leave decoder->imageDimensionLimit/imageSizeLimit at
  // libavif's own built-in defaults (32768 per side / 16384*16384 total)
  // here, rather than tightening them to this project's own (much
  // smaller, e.g. 8192/32-megapixel) configured limits: avifDecoderParse()
  // only ever reads container metadata (it never allocates a
  // full-resolution pixel buffer), so libavif's generous defaults are
  // already a sufficient backstop against a truly pathological declared
  // size overflowing libavif's own internal arithmetic during Parse.
  // Reusing this project's tighter, *configurable* limit for libavif's
  // own field instead would make avifDecoderParse() itself reject a
  // too-large-for-us-but-fine-for-libavif image with a generic,
  // untyped parse-failure result -- pre-empting this function's own
  // explicit post-Parse check just below, which is what actually
  // produces the precise, typed AssetErrorCode::DimensionTooLarge /
  // PixelBudgetExceeded this project's callers depend on.

  const auto *data =
      reinterpret_cast<const uint8_t *>(encodedBytes.constData());
  avifResult result = avifDecoderSetIOMemory(
      decoder, data, static_cast<size_t>(encodedBytes.size()));
  if (result != AVIF_RESULT_OK) {
    const AssetErrorCode code = errorCodeForAvifResult(result);
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(AssetError{
        code,
        QStringLiteral("libavif failed to accept the encoded AVIF bytes: %1")
            .arg(QString::fromLatin1(avifResultToString(result)))});
  }

  result = avifDecoderParse(decoder);
  if (result != AVIF_RESULT_OK) {
    const AssetErrorCode code = errorCodeForAvifResult(result);
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(AssetError{
        code, QStringLiteral("libavif failed to parse the AVIF container: %1")
                  .arg(QString::fromLatin1(avifResultToString(result)))});
  }

  // avifDecoderParse() has populated decoder->image's container-declared
  // dimensions from metadata (an ISOBMFF `ispe` box) alone -- no AV1
  // payload has been decoded, and no full-resolution pixel buffer has been
  // allocated, at this point. See the header comment: this check runs
  // BEFORE avifDecoderNextImage() below, which is what actually performs
  // the AV1 decode and allocates the full pixel buffer.
  const uint32_t width = decoder->image->width;
  const uint32_t height = decoder->image->height;
  if (width == 0 || height == 0) {
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("AVIF container declares a non-positive dimension")});
  }
  if (width > static_cast<uint32_t>(maxDimensionPixels) ||
      height > static_cast<uint32_t>(maxDimensionPixels)) {
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::DimensionTooLarge,
        QStringLiteral("AVIF dimension %1x%2 exceeds the configured cap of "
                       "%3 pixels per side")
            .arg(width)
            .arg(height)
            .arg(maxDimensionPixels)});
  }
  // Both operands are already bounded above by maxDimensionPixels (a
  // configurable but always-small int), so this can never overflow.
  const qint64 totalPixels =
      static_cast<qint64>(width) * static_cast<qint64>(height);
  if (totalPixels > maxTotalPixels) {
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::PixelBudgetExceeded,
        QStringLiteral("AVIF totals %1 pixels, exceeding the configured cap "
                       "of %2")
            .arg(totalPixels)
            .arg(maxTotalPixels)});
  }

  // Only now -- strictly after both dimension checks above have already
  // passed -- does the real AV1 decode (and its full-resolution pixel
  // buffer allocation) happen.
  result = avifDecoderNextImage(decoder);
  if (result != AVIF_RESULT_OK) {
    const AssetErrorCode code = errorCodeForAvifResult(result);
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(AssetError{
        code, QStringLiteral("libavif failed to decode the AVIF image: %1")
                  .arg(QString::fromLatin1(avifResultToString(result)))});
  }

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, decoder->image);
  // Always convert to plain 8-bit interleaved RGBA, regardless of the
  // source AVIF's own bit depth (8/10/12-bit) -- libavif performs the
  // depth rescale during avifImageYUVToRGB() below -- so the result is
  // always constructible as a QImage::Format_RGBA8888 buffer.
  rgb.depth = 8;
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  rgb.maxThreads = 1;

  result = avifRGBImageAllocatePixels(&rgb);
  if (result != AVIF_RESULT_OK) {
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("failed to allocate the RGB conversion buffer: %1")
            .arg(QString::fromLatin1(avifResultToString(result)))});
  }

  result = avifImageYUVToRGB(decoder->image, &rgb);
  if (result != AVIF_RESULT_OK) {
    const AssetErrorCode code = errorCodeForAvifResult(result);
    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(AssetError{
        code, QStringLiteral("libavif failed to convert YUV to RGB: %1")
                  .arg(QString::fromLatin1(avifResultToString(result)))});
  }

  // Deep copy: rgb.pixels is about to be freed below, and decoder is about
  // to be destroyed, so the returned QImage must own its own buffer, never
  // alias libavif's (about-to-be-invalid) one.
  const QImage view(rgb.pixels, static_cast<int>(rgb.width),
                    static_cast<int>(rgb.height),
                    static_cast<int>(rgb.rowBytes), QImage::Format_RGBA8888);
  QImage owned = view.copy();

  avifRGBImageFreePixels(&rgb);
  avifDecoderDestroy(decoder);

  if (owned.isNull()) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("AVIF decode succeeded but produced an empty image")});
  }
  return AssetOutcome<QImage>(std::move(owned));
}

} // namespace Arkham
