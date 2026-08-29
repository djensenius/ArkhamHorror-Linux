#pragma once

#include "InputMapper.h"
#include "SemanticCommand.h"

#include <QObject>
#include <QPointer>

namespace Arkham {

// A Qt event filter that turns real QKeyEvents (from a real keyboard, or
// from a generic controller/Steam Input surfaced as ordinary QKeyEvents --
// see InputMapper.h for why there is no separate device-specific code
// path) into SemanticCommand dispatches via an InputMapper.
//
// Lifetime/threading contract:
//  - install(target) installs this router as an event filter on |target|.
//    It is rejected (returns false, no filter installed) if |target| is
//    null or lives on a different thread than this router -- Qt event
//    filters are not supported across threads, so this is a deterministic
//    safe refusal rather than undefined behavior. Calling install() again
//    with the exact same |target| that is already installed is a
//    deterministic no-op success (returns true, filter state unchanged).
//  - Only one target may be installed at a time; installing a *different*
//    new target first uninstalls any previous one.
//  - uninstall() removes the filter and is always safe to call, including
//    when nothing is installed, and including more than once. It also
//    forgets any keys the mapper currently considers held (see
//    InputMapper::clearHeldKeys()): once uninstalled, this router can
//    never observe that key's real release, so continuing to track it as
//    held could otherwise leak into a later install() on a different
//    target.
//  - If the installed target is destroyed without uninstall() being
//    called first, this router notices: installedTarget_ is a
//    QPointer<QObject>, which automatically nulls itself the moment the
//    target is destroyed (no explicit destroyed() connection is used or
//    needed). eventFilter() re-checks installedTarget_.isNull() on every
//    call, so it quietly reverts to "nothing installed" -- it never
//    dispatches a stale/queued event that arrives after that point, and
//    never dereferences the destroyed target.
//  - Destroying the router itself uninstalls it first: no further
//    eventFilter() call (e.g. from an event already queued at the moment
//    of destruction) can ever run any dispatch logic afterwards.
//  - The installed target losing keyboard focus (QEvent::FocusOut) or its
//    window/application becoming inactive (QEvent::WindowDeactivate /
//    QEvent::ApplicationDeactivate) also clears the mapper's held-key
//    state, without dispatching anything: the platform is free to never
//    deliver a matching KeyRelease once focus/activation is lost (e.g.
//    Alt-Tabbing away while a key is held down), so a held key can
//    otherwise stay stuck armed forever, silently swallowing whichever
//    physical key it was the next time it is pressed again. A future
//    controller-disconnect notification (there is no live
//    controller/gamepad input API in this codebase today -- see
//    InputMapper.h) should clear held state through the same seam,
//    InputMapper::clearHeldKeys().
//
// This class dispatches exactly one thing -- a (SemanticCommand,
// CommandPhase) pair via commandDispatched() -- and contains no Arkham
// rules and no virtual-cursor/pointer emulation of any kind.
//
// Event consumption: a key event whose physical key is bound in |mapper|
// (including the three permanently reserved keys) is always consumed
// (this filter returns true), even on a transition InputMapper::
// processKey() suppresses via its dedup rules (a stray duplicate press,
// an auto-repeat before any press, or a stray release) -- letting such a
// transition fall through to default Qt key handling could re-trigger a
// side effect (e.g. Qt's own Tab-key focus-chain handling) for a key this
// router already owns, even though no new command is dispatched for it.
// A key event whose physical key is not bound at all is never consumed.
//
// Text entry: many default bindings (see InputMapper::resetToDefaults())
// are ordinary alphanumeric letters -- W/A/S/D, X, H, P, I, L, Q, E, J,
// Y, C, R, 0, etc. -- chosen to match generic-controller/Steam Input
// conventions. Installed on a real window, this router would otherwise
// unconditionally intercept every one of those keys at the event-filter
// level *before* a focused native/QML text-entry control (TextField,
// TextInput, TextArea, a SpinBox's editable text, ...) ever sees them,
// making it impossible to type ordinary text into any such control while
// this router is installed. isTextEntrySuspended() below is the
// production gate for this: while it reports true, every KeyPress/
// KeyRelease is treated exactly like an event for an unbound key --
// never consumed, never dispatched, regardless of any binding, including
// the three reserved keys -- so a focused text control (and Qt's own
// default key handling generally) behaves exactly as if this router were
// not installed at all. There are two independent, ORed ways this can
// become true; see their own doc comments below for the full contract:
// setSemanticInputSuspended() (an explicit host-driven override) and
// setAutomaticTextEntryDetectionEnabled() (automatic per-event detection
// via Qt's own input-method query mechanism, on by default). Because
// this codebase has no separate device-specific input path (see
// InputMapper.h: generic-controller/Steam Input events arrive as
// ordinary QKeyEvents carrying standard Qt::Key values -- Qt has no
// distinct "gamepad key" enum family -- through this exact same
// filter), suspension applies uniformly to keyboard- and
// controller-sourced events alike -- the simplest safe policy, so a
// controller cannot "type" semantic commands into a focused text field
// either. Reserved Escape/Back/Menu are also suspended: while a text
// control owns focus, this router does not decide what Escape/Back
// means (e.g. discarding entered text vs. dismissing a dialog vs. doing
// nothing) -- that decision is left entirely to the focused control /
// host, exactly as it would be with no InputRouter installed.
class InputRouter final : public QObject {
  Q_OBJECT

public:
  // |mapper| is borrowed by reference and must outlive this router; it is
  // never owned or destroyed by InputRouter.
  explicit InputRouter(InputMapper &mapper, QObject *parent = nullptr);
  ~InputRouter() override;

  // Installs this router as an event filter on |target|. See the class
  // comment for the exact rejection conditions.
  bool install(QObject *target);

  // Removes the currently installed event filter, if any. Always safe to
  // call, any number of times.
  void uninstall();

  [[nodiscard]] bool isInstalled() const;
  [[nodiscard]] QObject *installedTarget() const;

  // True if semantic-command dispatch is currently suspended for every
  // physical key -- see the class comment's "Text entry" section. This
  // is the logical OR of isSemanticInputExplicitlySuspended() and a live
  // automatic-detection check (only performed when
  // isAutomaticTextEntryDetectionEnabled() is true), so it can change
  // from one call to the next purely because focus moved, with no
  // setter having been called at all.
  [[nodiscard]] bool isTextEntrySuspended() const;

  // Explicit host-driven override, independent of automatic detection.
  // Defaults to false (not suspended). Set this true for a text-entry
  // surface automatic detection cannot see on its own (e.g. a plain
  // QObject/QWidget with no Qt input-method-query support), or whenever a
  // host wants full manual control regardless of what is currently
  // focused. Setting this false does not disable automatic detection --
  // isTextEntrySuspended() still ORs both together. Held/armed key state
  // is cleared immediately (without dispatching anything) whenever this
  // call actually changes the *effective* (OR'd) suspended state, so a
  // hold spanning the transition can never later swallow a stray
  // release or silently eat the next unrelated press. Safe to call at
  // any time, including with nothing installed.
  void setSemanticInputSuspended(bool suspended);
  [[nodiscard]] bool isSemanticInputExplicitlySuspended() const;

  // Automatic per-event detection of a focused text-entry control, using
  // Qt's own input-method query mechanism: a QInputMethodQueryEvent
  // requesting Qt::ImEnabled is sent synchronously to whichever focus
  // object Qt itself currently considers focused for input-method
  // purposes, each time eventFilter() considers a KeyPress/KeyRelease.
  // Never reports enabled when nothing is installed (installedTarget()
  // is null): with no target, this router is not filtering any events
  // at all, so there is nothing for automatic detection to protect, and
  // it must not consult some unrelated window's focus object elsewhere
  // in the application. Otherwise, that focus object is
  // installedTarget()'s own QWindow::focusObject() when installedTarget()
  // is a QWindow (for a QQuickWindow target this is the focused
  // QQuickItem, which is not necessarily installedTarget() itself) --
  // this instance-level accessor reflects the window's own Qt Quick
  // scene-graph focus regardless of real platform-level window
  // activation, unlike the static QGuiApplication::focusObject(), which
  // stays null under an unactivated/offscreen window (as in this
  // project's own headless tests, and potentially some
  // embedded/composited real deployments) even though the window's
  // scene genuinely has an active focus item. QGuiApplication::
  // focusObject() is used as a fallback for a non-QWindow
  // installedTarget(), or when the target's own focusObject() is null.
  // A focus object that never overrides input-method handling (an
  // ordinary non-text QQuickItem, or no focus object at all, or no
  // QGuiApplication instance at all -- e.g. under QTEST_GUILESS_MAIN)
  // safely reports "not enabled" rather than suspending. Enabled by
  // default; disable this if a host wants total manual control via
  // setSemanticInputSuspended() alone. Deliberately does NOT depend on
  // any on-screen/virtual-keyboard visibility signal: that is
  // compositor/platform-specific, can lag arbitrarily far behind the
  // actual focus change, and never appears at all on a desktop/
  // Steam-Deck-desktop-mode session with a physical keyboard attached --
  // an unreliable proxy for "the focused control currently wants raw key
  // events routed to it instead of semantic commands." Held/armed key
  // state is cleared immediately whenever this call actually changes the
  // effective suspended state, exactly like setSemanticInputSuspended().
  void setAutomaticTextEntryDetectionEnabled(bool enabled);
  [[nodiscard]] bool isAutomaticTextEntryDetectionEnabled() const;

signals:
  void commandDispatched(Arkham::SemanticCommand command,
                         Arkham::CommandPhase phase);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  // Recomputes the effective (OR'd) suspended state from the current
  // explicit override and (if enabled) a fresh automatic-detection
  // query, and clears held/armed key state iff the result differs from
  // the last-known value -- called from every public setter above (so a
  // host-driven change takes effect immediately) and from eventFilter()
  // itself right before handling each KeyPress/KeyRelease (so a focus
  // change with no setter call still clears stale held state no later
  // than the very next key event).
  void refreshTextEntrySuspension();

  InputMapper &mapper_;
  QPointer<QObject> installedTarget_;
  bool destroying_ = false;
  bool explicitlySuspended_ = false;
  bool automaticDetectionEnabled_ = true;
  bool effectiveSuspended_ = false;
};

} // namespace Arkham
