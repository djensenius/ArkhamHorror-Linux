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

  if (event->type() != QEvent::KeyPress &&
      event->type() != QEvent::KeyRelease) {
    return QObject::eventFilter(watched, event);
  }

  auto *keyEvent = static_cast<QKeyEvent *>(event);
  const PhysicalKey physicalKey{static_cast<Qt::Key>(keyEvent->key()),
                                keyEvent->modifiers()};
  const bool isPress = event->type() == QEvent::KeyPress;

  // Known ahead of calling processKey() so a dedup-suppressed transition
  // (a stray duplicate press, an auto-repeat before any press, or a
  // stray release) can still be consumed below: those transitions belong
  // to a physical key this router already owns, and letting them fall
  // through to default Qt key handling could re-trigger a side effect
  // (e.g. Qt's own Tab-key focus-chain handling) for a key our own
  // dedup state machine has already accounted for via a different
  // phase.
  const bool isBoundKey = mapper_.commandFor(physicalKey).has_value();

  const std::optional<DispatchedCommand> dispatched =
      mapper_.processKey(physicalKey, isPress, keyEvent->isAutoRepeat());
  if (!dispatched.has_value()) {
    return isBoundKey;
  }

  emit commandDispatched(dispatched->command, dispatched->phase);
  return true;
}

} // namespace Arkham
