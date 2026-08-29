#include "AssetLocator.h"

#include "AssetLocaleDigest.h"
#include "UrlValidator.h"

#include <QLatin1StringView>
#include <QSet>
#include <QString>
#include <QtAssert>

using namespace Qt::StringLiterals;

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

bool isAsciiLowerAlnum(QChar c) {
  const char16_t u = c.unicode();
  return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'z');
}

bool isAllowedIdentifierChar(QChar c) {
  return isAsciiLowerAlnum(c) || c == u'-' || c == u'_';
}

// Strict allow-list grammar shared by every category: only ASCII lowercase
// letters, digits, '-', and '_'; must start and end with an alphanumeric
// character. This structurally rejects "/", "\\", "..", ".", control
// characters, "%", "@", ":", "?", "#", whitespace, and any non-ASCII code
// point -- there is no separate sanitisation step, so a hostile identifier
// is rejected outright rather than silently reinterpreted as a different,
// unintended asset.
bool isValidIdentifier(const QString &identifier, qsizetype maxLength) {
  if (identifier.isEmpty() || identifier.size() > maxLength) {
    return false;
  }
  if (!isAsciiLowerAlnum(identifier.front()) ||
      !isAsciiLowerAlnum(identifier.back())) {
    return false;
  }
  for (const QChar c : identifier) {
    if (!isAllowedIdentifierChar(c)) {
      return false;
    }
  }
  return true;
}

QLatin1StringView cardSideSuffix(AssetSide side) {
  switch (side) {
  case AssetSide::Front:
    return ""_L1;
  case AssetSide::Back:
    return "b"_L1;
  case AssetSide::AlternateFront:
    return "a"_L1;
  case AssetSide::ResolvedFront:
    return "-resolved"_L1;
  case AssetSide::MutatedFront:
    return "-mutated"_L1;
  }
  Q_UNREACHABLE_RETURN(""_L1);
}

// Category-specific canonical path segment, matching the current web
// client's routing scheme (see AssetLocator.h class comment). `localeDir`
// is the mapped web-locale directory segment (e.g. "ita"), or empty for
// the English/default candidate.
QString buildRelativePath(AssetCategory category, const QString &identifier,
                          AssetSide side, const QString &localeDir,
                          AssetFormat format) {
  const QString ext = assetFormatExtension(format);
  const QString localePrefix =
      localeDir.isEmpty() ? QString() : localeDir + u'/';

  switch (category) {
  case AssetCategory::Card:
    return localePrefix + "cards/"_L1 + identifier + cardSideSuffix(side) +
           u'.' + ext;
  case AssetCategory::HomebrewCard:
    return localePrefix + "homebrew/cards/"_L1 + identifier +
           cardSideSuffix(side) + u'.' + ext;
  case AssetCategory::InvestigatorPortrait:
    return "investigators/"_L1 + identifier + u'.' + ext;
  case AssetCategory::ChaosToken:
    return "chaos-tokens/"_L1 + identifier + u'.' + ext;
  case AssetCategory::SetIcon:
    return "sets/"_L1 + identifier + u'.' + ext;
  case AssetCategory::CampaignBox:
    return "campaigns/"_L1 + identifier + u'.' + ext;
  case AssetCategory::SlotIcon:
    return "slots/"_L1 + identifier + u'.' + ext;
  case AssetCategory::HomebrewSet:
    return "homebrew/sets/"_L1 + identifier + u'.' + ext;
  case AssetCategory::HomebrewBox:
    return "homebrew/boxes/"_L1 + identifier + u'.' + ext;
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

QUrl AssetLocator::defaultAssetBase() {
  return QUrl(QStringLiteral("https://assets.arkhamhorror.app"));
}

AssetOutcome<QVector<AssetCandidate>>
AssetLocator::resolveCandidates(const AssetKey &key) {
  const UrlValidationResult baseValidation =
      validateCustomUrl(key.assetBase.toString(QUrl::FullyEncoded));
  if (!baseValidation) {
    return AssetError{
        AssetErrorCode::InvalidAssetBase,
        QStringLiteral("asset base URL failed validation: %1")
            .arg(baseValidation.error().message),
    };
  }
  const QUrl normalizedBase = *baseValidation;

  if (!isValidIdentifier(key.identifier,
                         identifierMaxLengthFor(key.category))) {
    return AssetError{
        AssetErrorCode::InvalidIdentifier,
        QStringLiteral("asset identifier is empty, too long, or contains a "
                       "character outside [a-z0-9_-]"),
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

  QVector<AssetCandidate> candidates;
  QSet<QString> seenUrls;

  auto tryAppend = [&](const QString &localeDir, const QString &isoLocale,
                       AssetSide side, bool isAlternateFrontFallback) {
    const QString relativePath = buildRelativePath(key.category, key.identifier,
                                                   side, localeDir, key.format);
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
  // client already knows will fail.
  if (localizable && !key.locale.isEmpty() && key.locale != "en"_L1) {
    const QString webLocale = AssetLocaleDigest::webLocaleFor(key.locale);
    if (!webLocale.isEmpty() &&
        AssetLocaleDigest::hasLocalizedVariant(webLocale, key.category,
                                               key.identifier, key.side)) {
      tryAppend(webLocale, key.locale, key.side, false);
    }
  }

  // 2. English/default candidate: always present.
  tryAppend(QString(), QString(), key.side, false);

  // 3. Alternate-front fallback: only for a plain Front card request.
  if (localizable && key.side == AssetSide::Front) {
    tryAppend(QString(), QString(), AssetSide::AlternateFront, true);
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
