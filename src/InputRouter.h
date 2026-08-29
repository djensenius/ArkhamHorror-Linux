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
//    when nothing is installed, and including more than once.
//  - If the installed target is destroyed without uninstall() being
//    called first, this router notices (via QPointer/destroyed()) and
//    quietly reverts to "nothing installed" -- it never dispatches a
//    stale/queued event that arrives after that point, and never
//    dereferences the destroyed target.
//  - Destroying the router itself uninstalls it first: no further
//    eventFilter() call (e.g. from an event already queued at the moment
//    of destruction) can ever run any dispatch logic afterwards.
//
// This class dispatches exactly one thing -- a (SemanticCommand,
// CommandPhase) pair via commandDispatched() -- and contains no Arkham
// rules and no virtual-cursor/pointer emulation of any kind.
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

signals:
  void commandDispatched(Arkham::SemanticCommand command,
                         Arkham::CommandPhase phase);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  InputMapper &mapper_;
  QPointer<QObject> installedTarget_;
  bool destroying_ = false;
};

} // namespace Arkham
