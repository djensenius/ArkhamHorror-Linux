#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include <cstdlib>

#include "ServerProfile.h"

int main(int argc, char *argv[]) {
  QGuiApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("Arkham Horror"));
  QCoreApplication::setOrganizationName(QStringLiteral("djensenius"));

  const Arkham::ServerProfile profile = Arkham::ServerProfile::hostedDefault();

  QQmlApplicationEngine engine;
  engine.setInitialProperties(
      {{QStringLiteral("configuredServer"), profile.baseUrl().toString()}});
  engine.loadFromModule(QStringLiteral("ArkhamHorror"), QStringLiteral("Main"));

  if (engine.rootObjects().isEmpty()) {
    return EXIT_FAILURE;
  }

  return app.exec();
}
