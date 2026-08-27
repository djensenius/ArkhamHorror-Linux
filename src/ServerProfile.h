#pragma once

#include <QStringView>
#include <QUrl>

namespace Arkham {

class ServerProfile {
public:
  explicit ServerProfile(QUrl baseUrl = {});

  [[nodiscard]] const QUrl &baseUrl() const;
  [[nodiscard]] QUrl apiUrl(QStringView path) const;
  [[nodiscard]] QUrl websocketUrl(QStringView path) const;
  [[nodiscard]] bool isValid() const;

private:
  QUrl m_baseUrl;
};

} // namespace Arkham
