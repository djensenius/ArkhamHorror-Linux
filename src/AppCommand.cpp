#include "AppCommand.h"

namespace Arkham {

QString commandName(const AppCommand command) {
  switch (command) {
  case AppCommand::MoveUp:
    return QStringLiteral("move-up");
  case AppCommand::MoveDown:
    return QStringLiteral("move-down");
  case AppCommand::MoveLeft:
    return QStringLiteral("move-left");
  case AppCommand::MoveRight:
    return QStringLiteral("move-right");
  case AppCommand::Select:
    return QStringLiteral("select");
  case AppCommand::Back:
    return QStringLiteral("back");
  case AppCommand::Inspect:
    return QStringLiteral("inspect");
  case AppCommand::OpenActions:
    return QStringLiteral("open-actions");
  case AppCommand::OpenHand:
    return QStringLiteral("open-hand");
  case AppCommand::OpenLog:
    return QStringLiteral("open-log");
  case AppCommand::RestoreCamera:
    return QStringLiteral("restore-camera");
  }

  Q_UNREACHABLE_RETURN({});
}

} // namespace Arkham
