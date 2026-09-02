#include "AssetAvifDecoder.h"

#include <avif/avif.h>

#include <algorithm>
#include <limits>

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

// Validates a candidate width/height pair against the caller's configured
// caps, returning the (already-overflow-safe) total pixel count on
// success or a typed AssetError on the first violated rule. Shared by
// BOTH the pre-decode (container-metadata-only) check and the post-decode
// (actual-decoded-buffer) recheck below, so the two call sites can never
// silently drift apart (e.g. one gaining a rule the other lacks).
AssetOutcome<qint64> validateAvifDimensions(uint32_t width, uint32_t height,
                                            int maxDimensionPixels,
                                            qint64 maxTotalPixels) {
  if (width == 0 || height == 0) {
    return AssetOutcome<qint64>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("AVIF container declares a non-positive dimension")});
  }
  if (width > static_cast<uint32_t>(maxDimensionPixels) ||
      height > static_cast<uint32_t>(maxDimensionPixels)) {
    return AssetOutcome<qint64>(AssetError{
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
    return AssetOutcome<qint64>(AssetError{
        AssetErrorCode::PixelBudgetExceeded,
        QStringLiteral("AVIF totals %1 pixels, exceeding the configured cap "
                       "of %2")
            .arg(totalPixels)
            .arg(maxTotalPixels)});
  }
  return AssetOutcome<qint64>(totalPixels);
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

  // Cumulative review (PR #18): this project only ever serves/decodes a
  // single still AVIF image, never a sequence/animation. Setting
  // imageCountLimit=1 BEFORE avifDecoderParse() (rather than only
  // discovering decoder->imageCount != 1 AFTER Parse has already fully
  // walked and allocated structures for every declared sample/item) makes
  // libavif itself abort enumerating a hostile container's sample table /
  // item list the moment a second image is discovered, bounding Parse's
  // own internal cost against a declared-count DoS (a container claiming
  // millions of images) rather than only rejecting the *result* after
  // libavif already paid to process all of them. Confirmed empirically
  // against this project's exact pinned libavif 0.9.3 (Ubuntu 22.04):
  // Parse() itself now fails with AVIF_RESULT_BMFF_PARSE_FAILED for any
  // container declaring more than one image, and decoder->imageCount is
  // never populated in that case -- see the imageCount!=1 handling below,
  // which is retained as a secondary, defense-in-depth guard for any
  // structural variant (if one exists) where libavif's own
  // imageCountLimit enforcement does not apply but Parse still succeeds
  // with imageCount>1.
  decoder->imageCountLimit = 1;

  // Cumulative review (PR #18): imageSizeLimit is libavif's OWN total
  // (width*height) pixel budget, enforced not only against
  // container-declared (ispe) metadata during Parse, but -- for codec
  // backends that support it (confirmed empirically for this project's
  // exact bundled/pinned dav1d backend against libavif 0.9.3) -- also
  // against the ACTUAL decoded AV1 frame's own internal dimensions
  // *before* any post-decode rescale to the container-declared size. This
  // closes a real gap this project's own post-Parse dimension check
  // (below) cannot see: a hostile AVIF can declare a tiny `ispe` (passing
  // that check trivially) while its embedded AV1 bitstream itself encodes
  // a far larger frame -- the underlying codec must otherwise fully
  // allocate/decode that oversized internal frame before
  // avifDecoderNextImage() ever rescales the result back down to the
  // small declared size, by which point the resource cost has already
  // been paid and this function's own post-decode dimensions would appear
  // deceptively small. Setting imageSizeLimit to this project's own
  // configured maxTotalPixels cap (rather than leaving libavif's much
  // looser built-in default of 268,435,456) means the codec itself
  // refuses to decode that oversized internal frame at all. 0 is
  // libavif's own documented "reserved" (invalid) sentinel for this
  // field, so the clamp below never produces it.
  const uint32_t clampedImageSizeLimit =
      static_cast<uint32_t>(std::clamp<qint64>(
          maxTotalPixels, 1, std::numeric_limits<uint32_t>::max()));
  decoder->imageSizeLimit = clampedImageSizeLimit;

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

  // Review item 6 / round-4 item 10: this project only ever serves/
  // decodes a SINGLE still AVIF image (the "avif" major/compatible
  // brand, `imageCount == 1`). An animated/sequence AVIF ("avis", or a
  // still-image container that nonetheless declares more than one coded
  // image) is rejected outright -- decoding only its first frame and
  // silently discarding the rest would misrepresent a multi-frame asset
  // as if it were the single canonical image, which the CDN never
  // actually serves for card art but a hostile/misconfigured server
  // could still attempt to send. `imageCount` is authoritative only
  // after avifDecoderParse() has already succeeded, so this check is
  // placed strictly after it (and, like the dimension check just below,
  // strictly before avifDecoderNextImage() ever decodes a single pixel).
  //
  // Classified as MalformedImage, NOT UnsupportedCodec: this build's
  // libavif backend is fully capable of decoding this bytestream -- the
  // stream itself violates this project's single-still-image contract,
  // which is an integrity/content-policy failure, not a "this runtime
  // lacks a decoder for this format" one. AssetRequestCoordinator's
  // quarantine-and-refetch handling (review round-4 item 9/10) treats
  // MalformedImage as a corrupt/self-inconsistent disk/network entry
  // eligible for quarantine + a single retry as a network miss, whereas
  // UnsupportedCodec is reserved for a genuine, permanent, build-wide
  // decoder-absence outcome that quarantining a specific cached entry
  // could never resolve by retrying.
  //
  // In practice, decoder->imageCountLimit=1 (set above) now makes
  // avifDecoderParse() itself fail with AVIF_RESULT_BMFF_PARSE_FAILED for
  // any container declaring more than one image -- decoder->imageCount is
  // never populated to a value other than 0 or 1 in that case, so this
  // block is unreachable via that path (Parse's own error branch above
  // already returned). It is retained as a secondary, defense-in-depth
  // guard in case any future libavif version or structural container
  // variant ever lets Parse succeed with imageCount>1 despite
  // imageCountLimit=1.
  if (decoder->imageCount != 1) {
    const int imageCount = decoder->imageCount;
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("AVIF image sequences/animations (imageCount=%1) "
                       "are not supported; only a single still image is "
                       "accepted")
            .arg(imageCount)});
  }

  // avifDecoderParse() has populated decoder->image's container-declared
  // dimensions from metadata (an ISOBMFF `ispe` box) alone -- no AV1
  // payload has been decoded, and no full-resolution pixel buffer has been
  // allocated, at this point. See the header comment: this check runs
  // BEFORE avifDecoderNextImage() below, which is what actually performs
  // the AV1 decode and allocates the full pixel buffer.
  const AssetOutcome<qint64> preDecodeValidation =
      validateAvifDimensions(decoder->image->width, decoder->image->height,
                             maxDimensionPixels, maxTotalPixels);
  if (!preDecodeValidation) {
    const AssetError error = preDecodeValidation.error();
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(error);
  }

  // Only now -- strictly after the pre-decode dimension check above has
  // already passed -- does the real AV1 decode (and its full-resolution
  // pixel buffer allocation) happen. imageSizeLimit (set above, before
  // Parse) already bounds the codec's own internal frame allocation for
  // backends that honor it; the re-validation immediately below closes
  // the remainder of that gap for this function's own downstream RGB
  // buffer regardless of codec-specific behavior.
  result = avifDecoderNextImage(decoder);
  if (result != AVIF_RESULT_OK) {
    const AssetErrorCode code = errorCodeForAvifResult(result);
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(AssetError{
        code, QStringLiteral("libavif failed to decode the AVIF image: %1")
                  .arg(QString::fromLatin1(avifResultToString(result)))});
  }

  // Cumulative review (PR #18): re-validate decoder->image->width/height
  // AGAIN here, immediately after avifDecoderNextImage() and strictly
  // before avifRGBImageSetDefaults()/avifRGBImageAllocatePixels() below
  // ever size an RGB conversion buffer from it. The pre-decode check above
  // only ever examined container-declared (ispe) metadata; the actual AV1
  // decode this project's own libavif backend just performed could, in
  // principle -- for a structural variant this function's author has not
  // enumerated, or a future libavif/codec-backend behavior change --
  // produce a decoder->image whose final dimensions differ from what was
  // checked pre-decode (e.g. a derived "grid" image's composite size, or
  // any codec path that does not rescale back to the declared size). This
  // is intentionally the exact same shared validateAvifDimensions() rule
  // as the pre-decode check, so the two can never silently drift apart.
  const AssetOutcome<qint64> postDecodeValidation =
      validateAvifDimensions(decoder->image->width, decoder->image->height,
                             maxDimensionPixels, maxTotalPixels);
  if (!postDecodeValidation) {
    const AssetError error = postDecodeValidation.error();
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(error);
  }

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, decoder->image);
  // Always convert to plain 8-bit interleaved RGBA, regardless of the
  // source AVIF's own bit depth (8/10/12-bit) -- libavif performs the
  // depth rescale during avifImageYUVToRGB() below -- so the result is
  // always constructible as a QImage::Format_RGBA8888 buffer.
  rgb.depth = 8;
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  // Deliberately NOT setting rgb.maxThreads here: that field does not
  // exist on avifRGBImage in every libavif release this project must
  // support (e.g. Ubuntu 22.04's packaged 0.9.3 predates it), so setting
  // it would be a portability/build break on some targets. The
  // resource-exhaustion bound that matters -- the actual AV1 decode via
  // avifDecoderNextImage() above -- is already constrained by
  // decoder->maxThreads = 1 near the top of this function, which has
  // been present and stable across libavif versions for far longer; the
  // YUV->RGB pixel-format conversion below is comparatively lightweight
  // per-frame CPU work, not the primary threading/resource concern here.

  // avifRGBImageAllocatePixels() returns void in some libavif releases
  // this project must support (e.g. Ubuntu 22.04's packaged 0.9.3) and
  // avifResult in later ones -- its return value (where present) is
  // therefore deliberately never captured, for portability across both
  // signatures. Failure is instead detected the one way that is valid
  // under every version: a null `rgb.pixels` after the call (the
  // documented behavior of both signatures on an allocation failure).
  (void)avifRGBImageAllocatePixels(&rgb);
  if (rgb.pixels == nullptr) {
    avifDecoderDestroy(decoder);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("failed to allocate the RGB conversion buffer")});
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
