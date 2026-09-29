#pragma once

// Implementation-detail header declared only for the project's src/ header
// inventory. AVIF decoding is reached through decodeAssetImage() in
// AssetTypes.h; no per-format decoder is public API. The implementation asks
// libavif for the primary still-image item only: sequence tracks may be present
// but are ignored, while files with no primary still image are unsupported.
