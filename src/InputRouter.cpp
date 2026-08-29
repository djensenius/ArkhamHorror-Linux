#include "InputRouter.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QThread>

namespace Arkham {

InputRouter::InputRouter(InputMapper &mapper, QObject *parent)
    : QObject(parent), mapper_(mapper) {}

InputRouter::~InputRouter() {
  // Set first: even if something below were to somehow trigger a
  // reentrant eventFilter() call during teardown, the guard at the top of
  // eventFilter() already refuses to dispatch once destroying_ is true.
  destroying_ = true;
  uninstall();
}

bool InputRouter::install(QObject *target) {
  if (target == nullptr) {
    return false;
  }
  if (target->thread() != thread()) {
    // Qt event filters are not supported across threads; refuse rather
    // than install something that could never correctly deliver events.
    return false;
  }
  if (installedTarget_ == target) {
    return true;
  }

  uninstall();

  target->installEventFilter(this);
  installedTarget_ = target;
  return true;
}

void InputRouter::uninstall() {
  if (installedTarget_) {
    installedTarget_->removeEventFilter(this);
  }
  installedTarget_.clear();
  // See the class comment: once uninstalled, this router can never
  // observe a real KeyRelease for whatever was held while it was
  // installed, so forget it now rather than let it leak into a later
  // install() on a different (or the same) target.
  mapper_.clearHeldKeys();
}

bool InputRouter::isInstalled() const { return !installedTarget_.isNull(); }

QObject *InputRouter::installedTarget() const { return installedTarget_; }

bool InputRouter::eventFilter(QObject *watched, QEvent *event) {
  // Guards against dispatching from an event that was already queued (via
  // postEvent, or simply pending in the queue) before uninstall()/
  // destruction, but is only actually delivered afterwards: this check is
  // evaluated fresh at delivery time, not captured when the event was
  // posted, so it always reflects the router's true current state.
  if (destroying_ || installedTarget_.isNull() || watched != installedTarget_) {
    return QObject::eventFilter(watched, event);
  }

  switch (event->type()) {
  case QEvent::FocusOut:
  case QEvent::WindowDeactivate:
  case QEvent::ApplicationDeactivate:
    // See the class comment: the platform can drop a key's real release
    // once focus/activation is lost, so forget all held-key state here
    // (without dispatching anything) rather than risk a permanently
    // stuck hold. These event types are never consumed -- only
    // KeyPress/KeyRelease ever are (see below).
    mapper_.clearHeldKeys();
    return QObject::eventFilter(watched, event);
  default:
    break;
  }

  if (event->type() != QEvent::KeyPress &&
      event->type() != QEvent::KeyRelease) {
    return QObject::eventFilter(watched, event);
  }

  auto *keyEvent = static_cast<QKeyEvent *>(event);
  // Qt::KeypadModifier records that a key came from the numeric keypad --
  // it is device-origin information, not a remappable modifier
  // combination like Ctrl/Shift/Alt/Meta, and no default or user binding
  // ever includes it. Real keypad Enter events carry it alongside
  // Qt::NoModifier, which would otherwise never match the plain
  // PhysicalKey{Key_Enter, NoModifier} binding and make keypad Enter
  // permanently unreachable; stripping just this one bit fixes that
  // without affecting any other modifier-based remap.
  const PhysicalKey physicalKey{static_cast<Qt::Key>(keyEvent->key()),
                                keyEvent->modifiers() & ~Qt::KeypadModifier};
  const bool isPress = event->type() == QEvent::KeyPress;

  // Known ahead of calling processKey() so a dedup-suppressed transition
  // (a stray duplicate press, an auto-repeat before any press, or a
  // stray release) can still be consumed below: those transitions belong
  // to a physical key this router already owns, and letting them fall
  // through to default Qt key handling could re-trigger a side effect
  // (e.g. Qt's own Tab-key focus-chain handling) for a key our own
  // dedup state machine has already accounted for via a different
  // phase. commandFor() alone is not enough here: it is modifier-
  // sensitive, but held-key identity is deliberately modifier-
  // insensitive (see InputMapper::heldKeys_'s comment), so a stray
  // duplicate transition whose *current* modifiers no longer match any
  // binding (e.g. Ctrl+Z is held, then a duplicate Z arrives with Ctrl
  // no longer down) must still be recognized as owned via
  // isPhysicalKeyHeld() -- checked before processKey() runs, since a
  // release can make the key no longer held by the time processKey()
  // returns.
  const bool isOwnedKey = mapper_.commandFor(physicalKey).has_value() ||
                          mapper_.isPhysicalKeyHeld(physicalKey.key);

  const std::optional<DispatchedCommand> dispatched =
      mapper_.processKey(physicalKey, isPress, keyEvent->isAutoRepeat());
  if (!dispatched.has_value()) {
    return isOwnedKey;
  }

  emit commandDispatched(dispatched->command, dispatched->phase);
  return true;
}

} // namespace Arkham
