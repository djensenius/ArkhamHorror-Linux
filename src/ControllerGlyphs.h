#pragma once

#include <QString>

namespace Arkham {

// Which controller glyph family to display. This is presentation-only
// state (which icon/label set to show for a physical button); it never
// changes what a button *does* (that is always exactly one
// SemanticCommand, chosen by InputMapper, identically regardless of
// glyph family).
enum class ControllerFamily {
  Generic,
  Xbox,
  PlayStation,
  SteamDeck,
};

// A closed set of physical controller buttons this product ever shows a
// glyph for. Deliberately independent of any specific Qt::Key: InputMapper
// owns the actual physical-key-to-SemanticCommand bindings (see
// InputMapper.h's comment on why generic controllers/Steam Input surface
// as ordinary QKeyEvents); this enum exists purely so the same closed set
// of buttons can be labeled consistently across controller families in
// UI/glyph contexts.
enum class GamepadButton {
  South, // Xbox A / PlayStation Cross / Steam Deck A
  East,  // Xbox B / PlayStation Circle / Steam Deck B
  West,  // Xbox X / PlayStation Square / Steam Deck X
  North, // Xbox Y / PlayStation Triangle / Steam Deck Y
  Start,
  Select,
  ShoulderLeft,
  ShoulderRight,
  DpadUp,
  DpadDown,
  DpadLeft,
  DpadRight,
};

// Pure, deterministic controller glyph/profile state: no hardware
// polling, no Steamworks/QtGamepad dependency, no I/O. Detection is a
// pure function of a caller-supplied device-name hint (e.g. from
// whatever future real-device-enumeration signal the app eventually
// wires in); glyph lookup is a pure, exhaustive, closed table.
class ControllerGlyphs {
public:
  // Classifies a device-name hint into a ControllerFamily using a fixed,
  // deterministic, case-insensitive substring match. An empty or
  // unrecognized hint always yields ControllerFamily::Generic -- never a
  // guess and never dependent on iteration order over multiple
  // candidates, since exactly one deterministic priority order is
  // checked: Steam Deck, then PlayStation, then Xbox, then Generic.
  [[nodiscard]] static ControllerFamily
  detectFamily(const QString &deviceNameHint);

  // Returns the fixed glyph label for |button| under |family|. Every
  // (family, button) pair is covered by an exhaustive switch, so this
  // never falls through to a default/placeholder label.
  [[nodiscard]] static QString glyphLabel(ControllerFamily family,
                                          GamepadButton button);
};

} // namespace Arkham
