#include "AssetLocator.h"

#include "AuthTransportSecurity.h"
#include "CardArt.h"

#include <QStringList>

using namespace Qt::StringLiterals;

namespace Arkham::AssetLocator {

namespace {

constexpr QStringView kDefaultAssetBase = u"https://assets.arkhamhorror.app";

bool containsControlCharacter(const QString &text) {
  for (const QChar c : text) {
    if (c.category() == QChar::Other_Control) {
      return true;
    }
  }
  return false;
}

bool isAsciiDigit(QChar c) { return c >= u'0' && c <= u'9'; }

bool isAsciiLower(QChar c) { return c >= u'a' && c <= u'z'; }

bool isLegacyXArtCodeSegment(const QString &segment) {
  if (segment.size() < 2 || segment.front() != u'x') {
    return false;
  }
  for (qsizetype index = 1; index < segment.size(); ++index) {
    if (!isAsciiLower(segment.at(index))) {
      return false;
    }
  }
  return true;
}

bool isNumberedArtCodeSegment(const QString &segment) {
  qsizetype index = 0;
  if (!segment.isEmpty() && isAsciiLower(segment.at(index)) &&
      index + 1 < segment.size() && isAsciiDigit(segment.at(index + 1))) {
    ++index;
  }
  const qsizetype digitsStart = index;
  while (index < segment.size() && isAsciiDigit(segment.at(index))) {
    ++index;
  }
  if (index == digitsStart) {
    return false;
  }
  while (index < segment.size()) {
    if (!isAsciiLower(segment.at(index))) {
      return false;
    }
    ++index;
  }
  return true;
}

bool isOfficialCardCodeSegment(const QString &segment) {
  if (segment.isEmpty()) {
    return false;
  }
  return isNumberedArtCodeSegment(segment) || isLegacyXArtCodeSegment(segment);
}

bool isHomebrewCampaignSegment(const QString &segment) {
  if (segment.isEmpty()) {
    return false;
  }
  for (const QChar c : segment) {
    if (!isAsciiDigit(c) && !isAsciiLower(c) && c != u'-') {
      return false;
    }
  }
  return true;
}

AssetError invalidKey(const QString &message) {
  return AssetError{AssetErrorCode::InvalidAssetKey, message};
}

AssetOutcome<bool> validateAssetBaseUrl(const QUrl &url) {
  if (!url.isValid() || url.host().isEmpty()) {
    return AssetError{
        AssetErrorCode::InvalidAssetBaseUrl,
        QStringLiteral("asset base URL must be absolute and include a host")};
  }
  if (!url.userInfo().isEmpty()) {
    return AssetError{
        AssetErrorCode::InvalidAssetBaseUrl,
        QStringLiteral("asset base URL must not contain credentials")};
  }
  if (url.hasQuery() || url.hasFragment()) {
    return AssetError{
        AssetErrorCode::InvalidAssetBaseUrl,
        QStringLiteral("asset base URL must not contain a query or fragment")};
  }
  const QString scheme = url.scheme();
  if (scheme != "https"_L1 && scheme != "http"_L1) {
    return AssetError{AssetErrorCode::UnsupportedScheme,
                      QStringLiteral("asset base URL must use https, except "
                                     "http loopback for local development")};
  }
  if (!isSecureOrLoopbackAuthTransport(url)) {
    return AssetError{
        AssetErrorCode::InsecureTransport,
        QStringLiteral(
            "asset base URL requires https unless the host is loopback")};
  }
  return true;
}

QString stripOneServerCardPrefix(const QString &cardCode) {
  if (cardCode.startsWith(u'c')) {
    return cardCode.mid(1);
  }
  return cardCode;
}

// The file name without ".avif": the whole art the web's imgsrc and
// cardImgPath match their reprint and homebrew rules against.
QString imageFileStem(QString artCode, const AssetLocator::CardImageKey &key) {
  if (key.face == AssetLocator::CardFace::Back) {
    artCode += u'b';
  }
  artCode += key.classVariantSuffix;
  artCode += key.mutationSuffix;
  return artCode;
}

QUrl appendPathSegments(QUrl base, const QStringList &segments) {
  QString path = base.path();
  while (path.endsWith(u'/')) {
    path.chop(1);
  }
  for (const QString &segment : segments) {
    path += u'/';
    path += segment;
  }
  base.setPath(path, QUrl::DecodedMode);
  return base;
}

AssetOutcome<QUrl> normalizeAssetBaseUrl(const QString &input) {
  if (containsControlCharacter(input)) {
    return AssetError{
        AssetErrorCode::InvalidAssetBaseUrl,
        QStringLiteral("asset base URL must not contain control characters")};
  }
  const QString trimmedInput = input.trimmed();
  if (trimmedInput.isEmpty()) {
    return AssetError{AssetErrorCode::InvalidAssetBaseUrl,
                      QStringLiteral("asset base URL must not be empty")};
  }

  const QUrl url(trimmedInput, QUrl::StrictMode);
  if (!url.isValid()) {
    return AssetError{
        AssetErrorCode::InvalidAssetBaseUrl,
        QStringLiteral("invalid asset base URL: %1").arg(url.errorString())};
  }

  const QString scheme = url.scheme();
  if (scheme != "https"_L1 && scheme != "http"_L1) {
    return AssetError{AssetErrorCode::UnsupportedScheme,
                      QStringLiteral("asset base URL must use https, except "
                                     "http loopback for local development")};
  }
  if (url.host().isEmpty()) {
    return AssetError{AssetErrorCode::InvalidAssetBaseUrl,
                      QStringLiteral("asset base URL must include a host")};
  }
  if (!url.userInfo().isEmpty()) {
    return AssetError{
        AssetErrorCode::InvalidAssetBaseUrl,
        QStringLiteral("asset base URL must not contain credentials")};
  }
  if (url.hasQuery() || url.hasFragment()) {
    return AssetError{
        AssetErrorCode::InvalidAssetBaseUrl,
        QStringLiteral("asset base URL must not contain a query or fragment")};
  }
  if (!isCleartextAuthAllowedForRawInput(scheme, trimmedInput)) {
    return AssetError{AssetErrorCode::InsecureTransport,
                      QStringLiteral("asset base URL requires https unless the "
                                     "host is exact loopback")};
  }

  QUrl normalized;
  normalized.setScheme(scheme);
  normalized.setHost(url.host());
  if (url.port() != -1) {
    normalized.setPort(url.port());
  }
  QString cleanPath = url.path();
  while (cleanPath.size() > 1 && cleanPath.endsWith(u'/')) {
    cleanPath.chop(1);
  }
  if (!cleanPath.isEmpty() && cleanPath != "/"_L1) {
    normalized.setPath(cleanPath, QUrl::DecodedMode);
  }
  return normalized;
}

} // namespace

QUrl defaultAssetBaseUrl() { return QUrl(kDefaultAssetBase.toString()); }

AssetOutcome<QUrl> assetBaseUrlFromString(const QString &input) {
  return normalizeAssetBaseUrl(input);
}

bool isMutationSuffix(const QString &suffix) {
  constexpr QStringView kPrefix = u"_Mutated";
  if (suffix.isEmpty()) {
    return true;
  }
  if (!suffix.startsWith(kPrefix) || suffix.size() == kPrefix.size()) {
    return false;
  }
  for (qsizetype index = kPrefix.size(); index < suffix.size(); ++index) {
    if (!isAsciiDigit(suffix.at(index))) {
      return false;
    }
  }
  return true;
}

bool isClassVariantSuffix(const QString &suffix) {
  return suffix.isEmpty() || suffix == "_Guardian"_L1 ||
         suffix == "_Seeker"_L1 || suffix == "_Rogue"_L1 ||
         suffix == "_Mystic"_L1 || suffix == "_Survivor"_L1;
}

bool isCardArtId(const QString &cardCode) {
  return buildCardImageUrl(defaultAssetBaseUrl(), {cardCode}).has_value();
}

AssetOutcome<QUrl> buildCardImageUrl(const QUrl &assetBaseUrl,
                                     const CardImageKey &key) {
  const auto baseValidation = validateAssetBaseUrl(assetBaseUrl);
  if (!baseValidation) {
    return baseValidation.error();
  }

  if (!isMutationSuffix(key.mutationSuffix)) {
    return invalidKey(QStringLiteral("mutation suffix must match _Mutated<N>"));
  }
  if (!isClassVariantSuffix(key.classVariantSuffix)) {
    return invalidKey(QStringLiteral(
        "class variant suffix must be _Guardian, _Seeker, _Rogue, _Mystic "
        "or _Survivor"));
  }

  const QString artId = stripOneServerCardPrefix(key.cardCode);
  QStringList pathSegments{QStringLiteral("img"), QStringLiteral("arkham")};
  if (artId.startsWith(u':')) {
    const qsizetype separator = artId.lastIndexOf(u':');
    if (separator <= 1 || separator == artId.size() - 1) {
      return invalidKey(
          QStringLiteral("homebrew card art id must be :campaign:code with "
                         "non-empty segments"));
    }
    const QString campaign = artId.mid(1, separator - 1);
    const QString code = artId.mid(separator + 1);
    if (!isHomebrewCampaignSegment(campaign) ||
        !isOfficialCardCodeSegment(code)) {
      return invalidKey(QStringLiteral(
          "homebrew card art id contains an invalid campaign or code segment"));
    }
    pathSegments << QStringLiteral("homebrew") << campaign
                 << QStringLiteral("cards")
                 << CardArt::homebrewArt(campaign, imageFileStem(code, key)) +
                        ".avif"_L1;
  } else {
    if (!isOfficialCardCodeSegment(artId)) {
      return invalidKey(
          QStringLiteral("card code is not a valid asset code segment"));
    }
    pathSegments << QStringLiteral("cards")
                 << CardArt::reprintedArt(imageFileStem(artId, key)) +
                        ".avif"_L1;
  }

  return appendPathSegments(assetBaseUrl, pathSegments);
}

} // namespace Arkham::AssetLocator
