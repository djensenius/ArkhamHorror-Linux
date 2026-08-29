#pragma once

#include "SemanticCommand.h"

#include <QHash>
#include <Qt>
#include <QtGlobal>

#include <optional>

namespace Arkham {

// One physical keyboard-style input: a Qt::Key plus the modifiers held
// alongside it. This is the *only* physical-input identity InputMapper
// ever deals with.
//
// Qt has no distinct "gamepad key" enum family (there is no
// Qt::Key_Gamepad* in Qt 6): on Linux, generic USB/Bluetooth game
// controllers and Steam Input's default desktop/generic-controller
// keyboard-emulation templates surface D-pad and face-button presses as
// perfectly ordinary QKeyEvents carrying standard Qt::Key values (e.g.
// arrow keys for the D-pad, Return/Enter for the primary face button,
// Escape for the secondary face button, Page Up/Page Down for shoulder
// buttons). That is precisely why a single physical-key-based mapping
// table, rather than a separate device-specific code path, correctly and
// portably covers keyboard, generic controllers, and Steam Input alike --
// without adding a QtGamepad or Steamworks dependency, and without ever
// inventing a nonexistent Qt enum. See InputRouter for where these events
// are actually captured.
struct PhysicalKey {
  Qt::Key key = Qt::Key_unknown;
  Qt::KeyboardModifiers modifiers = Qt::NoModifier;

  friend bool operator==(const PhysicalKey &, const PhysicalKey &) = default;
};

inline size_t qHash(const PhysicalKey &physicalKey, size_t seed = 0) {
  return qHashMulti(seed, static_cast<int>(physicalKey.key),
                    static_cast<int>(physicalKey.modifiers));
}

// Which phase of a physical key's lifecycle produced a dispatched command.
// A held key produces exactly one Pressed, zero or more Repeated (from the
// platform's key-repeat), and exactly one Released -- never two Presseds
// in a row without an intervening Released, and never a Released without a
// preceding Pressed. See InputMapper::processKey for the exact dedup rules
// that guarantee this.
enum class CommandPhase {
  Pressed,
  Repeated,
  Released,
};

// The outcome of a single physical key transition (a press, a platform
// auto-repeat, or a release), if it produced a semantic command at all.
// An unknown/unbound key, or a redundant/stray transition suppressed by
// dedup, safely produces std::nullopt rather than a partially-filled
// result or a crash.
struct DispatchedCommand {
  SemanticCommand command;
  CommandPhase phase;

  friend bool operator==(const DispatchedCommand &,
                         const DispatchedCommand &) = default;
};

// Why a requested remap() was rejected, if it was.
enum class RemapError {
  ReservedKey,     ///< |physicalKey| is Escape/Back/Menu; permanently fixed.
  KeyAlreadyBound, ///< |physicalKey| is already bound to a different command.
};

// Pure physical-input mapping/remapping/conflict model: turns physical
// keyboard-style input (see PhysicalKey) into SemanticCommand values, with
// user-configurable rebinding and deterministic conflict handling. "Pure"
// here means it performs no I/O, owns no QObject/event-loop machinery, and
// never touches a real QKeyEvent directly (InputRouter adapts real events
// into calls on this class) -- but it is not stateless: it tracks which
// physical keys are currently held down, which is exactly what makes
// press/repeat/release dedup possible.
//
// Escape, the dedicated "Back" key, and the dedicated "Menu" key are
// permanently reserved (Escape and Back to SecondaryAction, Menu to
// OpenMenu) and can never be remapped or unbound: this is what "reserve
// escape/back/menu behavior" means structurally, not just by convention.
class InputMapper {
public:
  InputMapper();

  // Returns the command currently bound to |physicalKey|, or std::nullopt
  // if it is unbound. Never throws; an unrecognized key is simply unbound.
  [[nodiscard]] std::optional<SemanticCommand>
  commandFor(const PhysicalKey &physicalKey) const;

  // True for exactly the three permanently reserved physical keys
  // (Escape, Back, Menu, each with Qt::NoModifier), regardless of what
  // they currently map to.
  [[nodiscard]] bool isReserved(const PhysicalKey &physicalKey) const;

  // Binds |physicalKey| to |command|. Multiple physical keys may be
  // bound to the same command at once -- this is the normal case, not an
  // edge case: the built-in defaults deliberately bind several aliases to
  // some commands (e.g. Up and W both to FocusUp; Return, Enter, and
  // Space all to PrimaryAction). remap() only ever adds or moves the one
  // |physicalKey| given; it never inspects or touches any *other* key's
  // existing binding, so remapping one alias of a command can never
  // silently evict a sibling default alias, and can never evict a
  // permanently reserved key's binding either (see the class comment). A
  // caller that actually wants to free up a specific old key must
  // explicitly unbind() it first -- exactly the same explicit step
  // already required to steal a key away from a different command (see
  // the rejection rule below).
  //
  // Rejected (and the existing binding table is left completely
  // unchanged) when:
  //  - |physicalKey| is one of the permanently reserved keys
  //    (RemapError::ReservedKey), or
  //  - |physicalKey| is already bound to a *different* command
  //    (RemapError::KeyAlreadyBound) -- the caller must explicitly
  //    unbind() the existing owner first if they intend to steal it.
  //
  // Re-binding a command to the physical key it is already bound to is a
  // deterministic no-op success.
  [[nodiscard]] std::optional<RemapError> remap(const PhysicalKey &physicalKey,
                                                SemanticCommand command);

  // Removes whatever command is bound to |physicalKey|. Returns false
  // (and leaves the table unchanged) if |physicalKey| is reserved or was
  // not bound to begin with; true otherwise.
  bool unbind(const PhysicalKey &physicalKey);

  // Restores the exact built-in default bindings (including the three
  // reserved keys) and clears all currently-held-key dedup state. Useful
  // for tests and for a future "reset to defaults" user action.
  void resetToDefaults();

  // Processes one physical key transition and returns the semantic
  // command it produces, if any.
  //
  // |isAutoRepeat| must be true only for platform-generated key-repeat
  // events (QKeyEvent::isAutoRepeat()), never for the original press.
  //
  // Dedup semantics, applied uniformly regardless of whether |physicalKey|
  // is bound, reserved, or completely unknown:
  //  - A non-auto-repeat press of a key that is already held down (e.g. a
  //    stray duplicate press delivered without an intervening release, as
  //    can happen across a focus-loss/regrab) is suppressed: no Pressed is
  //    re-emitted, and the key's held state is unchanged.
  //  - An auto-repeat "press" of a key that is *not* currently held is
  //    suppressed (a repeat can never establish new held-state on its
  //    own).
  //  - A release of a key that is not currently held (e.g. a stray
  //    release, or a release of a key that was released or never
  //    pressed) is suppressed.
  //  - An unbound/unrecognized key always safely returns std::nullopt; its
  //    held/not-held state is still tracked internally (purely so a later
  //    remap() of that same physical key starts from a correct held
  //    state), but it can never produce a dispatched command while
  //    unbound.
  //  - A key that was unbound for its entire press (or that only became
  //    bound again, via remap(), after already being released once) is
  //    never "armed": even if it is subsequently rebound while still
  //    physically held, its later auto-repeat and release transitions
  //    keep returning std::nullopt for the rest of that hold, rather
  //    than emitting a Repeated or Released with no preceding
  //    dispatched Pressed for that same hold.
  //  - Conversely, once a hold *is* armed (its Pressed was dispatched
  //    for some command), every later Repeated/Released for that same
  //    hold always reports that exact same command -- captured at
  //    press time -- even if the key is remapped to a different
  //    command or unbound entirely before it is released. A single
  //    physical hold can never appear to change or lose its command
  //    partway through.
  [[nodiscard]] std::optional<DispatchedCommand>
  processKey(const PhysicalKey &physicalKey, bool isPress, bool isAutoRepeat);

  // Forgets every currently-tracked held key without dispatching
  // anything -- in particular, this never synthesizes a Released for
  // whatever hold(s) were armed. Call this whenever physical key state
  // can no longer be trusted to eventually deliver a matching release:
  // the installed target/window losing focus or becoming inactive, an
  // InputRouter being uninstalled or losing its target, and (a hook for
  // future work, since this codebase has no live controller/gamepad
  // input API) a controller-disconnect notification, should all clear
  // held state through here. Afterwards, every physical key is treated
  // as fully released: the next press of any key -- even one that was
  // physically still held down when this was called -- always starts a
  // brand-new hold rather than being mistaken for a stray duplicate.
  void clearHeldKeys();

  // True if |key| currently has an *armed* held hold -- i.e. it was
  // bound to some command at press time (see the heldKeys_ comment
  // below) and has not yet been released -- regardless of what
  // modifiers currently accompany it. An unarmed hold (a key that was
  // unbound for its whole press so far) does not count: such a key was
  // never actually owned by any dispatched command, so its later
  // events must still be free to propagate normally. InputRouter uses
  // this -- together with commandFor() -- to decide whether an event
  // for |key| must be consumed even when processKey() suppresses it as
  // a dedup no-op: held-state is modifier-insensitive by design (see
  // the heldKeys_ comment), so a duplicate/stray transition whose
  // *current* modifiers no longer match any binding can still belong
  // to a physical key this mapper already owns for the rest of its
  // hold.
  [[nodiscard]] bool isArmedKeyHeld(Qt::Key key) const;

private:
  // Per-held-key state: a key present in |heldKeys_| is physically held
  // down regardless of whether it is currently bound to a command. A
  // present-but-empty optional means the hold is *unarmed* (it was
  // unbound at press time); a present value means the hold is armed
  // and is the exact command captured at press time, reused verbatim
  // for every later Repeated/Released of this same hold regardless of
  // any remap()/unbind() that happens while the key is still held. Only
  // an armed hold may go on to dispatch Repeated/Released, so a key
  // that was unbound at press time (and only became bound later,
  // mid-hold, via remap()) can never emit a Repeated or Released with
  // no matching Pressed -- it stays silent for the rest of that hold
  // instead, preserving the Pressed/Repeated*/Released contract
  // documented on CommandPhase.
  //
  // Keyed by Qt::Key alone -- not the full PhysicalKey (which also
  // carries live modifiers) -- because a physical key is a single
  // switch with exactly one held/not-held state regardless of what
  // modifiers happen to accompany any one event for it. Live modifiers
  // routinely differ between a key's press and its own release: e.g.
  // pressing Z while Ctrl is down reports modifiers() ==
  // Qt::ControlModifier, but if the user releases Ctrl *before*
  // releasing Z (an entirely ordinary way to type a chord), the
  // eventual KeyRelease for Z reports modifiers() == Qt::NoModifier.
  // Keying held-state by the full PhysicalKey would make that release
  // fail to find the entry armed at press time -- treating a real
  // release as a stray one, leaking the stale entry forever, and
  // silently swallowing the *next* legitimate press of that same key
  // once it collides again with the never-cleared old entry. Keying by
  // Qt::Key alone matches the actual physical model and sidesteps this
  // entirely; commandFor()/bindings_ still use the full PhysicalKey (key
  // + modifiers) for the one-time lookup at press time, which is all
  // that ever decides which command a hold is armed with.
  QHash<PhysicalKey, SemanticCommand> bindings_;
  QHash<Qt::Key, std::optional<SemanticCommand>> heldKeys_;

  static bool isReservedKey(const PhysicalKey &physicalKey);
};

} // namespace Arkham

Q_DECLARE_METATYPE(Arkham::CommandPhase)
