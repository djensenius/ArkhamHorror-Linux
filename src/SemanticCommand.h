#pragma once

#include <QMetaType>
#include <QString>

namespace Arkham {

// The closed, strongly typed vocabulary of semantic commands the whole
// input stack (InputMapper/InputRouter, FocusController, and any QML
// adapter) ever dispatches. Every value names *what the product means*
// (directional focus, "primary action", "open the hand", "cycle to the
// next zone", ...), never a physical key, controller button, or Arkham
// game rule. QML/C++ adapters are only ever allowed to react to one of
// these values; they must never invent a new one ad hoc (e.g. by passing
// a raw string or key code around instead), which is what keeps the whole
// stack a closed, exhaustively testable enumeration instead of an
// open-ended pile of stringly-typed actions.
//
// There is deliberately no virtual-cursor/pointer-motion command here:
// every entry names a discrete action or a focus-graph move, never a
// continuous (x, y) position update. See FocusController for how
// directional-focus commands are turned into concrete focus changes over
// a stable graph of semantic zone/entity IDs, never a simulated pointer.
enum class SemanticCommand {
  // Directional focus: move the current focus one step across the focus
  // graph (see FocusController). Never moves a virtual cursor.
  FocusUp,
  FocusDown,
  FocusLeft,
  FocusRight,

  // Primary/secondary action on whatever currently holds focus. Escape
  // and the dedicated "Back" key are permanently reserved to
  // SecondaryAction (see InputMapper's reserved-key handling); Enter/
  // Return/Space and the equivalent generic-controller/Steam Input face
  // button are the default binding for PrimaryAction.
  PrimaryAction,
  SecondaryAction,

  // Inspect the currently focused entity in more detail (e.g. a card
  // close-up) without performing its primary action.
  Inspect,

  // Jump directly to one of the product's fixed top-level panels. The
  // dedicated "Menu" key is permanently reserved to OpenMenu.
  OpenHand,
  OpenPrompt,
  OpenInvestigator,
  OpenLog,
  OpenMenu,

  // Cycle which player or which board zone currently has semantic focus
  // priority, independent of moving focus within the current zone.
  CycleNextPlayer,
  CyclePreviousPlayer,
  CycleNextZone,
  CyclePreviousZone,

  // Jump focus directly to whatever the current game prompt is, skipping
  // any intermediate directional navigation.
  JumpToPrompt,

  // Camera framing controls. These never move game pieces or emulate a
  // pointer; they only ever adjust how the (read-only, in this slice)
  // camera views the board.
  CameraZoomIn,
  CameraZoomOut,
  CameraRotateLeft,
  CameraRotateRight,
  CameraReset,

  // Confirm or cancel an in-progress multiselect interaction.
  MultiSelectConfirm,
  MultiSelectCancel,

  // Undo the most recent reversible action.
  Undo,

  // Explicitly enter/exit a dedicated arrange mode (e.g. rearranging
  // cards within a player's own area) rather than always-on drag
  // gestures.
  ToggleArrangeMode,
};

// Returns a fixed, stable, kebab-case name for |command|, used for
// diagnostics/logging and for tests that pin the vocabulary's exact
// spelling. These names are part of the vocabulary's external contract:
// once shipped, changing one is a breaking rename, not a refactor.
QString commandName(SemanticCommand command);

} // namespace Arkham

Q_DECLARE_METATYPE(Arkham::SemanticCommand)
