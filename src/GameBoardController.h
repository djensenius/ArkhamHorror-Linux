#pragma once

#include "GameClient.h"
#include "SessionCoordinator.h"

#include <QAbstractItemModel>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace Arkham {

class BoardCardsModel;

// Drives qml/BoardScreen.qml: the signed-in user's games list, then one
// selected game's board (a BoardCardsModel of its card art).
//
// Each fetch first asks |tokenSource| (SessionCoordinator::
// requestAccessToken in production) for the token, then issues one
// GameClient request with it; the token is never stored here. Every list
// or board fetch carries a generation number, and reset() bumps both, so a
// token or HTTP reply that lands after reset() (which setSignedIn(false)
// calls whenever the session leaves SignedIn; see bindToSession()) is
// dropped rather than shown.
//
// The first games-list fetch of a session also fetches the card catalog
// (GameClient::listCampaignCards) with the same token, for the board's
// card names. It is cached until reset() and fetched again only if it
// failed; a failure is logged and the board keeps its card-code labels.
class GameBoardController final : public QObject {
  Q_OBJECT
  // "idle", "loading", "loaded" or "error".
  Q_PROPERTY(QString listStatus READ listStatusName NOTIFY changed)
  Q_PROPERTY(QString boardStatus READ boardStatusName NOTIFY changed)
  // The user-facing reason for whichever stage is in "error".
  Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
  Q_PROPERTY(QStringList gameTitles READ gameTitles NOTIFY changed)
  Q_PROPERTY(QString selectedTitle READ selectedTitle NOTIFY changed)
  Q_PROPERTY(QAbstractItemModel *boardCards READ boardCards CONSTANT)

public:
  enum class Status { Idle, Loading, Loaded, Error };

  using AccessTokenSource =
      std::function<void(SessionCoordinator::AccessTokenCallback)>;

  GameBoardController(GameClient &client, AccessTokenSource tokenSource,
                      QObject *parent = nullptr);
  ~GameBoardController() override;

  [[nodiscard]] Status listStatus() const { return m_listStatus; }
  [[nodiscard]] Status boardStatus() const { return m_boardStatus; }
  [[nodiscard]] QString listStatusName() const;
  [[nodiscard]] QString boardStatusName() const;
  [[nodiscard]] QString errorText() const { return m_errorText; }
  [[nodiscard]] QStringList gameTitles() const;
  [[nodiscard]] QString selectedTitle() const;
  [[nodiscard]] QAbstractItemModel *boardCards() const;

  // Fetches the games list (dropping any board).
  Q_INVOKABLE void loadGames();
  // Fetches the board of gameTitles()[index].
  Q_INVOKABLE void openGame(int index);
  // Repeats whichever fetch failed.
  Q_INVOKABLE void retry();
  // Leaves the board for the list (a board fetch in flight is dropped).
  Q_INVOKABLE void closeGame();
  // Clears everything and drops every fetch in flight.
  Q_INVOKABLE void reset();

  // Tracks the session: false reset()s; a change to true emits
  // signedIn() so an open board screen can reload. While signed in, a
  // failed token read is a secure-store problem, not "sign in".
  void setSignedIn(bool signedIn);
  [[nodiscard]] bool isSignedIn() const { return m_signedIn; }

  // Drives setSignedIn() from |coordinator|'s state (now and on every
  // stateChanged) for as long as both exist.
  static void bindToSession(SessionCoordinator &coordinator,
                            GameBoardController &controller);

signals:
  void changed();
  void signedIn();

private:
  struct Game {
    QString id;
    QString title;
  };

  enum class CatalogStatus { None, Loading, Loaded };

  void fetchBoard();
  void fetchCatalog(const SessionCoordinator::AccessToken &token);
  void failList(const QString &text);
  void failBoard(const QString &text);
  [[nodiscard]] QString tokenFailureText(const QString &diagnostic) const;
  [[nodiscard]] static QString describe(const GameRequestStatus &status);

  GameClient &m_client;
  AccessTokenSource m_tokenSource;
  std::unique_ptr<BoardCardsModel> m_board;
  QList<Game> m_games;
  int m_selected{-1};
  Status m_listStatus{Status::Idle};
  Status m_boardStatus{Status::Idle};
  QString m_errorText;
  quint64 m_listGeneration{0};
  quint64 m_boardGeneration{0};
  // Whether the card catalog's names (kept by m_board, which holds them
  // across clear()) are loaded for this session; reset() drops them and
  // any catalog reply still in flight.
  CatalogStatus m_catalogStatus{CatalogStatus::None};
  quint64 m_catalogGeneration{0};
  bool m_signedIn{false};
};

} // namespace Arkham
