// GameBoardController over a real GameClient and a loopback MockHttpServer,
// with a stand-in for SessionCoordinator::requestAccessToken: list then
// board, every error text, retry, and dropping replies that land after
// reset() (what the app does when the session leaves SignedIn), and the
// card catalog behind the board's card names.

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QtTest>

#include <functional>
#include <optional>
#include <utility>

#include "BoardCardsModel.h"
#include "GameBoardController.h"
#include "GameClient.h"
#include "MockHttpServer.h"
#include "ServerProfile.h"

using namespace Arkham;
using namespace Qt::StringLiterals;
using Status = GameBoardController::Status;

namespace {

const QString kGameId = u"00000000-0000-0000-0000-000000000003"_s;
const QString kSecondGameId = u"00000000-0000-0000-0000-000000000004"_s;

QByteArray fixture(const QString &name) {
  QFile f(QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + u"/fixtures/"_s + name);
  if (!f.open(QIODevice::ReadOnly))
    qFatal("missing fixture: %s", qPrintable(name));
  return f.readAll();
}

MockHttpServer::Response jsonResponse(int status, QByteArray body) {
  MockHttpServer::Response response;
  response.status = status;
  response.contentType = "application/json"_ba;
  response.body = std::move(body);
  return response;
}

// A GET /arkham/cards?cardPool=campaign body naming get-game.json's
// agenda, act and location: catalog.json's one card with its code, type
// and names replaced.
QByteArray catalogBody() {
  const QJsonObject card = QJsonDocument::fromJson(fixture(u"catalog.json"_s))
                               .object()
                               .value("cards"_L1)
                               .toArray()
                               .first()
                               .toObject();
  if (card.isEmpty())
    qFatal("catalog.json has no cards");
  const auto entry = [&card](const QString &code, const QString &type,
                             const QString &title, const QString &revealed) {
    QJsonObject edited = card;
    edited.insert("cardCode"_L1, code);
    edited.insert("cardType"_L1, type);
    edited.insert("name"_L1, QJsonObject{{"title"_L1, title},
                                         {"subtitle"_L1, QJsonValue()}});
    if (!revealed.isEmpty())
      edited.insert(
          "revealedName"_L1,
          QJsonObject{{"title"_L1, revealed}, {"subtitle"_L1, QJsonValue()}});
    return edited;
  };
  return QJsonDocument(
             QJsonArray{
                 entry(u"c01105"_s, u"AgendaType"_s, u"What's Going On?!"_s,
                       QString()),
                 entry(u"c01108"_s, u"ActType"_s, u"Trapped"_s, QString()),
                 entry(u"c01111"_s, u"LocationType"_s, u"Study (unrevealed)"_s,
                       u"Study"_s),
             })
      .toJson(QJsonDocument::Compact);
}

QStringList labelsOf(const QAbstractItemModel *model) {
  QStringList labels;
  for (int row = 0; row < model->rowCount(); ++row)
    labels.append(model->data(model->index(row, 0), BoardCardsModel::LabelRole)
                      .toString());
  return labels;
}

const QStringList kCodeLabels{u"c01105"_s, u"c01108"_s, u"c01111"_s,
                              u"Roland Banks"_s};
const QStringList kNameLabels{u"What's Going On?!"_s, u"Trapped"_s, u"Study"_s,
                              u"Roland Banks"_s};

// Counts the controller's "catalog unavailable" warnings (or those with
// another |prefix|) while alive, and passes every message on to QtTest's
// own handler (so QTest::ignoreMessage() still applies). Lets a test wait
// for a failed catalog reply to be handled instead of guessing how long it
// takes.
class CatalogWarningCounter {
public:
  explicit CatalogWarningCounter(
      QLatin1StringView prefix = "Card catalog unavailable"_L1) {
    s_count = 0;
    s_prefix = prefix;
    s_previous = qInstallMessageHandler(&handle);
  }
  ~CatalogWarningCounter() { qInstallMessageHandler(s_previous); }
  CatalogWarningCounter(const CatalogWarningCounter &) = delete;
  CatalogWarningCounter &operator=(const CatalogWarningCounter &) = delete;
  [[nodiscard]] int count() const { return s_count; }

private:
  static void handle(QtMsgType type, const QMessageLogContext &context,
                     const QString &message) {
    if (type == QtWarningMsg && message.startsWith(s_prefix))
      ++s_count;
    if (s_previous)
      s_previous(type, context, message);
  }
  static inline int s_count = 0;
  static inline QLatin1StringView s_prefix;
  static inline QtMessageHandler s_previous = nullptr;
};

// The controller's one warning per catalog reply with skipped or degraded
// entries.
const QLatin1StringView kCatalogEntryPrefix = "Card catalog:"_L1;

const char kCatalog500Warning[] =
    "Card catalog unavailable; board cards show card codes: The server "
    "returned HTTP 500.";

MockHttpServer::Response held(MockHttpServer::Response response) {
  response.hold = true;
  return response;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 5000) {
  const QDeadlineTimer deadline(timeoutMs);
  while (!condition() && !deadline.hasExpired())
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return condition();
}

} // namespace

class GameBoardControllerTests : public QObject {
  Q_OBJECT

private:
  MockHttpServer *m_server{nullptr};
  QNetworkAccessManager *m_nam{nullptr};
  GameClient *m_client{nullptr};
  std::optional<ServerProfile> m_profile;
  // When set, the token source holds the callback here instead of
  // answering (to deliver it later, after a reset()).
  bool m_holdToken{false};
  bool m_signedIn{true};
  int m_tokenRequests{0};
  QString m_tokenDiagnostic{u"Not signed in."_s};
  SessionCoordinator::AccessTokenCallback m_heldToken;

  QString listPath() const { return m_profile->apiUrl(u"arkham/games").path(); }
  QString gamePath() const {
    return m_profile->apiUrl(u"arkham/games/"_s + kGameId).path();
  }
  // The request target, query included, as MockHttpServer keys it.
  QString cardsTarget() const {
    return m_profile->apiUrl(u"arkham/cards").path() + u"?cardPool=campaign"_s;
  }
  int catalogRequests() const {
    return m_server->requestCountForPath(cardsTarget());
  }

  GameBoardController::AccessTokenSource tokenSource() {
    return [this](SessionCoordinator::AccessTokenCallback callback) {
      ++m_tokenRequests;
      if (m_holdToken) {
        m_heldToken = std::move(callback);
        return;
      }
      const ServerProfile profile = *m_profile;
      const bool signedIn = m_signedIn;
      const QString diagnostic = m_tokenDiagnostic;
      QMetaObject::invokeMethod(
          this,
          [callback = std::move(callback), profile, signedIn, diagnostic] {
            if (signedIn)
              callback(SessionCoordinator::AccessToken{profile, u"tok-1"_s},
                       QString());
            else
              callback(std::nullopt, diagnostic);
          },
          Qt::QueuedConnection);
    };
  }

  // Loads the fixture list into |c| and returns whether it loaded.
  bool loadList(GameBoardController &c) {
    m_server->setResponse(listPath(),
                          jsonResponse(200, fixture(u"game-list.json"_s)));
    c.loadGames();
    return waitUntil([&c] { return c.listStatus() == Status::Loaded; });
  }

private slots:
  void init() {
    m_server = new MockHttpServer(this);
    QVERIFY(m_server->start());
    auto profile = ServerProfile::custom(
        u"Mock"_s, u"http://127.0.0.1:%1"_s.arg(m_server->serverPort()));
    QVERIFY(profile.has_value());
    m_profile = *profile;
    // An empty catalog unless a test sets one: no names, nothing logged.
    m_server->setResponse(cardsTarget(), jsonResponse(200, "[]"_ba));
    m_nam = new QNetworkAccessManager(this);
    m_client = new GameClient(*m_nam, std::chrono::seconds(5), this);
    m_holdToken = false;
    m_signedIn = true;
    m_tokenRequests = 0;
    m_tokenDiagnostic = u"Not signed in."_s;
    m_heldToken = nullptr;
  }

  void cleanup() {
    delete m_client;
    delete m_nam;
    delete m_server;
    m_profile.reset();
  }

  void listThenBoardLoads() {
    GameBoardController c(*m_client, tokenSource());
    QCOMPARE(c.listStatusName(), u"idle"_s);
    m_server->setResponse(listPath(),
                          jsonResponse(200, fixture(u"game-list.json"_s)));
    c.loadGames();
    QCOMPARE(c.listStatusName(), u"loading"_s);
    QVERIFY(waitUntil([&c] { return c.listStatus() == Status::Loaded; }));
    // The broken (failure) row has no board, so it is not listed.
    QCOMPARE(c.gameTitles(), QStringList({u"Contract fixture game"_s,
                                          u"Campaign contract fixture"_s,
                                          u"Active contract fixture"_s,
                                          u"Completed contract fixture"_s}));
    QVERIFY(
        m_server->lastRequest().contains("\r\nAuthorization: Token tok-1\r\n"));

    m_server->setResponse(gamePath(),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    c.openGame(0);
    QCOMPARE(c.boardStatusName(), u"loading"_s);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QCOMPARE(c.selectedTitle(), u"Contract fixture game"_s);
    QVERIFY(c.boardCards()->rowCount() > 0);
    QVERIFY(c.errorText().isEmpty());
    QCOMPARE(m_server->requestCountForPath(gamePath()), 1);
    QCOMPARE(m_tokenRequests, 2); // one token read per request

    c.closeGame();
    QCOMPARE(c.boardStatus(), Status::Idle);
    QCOMPARE(c.boardCards()->rowCount(), 0);
    QCOMPARE(c.listStatus(), Status::Loaded);
  }

  void boardErrors_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<QString>("errorText");
    QByteArray party = fixture(u"get-game.json"_s);
    party.replace(R"("multiplayerMode": "Solo")"_ba,
                  R"("multiplayerMode": "Party")"_ba);
    QTest::newRow("decode failure")
        << 200 << party
        << u"Could not read the server's reply (getGame.multiplayerMode: "
           "unrecognized value \"Party\")."_s;
    QTest::newRow("401") << 401 << "{}"_ba
                         << u"The server no longer accepts your sign-in. "
                            "Sign in again."_s;
    QTest::newRow("404") << 404 << "{}"_ba << u"Game not found."_s;
    QTest::newRow("500") << 500 << "{}"_ba
                         << u"The server returned HTTP 500."_s;
    QTest::newRow("maintenance")
        << 503
        << R"({"maintenance":true,"message":"Back soon.","since":"2026-11-03T22:15:00Z"})"_ba
        << u"Back soon. Try again later."_s;
  }

  void boardErrors() {
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    QFETCH(QString, errorText);
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    m_server->setResponse(gamePath(), jsonResponse(status, body));
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Error; }));
    QCOMPARE(c.errorText(), errorText);
    QCOMPARE(c.boardCards()->rowCount(), 0);
    // The list stays loaded underneath.
    QCOMPARE(c.listStatus(), Status::Loaded);
  }

  void listErrors_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("errorText");
    QTest::newRow("401") << 401
                         << u"The server no longer accepts your sign-in. "
                            "Sign in again."_s;
    QTest::newRow("404") << 404 << u"Games list not found."_s;
  }

  void listErrors() {
    QFETCH(int, status);
    QFETCH(QString, errorText);
    GameBoardController c(*m_client, tokenSource());
    m_server->setResponse(listPath(), jsonResponse(status, "{}"_ba));
    c.loadGames();
    QVERIFY(waitUntil([&c] { return c.listStatus() == Status::Error; }));
    QCOMPARE(c.errorText(), errorText);
    QVERIFY(c.gameTitles().isEmpty());
  }

  void signedOutTokenShowsSignIn() {
    m_signedIn = false;
    GameBoardController c(*m_client, tokenSource());
    c.loadGames();
    QVERIFY(waitUntil([&c] { return c.listStatus() == Status::Error; }));
    QCOMPARE(c.errorText(), u"Sign in to see your games."_s);
    QCOMPARE(m_server->requestCount(), 0);
  }

  void retryRepeatsTheFailedFetch() {
    GameBoardController c(*m_client, tokenSource());
    m_server->setResponse(listPath(), jsonResponse(500, "{}"_ba));
    c.loadGames();
    QVERIFY(waitUntil([&c] { return c.listStatus() == Status::Error; }));
    m_server->setResponse(listPath(),
                          jsonResponse(200, fixture(u"game-list.json"_s)));
    c.retry();
    QVERIFY(waitUntil([&c] { return c.listStatus() == Status::Loaded; }));
    QVERIFY(c.errorText().isEmpty());

    m_server->setResponse(gamePath(), jsonResponse(500, "{}"_ba));
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Error; }));
    m_server->setResponse(gamePath(),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    c.retry();
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QCOMPARE(m_server->requestCountForPath(gamePath()), 2);
    QCOMPARE(m_server->requestCountForPath(listPath()), 2);
  }

  void lateRepliesAfterSignOutAreDropped() {
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    MockHttpServer::Response slow =
        jsonResponse(200, fixture(u"get-game.json"_s));
    slow.headerDelayMs = 150;
    m_server->setResponse(gamePath(), slow);
    c.openGame(0);
    QVERIFY(waitUntil(
        [this] { return m_server->requestCountForPath(gamePath()) == 1; }));
    // The session leaves SignedIn while the board reply is in flight.
    c.reset();
    QCOMPARE(c.listStatus(), Status::Idle);
    QCOMPARE(c.boardStatus(), Status::Idle);
    QTest::qWait(400);
    QCOMPARE(c.boardStatus(), Status::Idle);
    QCOMPARE(c.boardCards()->rowCount(), 0);
    QVERIFY(c.gameTitles().isEmpty());
    QVERIFY(c.errorText().isEmpty());

    // Same for a list reply.
    MockHttpServer::Response slowList =
        jsonResponse(200, fixture(u"game-list.json"_s));
    slowList.headerDelayMs = 150;
    m_server->setResponse(listPath(), slowList);
    c.loadGames();
    QVERIFY(waitUntil(
        [this] { return m_server->requestCountForPath(listPath()) == 2; }));
    c.reset();
    QTest::qWait(400);
    QCOMPARE(c.listStatus(), Status::Idle);
    QVERIFY(c.gameTitles().isEmpty());
  }

  void secureStoreFailureWhileSignedInIsNotSignIn() {
    // The session is SignedIn but the token store could not be read.
    m_signedIn = false; // the stand-in answers with no token
    m_tokenDiagnostic = u"keyring locked"_s;
    GameBoardController c(*m_client, tokenSource());
    c.setSignedIn(true);
    c.loadGames();
    QVERIFY(waitUntil([&c] { return c.listStatus() == Status::Error; }));
    QCOMPARE(c.errorText(),
             u"Could not read your saved sign-in (keyring locked)."_s);
  }

  void oversizedReplyHasItsOwnText() {
    m_client->setMaxResponseBytes(16);
    GameBoardController c(*m_client, tokenSource());
    m_server->setResponse(listPath(),
                          jsonResponse(200, fixture(u"game-list.json"_s)));
    c.loadGames();
    QVERIFY(waitUntil([&c] { return c.listStatus() == Status::Error; }));
    QCOMPARE(c.errorText(), u"The server's response was too large."_s);
  }

  void closeGameDropsALateBoardReply() {
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    MockHttpServer::Response slow =
        jsonResponse(200, fixture(u"get-game.json"_s));
    slow.headerDelayMs = 150;
    m_server->setResponse(gamePath(), slow);
    c.openGame(0);
    QVERIFY(waitUntil(
        [this] { return m_server->requestCountForPath(gamePath()) == 1; }));
    c.closeGame();
    QTest::qWait(400);
    QCOMPARE(c.boardStatus(), Status::Idle);
    QCOMPARE(c.boardCards()->rowCount(), 0);
    QVERIFY(c.selectedTitle().isEmpty());
    QCOMPARE(c.listStatus(), Status::Loaded);
  }

  void openingAnotherGameReplacesTheFetchInFlight() {
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    // Game 0 answers late with a body that would fail to decode; only
    // game 1's board may show.
    QByteArray broken = fixture(u"get-game.json"_s);
    broken.replace(R"("multiplayerMode": "Solo")"_ba,
                   R"("multiplayerMode": "Party")"_ba);
    MockHttpServer::Response slow = jsonResponse(200, broken);
    slow.headerDelayMs = 200;
    m_server->setResponse(gamePath(), slow);
    const QString secondPath =
        m_profile->apiUrl(u"arkham/games/"_s + kSecondGameId).path();
    m_server->setResponse(secondPath,
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    c.openGame(0);
    QVERIFY(waitUntil(
        [this] { return m_server->requestCountForPath(gamePath()) == 1; }));
    c.openGame(1);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QTest::qWait(400);
    QCOMPARE(c.boardStatus(), Status::Loaded);
    QCOMPARE(c.selectedTitle(), u"Campaign contract fixture"_s);
    QVERIFY(c.errorText().isEmpty());
    QVERIFY(c.boardCards()->rowCount() > 0);
  }

  void retryAfterABoardErrorRefetchesOnlyTheBoard() {
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    m_server->setResponse(gamePath(), jsonResponse(500, "{}"_ba));
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Error; }));
    m_server->setResponse(gamePath(),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    c.retry();
    QCOMPARE(c.listStatus(), Status::Loaded);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QCOMPARE(m_server->requestCountForPath(listPath()), 1);
    QCOMPARE(m_server->requestCountForPath(gamePath()), 2);
    QCOMPARE(c.gameTitles().size(), 4);
  }

  void setSignedInResetsAndAnnounces() {
    GameBoardController c(*m_client, tokenSource());
    QSignalSpy signedIn(&c, &GameBoardController::signedIn);
    c.setSignedIn(true);
    c.setSignedIn(true);
    QCOMPARE(signedIn.count(), 1);
    QVERIFY(loadList(c));
    c.setSignedIn(false);
    QCOMPARE(c.listStatus(), Status::Idle);
    QVERIFY(c.gameTitles().isEmpty());
    c.setSignedIn(true);
    QCOMPARE(signedIn.count(), 2);
  }

  // The first list fetch also fetches the catalog, with the same token;
  // the board is labelled with its names, and later fetches reuse it.
  void catalogIsFetchedOncePerSession() {
    m_server->setResponse(cardsTarget(), jsonResponse(200, catalogBody()));
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    QVERIFY(waitUntil([this] { return catalogRequests() == 1; }));
    m_server->setResponse(gamePath(),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QTRY_COMPARE(labelsOf(c.boardCards()), kNameLabels);
    QCOMPARE(m_tokenRequests, 2); // no extra token read for the catalog

    c.closeGame();
    QVERIFY(loadList(c));
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QCOMPARE(labelsOf(c.boardCards()), kNameLabels);
    QTest::qWait(100);
    QCOMPARE(catalogRequests(), 1);

    // Leaving the session drops the cache; the next one fetches again.
    c.reset();
    QVERIFY(loadList(c));
    QVERIFY(waitUntil([this] { return catalogRequests() == 2; }));
  }

  // A catalog that answers after the board relabels the open board in
  // place. The catalog reply is held until the board has loaded.
  void lateCatalogRelabelsTheBoard() {
    m_server->setResponse(cardsTarget(),
                          held(jsonResponse(200, catalogBody())));
    m_server->setResponse(gamePath(),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QVERIFY(waitUntil([this] { return m_server->heldCount() == 1; }));
    QCOMPARE(labelsOf(c.boardCards()), kCodeLabels);

    QSignalSpy changed(c.boardCards(), &QAbstractItemModel::dataChanged);
    QSignalSpy reset(c.boardCards(), &QAbstractItemModel::modelReset);
    m_server->releaseHeld();
    QTRY_COMPARE(labelsOf(c.boardCards()), kNameLabels);
    // In place: one dataChanged per renamed card, no reset.
    QCOMPARE(changed.count(), 3);
    QCOMPARE(reset.count(), 0);
  }

  // A failed catalog is logged, never shown: the board loads with card
  // codes, and a later list fetch tries the catalog again.
  void catalogFailureKeepsCodeLabels() {
    CatalogWarningCounter warnings;
    m_server->setResponse(cardsTarget(), held(jsonResponse(500, "{}"_ba)));
    m_server->setResponse(gamePath(),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QVERIFY(waitUntil([this] { return m_server->heldCount() == 1; }));

    QTest::ignoreMessage(QtWarningMsg, kCatalog500Warning);
    m_server->releaseHeld();
    QVERIFY(waitUntil([&warnings] { return warnings.count() == 1; }));
    QCOMPARE(labelsOf(c.boardCards()), kCodeLabels);
    QVERIFY(c.errorText().isEmpty());
    QCOMPARE(c.listStatus(), Status::Loaded);
    QCOMPARE(c.boardStatus(), Status::Loaded);

    m_server->setResponse(cardsTarget(), jsonResponse(200, catalogBody()));
    c.closeGame();
    QVERIFY(loadList(c));
    QVERIFY(waitUntil([this] { return catalogRequests() == 2; }));
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QTRY_COMPARE(labelsOf(c.boardCards()), kNameLabels);
  }

  // TASK-3.3.6: a catalog value this client has no enum case for (here a
  // new cardType) costs no card its name, and nothing is logged.
  void oneUnknownCatalogValueKeepsOtherNames() {
    // Both "Card catalog unavailable" and "Card catalog:" warnings.
    CatalogWarningCounter catalogWarnings("Card catalog"_L1);
    QByteArray body = catalogBody();
    QCOMPARE(body.count(R"("cardType":"AgendaType")"_ba), 1);
    body.replace(R"("cardType":"AgendaType")"_ba,
                 R"("cardType":"FutureType")"_ba);
    m_server->setResponse(cardsTarget(), jsonResponse(200, body));
    m_server->setResponse(gamePath(),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QTRY_COMPARE(labelsOf(c.boardCards()), kNameLabels);
    QCOMPARE(catalogWarnings.count(), 0);
  }

  // An entry without a readable name is skipped with one warning; that card
  // keeps its code and every other card is named.
  void unreadableCatalogEntryIsSkippedAndLogged() {
    CatalogWarningCounter skipped(kCatalogEntryPrefix);
    QByteArray body = catalogBody();
    const QByteArray trappedName = R"({"subtitle":null,"title":"Trapped"})"_ba;
    QCOMPARE(body.count(trappedName), 1);
    body.replace(trappedName, "null"_ba);
    m_server->setResponse(cardsTarget(), held(jsonResponse(200, body)));
    m_server->setResponse(gamePath(),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QVERIFY(waitUntil([this] { return m_server->heldCount() == 1; }));

    QTest::ignoreMessage(
        QtWarningMsg,
        "Card catalog: skipped 1 unreadable entry; those cards show card "
        "codes (first: cards[1].name: expected object, got null)");
    m_server->releaseHeld();
    QVERIFY(waitUntil([&skipped] { return skipped.count() == 1; }));
    QCOMPARE(labelsOf(c.boardCards()),
             QStringList({u"What's Going On?!"_s, u"c01108"_s, u"Study"_s,
                          u"Roland Banks"_s}));
    QTest::qWait(100);
    QCOMPARE(skipped.count(), 1);
    QVERIFY(c.errorText().isEmpty());
  }

  // A malformed revealedName keeps the card's name (so the revealed
  // location shows its unrevealed name) and is logged once.
  void malformedRevealedNameIsLogged() {
    CatalogWarningCounter degraded(kCatalogEntryPrefix);
    QByteArray body = catalogBody();
    const QByteArray studyName = R"({"subtitle":null,"title":"Study"})"_ba;
    QCOMPARE(body.count(studyName), 1);
    body.replace(studyName, R"("Study")"_ba);
    m_server->setResponse(cardsTarget(), held(jsonResponse(200, body)));
    m_server->setResponse(gamePath(),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QVERIFY(waitUntil([this] { return m_server->heldCount() == 1; }));

    QTest::ignoreMessage(
        QtWarningMsg,
        "Card catalog: ignored the malformed revealedName of 1 entry (first: "
        "cards[2].revealedName: expected object, got string)");
    m_server->releaseHeld();
    QVERIFY(waitUntil([&degraded] { return degraded.count() == 1; }));
    QCOMPARE(labelsOf(c.boardCards()),
             QStringList({u"What's Going On?!"_s, u"Trapped"_s,
                          u"Study (unrevealed)"_s, u"Roland Banks"_s}));
    QTest::qWait(100);
    QCOMPARE(degraded.count(), 1);
  }

  // A reply with both a skipped and a degraded entry still logs exactly one
  // warning, naming both.
  void skippedAndDegradedEntriesShareOneWarning() {
    CatalogWarningCounter warnings(kCatalogEntryPrefix);
    QByteArray body = catalogBody();
    const QByteArray trappedName = R"({"subtitle":null,"title":"Trapped"})"_ba;
    const QByteArray studyName = R"({"subtitle":null,"title":"Study"})"_ba;
    QCOMPARE(body.count(trappedName), 1);
    QCOMPARE(body.count(studyName), 1);
    body.replace(trappedName, "null"_ba);
    body.replace(studyName, R"("Study")"_ba);
    m_server->setResponse(cardsTarget(), held(jsonResponse(200, body)));
    m_server->setResponse(gamePath(),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QVERIFY(waitUntil([this] { return m_server->heldCount() == 1; }));

    QTest::ignoreMessage(
        QtWarningMsg,
        "Card catalog: skipped 1 unreadable entry; those cards show card "
        "codes (first: cards[1].name: expected object, got null); ignored "
        "the malformed revealedName of 1 entry (first: cards[2].revealedName: "
        "expected object, got string)");
    m_server->releaseHeld();
    QVERIFY(waitUntil([&warnings] { return warnings.count() == 1; }));
    QCOMPARE(labelsOf(c.boardCards()),
             QStringList({u"What's Going On?!"_s, u"c01108"_s,
                          u"Study (unrevealed)"_s, u"Roland Banks"_s}));
    QTest::qWait(100);
    QCOMPARE(warnings.count(), 1);
  }

  // A catalog reply that lands after reset() names nothing. The first
  // reply is held until after reset(); the next session's catalog fails,
  // so any name on the board could only have come from the dropped reply.
  void lateCatalogAfterResetIsDropped() {
    CatalogWarningCounter warnings;
    m_server->setResponse(cardsTarget(),
                          held(jsonResponse(200, catalogBody())));
    m_server->setResponse(gamePath(),
                          jsonResponse(200, fixture(u"get-game.json"_s)));
    GameBoardController c(*m_client, tokenSource());
    QVERIFY(loadList(c));
    QVERIFY(waitUntil([this] { return m_server->heldCount() == 1; }));
    c.reset();
    // Wait for the client to finish the stale reply before the next
    // session starts, so the drop path is exercised rather than raced.
    // The handler below (via the manager's finished signal) and
    // GameClient::handleFinished both run synchronously in the same
    // QNetworkReply::finished emission, ours first; so once the wait sees
    // it, the controller has already handled and dropped the stale reply.
    // The reply is read inside the emission because the client deletes it
    // afterwards. staleWatch is scoped so later replies are not counted.
    {
      int staleFinished = 0;
      QString stalePath;
      QNetworkReply::NetworkError staleError =
          QNetworkReply::UnknownNetworkError;
      QObject staleWatch;
      connect(m_nam, &QNetworkAccessManager::finished, &staleWatch,
              [&](QNetworkReply *reply) {
                ++staleFinished;
                stalePath = reply->url().path();
                staleError = reply->error();
              });
      m_server->releaseHeld();
      QVERIFY(waitUntil([&staleFinished] { return staleFinished == 1; }));
      QCoreApplication::processEvents();
      QCOMPARE(staleFinished, 1);
      QCOMPARE(stalePath, m_profile->apiUrl(u"arkham/cards").path());
      QCOMPARE(staleError, QNetworkReply::NoError);
    }

    m_server->setResponse(cardsTarget(), held(jsonResponse(500, "{}"_ba)));
    QVERIFY(loadList(c));
    QVERIFY(waitUntil([this] { return m_server->heldCount() == 1; }));
    QCOMPARE(catalogRequests(), 2);
    c.openGame(0);
    QVERIFY(waitUntil([&c] { return c.boardStatus() == Status::Loaded; }));
    QTest::ignoreMessage(QtWarningMsg, kCatalog500Warning);
    m_server->releaseHeld();
    QVERIFY(waitUntil([&warnings] { return warnings.count() == 1; }));
    QCOMPARE(labelsOf(c.boardCards()), kCodeLabels);
  }

  void tokenArrivingAfterResetSendsNothing() {
    GameBoardController c(*m_client, tokenSource());
    m_holdToken = true;
    c.loadGames();
    QVERIFY(m_heldToken);
    c.reset();
    m_heldToken(SessionCoordinator::AccessToken{*m_profile, u"tok-1"_s},
                QString());
    QTest::qWait(100);
    QCOMPARE(m_server->requestCount(), 0);
    QCOMPARE(c.listStatus(), Status::Idle);
  }
};

QTEST_GUILESS_MAIN(GameBoardControllerTests)

#include "GameBoardControllerTests.moc"
