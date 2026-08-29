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
  void remapMovesABindingAndEvictsThePreviousOwner();
  void remapRejectsStealingAnAlreadyBoundKeyWithoutExplicitUnbind();
  void remapToleratesReassigningACommandToItsOwnCurrentKey();
  void unbindRemovesANonReservedBinding();
  void pressRepeatReleaseProducesTheRightPhases();
  void duplicatePressWithoutReleaseIsDeduped();
  void strayReleaseWithoutAPriorPressIsDeduped();
  void autoRepeatBeforeAnyRealPressIsDeduped();
  void bindingAKeyMidHoldNeverProducesARepeatedOrReleasedWithoutAPressed();
  void resetToDefaultsClearsHeldStateAndCustomBindings();
  void modifiersDistinguishOtherwiseIdenticalKeys();

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

void InputMapperTests::remapMovesABindingAndEvictsThePreviousOwner() {
  InputMapper mapper;
  QVERIFY(!mapper.remap(key(Qt::Key_F1), SemanticCommand::Undo).has_value());
  QCOMPARE(mapper.commandFor(key(Qt::Key_F1)), SemanticCommand::Undo);

  // Undo was already bound to Ctrl+Z by default; moving it to F1 must
  // evict the old Ctrl+Z -> Undo binding entirely (a command may only
  // ever be bound to one physical key at a time).
  QVERIFY(!mapper.commandFor(key(Qt::Key_Z, Qt::ControlModifier)).has_value());
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

void InputMapperTests::modifiersDistinguishOtherwiseIdenticalKeys() {
  InputMapper mapper;
  // Ctrl+Z (Undo) and plain Z (unbound by default) are distinct physical
  // inputs.
  QCOMPARE(mapper.commandFor(key(Qt::Key_Z, Qt::ControlModifier)),
           SemanticCommand::Undo);
  QVERIFY(!mapper.commandFor(key(Qt::Key_Z)).has_value());

  QVERIFY(mapper.processKey(key(Qt::Key_Z, Qt::ControlModifier), true, false)
              .has_value());
  // Pressing plain Z (no modifier) while Ctrl+Z is "held" is an entirely
  // separate physical key and must not be affected by Ctrl+Z's held
  // state.
  QVERIFY(!mapper.processKey(key(Qt::Key_Z), true, false).has_value());
}

QTEST_APPLESS_MAIN(InputMapperTests)

#include "InputMapperTests.moc"
