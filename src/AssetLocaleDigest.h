#pragma once

#include "AssetTypes.h"

#include <QString>

namespace Arkham {

// Locale mapping and localized-asset digest, backed by the generated table
// in AssetLocaleDigestData.generated.h (produced from
// contracts/asset-locale-digest.json by tools/generate_asset_locale_digest.py
// -- see that JSON file's own "provenance" note for exactly what it covers).
//
// AssetLocator consults this before ever proposing a localized network
// candidate: if the digest has no entry for a given (locale, category,
// identifier, side) tuple, the localized request is skipped entirely and
// resolution proceeds directly to the English candidate. This keeps the
// fallback chain pure/deterministic and avoids ever issuing a network
// request this client already knows in advance will 404.
namespace AssetLocaleDigest {

// Maps an ISO-ish locale code (e.g. "it") to the web asset host's own
// locale directory segment (e.g. "ita"), per the pinned mapping
// `it -> ita, fr -> fr, es -> es, ko -> ko, zh -> zh`. Returns an empty
// string for any locale not in this fixed mapping (including "en" and the
// empty string, both of which always mean "no localisation").
[[nodiscard]] QString webLocaleFor(const QString &isoLocale);

// True iff the digest lists a localized asset for exactly this
// (webLocale, category, identifier, side) tuple. `webLocale` must already
// be the mapped web-locale segment (the result of webLocaleFor()), not the
// original ISO code.
[[nodiscard]] bool hasLocalizedVariant(const QString &webLocale,
                                       AssetCategory category,
                                       const QString &identifier,
                                       AssetSide side);

// SHA-256 (hex) of contracts/asset-locale-digest.json, embedded in the
// generated header at generation time. Exposed so tests can independently
// recompute the JSON file's hash and assert equality, catching drift
// between the checked-in JSON and the generated header without needing to
// invoke Python at test time.
[[nodiscard]] QString pinnedSourceJsonSha256();

} // namespace AssetLocaleDigest

} // namespace Arkham
