#pragma once

#include "ServerMaintenance.h"
#include "ServerProfile.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <chrono>
#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QUrlQuery;

namespace Arkham {

struct CatalogNames;
class GameListRow;
class GetGame;

enum class GameRequestOutcome {
  Success,
  Unauthorized,   ///< HTTP 401: the token was rejected.
  NotFound,       ///< HTTP 404: no such game, or not a member of it.
  HttpError,      ///< Any other non-2xx status (redirects are never followed).
  Maintenance,    ///< The server's maintenance 503 (see ServerMaintenance).
  Transport,      ///< No HTTP response, a timeout, or an oversized body.
  DecodeFailed,   ///< 2xx whose body the raw-bytes decoder rejected.
  InvalidRequest, ///< Unusable profile, token, game ID or cleartext host;
                  ///< nothing was sent.
};

// Secret-free outcome of one game request. |diagnostic| never contains the
// token or the response body; for DecodeFailed it is the decoder's own
// field-path error, and for Maintenance it is the maintenance notice.
struct GameRequestStatus {
  GameRequestOutcome outcome{GameRequestOutcome::Transport};
  QString diagnostic;
  int httpStatus{0};
  std::optional<ServerMaintenance> maintenance;
};

// Authenticated REST client for the game endpoints a player needs to see
// their board: GET /arkham/games (the caller's games),
// GET /arkham/games/{id} (one game) and GET /arkham/cards?cardPool=campaign
// (the encounter-card catalog the board's card names come from). Each call
// sends
// "Authorization: Token <token>" for exactly that request; the token is not
// kept after the request is issued. Responses are decoded only through the
// existing raw-bytes decoders (decodeGameListFromRawBytes,
// GetGame::fromRawBytes and decodeCatalogNamesFromRawBytes).
//
// Every callback runs exactly once, asynchronously, unless this client is
// destroyed first (pending requests are then aborted and their callbacks
// never run). The decoded pointer is non-null only for Success and is
// valid only for the duration of the callback.
//
// The card catalog is large, so a successful listCampaignCards() body is
// decoded off the GUI thread, on this client's own one-thread pool (so
// decodes finish in the order their replies did). Its callback is still
// delivered on this client's thread, exactly once, unless this client is
// destroyed first. The destructor waits for a decode already running
// (dropping queued ones) so no worker touches a destroyed client.
//
// The QNetworkAccessManager must outlive this client: pending replies are
// owned by it, and the destructor aborts them through raw pointers.
class GameClient final : public QObject {
  Q_OBJECT

public:
  static constexpr std::chrono::seconds kDefaultTimeout{30};
  // A full PublicGame snapshot can be large; anything bigger than this is
  // treated as a transport failure rather than buffered without bound.
  static constexpr qsizetype kMaxResponseBytes = 32 * 1024 * 1024;

  using GameListCallback = std::function<void(const GameRequestStatus &,
                                              const QList<GameListRow> *)>;
  using GameCallback =
      std::function<void(const GameRequestStatus &, const GetGame *)>;
  using CatalogNamesCallback =
      std::function<void(const GameRequestStatus &, const CatalogNames *)>;

  explicit GameClient(QNetworkAccessManager &nam,
                      std::chrono::milliseconds timeout = kDefaultTimeout,
                      QObject *parent = nullptr);
  ~GameClient() override;

  // Test-only: lowers the response-body cap (default kMaxResponseBytes) so
  // tests can exercise the size limit cheaply. Each request snapshots the
  // cap when it is issued, so this applies only to requests issued
  // afterwards; production code never calls it.
  void setMaxResponseBytes(qsizetype maxResponseBytes);

  // Test-only: |observer| runs on the decode worker thread just before each
  // successful catalog body is decoded, so a test can see which thread
  // decodes and hold a decode in flight. Snapshotted per request like the
  // response cap. An observer that blocks must be released before this
  // client is destroyed: the destructor waits for a running decode.
  void setCatalogDecodeObserverForTesting(std::function<void()> observer);

  // The Transport diagnostic for a body over the size cap, so callers can
  // word it apart from a network failure.
  [[nodiscard]] static QString oversizedResponseDiagnostic();

  void listGames(const ServerProfile &profile, const QString &token,
                 GameListCallback callback);
  void getGame(const ServerProfile &profile, const QString &gameId,
               const QString &token, GameCallback callback);
  // The English names of the campaign card pool (agendas, acts, locations
  // and the rest of the encounter cards). Only names are decoded, entry by
  // entry: an entry without a readable code or name is skipped and counted
  // in the result rather than failing the whole pool. The endpoint is
  // public; the token is sent like every other request to this server.
  void listCampaignCards(const ServerProfile &profile, const QString &token,
                         CatalogNamesCallback callback);

private:
  using Completion =
      std::function<void(const GameRequestStatus &, const QByteArray &body)>;

  struct PendingReply {
    QTimer *timer{nullptr};
    QByteArray body;
    QString notFoundDiagnostic;
    Completion completion;
    // The cap in force when the request was issued.
    qsizetype maxResponseBytes{kMaxResponseBytes};
  };

  void issue(const ServerProfile &profile, const QString &path,
             const QUrlQuery &query, const QString &token,
             QString notFoundDiagnostic, Completion completion);
  void rejectQueued(Completion completion, QString diagnostic);
  void handleMetadataChanged(QNetworkReply *reply);
  bool readAvailable(QNetworkReply *reply);
  void handleFinished(QNetworkReply *reply);
  void complete(QNetworkReply *reply, GameRequestStatus status,
                bool abortReply);

  QNetworkAccessManager &m_nam;
  std::chrono::milliseconds m_timeout;
  qsizetype m_maxResponseBytes{kMaxResponseBytes};
  QHash<QNetworkReply *, PendingReply> m_pendingReplies;
  std::function<void()> m_catalogDecodeObserver;
  // One thread, so catalog decodes run (and deliver) in FIFO order.
  QThreadPool m_decodePool;
};

} // namespace Arkham
