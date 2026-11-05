// GameClient against a real loopback socket (MockHttpServer): request path
// and Authorization header, decode through the raw-bytes decoders, and the
// typed outcome for every failure class.

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QtTest>

#include <chrono>
#include <functional>
#include <optional>

#include "GameClient.h"
#include "Games.h"
#include "MockHttpServer.h"
#include "ServerProfile.h"

using namespace Arkham;
using namespace Qt::StringLiterals;

namespace {

const QString kGameId = u"00000000-0000-0000-0000-000000000003"_s;
const QString kToken = u"test-token-123"_s;

QByteArray fixture(const QString &name) {
  QFile f(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + u"/fixtures/"_s + name);
  if (!f.open(QIODevice::ReadOnly))
    qFatal("missing fixture: %s", qPrintable(name));
  return f.readAll();
}

QByteArray replaced(QByteArray bytes, const QByteArray &from,
                    const QByteArray &to) {
  const qsizetype at = bytes.indexOf(from);
  if (at < 0)
    qFatal("fixture edit target not found: %s", from.constData());
  return bytes.replace(at, from.size(), to);
}

MockHttpServer::Response jsonResponse(int status, QByteArray body) {
  MockHttpServer::Response response;
  response.status = status;
  response.contentType = "application/json; charset=utf-8"_ba;
  response.body = std::move(body);
  return response;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 5000) {
  const QDeadlineTimer deadline(timeoutMs);
  while (!condition() && !deadline.hasExpired())
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return condition();
}

struct GameCall {
  int calls{0};
  GameRequestStatus status;
  std::optional<GetGame> game;
};

struct ListCall {
  int calls{0};
  GameRequestStatus status;
  std::optional<QList<GameListRow>> rows;
};

} // namespace

class GameClientTests : public QObject {
  Q_OBJECT

private:
  MockHttpServer *m_server{nullptr};
  std::optional<ServerProfile> m_profile;
  QNetworkAccessManager *m_nam{nullptr};

  QString gamePath() const {
    return m_profile->apiUrl(u"arkham/games/"_s + kGameId).path();
  }
  QString listPath() const { return m_profile->apiUrl(u"arkham/games").path(); }

  GameCall fetchGame(const QString &id = kGameId,
                     const QString &token = kToken) {
    GameClient client(*m_nam);
    GameCall result;
    client.getGame(
        *m_profile, id, token,
        [&result](const GameRequestStatus &status, const GetGame *game) {
          ++result.calls;
          result.status = status;
          if (game)
            result.game = *game;
        });
    // Never reentrant: nothing runs before the event loop does.
    if (result.calls != 0)
      qFatal("GameClient invoked its callback synchronously");
    if (!waitUntil([&result] { return result.calls > 0; }))
      qWarning("GameClient callback never ran");
    return result;
  }

  ListCall fetchList() {
    GameClient client(*m_nam);
    ListCall result;
    client.listGames(*m_profile, kToken,
                     [&result](const GameRequestStatus &status,
                               const QList<GameListRow> *rows) {
                       ++result.calls;
                       result.status = status;
                       if (rows)
                         result.rows = *rows;
                     });
    if (!waitUntil([&result] { return result.calls > 0; }))
      qWarning("GameClient callback never ran");
    return result;
  }

private slots:
  void init() {
    m_server = new MockHttpServer(this);
    QVERIFY(m_server->start());
    auto profile = ServerProfile::custom(
        u"Mock"_s, u"http://127.0.0.1:%1"_s.arg(m_server->serverPort()));
    QVERIFY2(profile.has_value(), qPrintable(profile.error()));
    m_profile = *profile;
    m_nam = new QNetworkAccessManager(this);
  }

  void cleanup() {
    delete m_nam;
    m_nam = nullptr;
    delete m_server;
    m_server = nullptr;
    m_profile.reset();
  }

  void getGameDecodesFixtureAndSendsToken() {
    QCOMPARE(gamePath(), u"/api/v1/arkham/games/"_s + kGameId);
    m_server->setResponse(gamePath(),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    const GameCall call = fetchGame();
    QCOMPARE(call.calls, 1);
    QCOMPARE(call.status.outcome, GameRequestOutcome::Success);
    QCOMPARE(call.status.httpStatus, 200);
    QVERIFY(call.status.diagnostic.isEmpty());
    QVERIFY(call.game.has_value());
    QVERIFY(call.game->playerId().has_value());
    QCOMPARE(call.game->playerId()->value(),
             u"00000000-0000-0000-0000-000000000001"_s);
    QCOMPARE(call.game->multiplayerMode(), MultiplayerVariant::Solo);
    QCOMPARE(call.game->game().encounterDeckSize(), 26);

    const QByteArray request = m_server->lastRequest();
    QVERIFY2(request.startsWith("GET /api/v1/arkham/games/" +
                                kGameId.toLatin1() + " HTTP/1.1\r\n"),
             request.constData());
    QVERIFY(request.contains("\r\nAuthorization: Token test-token-123\r\n"));
    QVERIFY(request.contains("\r\nAccept: application/json\r\n"));
    QVERIFY(!request.contains("Cookie"));
    QCOMPARE(m_server->requestCountForPath(gamePath()), 1);
  }

  void getGameDecodeFailureReportsDecoderError() {
    m_server->setResponse(
        gamePath(),
        jsonResponse(200, replaced(fixture(u"get-game.json"_s),
                                   R"("multiplayerMode": "Solo")"_ba,
                                   R"("multiplayerMode": "Party")"_ba)));
    const GameCall call = fetchGame();
    QCOMPARE(call.calls, 1);
    QCOMPARE(call.status.outcome, GameRequestOutcome::DecodeFailed);
    QCOMPARE(call.status.httpStatus, 200);
    QCOMPARE(call.status.diagnostic,
             uR"(getGame.multiplayerMode: unrecognized value "Party")"_s);
    QVERIFY(!call.game.has_value());
  }

  void getGameHttpErrors_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<GameRequestOutcome>("outcome");
    QTest::addColumn<QString>("diagnostic");
    QTest::newRow("401") << 401 << GameRequestOutcome::Unauthorized
                         << u"server rejected the request as unauthorized"_s;
    QTest::newRow("404") << 404 << GameRequestOutcome::NotFound
                         << u"game not found"_s;
    QTest::newRow("500") << 500 << GameRequestOutcome::HttpError
                         << u"unexpected HTTP status 500"_s;
    QTest::newRow("plain 503") << 503 << GameRequestOutcome::HttpError
                               << u"unexpected HTTP status 503"_s;
    QTest::newRow("redirect not followed")
        << 302 << GameRequestOutcome::HttpError
        << u"unexpected HTTP status 302"_s;
  }

  void getGameHttpErrors() {
    QFETCH(int, status);
    QFETCH(GameRequestOutcome, outcome);
    QFETCH(QString, diagnostic);
    MockHttpServer::Response response = jsonResponse(status, "{}"_ba);
    if (status == 302)
      response.location = "http://127.0.0.1:1/elsewhere"_ba;
    m_server->setResponse(gamePath(), response);
    const GameCall call = fetchGame();
    QCOMPARE(call.calls, 1);
    QCOMPARE(call.status.outcome, outcome);
    QCOMPARE(call.status.httpStatus, status);
    QCOMPARE(call.status.diagnostic, diagnostic);
    QVERIFY(!call.game.has_value());
    QCOMPARE(m_server->requestCount(), 1);
  }

  void getGameMaintenanceIsTyped() {
    MockHttpServer::Response response =
        jsonResponse(503, R"({"maintenance":true,"message":"Back soon.",)"
                          R"("since":"2026-11-03T22:15:00Z"})"_ba);
    response.extraHeaders = {{"Retry-After"_ba, "120"_ba}};
    m_server->setResponse(gamePath(), response);
    const GameCall call = fetchGame();
    QCOMPARE(call.status.outcome, GameRequestOutcome::Maintenance);
    QCOMPARE(call.status.httpStatus, 503);
    QVERIFY(call.status.maintenance.has_value());
    QCOMPARE(call.status.maintenance->retryAfterSeconds, std::optional(120));
    QCOMPARE(call.status.diagnostic,
             u"Back soon. Try again in about 2 minutes."_s);
  }

  void getGameTransportFailure() {
    MockHttpServer::Response response;
    response.hang = true;
    m_server->setResponse(gamePath(), response);
    GameClient client(*m_nam, std::chrono::milliseconds(200));
    GameCall result;
    client.getGame(*m_profile, kGameId, kToken,
                   [&result](const GameRequestStatus &status, const GetGame *) {
                     ++result.calls;
                     result.status = status;
                   });
    QVERIFY(waitUntil([&result] { return result.calls > 0; }));
    QCOMPARE(result.calls, 1);
    QCOMPARE(result.status.outcome, GameRequestOutcome::Transport);
    QCOMPARE(result.status.diagnostic, u"request timed out"_s);
  }

  void getGameRejectsBadInputWithoutSending_data() {
    QTest::addColumn<QString>("id");
    QTest::addColumn<QString>("token");
    QTest::addColumn<QString>("diagnostic");
    QTest::newRow("bad id")
        << u"not-a-uuid"_s << kToken << u"game ID is not a valid UUID"_s;
    QTest::newRow("header injection")
        << kGameId << u"abc\r\nX-Evil: 1"_s
        << u"token is not a usable token and cannot be used in an HTTP "
           "header"_s;
  }

  void getGameRejectsBadInputWithoutSending() {
    QFETCH(QString, id);
    QFETCH(QString, token);
    QFETCH(QString, diagnostic);
    const GameCall call = fetchGame(id, token);
    QCOMPARE(call.calls, 1);
    QCOMPARE(call.status.outcome, GameRequestOutcome::InvalidRequest);
    QCOMPARE(call.status.diagnostic, diagnostic);
    QCOMPARE(m_server->requestCount(), 0);
  }

  void oversizedBodyIsTransportFailure() {
    MockHttpServer::Response response = jsonResponse(200, "[]"_ba);
    response.contentLengthOverride = GameClient::kMaxResponseBytes + 1;
    response.hangAfterHeaders = true;
    m_server->setResponse(gamePath(), response);
    const GameCall call = fetchGame();
    QCOMPARE(call.status.outcome, GameRequestOutcome::Transport);
    QCOMPARE(call.status.diagnostic, u"response body exceeds the size limit"_s);
  }

  void listGamesDecodesFixtureAndSendsToken() {
    QCOMPARE(listPath(), u"/api/v1/arkham/games"_s);
    m_server->setResponse(listPath(),
                          jsonResponse(200, fixture(u"game-list.json"_s)));
    const ListCall call = fetchList();
    QCOMPARE(call.calls, 1);
    QCOMPARE(call.status.outcome, GameRequestOutcome::Success);
    QVERIFY(call.rows.has_value());
    QCOMPARE(call.rows->size(), 5);
    QCOMPARE(call.rows->first().kind(), GameListRow::Kind::Success);
    QCOMPARE(call.rows->first().id()->value(), kGameId);
    QCOMPARE(call.rows->first().name(), u"Contract fixture game"_s);
    QCOMPARE(call.rows->last().kind(), GameListRow::Kind::Failure);

    const QByteArray request = m_server->lastRequest();
    QVERIFY2(request.startsWith("GET /api/v1/arkham/games HTTP/1.1\r\n"),
             request.constData());
    QVERIFY(request.contains("\r\nAuthorization: Token test-token-123\r\n"));
  }

  void listGamesDecodeFailureAndUnauthorized() {
    m_server->setResponse(listPath(), jsonResponse(200, "{}"_ba));
    const ListCall bad = fetchList();
    QCOMPARE(bad.status.outcome, GameRequestOutcome::DecodeFailed);
    QVERIFY(!bad.status.diagnostic.isEmpty());
    QVERIFY(!bad.rows.has_value());

    m_server->setResponse(listPath(), jsonResponse(401, "{}"_ba));
    const ListCall unauthorized = fetchList();
    QCOMPARE(unauthorized.status.outcome, GameRequestOutcome::Unauthorized);
    QCOMPARE(unauthorized.status.httpStatus, 401);
  }
};

QTEST_GUILESS_MAIN(GameClientTests)

#include "GameClientTests.moc"
