#include "ServerProfile.h"

namespace Arkham {

ServerProfile::ServerProfile(QUrl baseUrl) : m_baseUrl(std::move(baseUrl)) {
  m_baseUrl.setPath({});
  m_baseUrl.setQuery({});
  m_baseUrl.setFragment({});
}

const QUrl &ServerProfile::baseUrl() const { return m_baseUrl; }

QUrl ServerProfile::apiUrl(const QStringView path) const {
  if (!isValid()) {
    return {};
  }

  QUrl result = m_baseUrl;
  const QString pathString = path.toString();
  const QString normalizedPath = pathString.startsWith(QLatin1Char('/'))
                                     ? pathString
                                     : QStringLiteral("/") + pathString;
  result.setPath(QStringLiteral("/api/v1") + normalizedPath);
  return result;
}

QUrl ServerProfile::websocketUrl(const QStringView path) const {
  if (!isValid()) {
    return {};
  }

  QUrl result = apiUrl(path);
  if (result.scheme() == QStringLiteral("https")) {
    result.setScheme(QStringLiteral("wss"));
  } else if (result.scheme() == QStringLiteral("http")) {
    result.setScheme(QStringLiteral("ws"));
  }
  return result;
}

bool ServerProfile::isValid() const {
  return m_baseUrl.isValid() && !m_baseUrl.host().isEmpty() &&
         (m_baseUrl.scheme() == QStringLiteral("https") ||
          m_baseUrl.scheme() == QStringLiteral("http"));
}

} // namespace Arkham
