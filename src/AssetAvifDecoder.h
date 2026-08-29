#pragma once

#include "AssetTypes.h"

#include <QByteArray>
#include <QImage>
#include <cstdint>

// Review item 4 (PR #18 cumulative review): Qt has no official AVIF plugin
// (upstream qtimageformats does not ship one), and card art defaults to
// AVIF -- a permanent AssetErrorCode::UnsupportedCodec for every card asset
// is not acceptable. This module decodes AVIF directly against libavif's C
// API, never through QImageReader/Qt's plugin registry, so AVIF support no
// longer depends on which (if any) Qt image plugins happen to be installed
// or bundled. libavif is a required build/runtime dependency of this
// project (see CMakeLists.txt) precisely so this path is always available,
// both in local development and in the packaged AppImage (see
// packaging/build-appimage.sh, which bundles libavif and its own decode
// backend's shared-library closure).
namespace Arkham {

// Decodes a complete, in-memory AVIF payload (`encodedBytes`, already
// magic-byte-sniffed and Content-Type-checked by the caller -- see
// AssetNetworkFetcher::decodeAndValidate()) into a QImage.
//
// Dimension/pixel-budget limits are enforced by this function explicitly
// re-checking `decoder->image->width/height` against the caller-supplied
// `maxDimensionPixels`/`maxTotalPixels` limits immediately after a
// successful avifDecoderParse() call -- which only reads container-level
// metadata (an ISOBMFF `ispe` box), never decodes or allocates any AV1
// pixel data -- and strictly BEFORE ever calling avifDecoderNextImage()
// (which performs the actual AV1 decode and full-resolution pixel-buffer
// allocation). A hostile AVIF that declares a huge width/height in its
// container metadata therefore never reaches pixel decode/allocation at
// all, regardless of how small its actual (possibly absent or degenerate)
// AV1 payload is. libavif's own built-in `imageDimensionLimit`/
// `imageSizeLimit` defaults (32768 per side / 16384*16384 total) are left
// untouched as a coarse backstop against libavif's own internal arithmetic
// during Parse -- deliberately NOT tightened to this project's own
// (smaller, configurable) limits, since doing so would make
// avifDecoderParse() itself fail with a generic, untyped parse error for
// an image this function's own explicit check is designed to reject with
// a precise, typed AssetErrorCode::DimensionTooLarge/PixelBudgetExceeded.
//
// `maxDimensionPixels` and `maxTotalPixels` mirror
// AssetNetworkFetcher::Limits' fields of the same name and are applied
// identically to the JPEG/PNG path in decodeAndValidate().
[[nodiscard]] AssetOutcome<QImage>
decodeAvifImage(const QByteArray &encodedBytes, int maxDimensionPixels,
                qint64 maxTotalPixels);

} // namespace Arkham
