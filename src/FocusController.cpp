#include "FocusController.h"

#include <algorithm>
#include <array>
#include <climits>

namespace Arkham {

FocusController::FocusController(const WrapPolicy wrapPolicy, QObject *parent)
    : QObject(parent), wrapPolicy_(wrapPolicy) {}

void FocusController::setWrapPolicy(const WrapPolicy policy) {
  wrapPolicy_ = policy;
}

WrapPolicy FocusController::wrapPolicy() const { return wrapPolicy_; }

void FocusController::setGeometryFallback(GeometryFallback fallback) {
  geometryFallback_ = std::move(fallback);
}

void FocusController::registerNode(const FocusNodeSpec &spec) {
  const auto existing = nodes_.constFind(spec.id);
  const bool wasRegistered = existing != nodes_.constEnd();
  const QString previousZoneId =
      wasRegistered ? existing.value().zoneId : QString();

  Node node;
  node.zoneId = spec.zoneId;
  node.neighbors = spec.neighbors;

  // Last registration always wins for content (zone/neighbors), but the
  // original registration order is preserved across re-registration so a
  // node's position in its zone's deterministic ordering (used by
  // wrap-around and cycleZone) never shifts just because it was updated
  // in place.
  node.registrationOrder = wasRegistered ? existing.value().registrationOrder
                                         : nextRegistrationOrder_++;

  nodes_.insert(spec.id, node);

  if (!zoneOrder_.contains(spec.zoneId)) {
    zoneOrder_.push_back(spec.zoneId);
  }

  // If this re-registration moved the node out of a different previous
  // zone, that old zone may now be empty; prune it just like removeNode()
  // does, so zoneOrder_ never retains a zone with no member nodes
  // regardless of whether it emptied via removal or via re-registration
  // into a new zone.
  if (wasRegistered && previousZoneId != spec.zoneId) {
    pruneZoneIfEmpty(previousZoneId);
  }
}

void FocusController::removeNode(const QString &id, const QString &fallbackId) {
  const auto it = nodes_.constFind(id);
  if (it == nodes_.constEnd()) {
    return;
  }
  const Node removedNode = it.value();
  nodes_.remove(id);

  // Prune the zone from zoneOrder_ once it has no remaining member node,
  // so a long session that dynamically creates and destroys many
  // transient zone ids (e.g. per-scenario/per-investigator zones) does
  // not grow this vector without bound. If the same zone id is
  // registered again later, registerNode() simply treats it as a
  // brand-new zone appended at the current end of zoneOrder_ -- exactly
  // like any other zone id seen for the first time -- rather than trying
  // to reinsert it at its old position.
  pruneZoneIfEmpty(removedNode.zoneId);

  for (auto zoneIt = zoneLastFocused_.begin();
       zoneIt != zoneLastFocused_.end();) {
    if (zoneIt.value() == id) {
      zoneIt = zoneLastFocused_.erase(zoneIt);
    } else {
      ++zoneIt;
    }
  }

  for (auto &entry : modalStack_) {
    if (entry.second == id) {
      entry.second = resolveRemovalFallback(id, removedNode, fallbackId)
                         .value_or(QString());
    }
  }
  // Deliberately *not* touched above: entry.first (a modal level's own
  // entry node). popModal() only ever reads entry.second -- the return
  // target, which is kept valid by the loop above -- so a stale
  // entry.first left behind here is inert: it is never dereferenced
  // against |nodes_| again during live operation, and correctly gates
  // nested unwinding one level at a time regardless of whether that
  // level's own node still exists. The only place entry.first is later
  // revalidated against the graph is restoreSnapshot(), which discards
  // any level whose entry no longer exists at *that* point in time, on
  // a copy of this stack it was independently given.

  if (currentFocusId_ == id) {
    setCurrentFocus(resolveRemovalFallback(id, removedNode, fallbackId)
                        .value_or(QString()));
  }
}

bool FocusController::hasNode(const QString &id) const {
  return nodes_.contains(id);
}

QString FocusController::zoneOf(const QString &id) const {
  const auto it = nodes_.constFind(id);
  return it == nodes_.constEnd() ? QString() : it.value().zoneId;
}

bool FocusController::setInitialFocus(const QString &id) {
  if (isModalActive() || !nodes_.contains(id)) {
    return false;
  }
  setCurrentFocus(id);
  return true;
}

bool FocusController::moveFocus(const FocusDirection direction) {
  if (currentFocusId_.isEmpty()) {
    return false;
  }
  const auto currentIt = nodes_.constFind(currentFocusId_);
  if (currentIt == nodes_.constEnd()) {
    return false;
  }
  const Node &current = currentIt.value();

  const auto neighborIt = current.neighbors.constFind(direction);
  if (neighborIt != current.neighbors.constEnd() &&
      nodes_.contains(*neighborIt)) {
    setCurrentFocus(*neighborIt);
    return true;
  }

  // Only pay for scanning/sorting the whole zone when a feature that
  // actually needs the zone's node list is enabled; a plain neighborless
  // move with no wrap and no geometry fallback is just a no-op.
  const bool needsZoneNodes =
      wrapPolicy_ == WrapPolicy::WrapWithinZone || geometryFallback_;
  const QVector<QString> zoneNodes =
      needsZoneNodes ? nodesInZone(current.zoneId) : QVector<QString>{};

  if (wrapPolicy_ == WrapPolicy::WrapWithinZone && zoneNodes.size() > 1) {
    const qsizetype idx = zoneNodes.indexOf(currentFocusId_);
    if (idx >= 0) {
      qsizetype nextIdx = idx;
      switch (direction) {
      case FocusDirection::Right:
      case FocusDirection::Down:
        nextIdx = (idx + 1) % zoneNodes.size();
        break;
      case FocusDirection::Left:
      case FocusDirection::Up:
        nextIdx = (idx - 1 + zoneNodes.size()) % zoneNodes.size();
        break;
      }
      if (zoneNodes[nextIdx] != currentFocusId_) {
        setCurrentFocus(zoneNodes[nextIdx]);
        return true;
      }
    }
  }

  if (geometryFallback_) {
    QVector<QString> candidates = zoneNodes;
    candidates.removeAll(currentFocusId_);
    const std::optional<QString> fallbackTarget =
        geometryFallback_(currentFocusId_, direction, candidates);
    if (fallbackTarget.has_value() && nodes_.contains(*fallbackTarget)) {
      setCurrentFocus(*fallbackTarget);
      return true;
    }
  }

  return false;
}

bool FocusController::cycleZone(const bool forward) {
  // Determine which zones currently have at least one node with a
  // single O(N) scan over nodes_ (collecting into a set), rather than
  // calling nodesInZone() -- which itself scans *and* sorts all of
  // nodes_ -- once per registered zone (that would be O(Z * N log N)
  // just to find the active zones).
  QSet<QString> zonesWithNodes;
  zonesWithNodes.reserve(static_cast<qsizetype>(nodes_.size()));
  for (auto it = nodes_.constBegin(); it != nodes_.constEnd(); ++it) {
    zonesWithNodes.insert(it.value().zoneId);
  }
  QVector<QString> activeZones;
  for (const QString &zoneId : zoneOrder_) {
    if (zonesWithNodes.contains(zoneId)) {
      activeZones.push_back(zoneId);
    }
  }
  if (activeZones.size() < 2) {
    return false;
  }

  const QString currentZone =
      currentFocusId_.isEmpty() ? QString() : zoneOf(currentFocusId_);
  qsizetype idx = activeZones.indexOf(currentZone);
  if (idx < 0) {
    // No current zone (e.g. no current focus, or the current zone is no
    // longer active): position just before the first zone for a forward
    // cycle, and just after the last zone for a backward cycle, so the
    // very first cycle deterministically lands on a real first/last
    // zone instead of skipping over one.
    idx = forward ? -1 : activeZones.size();
  }
  const qsizetype size = activeZones.size();
  const qsizetype nextIdx =
      forward ? (idx + 1) % size : (idx - 1 + size) % size;
  const QString &targetZone = activeZones[nextIdx];

  const QString remembered = zoneLastFocused_.value(targetZone);
  QString target;
  if (!remembered.isEmpty() && nodes_.contains(remembered) &&
      zoneOf(remembered) == targetZone) {
    target = remembered;
  } else {
    const QVector<QString> zoneNodes = nodesInZone(targetZone);
    if (zoneNodes.isEmpty()) {
      return false;
    }
    target = zoneNodes.first();
  }

  setCurrentFocus(target);
  return true;
}

bool FocusController::pushModal(const QString &modalEntryId) {
  if (!nodes_.contains(modalEntryId)) {
    return false;
  }
  modalStack_.push_back({modalEntryId, currentFocusId_});
  setCurrentFocus(modalEntryId);
  return true;
}

bool FocusController::popModal() {
  if (modalStack_.isEmpty()) {
    return false;
  }
  const auto entry = modalStack_.takeLast();
  const QString &returnToId = entry.second;
  if (!returnToId.isEmpty() && nodes_.contains(returnToId)) {
    setCurrentFocus(returnToId);
  } else {
    setCurrentFocus(firstRegisteredNodeId());
  }
  return true;
}

bool FocusController::isModalActive() const { return !modalStack_.isEmpty(); }

QString FocusController::currentFocusId() const { return currentFocusId_; }

FocusSnapshot FocusController::snapshot() const {
  FocusSnapshot snap;
  snap.focusedId = currentFocusId_;
  snap.modalStack = modalStack_;
  snap.zoneLastFocused = zoneLastFocused_;
  return snap;
}

void FocusController::restoreSnapshot(const FocusSnapshot &snapshot) {
  // Every field below is fully recomputed from |snapshot| and the current
  // graph -- nothing here incrementally merges with whatever state
  // existed before this call -- so calling this repeatedly (with graph
  // mutations in between, or not) never drifts: the same (snapshot,
  // current-graph) pair always produces the same result.
  modalStack_.clear();
  for (const auto &entry : snapshot.modalStack) {
    if (!nodes_.contains(entry.first)) {
      // The modal's own entry point no longer exists: collapse that
      // level entirely rather than leaving a dangling modal.
      continue;
    }
    const QString returnTo =
        nodes_.contains(entry.second) ? entry.second : firstRegisteredNodeId();
    modalStack_.push_back({entry.first, returnTo});
  }

  zoneLastFocused_.clear();
  for (auto it = snapshot.zoneLastFocused.constBegin();
       it != snapshot.zoneLastFocused.constEnd(); ++it) {
    if (nodes_.contains(it.value()) && zoneOf(it.value()) == it.key()) {
      zoneLastFocused_.insert(it.key(), it.value());
    }
  }

  QString target = snapshot.focusedId;
  if (target.isEmpty() || !nodes_.contains(target)) {
    if (!modalStack_.isEmpty() && nodes_.contains(modalStack_.last().first)) {
      target = modalStack_.last().first;
    } else {
      target = firstRegisteredNodeId();
    }
  }
  // Write the target's zone memory directly, rather than relying on
  // setCurrentFocus()'s own side effect for it: setCurrentFocus() no-ops
  // entirely (including that side effect) when |target| already equals
  // the *live* currentFocusId_ from before this call, which would
  // otherwise make the result of restoreSnapshot() depend on whatever
  // focus happened to be current beforehand -- rather than being a pure
  // function of (snapshot, current graph) alone, as documented above and
  // on the class comment. This keeps repeated restoration of the same
  // snapshot fully deterministic regardless of what was focused first.
  if (!target.isEmpty()) {
    const QString targetZone = zoneOf(target);
    if (!targetZone.isEmpty()) {
      zoneLastFocused_.insert(targetZone, target);
    }
  }
  setCurrentFocus(target);
}

void FocusController::setCurrentFocus(const QString &id) {
  if (currentFocusId_ == id) {
    return;
  }
  currentFocusId_ = id;
  if (!id.isEmpty()) {
    const QString zone = zoneOf(id);
    if (!zone.isEmpty()) {
      zoneLastFocused_.insert(zone, id);
    }
  }
  // Emit a local copy, not the live member: a direct-connected slot that
  // reenters (e.g. calls moveFocus()/setCurrentFocus() again before this
  // emit's other connected slots have all run) would otherwise mutate
  // currentFocusId_ out from under this emit, making every slot -- even
  // ones invoked *before* the reentrant call -- observe the final,
  // already-mutated value instead of the value that was actually current
  // for their own invocation.
  const QString emittedId = currentFocusId_;
  emit currentFocusChanged(emittedId);
}

void FocusController::pruneZoneIfEmpty(const QString &zoneId) {
  const bool zoneStillHasNodes = std::any_of(
      nodes_.constBegin(), nodes_.constEnd(),
      [&zoneId](const Node &node) { return node.zoneId == zoneId; });
  if (!zoneStillHasNodes) {
    zoneOrder_.removeAll(zoneId);
    // Also forget this zone's last-focused memory: cycleZone() already
    // double-checks zoneOf(remembered) == targetZone before trusting a
    // remembered id (so a stale entry here could never actually be
    // *used* for the wrong zone), but leaving it behind regardless would
    // let zoneLastFocused_ grow without bound across many dynamically
    // emptied/re-emptied zones, exactly the unbounded-growth problem
    // zoneOrder_ pruning above already guards against.
    zoneLastFocused_.remove(zoneId);
  }
}

QVector<QString> FocusController::nodesInZone(const QString &zoneId) const {
  QVector<QPair<int, QString>> ordered;
  for (auto it = nodes_.constBegin(); it != nodes_.constEnd(); ++it) {
    if (it.value().zoneId == zoneId) {
      ordered.push_back({it.value().registrationOrder, it.key()});
    }
  }
  std::sort(
      ordered.begin(), ordered.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

  QVector<QString> ids;
  ids.reserve(ordered.size());
  for (const auto &entry : ordered) {
    ids.push_back(entry.second);
  }
  return ids;
}

QString FocusController::firstRegisteredNodeId() const {
  QString best;
  int bestOrder = INT_MAX;
  for (auto it = nodes_.constBegin(); it != nodes_.constEnd(); ++it) {
    if (it.value().registrationOrder < bestOrder) {
      bestOrder = it.value().registrationOrder;
      best = it.key();
    }
  }
  return best;
}

std::optional<QString> FocusController::resolveRemovalFallback(
    const QString &removedId, const Node &removedNode,
    const QString &explicitFallbackId) const {
  Q_UNUSED(removedId);

  if (!explicitFallbackId.isEmpty() && nodes_.contains(explicitFallbackId)) {
    return explicitFallbackId;
  }

  static constexpr std::array<FocusDirection, 4> priorityOrder{
      FocusDirection::Up, FocusDirection::Down, FocusDirection::Left,
      FocusDirection::Right};
  for (const FocusDirection direction : priorityOrder) {
    const auto neighborIt = removedNode.neighbors.constFind(direction);
    if (neighborIt != removedNode.neighbors.constEnd() &&
        nodes_.contains(*neighborIt)) {
      return *neighborIt;
    }
  }

  const QVector<QString> remainingInZone = nodesInZone(removedNode.zoneId);
  if (!remainingInZone.isEmpty()) {
    return remainingInZone.first();
  }

  return std::nullopt;
}

} // namespace Arkham
