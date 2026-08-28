#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include <cstdlib>
#include <memory>

#include "ServerProfile.h"

namespace {

// Generous enough for a software-rendered Qt Quick scenegraph to produce
// its first frame inside a cold, unaccelerated CI container, but still
// bounded: CI must never be able to mistake a hang for success, and this
// deadline is what makes that impossible -- if no frame renders in time,
// the process exits non-zero instead of running forever or (worse) being
// treated as passed by a wrapping `timeout` call.
constexpr int kSmokeTestDeadlineMs = 20000;

} // namespace

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
          "verify a root object was created, enter the real event loop, "
          "and exit only after that window's scenegraph has actually "
          "rendered and swapped its first frame (status 0), or after an "
          "internal deadline elapses with no frame rendered (non-zero). "
          "Does not bypass or shortcut any initialization step, and does "
          "not treat an external timeout as success: a genuine QML/plugin/"
          "rendering failure or hang is always reported as failure by this "
          "process's own exit code. Intended for deterministic headless "
          "CI startup verification (e.g. with QT_QPA_PLATFORM=offscreen), "
          "not for interactive use."));
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
    // The QML root is an ApplicationWindow, i.e. a QQuickWindow; if that
    // ever stopped being true (e.g. a non-Window root component), fail
    // immediately rather than silently skip the frame-rendering proof.
    auto *window =
        qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (window == nullptr) {
      return EXIT_FAILURE;
    }

    // Guards frameSwapped() and the deadline timer from both firing:
    // whichever happens first calls app.exit() and flips this so the
    // other is a no-op, so app.exec() below returns exactly one exit
    // code decided by exactly one of "first frame rendered" or "deadline
    // elapsed with no frame".
    auto decided = std::make_shared<bool>(false);

    QTimer::singleShot(kSmokeTestDeadlineMs, &app, [&app, decided]() {
      if (*decided) {
        return;
      }
      *decided = true;
      app.exit(EXIT_FAILURE);
    });

    // Qt::SingleShotConnection (Qt 6.7+) auto-disconnects after the first
    // emission, so only the window's genuine first rendered-and-swapped
    // frame can trigger success here, never a later one.
    QObject::connect(
        window, &QQuickWindow::frameSwapped, &app,
        [&app, decided]() {
          if (*decided) {
            return;
          }
          *decided = true;
          app.exit(EXIT_SUCCESS);
        },
        Qt::SingleShotConnection);

    return app.exec();
  }

  return app.exec();
}
