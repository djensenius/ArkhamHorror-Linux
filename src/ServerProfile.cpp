#include "ServerProfile.h"

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

ServerProfile::ServerProfile(QUrl baseUrl) : m_baseUrl(std::move(baseUrl)) {
  m_baseUrl.setUserInfo(QString{});
  m_baseUrl.setPath({});
  m_baseUrl.setQuery(QString{});
  m_baseUrl.setFragment({});
}

ServerProfile ServerProfile::hostedDefault() {
  ServerProfile p;
  p.m_kind = ServerProfileKind::HostedDefault;
  p.m_id = QString::fromLatin1(kHostedDefaultId);
  p.m_displayName = QStringLiteral("Arkham Horror");
  p.m_baseUrl = QUrl(QStringLiteral("https://arkhamhorror.app"));
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
  return p;
}

ValueOrError<ServerProfile>
ServerProfile::customWithId(const QString &id, QString displayName,
                            const QString &urlString) {
  // Parse and validate the UUID — null means unparseable.
  const QUuid parsed(id);
  if (parsed.isNull()) {
    return failure(
        QStringLiteral("profile ID is not a valid UUID: \"%1\"").arg(id));
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
  return p;
}

const QString &ServerProfile::profileId() const { return m_id; }

ServerProfileKind ServerProfile::kind() const { return m_kind; }

const QString &ServerProfile::displayName() const { return m_displayName; }

const QUrl &ServerProfile::baseUrl() const { return m_baseUrl; }

QUrl ServerProfile::apiUrl(const QStringView path) const {
  if (!isValid()) {
    return {};
  }

  // Honour any stored path prefix (e.g. /selfhosted set by custom()).
  // The default constructor clears the path, so basePath is empty for profiles
  // created via ServerProfile(QUrl).
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
  result.setPath(basePath + QStringLiteral("/api/v1") + normalizedPath);
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

} // namespace Arkham
