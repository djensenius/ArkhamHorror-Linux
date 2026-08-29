#include "ServerProfile.h"

#include "ContractPin.h"
#include "UrlValidator.h"

#include <QUuid>
#include <utility>

using namespace Qt::StringLiterals;

namespace Arkham {

// Fixed deterministic ID for the single hosted-default profile.
static constexpr QLatin1StringView kHostedDefaultId{
    "00000000-0000-0000-0000-000000000001"};

// Validate and normalise a display name: trim whitespace, reject blank.
static ValueOrError<QString> validateDisplayName(QString name) {
  name = name.trimmed();
  if (name.isEmpty()) {
    return failure(QStringLiteral("display name must not be blank"));
  }
  return name;
}

ServerProfile ServerProfile::hostedDefault() {
  ServerProfile p;
  p.m_kind = ServerProfileKind::HostedDefault;
  p.m_id = QString::fromLatin1(kHostedDefaultId);
  p.m_displayName = QStringLiteral("Arkham Horror");
  p.m_baseUrl = QUrl(QStringLiteral("https://arkhamhorror.app"));
  p.m_validatedProvenance = true;
  return p;
}

ValueOrError<ServerProfile> ServerProfile::custom(QString displayName,
                                                  const QString &urlString) {
  auto nameResult = validateDisplayName(std::move(displayName));
  if (!nameResult) {
    return failure(nameResult.error());
  }
  const auto urlResult = validateCustomUrl(urlString);
  if (!urlResult) {
    return failure(urlResult.error().message);
  }
  ServerProfile p;
  p.m_kind = ServerProfileKind::Custom;
  p.m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  p.m_displayName = *nameResult;
  p.m_baseUrl = *urlResult;
  p.m_validatedProvenance = true;
  return p;
}

ValueOrError<ServerProfile>
ServerProfile::customWithId(const QString &id, QString displayName,
                            const QString &urlString) {
  // Reject unparseable UUIDs and the all-zero UUID.
  const QUuid parsed(id);
  if (parsed.isNull()) {
    return failure(
        QStringLiteral("profile ID must be a non-null UUID: \"%1\"").arg(id));
  }
  // Canonicalize to WithoutBraces so stored IDs are always in the same form.
  const QString canonicalId = parsed.toString(QUuid::WithoutBraces);
  // Reject the reserved hosted-default UUID; custom profiles must have a
  // distinct identity from the canonical hosted profile.
  if (canonicalId == QString::fromLatin1(kHostedDefaultId)) {
    return failure(
        QStringLiteral("profile ID \"%1\" is reserved for the hosted default "
                       "profile")
            .arg(canonicalId));
  }
  auto nameResult = validateDisplayName(std::move(displayName));
  if (!nameResult) {
    return failure(nameResult.error());
  }
  const auto urlResult = validateCustomUrl(urlString);
  if (!urlResult) {
    return failure(urlResult.error().message);
  }
  ServerProfile p;
  p.m_kind = ServerProfileKind::Custom;
  p.m_id = canonicalId;
  p.m_displayName = *nameResult;
  p.m_baseUrl = *urlResult;
  p.m_validatedProvenance = true;
  return p;
}

ServerProfile ServerProfile::unvalidatedForTesting(QUrl baseUrl) {
  ServerProfile p;
  p.m_baseUrl = std::move(baseUrl);
  // m_id stays empty and m_validatedProvenance stays false: this is the
  // otherwise-unreachable state the defensive rejection tests exercise.
  return p;
}

const QString &ServerProfile::profileId() const { return m_id; }

ServerProfileKind ServerProfile::kind() const { return m_kind; }

const QString &ServerProfile::displayName() const { return m_displayName; }

const QUrl &ServerProfile::baseUrl() const { return m_baseUrl; }

namespace {
// The port a URL effectively targets: its explicit port if one was given,
// otherwise the well-known default for its scheme. QUrl::port() itself
// returns -1 whenever no port was given, so comparing raw port() values
// would treat "https://host" and "https://host:443" as different
// endpoints despite them being identical.
int effectivePort(const QUrl &url) {
  const int explicitPort = url.port();
  if (explicitPort != -1) {
    return explicitPort;
  }
  if (url.scheme() == "https"_L1) {
    return 443;
  }
  if (url.scheme() == "http"_L1) {
    return 80;
  }
  return -1;
}
} // namespace

bool ServerProfile::hasEquivalentEndpoint(const ServerProfile &other) const {
  return m_baseUrl.scheme() == other.m_baseUrl.scheme() &&
         m_baseUrl.host() == other.m_baseUrl.host() &&
         effectivePort(m_baseUrl) == effectivePort(other.m_baseUrl) &&
         m_baseUrl.path() == other.m_baseUrl.path();
}

QUrl ServerProfile::apiUrl(const QStringView path) const {
  if (!isValid()) {
    return {};
  }

  // Honour any stored path prefix (e.g. /selfhosted set by custom()).
  // basePath is empty for a default-constructed profile, since a default
  // profile's baseUrl is always empty and never reaches this point anyway
  // (isValid() is false for it).
  QString basePath = m_baseUrl.path();
  if (basePath.size() > 1 && basePath.endsWith(QLatin1Char('/'))) {
    basePath.chop(1);
  }
  if (basePath == QLatin1String("/")) {
    basePath.clear();
  }

  QUrl result = m_baseUrl;
  const QString pathString = path.toString();
  const QString normalizedPath = pathString.startsWith(QLatin1Char('/'))
                                     ? pathString
                                     : QStringLiteral("/") + pathString;
  result.setPath(basePath + currentPin().expectedApiBasePath + normalizedPath);
  return result;
}

QUrl ServerProfile::websocketUrl(const QStringView path) const {
  if (!isValid()) {
    return {};
  }

  QUrl result = apiUrl(path);
  if (result.scheme() == "https"_L1) {
    result.setScheme(QStringLiteral("wss"));
  } else if (result.scheme() == "http"_L1) {
    result.setScheme(QStringLiteral("ws"));
  }
  return result;
}

bool ServerProfile::isValid() const {
  return m_baseUrl.isValid() && !m_baseUrl.host().isEmpty() &&
         (m_baseUrl.scheme() == "https"_L1 || m_baseUrl.scheme() == "http"_L1);
}

bool ServerProfile::hasValidatedProvenance() const {
  return m_validatedProvenance;
}

} // namespace Arkham
