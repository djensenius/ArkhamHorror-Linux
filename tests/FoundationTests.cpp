#include <QtTest>

#include "ServerProfile.h"

class FoundationTests final : public QObject {
  Q_OBJECT

private slots:
  void normalizesApiUrls();
  void convertsWebsocketSchemes();
  void rejectsUnsupportedServerSchemes();
};

void FoundationTests::normalizesApiUrls() {
  // ServerProfile::custom() rejects a URL carrying userinfo or a query
  // string outright -- unlike the removed raw-QUrl constructor, which used
  // to silently strip credentials/path/query/fragment rather than
  // rejecting them. This is the structural fix for the public-constructor
  // bypass: there is no longer any way to construct a profile whose
  // original URL text was not run through UrlValidator::validateCustomUrl().
  const auto withCredentialsAndQuery = Arkham::ServerProfile::custom(
      QStringLiteral("Test"),
      QStringLiteral("https://investigator:secret@example.com/ignored?q=1"));
  QVERIFY(!withCredentialsAndQuery.has_value());

  const auto profile = Arkham::ServerProfile::custom(
      QStringLiteral("Test"), QStringLiteral("https://example.com"));
  QVERIFY2(profile.has_value(), qPrintable(profile.error()));
  QCOMPARE(profile->baseUrl(), QUrl(QStringLiteral("https://example.com")));
  QCOMPARE(profile->apiUrl(u"whoami"),
           QUrl(QStringLiteral("https://example.com/api/v1/whoami")));
}

void FoundationTests::convertsWebsocketSchemes() {
  const auto secure = Arkham::ServerProfile::custom(
      QStringLiteral("Secure"), QStringLiteral("https://example.com"));
  // http is only ever permitted to a canonical loopback host, so this uses
  // "localhost" rather than a LAN-style hostname.
  const auto local = Arkham::ServerProfile::custom(
      QStringLiteral("Local"), QStringLiteral("http://localhost:3000"));
  QVERIFY2(secure.has_value(), qPrintable(secure.error()));
  QVERIFY2(local.has_value(), qPrintable(local.error()));

  QCOMPARE(
      secure->websocketUrl(u"/arkham/games/game-id"),
      QUrl(QStringLiteral("wss://example.com/api/v1/arkham/games/game-id")));
  QCOMPARE(local->websocketUrl(u"/arkham/events/event-id"),
           QUrl(QStringLiteral(
               "ws://localhost:3000/api/v1/arkham/events/event-id")));
}

void FoundationTests::rejectsUnsupportedServerSchemes() {
  const auto unsupported = Arkham::ServerProfile::custom(
      QStringLiteral("Test"), QStringLiteral("file:///tmp/server"));
  QVERIFY(!unsupported.has_value());

  const auto missingHost = Arkham::ServerProfile::custom(
      QStringLiteral("Test"), QStringLiteral("https:///missing-host"));
  QVERIFY(!missingHost.has_value());

  const auto valid = Arkham::ServerProfile::custom(
      QStringLiteral("Test"), QStringLiteral("https://arkhamhorror.app"));
  QVERIFY2(valid.has_value(), qPrintable(valid.error()));
  QVERIFY(valid->isValid());

  // A default-constructed profile -- the only public constructor
  // ServerProfile exposes besides the three validated factories -- is
  // always invalid (no scheme, no host) and can never produce a usable
  // API/websocket URL.
  const Arkham::ServerProfile blank;
  QVERIFY(!blank.isValid());
  QVERIFY(blank.apiUrl(u"whoami").isEmpty());
  QVERIFY(blank.websocketUrl(u"/arkham/games/game-id").isEmpty());
}

QTEST_APPLESS_MAIN(FoundationTests)

#include "FoundationTests.moc"
