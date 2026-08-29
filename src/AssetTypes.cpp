#include "AssetTypes.h"

#include "UrlValidator.h"

#include <QLatin1StringView>
#include <QStringList>
#include <QtAssert>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

bool isHexDigit(QChar c) {
  const char16_t u = c.unicode();
  return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') ||
         (u >= u'A' && u <= u'F');
}

int hexValue(QChar c) {
  const char16_t u = c.unicode();
  if (u >= u'0' && u <= u'9') {
    return u - u'0';
  }
  if (u >= u'a' && u <= u'f') {
    return 10 + (u - u'a');
  }
  return 10 + (u - u'A');
}

// Iteratively percent-decodes `input`, case-insensitively, for up to a
// small bounded number of passes -- so a double- or even triple-encoded
// dot segment or separator (e.g. "%252e%252e" -> "%2e%2e" -> "..", or
// "%252f" -> "%2f" -> "/") cannot evade a single-pass check. Malformed
// "%" sequences (not followed by exactly two hex digits) are left
// verbatim; only complete, well-formed escapes are ever decoded. The pass
// bound (5) is generous for any realistic input while still terminating
// deterministically on adversarial "%25%25%25...".
QString iterativelyPercentDecoded(const QString &input) {
  QString current = input;
  for (int pass = 0; pass < 5; ++pass) {
    QString next;
    next.reserve(current.size());
    bool changed = false;
    qsizetype i = 0;
    while (i < current.size()) {
      if (current[i] == u'%' && i + 2 < current.size() &&
          isHexDigit(current[i + 1]) && isHexDigit(current[i + 2])) {
        const int value =
            hexValue(current[i + 1]) * 16 + hexValue(current[i + 2]);
        next += QChar(value);
        i += 3;
        changed = true;
      } else {
        next += current[i];
        ++i;
      }
    }
    current = next;
    if (!changed) {
      break;
    }
  }
  return current;
}

// Review item 3: raw-string path validation, run against the exact
// caller-supplied base URL text (never a QUrl that has already parsed/
// decoded it -- see ValidatedAssetSource's class comment in AssetTypes.h
// for why that evidence must not be destroyed first). Decodes the ENTIRE
// path portion iteratively before splitting on '/', so a percent-encoded
// separator (e.g. "%2f") cannot smuggle an extra segment boundary past a
// naive split-then-decode order. Rejects: any literal or (after full
// decoding) percent-encoded "." or ".." path segment, any literal or
// decoded backslash, and any literal or decoded control character.
bool rawPathIsHostile(const QString &rawPath) {
  const QString decoded = iterativelyPercentDecoded(rawPath);
  if (decoded.contains(u'\\')) {
    return true;
  }
  for (const QChar c : decoded) {
    if (c.category() == QChar::Other_Control) {
      return true;
    }
  }
  const QStringList segments = decoded.split(u'/');
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
        QStringLiteral("asset base path must not contain a dot segment, "
                       "backslash, or control character (literal or "
                       "percent-encoded)"),
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
