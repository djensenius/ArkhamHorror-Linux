#include <QtTest>

#include "InputMapper.h"

using Arkham::CommandPhase;
using Arkham::DispatchedCommand;
using Arkham::InputMapper;
using Arkham::PhysicalKey;
using Arkham::RemapError;
using Arkham::SemanticCommand;

class InputMapperTests final : public QObject {
  Q_OBJECT

private slots:
  void hasStableDefaultBindingsForFocusAndPrimaryAction();
  void reservedKeysAreFixedAndCannotBeRemapped();
  void reservedKeysCannotBeUnbound();
  void unknownKeyIsSafeAndProducesNoCommand();
  void remapAddsAnAliasWithoutEvictingTheOldBinding();
  void remapOfAnOrdinaryKeyNeverEvictsReservedKeyAliases();
  void remapOfOneDefaultAliasPreservesItsSiblingDefaultAliases();
  void remapRejectsStealingAnAlreadyBoundKeyWithoutExplicitUnbind();
  void remapToleratesReassigningACommandToItsOwnCurrentKey();
  void unbindRemovesANonReservedBinding();
  void pressRepeatReleaseProducesTheRightPhases();
  void duplicatePressWithoutReleaseIsDeduped();
  void strayReleaseWithoutAPriorPressIsDeduped();
  void autoRepeatBeforeAnyRealPressIsDeduped();
  void bindingAKeyMidHoldNeverProducesARepeatedOrReleasedWithoutAPressed();
  void rebindingAnArmedHeldKeyStillReleasesTheOriginallyDispatchedCommand();
  void unbindingAnArmedHeldKeyStillReleasesTheOriginallyDispatchedCommand();
  void resetToDefaultsClearsHeldStateAndCustomBindings();
  void modifiersAreOnlyPartOfBindingLookupNotHeldKeyIdentity();
  void
  releasingAHeldKeyWithChangedLiveModifiersStillReleasesThePressTimeCommand();
  void addingAModifierMidHoldStillRepeatsWithThePressTimeCommand();
  void pressAfterAModifierChangedReleaseIsNotSwallowed();
  void twoDifferentPhysicalKeysAliasedToTheSameCommandAreTrackedIndependently();
  void clearHeldKeysForgetsArmedHoldsWithoutDispatchingAndAllowsAFreshPress();

private:
  static PhysicalKey key(Qt::Key key,
                         Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    return PhysicalKey{key, modifiers};
  }
};

void InputMapperTests::hasStableDefaultBindingsForFocusAndPrimaryAction() {
  InputMapper mapper;
  QCOMPARE(mapper.commandFor(key(Qt::Key_Up)), SemanticCommand::FocusUp);
  QCOMPARE(mapper.commandFor(key(Qt::Key_W)), SemanticCommand::FocusUp);
  QCOMPARE(mapper.commandFor(key(Qt::Key_Down)), SemanticCommand::FocusDown);
  QCOMPARE(mapper.commandFor(key(Qt::Key_Left)), SemanticCommand::FocusLeft);
  QCOMPARE(mapper.commandFor(key(Qt::Key_Right)), SemanticCommand::FocusRight);
  QCOMPARE(mapper.commandFor(key(Qt::Key_Return)),
           SemanticCommand::PrimaryAction);
  QCOMPARE(mapper.commandFor(key(Qt::Key_Space)),
           SemanticCommand::PrimaryAction);
}

void InputMapperTests::reservedKeysAreFixedAndCannotBeRemapped() {
  InputMapper mapper;
  QVERIFY(mapper.isReserved(key(Qt::Key_Escape)));
  QVERIFY(mapper.isReserved(key(Qt::Key_Back)));
  QVERIFY(mapper.isReserved(key(Qt::Key_Menu)));
  QVERIFY(!mapper.isReserved(key(Qt::Key_A)));

  QCOMPARE(mapper.commandFor(key(Qt::Key_Escape)),
           SemanticCommand::SecondaryAction);
  QCOMPARE(mapper.commandFor(key(Qt::Key_Back)),
           SemanticCommand::SecondaryAction);
  QCOMPARE(mapper.commandFor(key(Qt::Key_Menu)), SemanticCommand::OpenMenu);

  const auto escapeResult =
      mapper.remap(key(Qt::Key_Escape), SemanticCommand::Undo);
  QVERIFY(escapeResult.has_value());
  QCOMPARE(*escapeResult, RemapError::ReservedKey);
  // Rejected: the reserved binding must be completely unchanged.
  QCOMPARE(mapper.commandFor(key(Qt::Key_Escape)),
           SemanticCommand::SecondaryAction);

  const auto backResult =
      mapper.remap(key(Qt::Key_Back), SemanticCommand::Undo);
  QVERIFY(backResult.has_value());
  QCOMPARE(*backResult, RemapError::ReservedKey);

  const auto menuResult =
      mapper.remap(key(Qt::Key_Menu), SemanticCommand::Undo);
  QVERIFY(menuResult.has_value());
  QCOMPARE(*menuResult, RemapError::ReservedKey);
}

void InputMapperTests::reservedKeysCannotBeUnbound() {
  InputMapper mapper;
  QVERIFY(!mapper.unbind(key(Qt::Key_Escape)));
  QVERIFY(!mapper.unbind(key(Qt::Key_Back)));
  QVERIFY(!mapper.unbind(key(Qt::Key_Menu)));
  QCOMPARE(mapper.commandFor(key(Qt::Key_Escape)),
           SemanticCommand::SecondaryAction);
}

void InputMapperTests::unknownKeyIsSafeAndProducesNoCommand() {
  InputMapper mapper;
  QVERIFY(!mapper.commandFor(key(Qt::Key_F13)).has_value());

  const auto pressed = mapper.processKey(key(Qt::Key_F13), true, false);
  QVERIFY(!pressed.has_value());
  const auto released = mapper.processKey(key(Qt::Key_F13), false, false);
  QVERIFY(!released.has_value());
}

void InputMapperTests::remapAddsAnAliasWithoutEvictingTheOldBinding() {
  InputMapper mapper;
  QVERIFY(!mapper.remap(key(Qt::Key_F1), SemanticCommand::Undo).has_value());
  QCOMPARE(mapper.commandFor(key(Qt::Key_F1)), SemanticCommand::Undo);

  // Undo was already bound to Ctrl+Z by default; remap() only ever adds
  // or moves the one physical key it was given (F1), so the old Ctrl+Z
  // binding must still be intact -- multiple physical keys may share a
  // command, exactly like the built-in multi-alias defaults (Up+W,
  // Return+Enter+Space) already do.
  QCOMPARE(mapper.commandFor(key(Qt::Key_Z, Qt::ControlModifier)),
           SemanticCommand::Undo);

  // Both aliases actually dispatch Undo independently.
  const auto f1Pressed = mapper.processKey(key(Qt::Key_F1), true, false);
  QVERIFY(f1Pressed.has_value());
  QCOMPARE(f1Pressed->command, SemanticCommand::Undo);
  const auto ctrlZPressed =
      mapper.processKey(key(Qt::Key_Z, Qt::ControlModifier), true, false);
  QVERIFY(ctrlZPressed.has_value());
  QCOMPARE(ctrlZPressed->command, SemanticCommand::Undo);

  // A caller that actually wants to free up the old key must explicitly
  // unbind() it -- the same explicit step already required to steal a
  // key away from a different command.
  QVERIFY(mapper.unbind(key(Qt::Key_Z, Qt::ControlModifier)));
  QVERIFY(!mapper.commandFor(key(Qt::Key_Z, Qt::ControlModifier)).has_value());
  QCOMPARE(mapper.commandFor(key(Qt::Key_F1)), SemanticCommand::Undo);
}

void InputMapperTests::remapOfAnOrdinaryKeyNeverEvictsReservedKeyAliases() {
  InputMapper mapper;
  // Escape and Back are both permanently reserved and both already own
  // SecondaryAction by default; Menu is reserved and owns OpenMenu.
  // Remapping an ordinary key to one of those same commands must not
  // silently unbind (or otherwise disturb) the reserved keys' own fixed
  // bindings.
  QVERIFY(!mapper.remap(key(Qt::Key_F1), SemanticCommand::SecondaryAction)
               .has_value());
  QCOMPARE(mapper.commandFor(key(Qt::Key_Escape)),
           SemanticCommand::SecondaryAction);
  QCOMPARE(mapper.commandFor(key(Qt::Key_Back)),
           SemanticCommand::SecondaryAction);

  QVERIFY(
      !mapper.remap(key(Qt::Key_F2), SemanticCommand::OpenMenu).has_value());
  QCOMPARE(mapper.commandFor(key(Qt::Key_Menu)), SemanticCommand::OpenMenu);

  // Escape/Back/Menu must still actually dispatch and consume their
  // reserved commands afterwards, not merely still report them via
  // commandFor().
  const auto escapePressed =
      mapper.processKey(key(Qt::Key_Escape), true, false);
  QVERIFY(escapePressed.has_value());
  QCOMPARE(escapePressed->command, SemanticCommand::SecondaryAction);
  const auto backPressed = mapper.processKey(key(Qt::Key_Back), true, false);
  QVERIFY(backPressed.has_value());
  QCOMPARE(backPressed->command, SemanticCommand::SecondaryAction);
  const auto menuPressed = mapper.processKey(key(Qt::Key_Menu), true, false);
  QVERIFY(menuPressed.has_value());
  QCOMPARE(menuPressed->command, SemanticCommand::OpenMenu);
}

void InputMapperTests::
    remapOfOneDefaultAliasPreservesItsSiblingDefaultAliases() {
  InputMapper mapper;
  // FocusUp is bound by default to both Up and W; PrimaryAction to
  // Return, Enter, and Space. Remapping a single alias of one of these
  // commands to a brand-new key must not collapse -- or otherwise
  // disturb -- the other, unrelated sibling aliases.
  QVERIFY(!mapper.remap(key(Qt::Key_F1), SemanticCommand::FocusUp).has_value());
  QCOMPARE(mapper.commandFor(key(Qt::Key_Up)), SemanticCommand::FocusUp);
  QCOMPARE(mapper.commandFor(key(Qt::Key_W)), SemanticCommand::FocusUp);
  QCOMPARE(mapper.commandFor(key(Qt::Key_F1)), SemanticCommand::FocusUp);

  QVERIFY(!mapper.remap(key(Qt::Key_F2), SemanticCommand::PrimaryAction)
               .has_value());
  QCOMPARE(mapper.commandFor(key(Qt::Key_Return)),
           SemanticCommand::PrimaryAction);
  QCOMPARE(mapper.commandFor(key(Qt::Key_Enter)),
           SemanticCommand::PrimaryAction);
  QCOMPARE(mapper.commandFor(key(Qt::Key_Space)),
           SemanticCommand::PrimaryAction);
  QCOMPARE(mapper.commandFor(key(Qt::Key_F2)), SemanticCommand::PrimaryAction);
}

void InputMapperTests::
    remapRejectsStealingAnAlreadyBoundKeyWithoutExplicitUnbind() {
  InputMapper mapper;
  // Qt::Key_Up is already bound to FocusUp by default.
  const auto result =
      mapper.remap(key(Qt::Key_Up), SemanticCommand::CameraZoomIn);
  QVERIFY(result.has_value());
  QCOMPARE(*result, RemapError::KeyAlreadyBound);
  // Rejected: the existing binding must be completely unchanged.
  QCOMPARE(mapper.commandFor(key(Qt::Key_Up)), SemanticCommand::FocusUp);

  // Explicitly unbinding first, then remapping, must succeed.
  QVERIFY(mapper.unbind(key(Qt::Key_Up)));
  QVERIFY(!mapper.remap(key(Qt::Key_Up), SemanticCommand::CameraZoomIn)
               .has_value());
  QCOMPARE(mapper.commandFor(key(Qt::Key_Up)), SemanticCommand::CameraZoomIn);
}

void InputMapperTests::remapToleratesReassigningACommandToItsOwnCurrentKey() {
  InputMapper mapper;
  QVERIFY(!mapper.remap(key(Qt::Key_Up), SemanticCommand::FocusUp).has_value());
  QCOMPARE(mapper.commandFor(key(Qt::Key_Up)), SemanticCommand::FocusUp);
}

void InputMapperTests::unbindRemovesANonReservedBinding() {
  InputMapper mapper;
  QVERIFY(mapper.unbind(key(Qt::Key_Up)));
  QVERIFY(!mapper.commandFor(key(Qt::Key_Up)).has_value());
  // Unbinding an already-unbound key is a safe, deterministic false.
  QVERIFY(!mapper.unbind(key(Qt::Key_Up)));
}

void InputMapperTests::pressRepeatReleaseProducesTheRightPhases() {
  InputMapper mapper;
  const PhysicalKey up = key(Qt::Key_Up);

  const auto pressed = mapper.processKey(up, true, false);
  QVERIFY(pressed.has_value());
  QCOMPARE(*pressed, (DispatchedCommand{SemanticCommand::FocusUp,
                                        CommandPhase::Pressed}));

  const auto repeated = mapper.processKey(up, true, true);
  QVERIFY(repeated.has_value());
  QCOMPARE(*repeated, (DispatchedCommand{SemanticCommand::FocusUp,
                                         CommandPhase::Repeated}));

  const auto repeatedAgain = mapper.processKey(up, true, true);
  QVERIFY(repeatedAgain.has_value());
  QCOMPARE(repeatedAgain->phase, CommandPhase::Repeated);

  const auto released = mapper.processKey(up, false, false);
  QVERIFY(released.has_value());
  QCOMPARE(*released, (DispatchedCommand{SemanticCommand::FocusUp,
                                         CommandPhase::Released}));
}

void InputMapperTests::duplicatePressWithoutReleaseIsDeduped() {
  InputMapper mapper;
  const PhysicalKey up = key(Qt::Key_Up);

  QVERIFY(mapper.processKey(up, true, false).has_value());
  // A second non-auto-repeat press without an intervening release is a
  // stray duplicate: suppressed entirely.
  QVERIFY(!mapper.processKey(up, true, false).has_value());

  // The key is still considered held: a release now must still produce
  // exactly one Released.
  const auto released = mapper.processKey(up, false, false);
  QVERIFY(released.has_value());
  QCOMPARE(released->phase, CommandPhase::Released);

  // A second release now (already released) must be suppressed.
  QVERIFY(!mapper.processKey(up, false, false).has_value());
}

void InputMapperTests::strayReleaseWithoutAPriorPressIsDeduped() {
  InputMapper mapper;
  QVERIFY(!mapper.processKey(key(Qt::Key_Up), false, false).has_value());
}

void InputMapperTests::autoRepeatBeforeAnyRealPressIsDeduped() {
  InputMapper mapper;
  // An auto-repeat "press" arriving without ever having seen the real
  // (non-auto-repeat) press first can never establish held-state or
  // dispatch on its own.
  QVERIFY(!mapper.processKey(key(Qt::Key_Up), true, true).has_value());
  // Because no held-state was established, a subsequent real press must
  // still behave as an ordinary fresh press, not be treated as a
  // duplicate.
  const auto pressed = mapper.processKey(key(Qt::Key_Up), true, false);
  QVERIFY(pressed.has_value());
  QCOMPARE(pressed->phase, CommandPhase::Pressed);
}

void InputMapperTests::
    bindingAKeyMidHoldNeverProducesARepeatedOrReleasedWithoutAPressed() {
  InputMapper mapper;
  // F1 is unbound by default (see
  // resetToDefaultsClearsHeldStateAndCustomBindings).
  const PhysicalKey f1 = key(Qt::Key_F1);
  QVERIFY(!mapper.commandFor(f1).has_value());

  // Press F1 while it is still unbound: correctly produces no command.
  QVERIFY(!mapper.processKey(f1, true, false).has_value());

  // Now bind F1 to a command *while it is still physically held down*
  // (e.g. the user opened a remap UI mid-hold). Neither an auto-repeat
  // nor the eventual release of this same hold may dispatch anything:
  // this hold never had a dispatched Pressed, so it must stay silent
  // for the rest of its lifetime rather than emit a Repeated or
  // Released with no matching Pressed (see CommandPhase's contract).
  QVERIFY(!mapper.remap(f1, SemanticCommand::Undo).has_value());
  QCOMPARE(mapper.commandFor(f1), SemanticCommand::Undo);

  QVERIFY(!mapper.processKey(f1, true, true).has_value());
  QVERIFY(!mapper.processKey(f1, false, false).has_value());

  // The release above still correctly cleared the held state: a brand
  // new press-repeat-release cycle for the now-bound key behaves
  // normally.
  const auto pressed = mapper.processKey(f1, true, false);
  QVERIFY(pressed.has_value());
  QCOMPARE(pressed->phase, CommandPhase::Pressed);
  const auto repeated = mapper.processKey(f1, true, true);
  QVERIFY(repeated.has_value());
  QCOMPARE(repeated->phase, CommandPhase::Repeated);
  const auto released = mapper.processKey(f1, false, false);
  QVERIFY(released.has_value());
  QCOMPARE(released->phase, CommandPhase::Released);
}

void InputMapperTests::
    rebindingAnArmedHeldKeyStillReleasesTheOriginallyDispatchedCommand() {
  InputMapper mapper;
  // Up is bound to FocusUp by default; press it so the hold is armed
  // with FocusUp specifically.
  const PhysicalKey up = key(Qt::Key_Up);
  const auto pressed = mapper.processKey(up, true, false);
  QVERIFY(pressed.has_value());
  QCOMPARE(pressed->command, SemanticCommand::FocusUp);

  // Remap the same physical key to an entirely different command while
  // it is still physically held (unbind first, since remap() refuses to
  // steal a key already bound to a different command).
  QVERIFY(mapper.unbind(up));
  QVERIFY(!mapper.remap(up, SemanticCommand::Undo).has_value());
  QCOMPARE(mapper.commandFor(up), SemanticCommand::Undo);

  // Every later phase of *this same hold* must keep reporting the
  // command that was actually dispatched at press time (FocusUp), never
  // the new binding (Undo): a single physical hold can never appear to
  // change which command it belongs to partway through.
  const auto repeated = mapper.processKey(up, true, true);
  QVERIFY(repeated.has_value());
  QCOMPARE(repeated->command, SemanticCommand::FocusUp);
  QCOMPARE(repeated->phase, CommandPhase::Repeated);

  const auto released = mapper.processKey(up, false, false);
  QVERIFY(released.has_value());
  QCOMPARE(released->command, SemanticCommand::FocusUp);
  QCOMPARE(released->phase, CommandPhase::Released);

  // A fresh press after the release correctly reports the new binding.
  const auto freshPress = mapper.processKey(up, true, false);
  QVERIFY(freshPress.has_value());
  QCOMPARE(freshPress->command, SemanticCommand::Undo);
}

void InputMapperTests::
    unbindingAnArmedHeldKeyStillReleasesTheOriginallyDispatchedCommand() {
  InputMapper mapper;
  const PhysicalKey up = key(Qt::Key_Up);
  const auto pressed = mapper.processKey(up, true, false);
  QVERIFY(pressed.has_value());
  QCOMPARE(pressed->command, SemanticCommand::FocusUp);

  // Unbind the key entirely while it is still physically held.
  QVERIFY(mapper.unbind(up));
  QVERIFY(!mapper.commandFor(up).has_value());

  // The already-dispatched Pressed for this hold still gets its
  // matching Released (and would still get Repeated, if repeated)
  // reporting the originally-dispatched command, even though the key is
  // now completely unbound.
  const auto repeated = mapper.processKey(up, true, true);
  QVERIFY(repeated.has_value());
  QCOMPARE(repeated->command, SemanticCommand::FocusUp);

  const auto released = mapper.processKey(up, false, false);
  QVERIFY(released.has_value());
  QCOMPARE(released->command, SemanticCommand::FocusUp);
  QCOMPARE(released->phase, CommandPhase::Released);
}

void InputMapperTests::resetToDefaultsClearsHeldStateAndCustomBindings() {
  InputMapper mapper;
  QVERIFY(mapper.processKey(key(Qt::Key_Up), true, false).has_value());
  QVERIFY(!mapper.remap(key(Qt::Key_F1), SemanticCommand::Undo).has_value());

  mapper.resetToDefaults();

  QVERIFY(!mapper.commandFor(key(Qt::Key_F1)).has_value());
  QCOMPARE(mapper.commandFor(key(Qt::Key_Up)), SemanticCommand::FocusUp);
  // Held state was cleared: a fresh (non-auto-repeat) press of Up must
  // behave as a brand-new press, not a suppressed duplicate.
  const auto pressed = mapper.processKey(key(Qt::Key_Up), true, false);
  QVERIFY(pressed.has_value());
  QCOMPARE(pressed->phase, CommandPhase::Pressed);
}

void InputMapperTests::modifiersAreOnlyPartOfBindingLookupNotHeldKeyIdentity() {
  InputMapper mapper;
  // Ctrl+Z (Undo) and plain Z (unbound by default) are distinct
  // *bindings* -- modifiers are part of a physical key's identity for
  // commandFor()/bindings_ lookup purposes.
  QCOMPARE(mapper.commandFor(key(Qt::Key_Z, Qt::ControlModifier)),
           SemanticCommand::Undo);
  QVERIFY(!mapper.commandFor(key(Qt::Key_Z)).has_value());

  // But once Z is physically pressed (as Ctrl+Z), it is the exact same
  // physical Z key regardless of what modifiers a later event for it
  // reports: held-key tracking is keyed by Qt::Key alone (see
  // heldKeys_'s doc comment), so a second "press" of Z with different
  // modifiers while it is still held is correctly a stray duplicate of
  // the *same* hold, not an independent second press. (See
  // releasingAHeldKeyWithChangedLiveModifiersStillReleasesThePressTimeCommand
  // for the release-side counterpart of this exact scenario.)
  QVERIFY(mapper.processKey(key(Qt::Key_Z, Qt::ControlModifier), true, false)
              .has_value());
  QVERIFY(!mapper.processKey(key(Qt::Key_Z), true, false).has_value());

  // A totally different physical key (Y) remains fully independent: it
  // was never pressed, so a release for it is a stray release.
  QVERIFY(!mapper.processKey(key(Qt::Key_Y), false, false).has_value());
}

void InputMapperTests::
    releasingAHeldKeyWithChangedLiveModifiersStillReleasesThePressTimeCommand() {
  InputMapper mapper;
  // Press Ctrl+Z: Undo is armed for the physical Z key.
  const auto pressed =
      mapper.processKey(key(Qt::Key_Z, Qt::ControlModifier), true, false);
  QVERIFY(pressed.has_value());
  QCOMPARE(pressed->command, SemanticCommand::Undo);
  QCOMPARE(pressed->phase, CommandPhase::Pressed);

  // A perfectly ordinary way to type a chord: release Ctrl *before*
  // releasing Z. The eventual KeyRelease event for Z then reports
  // Qt::NoModifier (Ctrl is no longer held), not the Qt::ControlModifier
  // that was live when Z was pressed. This must still be recognized as
  // releasing the same physical Z key and still report the command that
  // was actually armed at press time (Undo), not be treated as an
  // unrelated stray release.
  const auto released = mapper.processKey(key(Qt::Key_Z), false, false);
  QVERIFY(released.has_value());
  QCOMPARE(released->command, SemanticCommand::Undo);
  QCOMPARE(released->phase, CommandPhase::Released);
}

void InputMapperTests::
    addingAModifierMidHoldStillRepeatsWithThePressTimeCommand() {
  InputMapper mapper;
  const auto pressed =
      mapper.processKey(key(Qt::Key_Z, Qt::ControlModifier), true, false);
  QVERIFY(pressed.has_value());
  QCOMPARE(pressed->command, SemanticCommand::Undo);

  // The user presses Shift *while still holding* Ctrl+Z, so the
  // platform's auto-repeat for Z now reports Ctrl+Shift instead of just
  // Ctrl. The repeat must still report the command armed at press time
  // (Undo), regardless of the now-different live modifiers.
  const auto repeated = mapper.processKey(
      key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier), true, true);
  QVERIFY(repeated.has_value());
  QCOMPARE(repeated->command, SemanticCommand::Undo);
  QCOMPARE(repeated->phase, CommandPhase::Repeated);

  const auto released = mapper.processKey(
      key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier), false, false);
  QVERIFY(released.has_value());
  QCOMPARE(released->command, SemanticCommand::Undo);
  QCOMPARE(released->phase, CommandPhase::Released);
}

void InputMapperTests::pressAfterAModifierChangedReleaseIsNotSwallowed() {
  InputMapper mapper;
  // This is the exact regression the modifier-independent held-key
  // tracking fixes: without it, the release below (with changed live
  // modifiers) would be misclassified as a stray release, permanently
  // leaking the original press-time hold and causing the *next*
  // legitimate Ctrl+Z press to be misclassified as a stray duplicate and
  // suppressed entirely.
  QVERIFY(mapper.processKey(key(Qt::Key_Z, Qt::ControlModifier), true, false)
              .has_value());
  QVERIFY(mapper.processKey(key(Qt::Key_Z), false, false).has_value());

  const auto secondPress =
      mapper.processKey(key(Qt::Key_Z, Qt::ControlModifier), true, false);
  QVERIFY(secondPress.has_value());
  QCOMPARE(secondPress->command, SemanticCommand::Undo);
  QCOMPARE(secondPress->phase, CommandPhase::Pressed);
}

void InputMapperTests::
    twoDifferentPhysicalKeysAliasedToTheSameCommandAreTrackedIndependently() {
  InputMapper mapper;
  // Up and W are both bound to FocusUp by default; holding one must
  // never affect the other's independent held-key state.
  const auto upPressed = mapper.processKey(key(Qt::Key_Up), true, false);
  QVERIFY(upPressed.has_value());
  const auto wPressed = mapper.processKey(key(Qt::Key_W), true, false);
  QVERIFY(wPressed.has_value());

  // A duplicate press of either individual key (without its own release)
  // is still a stray duplicate for *that* key.
  QVERIFY(!mapper.processKey(key(Qt::Key_Up), true, false).has_value());
  QVERIFY(!mapper.processKey(key(Qt::Key_W), true, false).has_value());

  // Releasing Up must not affect W's still-held state, and vice versa.
  const auto upReleased = mapper.processKey(key(Qt::Key_Up), false, false);
  QVERIFY(upReleased.has_value());
  QCOMPARE(upReleased->phase, CommandPhase::Released);

  const auto wRepeated = mapper.processKey(key(Qt::Key_W), true, true);
  QVERIFY(wRepeated.has_value());
  QCOMPARE(wRepeated->phase, CommandPhase::Repeated);

  const auto wReleased = mapper.processKey(key(Qt::Key_W), false, false);
  QVERIFY(wReleased.has_value());
  QCOMPARE(wReleased->phase, CommandPhase::Released);
}

void InputMapperTests::
    clearHeldKeysForgetsArmedHoldsWithoutDispatchingAndAllowsAFreshPress() {
  InputMapper mapper;
  const auto pressed = mapper.processKey(key(Qt::Key_Up), true, false);
  QVERIFY(pressed.has_value());

  // clearHeldKeys() itself never returns anything to dispatch -- there
  // is nothing here to assert a "no dispatch" outcome against beyond the
  // fact that it compiles/returns void, but the key behavioral guarantee
  // is exercised below: the still-physically-held Up key is now treated
  // as fully released.
  mapper.clearHeldKeys();

  // A fresh, non-auto-repeat press of the same key (with no real release
  // ever having been observed) must be treated as a brand-new hold, not
  // a suppressed stray duplicate.
  const auto freshPress = mapper.processKey(key(Qt::Key_Up), true, false);
  QVERIFY(freshPress.has_value());
  QCOMPARE(freshPress->phase, CommandPhase::Pressed);

  // And an auto-repeat/release for what would have been the *old* hold,
  // had it not been cleared, must not resurrect it: this is exactly a
  // fresh hold now, so it behaves like any other normal one.
  const auto released = mapper.processKey(key(Qt::Key_Up), false, false);
  QVERIFY(released.has_value());
  QCOMPARE(released->phase, CommandPhase::Released);
}

QTEST_APPLESS_MAIN(InputMapperTests)

#include "InputMapperTests.moc"
