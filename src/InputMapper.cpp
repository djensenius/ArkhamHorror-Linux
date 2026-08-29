#include "InputMapper.h"

#include <array>

namespace Arkham {

namespace {

// The three permanently reserved physical keys, each with no modifiers.
// Kept as a small fixed list (rather than, say, a bit of key-range
// arithmetic) so the "is this reserved?" question is exactly as
// closed/enumerable as the reservation itself.
constexpr std::array<Qt::Key, 3> kReservedKeys{
    Qt::Key_Escape,
    Qt::Key_Back,
    Qt::Key_Menu,
};

} // namespace

InputMapper::InputMapper() { resetToDefaults(); }

bool InputMapper::isReservedKey(const PhysicalKey &physicalKey) {
  if (physicalKey.modifiers != Qt::NoModifier) {
    return false;
  }
  for (const Qt::Key reserved : kReservedKeys) {
    if (physicalKey.key == reserved) {
      return true;
    }
  }
  return false;
}

std::optional<SemanticCommand>
InputMapper::commandFor(const PhysicalKey &physicalKey) const {
  const auto it = bindings_.constFind(physicalKey);
  if (it == bindings_.constEnd()) {
    return std::nullopt;
  }
  return *it;
}

bool InputMapper::isReserved(const PhysicalKey &physicalKey) const {
  return isReservedKey(physicalKey);
}

std::optional<RemapError> InputMapper::remap(const PhysicalKey &physicalKey,
                                             const SemanticCommand command) {
  if (isReservedKey(physicalKey)) {
    return RemapError::ReservedKey;
  }

  const auto existingKeyIt = bindings_.constFind(physicalKey);
  if (existingKeyIt != bindings_.constEnd() && *existingKeyIt != command) {
    return RemapError::KeyAlreadyBound;
  }

  // A command may only ever be bound to one physical key at a time: drop
  // whatever key currently owns |command| (if any, and if it is not
  // |physicalKey| itself) before installing the new binding.
  for (auto it = bindings_.begin(); it != bindings_.end();) {
    if (it.value() == command && it.key() != physicalKey) {
      it = bindings_.erase(it);
    } else {
      ++it;
    }
  }

  bindings_.insert(physicalKey, command);
  return std::nullopt;
}

bool InputMapper::unbind(const PhysicalKey &physicalKey) {
  if (isReservedKey(physicalKey)) {
    return false;
  }
  return bindings_.remove(physicalKey) > 0;
}

void InputMapper::resetToDefaults() {
  bindings_.clear();
  heldKeys_.clear();

  const auto bind = [this](const Qt::Key key, const SemanticCommand command,
                           const Qt::KeyboardModifiers modifiers =
                               Qt::NoModifier) {
    bindings_.insert(PhysicalKey{key, modifiers}, command);
  };

  // Directional focus: arrow keys and WASD both move focus, matching
  // keyboard convention and the D-pad/left-stick keys a generic
  // controller/Steam Input template maps to arrow keys.
  bind(Qt::Key_Up, SemanticCommand::FocusUp);
  bind(Qt::Key_W, SemanticCommand::FocusUp);
  bind(Qt::Key_Down, SemanticCommand::FocusDown);
  bind(Qt::Key_S, SemanticCommand::FocusDown);
  bind(Qt::Key_Left, SemanticCommand::FocusLeft);
  bind(Qt::Key_A, SemanticCommand::FocusLeft);
  bind(Qt::Key_Right, SemanticCommand::FocusRight);
  bind(Qt::Key_D, SemanticCommand::FocusRight);

  // Primary action: Enter/Return, keypad Enter, and Space all agree with
  // the generic-controller "A"/south-face-button convention.
  bind(Qt::Key_Return, SemanticCommand::PrimaryAction);
  bind(Qt::Key_Enter, SemanticCommand::PrimaryAction);
  bind(Qt::Key_Space, SemanticCommand::PrimaryAction);

  // Escape and Back are reserved (see isReservedKey) and always resolve to
  // SecondaryAction; Menu is reserved and always resolves to OpenMenu.
  // These three insertions exist so commandFor()/processKey() have an
  // entry to find; remap()/unbind() reject any attempt to change them.
  bind(Qt::Key_Escape, SemanticCommand::SecondaryAction);
  bind(Qt::Key_Back, SemanticCommand::SecondaryAction);
  bind(Qt::Key_Menu, SemanticCommand::OpenMenu);

  bind(Qt::Key_X, SemanticCommand::Inspect);

  bind(Qt::Key_H, SemanticCommand::OpenHand);
  bind(Qt::Key_P, SemanticCommand::OpenPrompt);
  bind(Qt::Key_I, SemanticCommand::OpenInvestigator);
  bind(Qt::Key_L, SemanticCommand::OpenLog);

  // Player cycling: Q/E, adjacent to the WASD focus cluster.
  bind(Qt::Key_Q, SemanticCommand::CyclePreviousPlayer);
  bind(Qt::Key_E, SemanticCommand::CycleNextPlayer);

  // Zone cycling: Page Up/Page Down, matching the shoulder-button (L1/R1)
  // convention many generic-controller/Steam Input desktop templates use
  // for "switch tab/panel" -- this is exactly what drives shoulder-zone
  // switching in the QML fixture harness.
  bind(Qt::Key_PageUp, SemanticCommand::CyclePreviousZone);
  bind(Qt::Key_PageDown, SemanticCommand::CycleNextZone);

  bind(Qt::Key_J, SemanticCommand::JumpToPrompt);

  bind(Qt::Key_Equal, SemanticCommand::CameraZoomIn);
  bind(Qt::Key_Minus, SemanticCommand::CameraZoomOut);
  bind(Qt::Key_BracketLeft, SemanticCommand::CameraRotateLeft);
  bind(Qt::Key_BracketRight, SemanticCommand::CameraRotateRight);
  bind(Qt::Key_0, SemanticCommand::CameraReset);

  bind(Qt::Key_Y, SemanticCommand::MultiSelectConfirm);
  bind(Qt::Key_C, SemanticCommand::MultiSelectCancel);

  bind(Qt::Key_Z, SemanticCommand::Undo, Qt::ControlModifier);

  bind(Qt::Key_R, SemanticCommand::ToggleArrangeMode);
}

std::optional<DispatchedCommand>
InputMapper::processKey(const PhysicalKey &physicalKey, const bool isPress,
                        const bool isAutoRepeat) {
  const auto heldIt = heldKeys_.constFind(physicalKey);
  const bool currentlyHeld = heldIt != heldKeys_.constEnd();

  if (isPress) {
    if (isAutoRepeat) {
      // A repeat can never establish new held-state on its own, and can
      // never continue a hold that never actually had a dispatched
      // Pressed (e.g. the key was unbound for the whole press so far):
      // it must follow a real press that itself produced a Pressed.
      if (!currentlyHeld || heldIt.value() != HoldState::Armed) {
        return std::nullopt;
      }
    } else {
      if (currentlyHeld) {
        // Stray duplicate press without an intervening release (e.g.
        // across a focus-loss/regrab): suppressed, held state unchanged.
        return std::nullopt;
      }
      heldKeys_.insert(physicalKey, HoldState::Unarmed);
    }

    const auto command = commandFor(physicalKey);
    if (!command.has_value()) {
      return std::nullopt;
    }
    if (!isAutoRepeat) {
      // Only a real press arms the hold; this is what lets a later
      // repeat/release for this same hold actually dispatch.
      heldKeys_.insert(physicalKey, HoldState::Armed);
    }
    return DispatchedCommand{*command, isAutoRepeat ? CommandPhase::Repeated
                                                    : CommandPhase::Pressed};
  }

  // Release.
  if (!currentlyHeld) {
    // Stray release (never pressed, or already released): suppressed.
    return std::nullopt;
  }
  const bool wasArmed = heldIt.value() == HoldState::Armed;
  // Remove rather than store an unheld marker: a key that's not held has
  // no useful state to remember, so this keeps heldKeys_ bounded to only
  // the keys currently pressed instead of growing for every key ever
  // seen over the process lifetime.
  heldKeys_.remove(physicalKey);
  if (!wasArmed) {
    // This hold never had a dispatched Pressed (it was unbound for the
    // entire press), so it must not dispatch a Released either.
    return std::nullopt;
  }

  const auto command = commandFor(physicalKey);
  if (!command.has_value()) {
    return std::nullopt;
  }
  return DispatchedCommand{*command, CommandPhase::Released};
}

} // namespace Arkham
