#pragma once

// Implementation-detail header declared only for the project's src/ header
// inventory. JPEG decoding is reached through decodeAssetImage() in
// AssetTypes.h; no per-format decoder is public API. The implementation uses
// libjpeg directly, treats any libjpeg corrupt-data warning as fatal, rejects
// CMYK/YCCK input, and enforces progressive scan limits through libjpeg's
// progress callback rather than by trusting marker pre-scans.
