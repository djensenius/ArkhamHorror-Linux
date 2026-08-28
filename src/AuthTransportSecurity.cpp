#include "AuthTransportSecurity.h"

#include <QHostAddress>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

bool isLoopbackHost(const QString &host) {
  if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) {
    return true;
  }
  QHostAddress address;
  return address.setAddress(host) && address.isLoopback();
}

} // namespace

bool isSecureOrLoopbackAuthTransport(const QUrl &url) {
  if (!url.userInfo().isEmpty()) {
    return false;
  }
  if (url.scheme() == "https"_L1) {
    return true;
  }
  if (url.scheme() == "http"_L1) {
    return isLoopbackHost(url.host());
  }
  return false;
}

} // namespace Arkham
