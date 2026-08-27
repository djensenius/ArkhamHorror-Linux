#pragma once

#include <QMetaType>
#include <QString>

namespace Arkham {

enum class AppCommand {
  MoveUp,
  MoveDown,
  MoveLeft,
  MoveRight,
  Select,
  Back,
  Inspect,
  OpenActions,
  OpenHand,
  OpenLog,
  RestoreCamera,
};

QString commandName(AppCommand command);

} // namespace Arkham

Q_DECLARE_METATYPE(Arkham::AppCommand)
