#pragma once

#include "AssetTypes.h"

#include <QUrl>

namespace Arkham::AssetLocator {

// The production asset host used when the server does not provide an
// override through site settings.
[[nodiscard]] QUrl defaultAssetBaseUrl();

// Validates and normalises a caller-supplied asset base URL. HTTPS is
// required except for exact loopback HTTP URLs, matching the production local
// development rule without applying API-base-path validation meant only for
// server API roots.
[[nodiscard]] AssetOutcome<QUrl> assetBaseUrlFromString(const QString &input);

// Web-compatible mutation suffix validation. The locator accepts only the
// frontend convention _Mutated<N>; game-state-to-N mapping belongs elsewhere.
[[nodiscard]] bool isMutationSuffix(const QString &suffix);

// Class-variant suffix validation: empty, or one of _Guardian, _Seeker,
// _Rogue, _Mystic and _Survivor (the web's c03006/c90087 role art, e.g.
// /img/arkham/cards/03006_Seeker.avif).
[[nodiscard]] bool isClassVariantSuffix(const QString &suffix);

// True when a server card art id matches the grammar buildCardImageUrl()
// accepts: an official code, or :<campaign>:<code> for homebrew, after
// stripping one leading 'c'. Anything else, including '/', '?', '#', '%' and
// whitespace, is rejected, so a validated id can go into an image provider
// id verbatim.
[[nodiscard]] bool isCardArtId(const QString &cardCode);

// Card faces served by the web asset host. Official cards map to
// /img/arkham/cards/<code>.avif and Back maps to
// /img/arkham/cards/<code>b.avif after stripping one leading 'c' from the
// card code. Homebrew compound art ids of the form :<campaign>:<code> map to
// /img/arkham/homebrew/<campaign>/cards/<code>.avif. Both shapes match the
// web client's cardArt/cardImgPath helpers, including the Revised Core
// reprint and Ages Unwound act file names (CardArt::reprintedArt and
// CardArt::homebrewArt).
enum class CardFace { Front = 1, Back };

struct CardImageKey {
  // Server-shaped card art id. Prefer the card definition's art id when the
  // server provides one (cdArt/art), falling back to the card code because the
  // backend defaults cdArt to the card code. The web client mirrors this by
  // choosing cardArt(card) before asCardCode(card), then stripping one leading
  // 'c' in cardImages.ts and routing through helpers.ts's cardImgPath().
  QString cardCode;
  CardFace face{CardFace::Front};
  // Optional web-compatible mutated art suffix, e.g. _Mutated1. The caller
  // supplies the game-state-to-suffix mapping; the locator only validates and
  // routes the resulting asset id.
  QString mutationSuffix;
  // Optional class-variant suffix, e.g. _Seeker. The file name is
  // <code>[b]<classVariantSuffix><mutationSuffix>.avif, as in the web's
  // Investigator.vue.
  QString classVariantSuffix;
};

// Builds a single canonical AVIF card-art URL under the supplied asset base.
// Card art ids are accepted only if they match the known ASCII asset-id
// grammar (official/legacy official ids, or :homebrew-campaign:code); unsafe
// characters such as separators, '.', '%', whitespace, and non-ASCII are
// rejected before the path is assembled.
[[nodiscard]] AssetOutcome<QUrl> buildCardImageUrl(const QUrl &assetBaseUrl,
                                                   const CardImageKey &key);

} // namespace Arkham::AssetLocator
