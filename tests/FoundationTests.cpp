#include <array>

#include <QtTest>

#include "AppCommand.h"
#include "ServerProfile.h"

class FoundationTests final : public QObject {
  Q_OBJECT

private slots:
  void normalizesApiUrls();
  void convertsWebsocketSchemes();
  void rejectsUnsupportedServerSchemes();
  void givesSemanticCommandsStableNames();
};

void FoundationTests::normalizesApiUrls() {
  const Arkham::ServerProfile profile(
      QUrl(QStringLiteral("https://example.com/ignored?q=1")));

  QCOMPARE(profile.baseUrl(), QUrl(QStringLiteral("https://example.com")));
  QCOMPARE(profile.apiUrl(u"whoami"),
           QUrl(QStringLiteral("https://example.com/api/v1/whoami")));
}

void FoundationTests::convertsWebsocketSchemes() {
  const Arkham::ServerProfile secure(
      QUrl(QStringLiteral("https://example.com")));
  const Arkham::ServerProfile local(
      QUrl(QStringLiteral("http://deck.local:3000")));

  QCOMPARE(
      secure.websocketUrl(u"/arkham/games/game-id"),
      QUrl(QStringLiteral("wss://example.com/api/v1/arkham/games/game-id")));
  QCOMPARE(local.websocketUrl(u"/arkham/events/event-id"),
           QUrl(QStringLiteral(
               "ws://deck.local:3000/api/v1/arkham/events/event-id")));
}

void FoundationTests::rejectsUnsupportedServerSchemes() {
  const Arkham::ServerProfile unsupported(
      QUrl(QStringLiteral("file:///tmp/server")));
  QVERIFY(!unsupported.isValid());
  QVERIFY(unsupported.apiUrl(u"whoami").isEmpty());
  QVERIFY(unsupported.websocketUrl(u"/arkham/games/game-id").isEmpty());
  QVERIFY(!Arkham::ServerProfile(QUrl(QStringLiteral("https:///missing-host")))
               .isValid());
  QVERIFY(
      Arkham::ServerProfile(QUrl(QStringLiteral("https://arkhamhorror.app")))
          .isValid());
}

void FoundationTests::givesSemanticCommandsStableNames() {
  const std::array mappings{
      std::pair{Arkham::AppCommand::MoveUp, QStringLiteral("move-up")},
      std::pair{Arkham::AppCommand::MoveDown, QStringLiteral("move-down")},
      std::pair{Arkham::AppCommand::MoveLeft, QStringLiteral("move-left")},
      std::pair{Arkham::AppCommand::MoveRight, QStringLiteral("move-right")},
      std::pair{Arkham::AppCommand::Select, QStringLiteral("select")},
      std::pair{Arkham::AppCommand::Back, QStringLiteral("back")},
      std::pair{Arkham::AppCommand::Inspect, QStringLiteral("inspect")},
      std::pair{Arkham::AppCommand::OpenActions,
                QStringLiteral("open-actions")},
      std::pair{Arkham::AppCommand::OpenHand, QStringLiteral("open-hand")},
      std::pair{Arkham::AppCommand::OpenLog, QStringLiteral("open-log")},
      std::pair{Arkham::AppCommand::RestoreCamera,
                QStringLiteral("restore-camera")},
  };

  for (const auto &[command, expectedName] : mappings) {
    QCOMPARE(Arkham::commandName(command), expectedName);
  }
}

QTEST_APPLESS_MAIN(FoundationTests)

#include "FoundationTests.moc"
