#pragma once

#include "AssetTypes.h"

namespace Arkham {

// Decodes a JPEG payload through libjpeg's C API. Dimensions are read and
// checked immediately after jpeg_read_header(), before any full-size pixel
// buffer is allocated. Progressive JPEGs are accepted only when their Start Of
// Scan marker count stays within AssetDecodeLimits::maxProgressiveJpegScans.
[[nodiscard]] AssetDecodeOutcome<QImage>
decodeJpegImage(const QByteArray &encodedBytes,
                const AssetDecodeLimits &limits);

} // namespace Arkham
