#pragma once

#include "AssetTypes.h"

#include <QSize>

// Implementation-detail header declared only for the project's src/ header
// inventory. JPEG decoding is reached through decodeAssetImage() in
// AssetTypes.h; no per-format decoder is public API. The implementation uses
// libjpeg directly, treats any libjpeg corrupt-data warning as fatal, rejects
// CMYK/YCCK input, and enforces progressive scan limits through libjpeg's
// progress callback rather than by trusting marker pre-scans.

namespace Arkham::detail {

// libjpeg can scale by 1/2, 1/4, or 1/8 during the IDCT. Returns the largest
// of those denominators whose output for a width x height source (each side
// rounded up, as libjpeg does) still covers target, so the final smooth fit in
// decodeAssetImage() only ever downscales. Returns 1 when no reduction covers
// target, or when target is empty or already covers the source. Declared here
// only so the decoder tests can check the choice directly.
[[nodiscard]] unsigned int jpegScaleDenominator(unsigned int width,
                                                unsigned int height,
                                                const QSize &target);

// Maps the libjpeg message code of a fatal error to a decode error code.
// JERR_OUT_OF_MEMORY is AllocationFailed, because it depends on memory
// pressure rather than on the bytes; every other fatal error is
// MalformedImage. Declared here so the decoder tests can check the mapping
// without having to exhaust memory.
[[nodiscard]] AssetDecodeErrorCode jpegFatalErrorCode(int messageCode);

} // namespace Arkham::detail
