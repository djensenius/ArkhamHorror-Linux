#include "ControllerGlyphs.h"

namespace Arkham {

ControllerFamily ControllerGlyphs::detectFamily(const QString &deviceNameHint) {
  // Fixed, deterministic priority order: Steam Deck is checked first
  // because its own built-in controller and Steam Input's virtual
  // device names never contain "xbox" or "playstation"/"sony"/
  // "dualshock"/"dualsense", so there is no ordering ambiguity between
  // these branches -- but the order is still pinned explicitly (rather
  // than relying on that non-overlap always holding) so future
  // additional substrings can never silently reorder classification.
  if (deviceNameHint.contains(QStringLiteral("Steam Deck"),
                              Qt::CaseInsensitive) ||
      deviceNameHint.contains(QStringLiteral("Steam Controller"),
                              Qt::CaseInsensitive)) {
    return ControllerFamily::SteamDeck;
  }
  if (deviceNameHint.contains(QStringLiteral("PlayStation"),
                              Qt::CaseInsensitive) ||
      deviceNameHint.contains(QStringLiteral("DualShock"),
                              Qt::CaseInsensitive) ||
      deviceNameHint.contains(QStringLiteral("DualSense"),
                              Qt::CaseInsensitive) ||
      deviceNameHint.contains(QStringLiteral("Sony"), Qt::CaseInsensitive)) {
    return ControllerFamily::PlayStation;
  }
  if (deviceNameHint.contains(QStringLiteral("Xbox"), Qt::CaseInsensitive)) {
    return ControllerFamily::Xbox;
  }
  return ControllerFamily::Generic;
}

QString ControllerGlyphs::glyphLabel(const ControllerFamily family,
                                     const GamepadButton button) {
  switch (family) {
  case ControllerFamily::Xbox:
    switch (button) {
    case GamepadButton::South:
      return QStringLiteral("A");
    case GamepadButton::East:
      return QStringLiteral("B");
    case GamepadButton::West:
      return QStringLiteral("X");
    case GamepadButton::North:
      return QStringLiteral("Y");
    case GamepadButton::Start:
      return QStringLiteral("Menu");
    case GamepadButton::Select:
      return QStringLiteral("View");
    case GamepadButton::ShoulderLeft:
      return QStringLiteral("LB");
    case GamepadButton::ShoulderRight:
      return QStringLiteral("RB");
    case GamepadButton::DpadUp:
      return QStringLiteral("D-Pad Up");
    case GamepadButton::DpadDown:
      return QStringLiteral("D-Pad Down");
    case GamepadButton::DpadLeft:
      return QStringLiteral("D-Pad Left");
    case GamepadButton::DpadRight:
      return QStringLiteral("D-Pad Right");
    }
    break;
  case ControllerFamily::PlayStation:
    switch (button) {
    case GamepadButton::South:
      return QStringLiteral("Cross");
    case GamepadButton::East:
      return QStringLiteral("Circle");
    case GamepadButton::West:
      return QStringLiteral("Square");
    case GamepadButton::North:
      return QStringLiteral("Triangle");
    case GamepadButton::Start:
      return QStringLiteral("Options");
    case GamepadButton::Select:
      return QStringLiteral("Create");
    case GamepadButton::ShoulderLeft:
      return QStringLiteral("L1");
    case GamepadButton::ShoulderRight:
      return QStringLiteral("R1");
    case GamepadButton::DpadUp:
      return QStringLiteral("D-Pad Up");
    case GamepadButton::DpadDown:
      return QStringLiteral("D-Pad Down");
    case GamepadButton::DpadLeft:
      return QStringLiteral("D-Pad Left");
    case GamepadButton::DpadRight:
      return QStringLiteral("D-Pad Right");
    }
    break;
  case ControllerFamily::SteamDeck:
    switch (button) {
    case GamepadButton::South:
      return QStringLiteral("A");
    case GamepadButton::East:
      return QStringLiteral("B");
    case GamepadButton::West:
      return QStringLiteral("X");
    case GamepadButton::North:
      return QStringLiteral("Y");
    case GamepadButton::Start:
      return QStringLiteral("Menu");
    case GamepadButton::Select:
      return QStringLiteral("View");
    case GamepadButton::ShoulderLeft:
      return QStringLiteral("L1");
    case GamepadButton::ShoulderRight:
      return QStringLiteral("R1");
    case GamepadButton::DpadUp:
      return QStringLiteral("D-Pad Up");
    case GamepadButton::DpadDown:
      return QStringLiteral("D-Pad Down");
    case GamepadButton::DpadLeft:
      return QStringLiteral("D-Pad Left");
    case GamepadButton::DpadRight:
      return QStringLiteral("D-Pad Right");
    }
    break;
  case ControllerFamily::Generic:
    switch (button) {
    case GamepadButton::South:
      return QStringLiteral("South");
    case GamepadButton::East:
      return QStringLiteral("East");
    case GamepadButton::West:
      return QStringLiteral("West");
    case GamepadButton::North:
      return QStringLiteral("North");
    case GamepadButton::Start:
      return QStringLiteral("Start");
    case GamepadButton::Select:
      return QStringLiteral("Select");
    case GamepadButton::ShoulderLeft:
      return QStringLiteral("L1");
    case GamepadButton::ShoulderRight:
      return QStringLiteral("R1");
    case GamepadButton::DpadUp:
      return QStringLiteral("D-Pad Up");
    case GamepadButton::DpadDown:
      return QStringLiteral("D-Pad Down");
    case GamepadButton::DpadLeft:
      return QStringLiteral("D-Pad Left");
    case GamepadButton::DpadRight:
      return QStringLiteral("D-Pad Right");
    }
    break;
  }

  Q_UNREACHABLE_RETURN({});
}

} // namespace Arkham
