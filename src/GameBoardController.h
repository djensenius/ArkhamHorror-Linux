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
// token or HTTP reply that lands after reset() (which the composition calls
// whenever the session leaves SignedIn) is dropped rather than shown.
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

signals:
  void changed();

private:
  struct Game {
    QString id;
    QString title;
  };

  void fetchBoard();
  void failList(const QString &text);
  void failBoard(const QString &text);
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
};

} // namespace Arkham
