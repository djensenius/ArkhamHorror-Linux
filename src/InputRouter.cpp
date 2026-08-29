#include "InputRouter.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QThread>
#include <QWindow>

namespace Arkham {

namespace {

// Whether the object Qt itself currently considers focused for input-
// method purposes accepts text entry right now, per Qt's own input-
// method query mechanism (the same one virtual keyboards use to decide
// whether to show themselves). Always false when |installedTarget| is
// null (nothing installed -- see below). Otherwise prefers the installed
// target window's own QWindow::focusObject() (an *instance* method,
// available without any QtQuick dependency: QWindow is QtGui) over the
// static QGuiApplication::focusObject(), because the static accessor
// only reflects QGuiApplication::focusWindow() -- i.e. real
// platform-level window activation -- which offscreen/headless test
// environments and some embedded/composited setups (Gamescope included)
// never grant, even though the window's own Qt Quick scene graph
// already has a perfectly real, correct activeFocusItem. Falling back
// to the static accessor keeps this correct for non-QWindow install()
// targets and for real, actually-activated windows where both agree.
// Safe to call with no QGuiApplication instance at all (e.g. under
// QTEST_GUILESS_MAIN, which only creates a QCoreApplication):
// qobject_cast fails cleanly in that case rather than risking undefined
// behavior from calling a QGuiApplication:: static through an instance
// that is not actually one. Equally safe when there is a QGuiApplication
// but no current focus object, or a focus object that never overrides
// input-method handling (an ordinary non-text QQuickItem):
// QInputMethodQueryEvent::value() returns a default-constructed
// (invalid) QVariant for any query the receiver never set, and
// QVariant().toBool() is false.
bool focusedObjectAcceptsTextEntry(QObject *installedTarget) {
  // Nothing installed means this router is not filtering any events at
  // all right now, so there is no target whose keys automatic detection
  // could ever need to protect; treat this as "not suspended" rather
  // than consulting whatever unrelated object the wider application
  // happens to consider focused (guiApp->focusObject() reflects the
  // *whole application's* focus window, not anything scoped to this
  // router), which could otherwise report suspended based on a
  // completely unrelated window's text field.
  if (installedTarget == nullptr) {
    return false;
  }
  auto *guiApp = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
  if (guiApp == nullptr) {
    return false;
  }
  QObject *focusObject = nullptr;
  if (auto *window = qobject_cast<QWindow *>(installedTarget)) {
    focusObject = window->focusObject();
  }
  if (focusObject == nullptr) {
    focusObject = guiApp->focusObject();
  }
  if (focusObject == nullptr) {
    return false;
  }
  QInputMethodQueryEvent query(Qt::ImEnabled);
  QCoreApplication::sendEvent(focusObject, &query);
  return query.value(Qt::ImEnabled).toBool();
}

} // namespace

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

bool InputRouter::isTextEntrySuspended() const {
  return explicitlySuspended_ ||
         (automaticDetectionEnabled_ &&
          focusedObjectAcceptsTextEntry(installedTarget_));
}

void InputRouter::setSemanticInputSuspended(const bool suspended) {
  explicitlySuspended_ = suspended;
  refreshTextEntrySuspension();
}

bool InputRouter::isSemanticInputExplicitlySuspended() const {
  return explicitlySuspended_;
}

void InputRouter::setAutomaticTextEntryDetectionEnabled(const bool enabled) {
  automaticDetectionEnabled_ = enabled;
  refreshTextEntrySuspension();
}

bool InputRouter::isAutomaticTextEntryDetectionEnabled() const {
  return automaticDetectionEnabled_;
}

void InputRouter::refreshTextEntrySuspension() {
  // Deliberately recomputes the same OR'd expression isTextEntrySuspended()
  // itself evaluates, rather than reusing any cached value: the sole
  // purpose of this helper is bookkeeping (clearing held/armed state
  // exactly on a transition), never to serve as the public getter's
  // backing store -- isTextEntrySuspended() must always reflect the
  // live state, even between key events, so a caller can observe a
  // focus-driven change the moment it asks, not just after the next key
  // arrives.
  const bool newSuspended = isTextEntrySuspended();
  if (newSuspended == effectiveSuspended_) {
    return;
  }
  effectiveSuspended_ = newSuspended;
  // See the class comment on setSemanticInputSuspended(): a hold
  // spanning a suspend/resume transition must never survive it, or a
  // stale release/next press could be mishandled once the transition
  // has already changed which physical keys this router is allowed to
  // touch.
  mapper_.clearHeldKeys();
}

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

  // Recomputed fresh before every KeyPress/KeyRelease decision, not just
  // when a setter is called: automatic detection depends on whatever Qt
  // currently considers the focus object, which can change (e.g. a
  // TextField gaining active focus) with no notification to this router
  // at all. This also guarantees held/armed state is cleared no later
  // than the very next key event after any such change -- see
  // refreshTextEntrySuspension()'s own comment.
  refreshTextEntrySuspension();
  if (effectiveSuspended_) {
    // Text entry owns this key: treat it exactly like an event for a
    // key this mapper has never heard of, regardless of any binding --
    // including the three reserved keys (see the class comment's "Text
    // entry" section for why Escape/Back/Menu are not exempted here).
    return false;
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
  // isArmedKeyHeld() -- checked before processKey() runs, since a
  // release can make the key no longer held by the time processKey()
  // returns.
  //
  // Conversely, commandFor() must NOT be trusted on its own for a key
  // that is currently held *unarmed* (pressed while unbound): a
  // remap()/bind() that runs while such a hold is still down would make
  // commandFor() start reporting a binding mid-hold, but processKey()
  // deliberately keeps replaying "unarmed" (dedup no-op) for the rest
  // of that same physical hold -- its initial press was correctly left
  // unconsumed, so its later repeat/release must stay unconsumed too,
  // or downstream code would observe a press with no matching release.
  // isKeyHeld() lets us tell "not held at all yet" (where commandFor()
  // alone is the right, and only, signal for a fresh press) apart from
  // "held unarmed" (where commandFor() must be ignored).
  const bool isOwnedKey = mapper_.isArmedKeyHeld(physicalKey.key) ||
                          (!mapper_.isKeyHeld(physicalKey.key) &&
                           mapper_.commandFor(physicalKey).has_value());

  const std::optional<DispatchedCommand> dispatched =
      mapper_.processKey(physicalKey, isPress, keyEvent->isAutoRepeat());
  if (!dispatched.has_value()) {
    return isOwnedKey;
  }

  emit commandDispatched(dispatched->command, dispatched->phase);
  return true;
}

} // namespace Arkham
