#pragma once

#include "AssetTypes.h"

#include <QUrl>

namespace Arkham::AssetLocator {

// The production asset host used when the server does not provide an
// override through site settings.
[[nodiscard]] QUrl defaultAssetBaseUrl();

// Validates and normalises a caller-supplied asset base URL. HTTPS is
// required except for exact loopback HTTP URLs, matching the production local
// development rule enforced by UrlValidator.
[[nodiscard]] AssetOutcome<QUrl> assetBaseUrlFromString(const QString &input);

// Card faces served by the web asset host. Official cards map to
// /img/arkham/cards/<code>.avif and Back maps to
// /img/arkham/cards/<code>b.avif after stripping one leading 'c' from the
// card code. Homebrew compound art ids of the form :<campaign>:<code> map to
// /img/arkham/homebrew/<campaign>/cards/<code>.avif. Both shapes match the
// web client's cardArt/cardImgPath helpers.
enum class CardFace { Front = 1, Back };

struct CardImageKey {
  QString cardCode;
  CardFace face{CardFace::Front};
};

// Builds a single canonical AVIF card-art URL under the supplied asset base.
// Asset key path segments are rejected if empty, ".", "..", or containing a
// slash, backslash, or control character so card codes cannot escape the
// /img/arkham/cards/ subtree.
[[nodiscard]] AssetOutcome<QUrl> buildCardImageUrl(const QUrl &assetBaseUrl,
                                                   const CardImageKey &key);

} // namespace Arkham::AssetLocator
