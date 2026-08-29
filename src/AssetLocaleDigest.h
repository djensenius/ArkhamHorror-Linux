#pragma once

#include "AssetTypes.h"

#include <QHash>
#include <QString>

namespace Arkham {

// Locale mapping and localized-asset digest, backed by the generated table
// in AssetLocaleDigestData.generated.h (produced from
// contracts/asset-locale-digest.json plus the pinned per-locale source
// files in contracts/asset-locale-digest-sources/ by
// tools/generate_asset_locale_digest.py -- see that JSON manifest's own
// "provenance" note for exactly what it covers and where it came from).
//
// AssetLocator consults this before ever proposing a localized network
// candidate: if the digest has no entry for a given (locale, category,
// artCode) tuple, the localized request is skipped entirely and
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
// (webLocale, category, artCode) tuple, where `artCode` is the EXACT,
// fully-resolved art code string AssetLocator::resolveArtCodeForSide()
// would compute for the requested identifier+side (e.g. "01001b" for
// identifier "01001"/AssetSide::Back) -- not a separately-decomposed
// identifier and side. This matches the real upstream digest's own model
// exactly: it is a flat list of already-resolved final path segments with
// no side annotation of its own, and a generic reverse-parse from art
// code back to (identifier, side) would be genuinely ambiguous (a
// trailing "b" can mean AssetSide::Back OR the generic ResolvedFront
// rule's output). `webLocale` must already be the mapped web-locale
// segment (the result of webLocaleFor()), not the original ISO code.
[[nodiscard]] bool hasLocalizedVariant(const QString &webLocale,
                                       AssetCategory category,
                                       const QString &artCode);

// SHA-256 (hex) of contracts/asset-locale-digest.json (the manifest),
// embedded in the generated header at generation time. Exposed so tests
// can independently recompute the manifest's hash and assert equality,
// catching drift between the checked-in manifest and the generated header
// without needing to invoke Python at test time.
[[nodiscard]] QString pinnedManifestJsonSha256();

// SHA-256 (hex), per web-locale, of each pinned source file in
// contracts/asset-locale-digest-sources/ (e.g. "ita" ->
// sha256(ita.json)), embedded in the generated header at generation time.
// Exposed so tests can independently recompute each checked-in source
// file's hash and assert equality -- catching drift or tampering in any
// individual pinned source file, not just the manifest that references
// it.
[[nodiscard]] QHash<QString, QString> pinnedSourceFileSha256();

} // namespace AssetLocaleDigest

} // namespace Arkham
