#pragma once

#include "AssetTypes.h"

namespace Arkham {

// Decodes a PNG payload after the public dispatcher has selected PNG by its
// signature. Qt's PNG reader is used, but its header-only size probe is checked
// against AssetDecodeLimits before the full image buffer is decoded.
[[nodiscard]] AssetDecodeOutcome<QImage>
decodePngImage(const QByteArray &encodedBytes, const AssetDecodeLimits &limits);

} // namespace Arkham
