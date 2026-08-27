#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include "ServerProfile.h"

int main(int argc, char *argv[]) {
  QGuiApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("Arkham Horror"));
  QCoreApplication::setOrganizationName(QStringLiteral("djensenius"));

  Arkham::ServerProfile profile(
      QUrl(QStringLiteral("https://arkhamhorror.app")));

  QQmlApplicationEngine engine;
  engine.setInitialProperties(
      {{QStringLiteral("configuredServer"), profile.baseUrl().toString()}});
  engine.loadFromModule(QStringLiteral("ArkhamHorror"), QStringLiteral("Main"));

  if (engine.rootObjects().isEmpty()) {
    return EXIT_FAILURE;
  }

  return app.exec();
}
