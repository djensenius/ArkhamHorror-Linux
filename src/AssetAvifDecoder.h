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
// Dimension/pixel-budget defense is layered THREE ways (cumulative review,
// PR #18):
//
// 1. Before avifDecoderParse() is ever called, decoder->imageCountLimit is
//    tightened to 1 (this project only ever serves/decodes a single still
//    image) and decoder->imageSizeLimit is tightened to the caller's own
//    `maxTotalPixels` (rather than left at libavif's much looser built-in
//    defaults of ~2.59M images / 268,435,456 pixels). This makes libavif
//    itself abort early -- during Parse's own sample-table/item
//    enumeration -- against a hostile container that merely *declares* an
//    enormous image/sample count, bounding Parse's own internal cost
//    rather than only rejecting the result afterward. For codec backends
//    that honor it (confirmed empirically for this project's exact
//    pinned libavif 0.9.3 + dav1d on Ubuntu 22.04), imageSizeLimit is ALSO
//    enforced against the actual decoded AV1 frame's own internal
//    dimensions, before any post-decode rescale back to a
//    container-declared size -- see point 3 below for why this matters.
// 2. Immediately after a successful avifDecoderParse() -- which only ever
//    reads container-level metadata (an ISOBMFF `ispe` box), never
//    decodes or allocates any AV1 pixel data -- this function explicitly
//    re-checks decoder->image->width/height/totalPixels against
//    `maxDimensionPixels`/`maxTotalPixels` and returns BEFORE ever calling
//    avifDecoderNextImage() (which performs the actual AV1 decode and
//    full-resolution pixel-buffer allocation) if they are exceeded.
// 3. Immediately AFTER avifDecoderNextImage() succeeds, the identical
//    check is repeated against decoder->image's (now actually-decoded)
//    dimensions, strictly before any RGB conversion buffer is sized from
//    them. This closes a real TOCTOU gap point 2 alone cannot: a hostile
//    AVIF can declare a tiny `ispe` (passing the point-2 check trivially)
//    while its embedded AV1 bitstream itself encodes a far larger frame --
//    the underlying codec must fully allocate/decode that oversized
//    internal frame before any rescale back to the small declared size,
//    by which point the resource cost has already been paid regardless of
//    what the container claimed. Point 1's imageSizeLimit is the
//    mechanism that actually prevents the codec from doing this in the
//    first place (for backends that honor it); point 3 is a
//    codec-independent backstop that re-validates whatever dimensions the
//    decode actually produced, regardless of which backend served it or
//    whether it honors imageSizeLimit internally.
//
// `maxDimensionPixels` and `maxTotalPixels` mirror
// AssetNetworkFetcher::Limits' fields of the same name and are applied
// identically to the JPEG/PNG path in decodeAndValidate().
[[nodiscard]] AssetOutcome<QImage>
decodeAvifImage(const QByteArray &encodedBytes, int maxDimensionPixels,
                qint64 maxTotalPixels);

} // namespace Arkham
