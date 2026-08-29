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
  Node node;
  node.zoneId = spec.zoneId;
  node.neighbors = spec.neighbors;

  // Last registration always wins for content (zone/neighbors), but the
  // original registration order is preserved across re-registration so a
  // node's position in its zone's deterministic ordering (used by
  // wrap-around and cycleZone) never shifts just because it was updated
  // in place.
  const auto existing = nodes_.constFind(spec.id);
  node.registrationOrder = existing != nodes_.constEnd()
                               ? existing.value().registrationOrder
                               : nextRegistrationOrder_++;

  nodes_.insert(spec.id, node);

  if (!zoneOrder_.contains(spec.zoneId)) {
    zoneOrder_.push_back(spec.zoneId);
  }
}

void FocusController::removeNode(const QString &id, const QString &fallbackId) {
  const auto it = nodes_.constFind(id);
  if (it == nodes_.constEnd()) {
    return;
  }
  const Node removedNode = it.value();
  nodes_.remove(id);

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
  QVector<QString> activeZones;
  for (const QString &zoneId : zoneOrder_) {
    if (!nodesInZone(zoneId).isEmpty()) {
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
  emit currentFocusChanged(currentFocusId_);
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
