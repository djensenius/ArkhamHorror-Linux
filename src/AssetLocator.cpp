#include "AssetLocator.h"

#include "AuthTransportSecurity.h"
#include "UrlValidator.h"

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

bool isRejectedSegment(const QString &segment) {
  return segment.isEmpty() || segment == "."_L1 || segment == ".."_L1 ||
         segment.contains(u'/') || segment.contains(u'\\') ||
         containsControlCharacter(segment);
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

QString strippedCardCode(const QString &cardCode) {
  if (cardCode.startsWith(u'c')) {
    return cardCode.mid(1);
  }
  return cardCode;
}

QString imageFileName(QString artCode, AssetLocator::CardFace face) {
  if (face == AssetLocator::CardFace::Back) {
    artCode += u'b';
  }
  return artCode + ".avif"_L1;
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
  base.setPath(path);
  return base;
}

} // namespace

QUrl defaultAssetBaseUrl() { return QUrl(kDefaultAssetBase.toString()); }

AssetOutcome<QUrl> assetBaseUrlFromString(const QString &input) {
  const UrlValidationResult validated = validateCustomUrl(input);
  if (!validated) {
    return AssetError{AssetErrorCode::InvalidAssetBaseUrl,
                      validated.error().message};
  }
  return *validated;
}

AssetOutcome<QUrl> buildCardImageUrl(const QUrl &assetBaseUrl,
                                     const CardImageKey &key) {
  const auto baseValidation = validateAssetBaseUrl(assetBaseUrl);
  if (!baseValidation) {
    return baseValidation.error();
  }

  QStringList pathSegments{QStringLiteral("img"), QStringLiteral("arkham")};
  if (key.cardCode.startsWith(u':')) {
    const qsizetype separator = key.cardCode.lastIndexOf(u':');
    if (separator <= 0 || separator == key.cardCode.size() - 1) {
      return AssetError{
          AssetErrorCode::InvalidAssetKey,
          QStringLiteral("homebrew card art id must be :campaign:code")};
    }
    const QString campaign = key.cardCode.mid(1, separator - 1);
    const QString fileName =
        imageFileName(key.cardCode.mid(separator + 1), key.face);
    if (isRejectedSegment(campaign) || isRejectedSegment(fileName)) {
      return AssetError{
          AssetErrorCode::InvalidAssetKey,
          QStringLiteral("homebrew card art id is not a safe asset path")};
    }
    pathSegments << QStringLiteral("homebrew") << campaign
                 << QStringLiteral("cards") << fileName;
  } else {
    const QString code = strippedCardCode(key.cardCode);
    const QString fileName = imageFileName(code, key.face);
    if (isRejectedSegment(code) || isRejectedSegment(fileName)) {
      return AssetError{
          AssetErrorCode::InvalidAssetKey,
          QStringLiteral("card code is not a safe asset path segment")};
    }
    pathSegments << QStringLiteral("cards") << fileName;
  }

  return appendPathSegments(assetBaseUrl, pathSegments);
}

} // namespace Arkham::AssetLocator
