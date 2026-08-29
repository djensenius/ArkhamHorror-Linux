// GENERATED FILE -- DO NOT EDIT BY HAND.
//
// Produced by tools/generate_asset_locale_digest.py from
// contracts/asset-locale-digest.json. Re-run that script after editing the JSON
// source; tests/AssetLocaleDigestTests.cpp fails the build if this file drifts
// from the JSON source's SHA-256.
#pragma once

namespace Arkham::AssetLocaleDigestData {

inline constexpr char kSourceJsonSha256[] =
    "84d3d6df7ee3155f3c08c0ce6df70690b42ce4f1b9da81861874a42623a728cd";

struct LocaleMapEntry {
  const char *isoLocale;
  const char *webLocale;
};

inline constexpr LocaleMapEntry kLocaleMap[] = {
    {"it", "ita"}, {"fr", "fr"}, {"es", "es"}, {"ko", "ko"}, {"zh", "zh"},
};

struct DigestEntry {
  const char *webLocale;
  const char *category;
  const char *identifier;
  const char *side;
};

inline constexpr DigestEntry kEntries[] = {
    {"ita", "card", "01001", "front"},
    {"ita", "card", "01001", "back"},
    {"ita", "card", "01002", "front"},
    {"fr", "card", "01001", "front"},
    {"fr", "card", "01001", "back"},
    {"es", "card", "01001", "front"},
    {"ko", "card", "01001", "front"},
    {"zh", "card", "01001", "front"},
    {"ita", "card", "01002", "resolved_front"},
    {"ita", "card", "01003", "mutated_front"},
};

} // namespace Arkham::AssetLocaleDigestData
