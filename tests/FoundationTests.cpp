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
  QVERIFY(!Arkham::ServerProfile(QUrl(QStringLiteral("file:///tmp/server")))
               .isValid());
  QVERIFY(!Arkham::ServerProfile(QUrl(QStringLiteral("https:///missing-host")))
               .isValid());
  QVERIFY(
      Arkham::ServerProfile(QUrl(QStringLiteral("https://arkhamhorror.app")))
          .isValid());
}

void FoundationTests::givesSemanticCommandsStableNames() {
  QCOMPARE(Arkham::commandName(Arkham::AppCommand::Select),
           QStringLiteral("select"));
  QCOMPARE(Arkham::commandName(Arkham::AppCommand::OpenHand),
           QStringLiteral("open-hand"));
}

QTEST_APPLESS_MAIN(FoundationTests)

#include "FoundationTests.moc"
