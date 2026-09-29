#pragma once

#include "AssetTypes.h"

namespace Arkham {

// Decodes a single still AVIF payload through libavif. AVIF image sequences or
// animations are rejected rather than silently decoding only frame 0, because
// native Arkham assets are canonical still images.
[[nodiscard]] AssetDecodeOutcome<QImage>
decodeAvifImage(const QByteArray &encodedBytes,
                const AssetDecodeLimits &limits);

} // namespace Arkham
