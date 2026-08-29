#include "AssetLocator.h"

#include "AssetLocaleDigest.h"

#include <QHash>
#include <QLatin1StringView>
#include <QSet>
#include <QString>
#include <QtAssert>

using namespace Qt::StringLiterals;

// Every category root, per-category fixed format, leading-'c' stripping
// rule, and card-side transform below is ported from the real web
// client's own asset-path helpers (halogenandtoast/ArkhamHorror,
// frontend/src/arkham/{helpers,cardArt,cardImages}.ts and the various
// call sites cited inline), NOT re-derived or guessed: this file is a
// pure, side-effect-free function of an AssetKey to the exact path shape
// that real host actually serves. No live CDN dependency is exercised at
// build or test time -- see tests/AssetLocatorTests.cpp's golden-path
// tests, which assert against literal strings copied from that
// inspection rather than any network call.
namespace Arkham {

namespace {

// Maximum identifier length for officially-namespaced categories (card
// codes, campaign/set/slot/chaos-token/investigator identifiers are all
// short fixed-shape tokens on the real web asset host).
constexpr qsizetype kOfficialIdentifierMaxLength = 32;
// Homebrew identifiers are community-authored slugs and may reasonably be
// longer, but are never unbounded.
constexpr qsizetype kHomebrewIdentifierMaxLength = 128;

bool isCategoryLocalizable(AssetCategory category) {
  return category == AssetCategory::Card ||
         category == AssetCategory::HomebrewCard;
}

bool isAsciiAlnum(QChar c) {
  const char16_t u = c.unicode();
  return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'z') ||
         (u >= u'A' && u <= u'Z');
}

bool isAllowedIdentifierChar(QChar c) {
  return isAsciiAlnum(c) || c == u'-' || c == u'_';
}

// Strict allow-list grammar shared by every category: only ASCII letters
// (both cases -- real pinned digest sources contain uppercase segments,
// e.g. contracts/asset-locale-digest-sources/ita.json's "cards/04242B"
// card code and fr.json's "cards/01514_Mutated19" mutationId, and case is
// never normalized away, per this project's case-sensitive-identifier
// policy), digits, '-', and '_'; must start and end with an alphanumeric
// character. This structurally rejects "/", "\\", "..", ".", control
// characters, "%", "@", ":", "?", "#", whitespace, and any non-ASCII code
// point -- there is no separate sanitisation step, so a hostile identifier
// is rejected outright rather than silently reinterpreted as a different,
// unintended asset.
bool isValidIdentifier(const QString &identifier, qsizetype maxLength) {
  if (identifier.isEmpty() || identifier.size() > maxLength) {
    return false;
  }
  if (!isAsciiAlnum(identifier.front()) || !isAsciiAlnum(identifier.back())) {
    return false;
  }
  for (const QChar c : identifier) {
    if (!isAllowedIdentifierChar(c)) {
      return false;
    }
  }
  return true;
}

bool isAsciiLower(QChar c) {
  const char16_t u = c.unicode();
  return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'z');
}

bool isAllowedHomebrewNamespaceChar(QChar c) {
  return isAsciiLower(c) || c == u'-' || c == u'_';
}

// homebrewNamespace is a community-authored slug (a campaign/collection
// name a homebrew author picks), not an upstream-assigned card code, and
// has no known real-world case-sensitive requirement the way official
// card identifiers/mutationIds do (see isValidIdentifier above) -- it
// keeps the original strict ASCII-lowercase-only grammar so homebrew
// authors get one unambiguous canonical spelling rather than "MyCampaign"
// and "mycampaign" silently addressing different cache/CDN paths.
bool isValidHomebrewNamespace(const QString &identifier, qsizetype maxLength) {
  if (identifier.isEmpty() || identifier.size() > maxLength) {
    return false;
  }
  if (!isAsciiLower(identifier.front()) || !isAsciiLower(identifier.back())) {
    return false;
  }
  for (const QChar c : identifier) {
    if (!isAllowedHomebrewNamespaceChar(c)) {
      return false;
    }
  }
  return true;
}

// Categories whose identifier is a card-code-shaped token that the real
// web client unconditionally strips a single leading 'c' tag-prefix from
// before building any path -- see cardArt() in cardImages.ts:
// `code.replace(/^c/, '')`, applied identically whether or not a leading
// 'c' was actually present, and the same pattern at the InvestigatorType/
// SetIcon call sites (e.g. `game.scenario.id.replace(/^c/, '')` in
// GameRow.vue and similar). No evidence of this strip was found for
// CampaignBox, HomebrewSet, HomebrewBox, ChaosToken, or SlotIcon
// identifiers, so it is deliberately NOT applied to those.
bool categoryStripsLeadingCardCodePrefix(AssetCategory category) {
  switch (category) {
  case AssetCategory::Card:
  case AssetCategory::HomebrewCard:
  case AssetCategory::InvestigatorPortrait:
  case AssetCategory::SetIcon:
    return true;
  default:
    return false;
  }
}

QString stripLeadingCardCodePrefix(const QString &identifier) {
  if (identifier.startsWith(u'c')) {
    return identifier.mid(1);
  }
  return identifier;
}

// Real per-instance resolved-code overrides the generic "strip a trailing
// [aceg], append b" rule below would otherwise get wrong -- these exist in
// the real client specifically because 03276/03279's "resolved" sides do
// not follow the generic pattern. See resolvedSideArt() in
// cardImages.ts's RESOLVED_SIDE_OVERRIDES constant.
const QHash<QString, QString> &resolvedSideOverrides() {
  static const QHash<QString, QString> overrides = {
      {QStringLiteral("03276a"), QStringLiteral("03276ab")},
      {QStringLiteral("03276b"), QStringLiteral("03276bb")},
      {QStringLiteral("03279a"), QStringLiteral("03279ab")},
      {QStringLiteral("03279b"), QStringLiteral("03279bb")},
  };
  return overrides;
}

// Pure, always-succeeding transform mirroring resolvedSideArt() in
// cardImages.ts: (1) consult the fixed override table above; (2) else
// strip one trailing [aceg] character if present, then append "b".
QString resolvedSideArtCode(const QString &strippedIdentifier) {
  const auto &overrides = resolvedSideOverrides();
  const auto it = overrides.find(strippedIdentifier);
  if (it != overrides.end()) {
    return *it;
  }
  QString base = strippedIdentifier;
  if (!base.isEmpty()) {
    const QChar last = base.back();
    if (last == u'a' || last == u'c' || last == u'e' || last == u'g') {
      base.chop(1);
    }
  }
  return base + u'b';
}

// Computes the final on-CDN "art code" (the strippedIdentifier plus
// whatever suffix/transform `side` implies) mirroring the exact per-side
// rules the real web client's cardArt.ts/cardImages.ts apply. Returns
// std::nullopt when `side` is structurally inapplicable to this specific
// identifier's shape -- this is NOT a grammar violation, just "no art
// code is resolvable for this transform" (mirrors altFrontImage()
// returning null in cardArt.ts for exactly this case).
std::optional<QString> resolveArtCodeForSide(AssetSide side,
                                             const QString &strippedIdentifier,
                                             const QString &mutationId) {
  switch (side) {
  case AssetSide::Front:
    return strippedIdentifier;
  case AssetSide::Back:
    return strippedIdentifier + u'b';
  case AssetSide::AlternateFront:
    // altFrontImage(): `src.replace(/(\d)\.avif$/, '$1a.avif')` -- only
    // ever produces a candidate when the base code ends in a digit.
    if (strippedIdentifier.isEmpty() || !strippedIdentifier.back().isDigit()) {
      return std::nullopt;
    }
    return strippedIdentifier + u'a';
  case AssetSide::ResolvedFront:
    return resolvedSideArtCode(strippedIdentifier);
  case AssetSide::MutatedFront:
    return strippedIdentifier + u'_' + mutationId;
  }
  Q_UNREACHABLE_RETURN(std::nullopt);
}

// Category-specific canonical path segment, ported from the real web
// client's routing scheme (see this file's header comment for the exact
// source citation). `localeDir` is the mapped web-locale directory
// segment (e.g. "ita"), or empty for the English/default candidate.
// Every path is rooted under "img/arkham/", matching imgsrc() in
// helpers.ts -- the current code's historical omission of this prefix was
// the primary cause of every category resolving to the wrong CDN route.
QString buildRelativePath(const AssetKey &key, const QString &artCode,
                          const QString &localeDir) {
  const QString ext =
      assetFormatExtension(AssetLocator::canonicalFormatFor(key.category));
  const QString localePrefix =
      localeDir.isEmpty() ? QString() : localeDir + u'/';

  switch (key.category) {
  case AssetCategory::Card:
    return "img/arkham/"_L1 + localePrefix + "cards/"_L1 + artCode + u'.' + ext;
  case AssetCategory::HomebrewCard:
    // homebrew/{campaignNamespace}/cards/{code}.avif -- see cardImgPath()
    // in helpers.ts matching `/^:(.+):(\d+[a-z]*)$/` and building
    // `homebrew/{campaign}/cards/{cardCode}.avif`.
    return "img/arkham/"_L1 + localePrefix + "homebrew/"_L1 +
           key.homebrewNamespace + "/cards/"_L1 + artCode + u'.' + ext;
  case AssetCategory::InvestigatorPortrait:
    // portraitImage() in cardImages.ts: root is "portraits/", not
    // "investigators/".
    return "img/arkham/portraits/"_L1 + artCode + u'.' + ext;
  case AssetCategory::ChaosToken:
    // ChaosToken.ts bakes a literal "ct-" prefix onto the token name.
    return "img/arkham/chaos-tokens/ct-"_L1 + artCode + u'.' + ext;
  case AssetCategory::SetIcon:
    return "img/arkham/sets/"_L1 + artCode + u'.' + ext;
  case AssetCategory::CampaignBox:
    // campaignBoxSrc() in ChooseMode.vue: root is "boxes/", not
    // "campaigns/".
    return "img/arkham/boxes/"_L1 + artCode + u'.' + ext;
  case AssetCategory::SlotIcon:
    return "img/arkham/slots/"_L1 + artCode + u'.' + ext;
  case AssetCategory::HomebrewSet:
    // homebrewSetImagePath() in Cards.vue: the SAME identifier is used as
    // both the per-homebrew directory and the file name.
    return "img/arkham/homebrew/"_L1 + artCode + "/sets/"_L1 + artCode + u'.' +
           ext;
  case AssetCategory::HomebrewBox:
    // ChooseMode.vue homebrew branch: same identifier used twice, as
    // above.
    return "img/arkham/homebrew/"_L1 + artCode + "/boxes/"_L1 + artCode + u'.' +
           ext;
  }
  Q_UNREACHABLE_RETURN(QString());
}

QUrl joinBaseAndPath(const QUrl &base, const QString &relativePath) {
  QUrl url = base;
  // `base.path()` is empty whenever the validated base URL had no
  // non-trivial path prefix at all (see UrlValidator::validateCustomUrl(),
  // which never sets a path of just "/"). QUrl::setPath() requires an
  // absolute path (leading '/') on any URL that has an authority
  // component, so the leading slash must always be ensured here -- not
  // only when the base path happens to already be non-empty -- or
  // setPath() silently produces an invalid, empty QUrl.
  QString fullPath = base.path();
  if (!fullPath.endsWith(u'/')) {
    fullPath += u'/';
  }
  fullPath += relativePath;
  url.setPath(fullPath);
  return url;
}

qsizetype identifierMaxLengthFor(AssetCategory category) {
  switch (category) {
  case AssetCategory::HomebrewCard:
  case AssetCategory::HomebrewSet:
  case AssetCategory::HomebrewBox:
    return kHomebrewIdentifierMaxLength;
  default:
    return kOfficialIdentifierMaxLength;
  }
}

} // namespace

AssetOutcome<ValidatedAssetSource> AssetLocator::defaultAssetBase() {
  return ValidatedAssetSource::fromRaw(
      QStringLiteral("https://assets.arkhamhorror.app"));
}

AssetFormat AssetLocator::canonicalFormatFor(AssetCategory category) {
  switch (category) {
  case AssetCategory::Card:
  case AssetCategory::HomebrewCard:
    return AssetFormat::Avif;
  case AssetCategory::InvestigatorPortrait:
  case AssetCategory::CampaignBox:
  case AssetCategory::HomebrewBox:
    return AssetFormat::Jpeg;
  case AssetCategory::ChaosToken:
  case AssetCategory::SetIcon:
  case AssetCategory::SlotIcon:
  case AssetCategory::HomebrewSet:
    return AssetFormat::Png;
  }
  Q_UNREACHABLE_RETURN(AssetFormat::Jpeg);
}

AssetOutcome<QVector<AssetCandidate>>
AssetLocator::resolveCandidates(const AssetKey &key) {
  // key.assetBase can only ever be genuinely valid if it was produced by
  // ValidatedAssetSource::fromRaw() against the caller's original raw
  // input (see AssetTypes.h) -- there is no QUrl round-trip left here to
  // "re-validate" defensively, because there is no way to construct a
  // valid instance that bypassed that policy in the first place. A
  // default-constructed (never-populated) assetBase fails isValid() and
  // is rejected exactly like any other invalid base.
  if (!key.assetBase.isValid()) {
    return AssetError{
        AssetErrorCode::InvalidAssetBase,
        QStringLiteral("asset base was never validated via "
                       "ValidatedAssetSource::fromRaw()"),
    };
  }
  const QUrl &normalizedBase = key.assetBase.normalizedUrl();

  if (!isValidIdentifier(key.identifier,
                         identifierMaxLengthFor(key.category))) {
    return AssetError{
        AssetErrorCode::InvalidIdentifier,
        QStringLiteral("asset identifier is empty, too long, or contains a "
                       "character outside [A-Za-z0-9_-]"),
    };
  }

  const bool localizable = isCategoryLocalizable(key.category);
  if (!localizable && key.side != AssetSide::Front) {
    return AssetError{
        AssetErrorCode::InvalidSideForCategory,
        QStringLiteral("only Card and HomebrewCard assets may specify a "
                       "non-Front side"),
    };
  }

  const bool wantsHomebrewNamespace =
      key.category == AssetCategory::HomebrewCard;
  if (wantsHomebrewNamespace) {
    if (!isValidHomebrewNamespace(key.homebrewNamespace,
                                  kHomebrewIdentifierMaxLength)) {
      return AssetError{
          AssetErrorCode::InvalidHomebrewNamespace,
          QStringLiteral("HomebrewCard requires a valid homebrewNamespace"),
      };
    }
  } else if (!key.homebrewNamespace.isEmpty()) {
    return AssetError{
        AssetErrorCode::InvalidHomebrewNamespace,
        QStringLiteral("homebrewNamespace must be empty for this category"),
    };
  }

  const bool wantsMutationId = key.side == AssetSide::MutatedFront;
  if (wantsMutationId) {
    if (!isValidIdentifier(key.mutationId, kOfficialIdentifierMaxLength)) {
      return AssetError{
          AssetErrorCode::InvalidMutationId,
          QStringLiteral("MutatedFront requires a valid mutationId"),
      };
    }
  } else if (!key.mutationId.isEmpty()) {
    return AssetError{
        AssetErrorCode::InvalidMutationId,
        QStringLiteral("mutationId must be empty unless side is "
                       "MutatedFront"),
    };
  }

  if (key.format != canonicalFormatFor(key.category)) {
    return AssetError{
        AssetErrorCode::FormatMismatchForCategory,
        QStringLiteral("this category is always served as %1, not the "
                       "declared format")
            .arg(assetFormatExtension(
                AssetLocator::canonicalFormatFor(key.category))),
    };
  }

  const QString strippedIdentifier =
      categoryStripsLeadingCardCodePrefix(key.category)
          ? stripLeadingCardCodePrefix(key.identifier)
          : key.identifier;

  // A direct (non-auto-derived) request for a side that is structurally
  // inapplicable to this specific identifier's shape (currently only
  // AlternateFront on an identifier not ending in a digit) has no
  // candidate at all to offer -- unlike the auto-derived alternate-front
  // fallback below, which simply omits itself silently in that case.
  const std::optional<QString> directArtCode =
      resolveArtCodeForSide(key.side, strippedIdentifier, key.mutationId);
  if (!directArtCode.has_value()) {
    return AssetError{
        AssetErrorCode::InvalidSideForIdentifier,
        QStringLiteral("the requested side has no valid art code for this "
                       "identifier"),
    };
  }

  QVector<AssetCandidate> candidates;
  QSet<QString> seenUrls;

  auto tryAppend = [&](const QString &localeDir, const QString &isoLocale,
                       const QString &artCode, bool isAlternateFrontFallback) {
    const QString relativePath = buildRelativePath(key, artCode, localeDir);
    const QUrl url = joinBaseAndPath(normalizedBase, relativePath);
    const QString urlString = url.toString(QUrl::FullyEncoded);
    if (seenUrls.contains(urlString)) {
      return;
    }
    seenUrls.insert(urlString);
    candidates.append(AssetCandidate{url, isoLocale, isAlternateFrontFallback});
  };

  // 1. Localized candidate, but ONLY if the digest confirms it exists.
  // Skipping here (rather than issuing a network request and relying on a
  // 404) keeps resolution pure/deterministic and avoids a request this
  // client already knows will fail. The digest is keyed by the EXACT
  // fully-resolved art code (the same string used to build the URL
  // itself), not a separately decomposed identifier/side pair -- see
  // AssetLocaleDigest::hasLocalizedVariant()'s header comment for why a
  // reverse-decomposition would be ambiguous.
  if (localizable && !key.locale.isEmpty() && key.locale != "en"_L1) {
    const QString webLocale = AssetLocaleDigest::webLocaleFor(key.locale);
    if (!webLocale.isEmpty() && AssetLocaleDigest::hasLocalizedVariant(
                                    webLocale, key.category, *directArtCode)) {
      tryAppend(webLocale, key.locale, *directArtCode, false);
    }
  }

  // 2. English/default candidate: always present.
  tryAppend(QString(), QString(), *directArtCode, false);

  // 3. Alternate-front fallback: only for a plain Front card request, and
  // only when the identifier's shape actually supports one (see
  // resolveArtCodeForSide()'s AlternateFront case) -- silently omitted,
  // not an error, exactly mirroring altFrontImage() returning null.
  if (localizable && key.side == AssetSide::Front) {
    const std::optional<QString> altArtCode = resolveArtCodeForSide(
        AssetSide::AlternateFront, strippedIdentifier, key.mutationId);
    if (altArtCode.has_value()) {
      tryAppend(QString(), QString(), *altArtCode, true);
    }
  }

  if (candidates.isEmpty()) {
    return AssetError{
        AssetErrorCode::NoCandidates,
        QStringLiteral("no candidates could be resolved for this asset key"),
    };
  }

  return candidates;
}

} // namespace Arkham
