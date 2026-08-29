#pragma once

#include "AssetTypes.h"

#include <QVector>

namespace Arkham {

// Pure, side-effect-free resolver from a typed AssetKey to an ordered,
// deduplicated, bounded list of concrete AssetCandidate URLs to try in
// order. AssetLocator issues no network I/O itself and holds no mutable
// state; every call is a deterministic function of its input plus the
// compiled-in AssetLocaleDigest lookup table.
//
// Base-URL policy is never re-implemented here: every call re-validates
// `key.assetBase` against UrlValidator::validateCustomUrl() -- the exact
// same policy ServerProfile/NetworkAuthenticationClient use -- rather than
// forking a weaker, asset-only interpretation. A base URL that fails that
// validation (wrong scheme, credentials embedded, control characters,
// etc.) yields AssetErrorCode::InvalidAssetBase before any candidate is
// built.
//
// Per-category identifier grammar is intentionally an allow-list: only
// ASCII lowercase letters, digits, '-', and '_' are ever accepted, and the
// identifier must start and end with an alphanumeric character. This
// structurally rejects path separators, ".."/"." segments, control
// characters, "%"-encoding, and userinfo/query/fragment-injection
// characters ("@", ":", "?", "#") outright -- there is no sanitising step
// that could turn a hostile identifier into a different, unintended asset.
// Identifiers are never re-cased: the grammar simply never accepts an
// uppercase letter, so a caller cannot smuggle a differently-cased request
// past this check by relying on later normalisation.
namespace AssetLocator {

// Returns the default asset base (https://assets.arkhamhorror.app),
// suitable for AssetKey::assetBase when no site-settings override is
// injected.
[[nodiscard]] QUrl defaultAssetBase();

// Validates `key` and resolves it to an ordered candidate list: an
// optional localized candidate (only when the digest confirms one
// exists), always the English/default candidate, and -- only for a Card
// or HomebrewCard request whose side is Front -- a final alternate-front
// English fallback candidate. The list is already deduplicated by URL and
// bounded to at most 3 entries; callers advance through it in order,
// stopping at the first non-404 outcome (see AssetNetworkFetcher /
// AssetRequestCoordinator for the fetch-time fallback policy this list is
// designed to drive).
[[nodiscard]] AssetOutcome<QVector<AssetCandidate>>
resolveCandidates(const AssetKey &key);

} // namespace AssetLocator

} // namespace Arkham
