#include "SemanticCommand.h"

namespace Arkham {

QString commandName(const SemanticCommand command) {
  switch (command) {
  case SemanticCommand::FocusUp:
    return QStringLiteral("focus-up");
  case SemanticCommand::FocusDown:
    return QStringLiteral("focus-down");
  case SemanticCommand::FocusLeft:
    return QStringLiteral("focus-left");
  case SemanticCommand::FocusRight:
    return QStringLiteral("focus-right");
  case SemanticCommand::PrimaryAction:
    return QStringLiteral("primary-action");
  case SemanticCommand::SecondaryAction:
    return QStringLiteral("secondary-action");
  case SemanticCommand::Inspect:
    return QStringLiteral("inspect");
  case SemanticCommand::OpenHand:
    return QStringLiteral("open-hand");
  case SemanticCommand::OpenPrompt:
    return QStringLiteral("open-prompt");
  case SemanticCommand::OpenInvestigator:
    return QStringLiteral("open-investigator");
  case SemanticCommand::OpenLog:
    return QStringLiteral("open-log");
  case SemanticCommand::OpenMenu:
    return QStringLiteral("open-menu");
  case SemanticCommand::CycleNextPlayer:
    return QStringLiteral("cycle-next-player");
  case SemanticCommand::CyclePreviousPlayer:
    return QStringLiteral("cycle-previous-player");
  case SemanticCommand::CycleNextZone:
    return QStringLiteral("cycle-next-zone");
  case SemanticCommand::CyclePreviousZone:
    return QStringLiteral("cycle-previous-zone");
  case SemanticCommand::JumpToPrompt:
    return QStringLiteral("jump-to-prompt");
  case SemanticCommand::CameraZoomIn:
    return QStringLiteral("camera-zoom-in");
  case SemanticCommand::CameraZoomOut:
    return QStringLiteral("camera-zoom-out");
  case SemanticCommand::CameraRotateLeft:
    return QStringLiteral("camera-rotate-left");
  case SemanticCommand::CameraRotateRight:
    return QStringLiteral("camera-rotate-right");
  case SemanticCommand::CameraReset:
    return QStringLiteral("camera-reset");
  case SemanticCommand::MultiSelectConfirm:
    return QStringLiteral("multiselect-confirm");
  case SemanticCommand::MultiSelectCancel:
    return QStringLiteral("multiselect-cancel");
  case SemanticCommand::Undo:
    return QStringLiteral("undo");
  case SemanticCommand::ToggleArrangeMode:
    return QStringLiteral("toggle-arrange-mode");
  }

  Q_UNREACHABLE_RETURN({});
}

} // namespace Arkham
