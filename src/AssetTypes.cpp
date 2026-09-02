#include "AssetTypes.h"

#include "UrlValidator.h"

#include <QLatin1StringView>
#include <QStringList>
#include <QtAssert>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// Review round-4 item 2: earlier revisions of this function iteratively
// percent-decoded the raw path (bounded to 5 passes) before checking for
// dot segments/backslashes/control characters, to catch multiply-encoded
// traversal attempts (e.g. "%252e%252e" -> "..") that a single decode
// pass would miss. That is provably still bypassable in principle by an
// arbitrarily deep encoding depth beyond the chosen pass bound, and it
// also had to accept and silently normalize completely benign-looking
// escapes (e.g. "%41" -> "A") as a side effect of being a *decoder*
// rather than a *validator*. An asset base path is a short, operator-
// configured literal string (never end-user input, never something that
// legitimately needs percent-encoding) -- so the simplest, provably
// complete policy is to reject the presence of the '%' character
// anywhere in the raw path outright, before any decoding is attempted at
// all. This has no legitimate false-positive cost (a real asset base
// path never needs a literal '%' either) and closes the entire class of
// "how many passes is enough" bypasses in one step, at any encoding
// depth.
bool rawPathIsHostile(const QString &rawPath) {
  if (rawPath.contains(u'%')) {
    return true;
  }
  if (rawPath.contains(u'\\')) {
    return true;
  }
  for (const QChar c : rawPath) {
    if (c.category() == QChar::Other_Control) {
      return true;
    }
  }
  const QStringList segments = rawPath.split(u'/');
  for (const QString &segment : segments) {
    if (segment == "."_L1 || segment == ".."_L1) {
      return true;
    }
  }
  return false;
}

// Extracts the raw path portion (everything after the authority, up to
// but not including any query/fragment) from the ORIGINAL trimmed input
// text -- not from a parsed QUrl -- so rawPathIsHostile() above sees the
// caller's literal bytes. Returns an empty string if there is no path at
// all (validateCustomUrl() has already confirmed the scheme/authority
// shape by the time this runs, so a naive "://" search here is safe).
QString extractRawPath(const QString &trimmedInput) {
  const qsizetype schemeEnd = trimmedInput.indexOf("://"_L1);
  if (schemeEnd < 0) {
    return QString();
  }
  const qsizetype authorityStart = schemeEnd + 3;
  qsizetype pathStart = trimmedInput.indexOf(u'/', authorityStart);
  if (pathStart < 0) {
    return QString();
  }
  QString rawPath = trimmedInput.mid(pathStart);
  qsizetype queryOrFragment = -1;
  for (qsizetype i = 0; i < rawPath.size(); ++i) {
    if (rawPath[i] == u'?' || rawPath[i] == u'#') {
      queryOrFragment = i;
      break;
    }
  }
  if (queryOrFragment >= 0) {
    rawPath = rawPath.left(queryOrFragment);
  }
  return rawPath;
}

} // namespace

AssetOutcome<ValidatedAssetSource>
ValidatedAssetSource::fromRaw(const QString &rawInput) {
  // Deliberately calls validateCustomUrl() with the caller's ORIGINAL raw
  // string -- never a QUrl the caller may have already constructed -- so
  // the raw-authority-only checks inside it (control characters before
  // trimming, ambiguous loopback spellings, Unicode homoglyphs) still have
  // the evidence they need. This is the exact same policy
  // ServerProfile/NetworkAuthenticationClient use; asset bases never fork
  // a weaker interpretation of it.
  const UrlValidationResult validated = validateCustomUrl(rawInput);
  if (!validated) {
    return AssetError{
        AssetErrorCode::InvalidAssetBase,
        validated.error().message,
    };
  }

  // Review item 3: asset-specific hardening ON TOP OF the shared policy
  // above (never instead of it) -- dot-segment/percent-encoded-separator/
  // backslash/control-character rejection in the path, checked against
  // the SAME raw input text validateCustomUrl() just accepted (trimmed
  // the same way: leading/trailing plain whitespace only, since a control
  // character anywhere would already have failed validateCustomUrl()).
  const QString rawPath = extractRawPath(rawInput.trimmed());
  if (rawPathIsHostile(rawPath)) {
    return AssetError{
        AssetErrorCode::InvalidAssetBase,
        QStringLiteral("asset base path must not contain a literal '%' "
                       "character, a dot segment, a backslash, or a "
                       "control character"),
    };
  }

  // Review item 3: canonicalize away an explicit default port (":443" for
  // https, ":80" for http) so two spellings of the exact same server
  // share the exact same cache-namespace/coalescing identity; a
  // non-default port is preserved verbatim, and the path's case is never
  // altered (validateCustomUrl() already preserves it).
  QUrl normalized = *validated;
  const bool isDefaultHttpsPort =
      normalized.scheme() == "https"_L1 && normalized.port() == 443;
  const bool isDefaultHttpPort =
      normalized.scheme() == "http"_L1 && normalized.port() == 80;
  if (isDefaultHttpsPort || isDefaultHttpPort) {
    normalized.setPort(-1);
  }

  return ValidatedAssetSource(normalized);
}

QString assetFormatExtension(AssetFormat format) {
  switch (format) {
  case AssetFormat::Avif:
    return QStringLiteral("avif");
  case AssetFormat::Jpeg:
    return QStringLiteral("jpg");
  case AssetFormat::Png:
    return QStringLiteral("png");
  }
  Q_UNREACHABLE_RETURN(QString());
}

QString assetFormatMimeType(AssetFormat format) {
  switch (format) {
  case AssetFormat::Avif:
    return QStringLiteral("image/avif");
  case AssetFormat::Jpeg:
    return QStringLiteral("image/jpeg");
  case AssetFormat::Png:
    return QStringLiteral("image/png");
  }
  Q_UNREACHABLE_RETURN(QString());
}

} // namespace Arkham
