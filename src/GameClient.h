#pragma once

#include "ServerMaintenance.h"
#include "ServerProfile.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <chrono>
#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Arkham {

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
// their board: GET /arkham/games (the caller's games) and
// GET /arkham/games/{id} (one game). Each call sends
// "Authorization: Token <token>" for exactly that request; the token is not
// kept after the request is issued. Responses are decoded only through the
// existing raw-bytes decoders (decodeGameListFromRawBytes and
// GetGame::fromRawBytes).
//
// Every callback runs exactly once, asynchronously, unless this client is
// destroyed first (pending requests are then aborted and their callbacks
// never run). The decoded pointer is non-null only for Success and is
// valid only for the duration of the callback.
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

  explicit GameClient(QNetworkAccessManager &nam,
                      std::chrono::milliseconds timeout = kDefaultTimeout,
                      QObject *parent = nullptr);
  ~GameClient() override;

  // Test-only: lowers the response-body cap (default kMaxResponseBytes) so
  // tests can exercise the size limit cheaply. Each request snapshots the
  // cap when it is issued, so this applies only to requests issued
  // afterwards; production code never calls it.
  void setMaxResponseBytes(qsizetype maxResponseBytes);

  void listGames(const ServerProfile &profile, const QString &token,
                 GameListCallback callback);
  void getGame(const ServerProfile &profile, const QString &gameId,
               const QString &token, GameCallback callback);

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
             const QString &token, QString notFoundDiagnostic,
             Completion completion);
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
};

} // namespace Arkham
