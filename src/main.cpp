#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include <cstdlib>

#include "ServerProfile.h"

int main(int argc, char *argv[]) {
  QGuiApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("Arkham Horror"));
  QCoreApplication::setOrganizationName(QStringLiteral("djensenius"));

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("Arkham Horror companion client"));
  parser.addHelpOption();
  const QCommandLineOption smokeTestOption(
      QStringLiteral("smoke-test"),
      QStringLiteral(
          "Perform full normal Qt/QML engine and plugin initialization, "
          "verify a root object was created, then exit immediately (status "
          "0 on success, non-zero on failure) without entering the event "
          "loop. Does not bypass or shortcut any initialization step; it "
          "only skips the indefinite app.exec() event loop afterwards. "
          "Intended for deterministic headless CI startup verification "
          "(e.g. with QT_QPA_PLATFORM=offscreen), not for interactive use."));
  parser.addOption(smokeTestOption);
  parser.process(app);

  const Arkham::ServerProfile profile = Arkham::ServerProfile::hostedDefault();

  QQmlApplicationEngine engine;
  engine.setInitialProperties(
      {{QStringLiteral("configuredServer"), profile.baseUrl().toString()}});
  engine.loadFromModule(QStringLiteral("ArkhamHorror"), QStringLiteral("Main"));

  if (engine.rootObjects().isEmpty()) {
    return EXIT_FAILURE;
  }

  if (parser.isSet(smokeTestOption)) {
    return EXIT_SUCCESS;
  }

  return app.exec();
}
