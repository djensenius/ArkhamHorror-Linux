#include "GameBoardController.h"

#include "BoardCardsModel.h"
#include "CardCatalog.h"
#include "Games.h"

#include <QDebug>
#include <QPointer>

#include <utility>

namespace Arkham {

namespace {

QString statusName(GameBoardController::Status status) {
  switch (status) {
  case GameBoardController::Status::Idle:
    return QStringLiteral("idle");
  case GameBoardController::Status::Loading:
    return QStringLiteral("loading");
  case GameBoardController::Status::Loaded:
    return QStringLiteral("loaded");
  case GameBoardController::Status::Error:
    return QStringLiteral("error");
  }
  return QStringLiteral("idle");
}

const QString kSignedOutText = QStringLiteral("Sign in to see your games.");

// "1 <adjective>entry" or "N <adjective>entries".
QString entryCount(qsizetype count, QStringView adjective = {}) {
  return count == 1 ? QStringLiteral("1 %1entry").arg(adjective)
                    : QStringLiteral("%1 %2entries").arg(count).arg(adjective);
}

// One warning for every catalog entry that lost its name or revealed name.
QString catalogEntryWarning(const CatalogNames &catalog) {
  QStringList parts;
  if (catalog.skippedCount > 0)
    parts.append(QStringLiteral("skipped %1; those cards show card codes "
                                "(first: %2)")
                     .arg(entryCount(catalog.skippedCount, u"unreadable "),
                          catalog.firstSkipDiagnostic));
  if (catalog.degradedCount > 0)
    parts.append(QStringLiteral("ignored the malformed revealedName of %1 "
                                "(first: %2)")
                     .arg(entryCount(catalog.degradedCount),
                          catalog.firstDegradeDiagnostic));
  return QStringLiteral("Card catalog: ") + parts.join(QStringLiteral("; "));
}

} // namespace

GameBoardController::GameBoardController(GameClient &client,
                                         AccessTokenSource tokenSource,
                                         QObject *parent)
    : QObject(parent), m_client(client), m_tokenSource(std::move(tokenSource)),
      m_board(std::make_unique<BoardCardsModel>()) {}

GameBoardController::~GameBoardController() = default;

QString GameBoardController::listStatusName() const {
  return statusName(m_listStatus);
}

QString GameBoardController::boardStatusName() const {
  return statusName(m_boardStatus);
}

QStringList GameBoardController::gameTitles() const {
  QStringList titles;
  titles.reserve(m_games.size());
  for (const Game &game : m_games)
    titles.append(game.title);
  return titles;
}

QString GameBoardController::selectedTitle() const {
  return m_selected >= 0 && m_selected < m_games.size()
             ? m_games.at(m_selected).title
             : QString();
}

QAbstractItemModel *GameBoardController::boardCards() const {
  return m_board.get();
}

QString GameBoardController::describe(const GameRequestStatus &status) {
  switch (status.outcome) {
  case GameRequestOutcome::Success:
    return QString();
  case GameRequestOutcome::Unauthorized:
    return QStringLiteral(
        "The server no longer accepts your sign-in. Sign in again.");
  case GameRequestOutcome::NotFound:
    // "game not found" / "games list not found" (see GameClient).
    return status.diagnostic.left(1).toUpper() + status.diagnostic.mid(1) +
           QLatin1Char('.');
  case GameRequestOutcome::Maintenance:
    // The server's own notice, with when to try again.
    return status.diagnostic;
  case GameRequestOutcome::HttpError:
    return QStringLiteral("The server returned HTTP %1.")
        .arg(status.httpStatus);
  case GameRequestOutcome::Transport:
    if (status.diagnostic == GameClient::oversizedResponseDiagnostic())
      return QStringLiteral("The server's response was too large.");
    return QStringLiteral("Could not reach the server (%1).")
        .arg(status.diagnostic);
  case GameRequestOutcome::DecodeFailed:
    return QStringLiteral("Could not read the server's reply (%1).")
        .arg(status.diagnostic);
  case GameRequestOutcome::InvalidRequest:
    return QStringLiteral("The request could not be sent (%1).")
        .arg(status.diagnostic);
  }
  return status.diagnostic;
}

void GameBoardController::loadGames() {
  const quint64 generation = ++m_listGeneration;
  ++m_boardGeneration;
  m_board->clear();
  m_games.clear();
  m_selected = -1;
  m_boardStatus = Status::Idle;
  m_listStatus = Status::Loading;
  m_errorText.clear();
  emit changed();

  QPointer<GameBoardController> self(this);
  m_tokenSource(
      [self, generation](std::optional<SessionCoordinator::AccessToken> token,
                         const QString &diagnostic) {
        if (!self || generation != self->m_listGeneration)
          return;
        if (!token) {
          self->failList(self->tokenFailureText(diagnostic));
          return;
        }
        if (self->m_catalogStatus == CatalogStatus::None)
          self->fetchCatalog(*token);
        self->m_client.listGames(
            token->profile, token->token,
            [self, generation](const GameRequestStatus &status,
                               const QList<GameListRow> *rows) {
              if (!self || generation != self->m_listGeneration)
                return;
              if (status.outcome != GameRequestOutcome::Success || !rows) {
                self->failList(describe(status));
                return;
              }
              // Only rows that decoded fully can be opened; a broken row has no
              // board to show.
              for (const GameListRow &row : *rows) {
                if (row.kind() == GameListRow::Kind::Success && row.id())
                  self->m_games.append({row.id()->value(), row.name()});
              }
              self->m_listStatus = Status::Loaded;
              emit self->changed();
            });
      });
}

void GameBoardController::fetchCatalog(
    const SessionCoordinator::AccessToken &token) {
  m_catalogStatus = CatalogStatus::Loading;
  const quint64 generation = m_catalogGeneration;
  QPointer<GameBoardController> self(this);
  m_client.listCampaignCards(
      token.profile, token.token,
      [self, generation](const GameRequestStatus &status,
                         const CatalogNames *catalog) {
        if (!self || generation != self->m_catalogGeneration)
          return;
        if (status.outcome != GameRequestOutcome::Success || !catalog) {
          // Not shown: the board still works, labelled with card codes.
          self->m_catalogStatus = CatalogStatus::None;
          qWarning().noquote()
              << "Card catalog unavailable; board cards show card codes:"
              << describe(status);
          return;
        }
        if (catalog->skippedCount > 0 || catalog->degradedCount > 0)
          qWarning().noquote() << catalogEntryWarning(*catalog);
        self->m_catalogStatus = CatalogStatus::Loaded;
        self->m_board->setCardNames(
            BoardCardsModel::cardNamesFromCatalog(*catalog));
      });
}

void GameBoardController::openGame(int index) {
  if (m_listStatus != Status::Loaded || index < 0 || index >= m_games.size())
    return;
  m_selected = index;
  fetchBoard();
}

void GameBoardController::fetchBoard() {
  const quint64 generation = ++m_boardGeneration;
  m_board->clear();
  m_boardStatus = Status::Loading;
  m_errorText.clear();
  emit changed();

  const QString gameId = m_games.at(m_selected).id;
  QPointer<GameBoardController> self(this);
  m_tokenSource([self, generation,
                 gameId](std::optional<SessionCoordinator::AccessToken> token,
                         const QString &diagnostic) {
    if (!self || generation != self->m_boardGeneration)
      return;
    if (!token) {
      self->failBoard(self->tokenFailureText(diagnostic));
      return;
    }
    self->m_client.getGame(
        token->profile, gameId, token->token,
        [self, generation](const GameRequestStatus &status,
                           const GetGame *game) {
          if (!self || generation != self->m_boardGeneration)
            return;
          if (status.outcome != GameRequestOutcome::Success || !game) {
            self->failBoard(describe(status));
            return;
          }
          self->m_board->setGame(game->game());
          self->m_boardStatus = Status::Loaded;
          emit self->changed();
        });
  });
}

void GameBoardController::retry() {
  if (m_boardStatus == Status::Error && m_selected >= 0)
    fetchBoard();
  else if (m_listStatus == Status::Error)
    loadGames();
}

void GameBoardController::closeGame() {
  if (m_selected < 0 && m_boardStatus == Status::Idle)
    return;
  ++m_boardGeneration;
  m_board->clear();
  m_selected = -1;
  m_boardStatus = Status::Idle;
  m_errorText.clear();
  emit changed();
}

void GameBoardController::reset() {
  ++m_listGeneration;
  ++m_boardGeneration;
  ++m_catalogGeneration;
  m_catalogStatus = CatalogStatus::None;
  m_board->clear();
  m_board->setCardNames({});
  m_games.clear();
  m_selected = -1;
  m_listStatus = Status::Idle;
  m_boardStatus = Status::Idle;
  m_errorText.clear();
  emit changed();
}

void GameBoardController::setSignedIn(bool signedIn) {
  if (!signedIn) {
    m_signedIn = false;
    reset();
    return;
  }
  if (m_signedIn)
    return;
  m_signedIn = true;
  emit this->signedIn();
}

void GameBoardController::bindToSession(SessionCoordinator &coordinator,
                                        GameBoardController &controller) {
  const auto sync = [&coordinator, &controller] {
    controller.setSignedIn(coordinator.state() ==
                           SessionCoordinator::State::SignedIn);
  };
  sync();
  // Context |controller|: the connection ends with either object.
  QObject::connect(&coordinator, &SessionCoordinator::stateChanged, &controller,
                   sync);
}

QString GameBoardController::tokenFailureText(const QString &diagnostic) const {
  // Signed out, or the session changed (which reset() already handles
  // while bound): ask to sign in. Still signed in: the secure store failed.
  if (!m_signedIn)
    return kSignedOutText;
  return QStringLiteral("Could not read your saved sign-in (%1).")
      .arg(diagnostic);
}

void GameBoardController::failList(const QString &text) {
  m_listStatus = Status::Error;
  m_errorText = text;
  emit changed();
}

void GameBoardController::failBoard(const QString &text) {
  m_boardStatus = Status::Error;
  m_errorText = text;
  emit changed();
}

} // namespace Arkham
