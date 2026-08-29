#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

namespace Arkham {

// The four cardinal directions a focus move can travel. There is
// deliberately no diagonal direction: the graph's adjacency is always
// explicit per direction (see registerNode), never geometrically derived
// by default.
enum class FocusDirection {
  Up,
  Down,
  Left,
  Right,
};

// Whether moving off the edge of a zone (no explicit neighbor registered
// in the requested direction) wraps around within the same zone -- to
// the next/previous node in that zone's *registration order* (not
// geometric position; there is no coordinate model here) -- or leaves
// focus unchanged.
enum class WrapPolicy {
  NoWrap,
  WrapWithinZone,
};

// One node's explicit adjacency and zone membership. |id| and |zoneId|
// are stable, product-meaningful semantic identifiers (e.g. "hand.card.3",
// "board" or "hand") -- never a pointer, index, or geometric coordinate.
// |neighbors| is the explicit, author-declared adjacency for this node;
// a direction absent from |neighbors| has no explicit neighbor at all
// (resolved only via wrap-around or an optional geometry fallback -- see
// FocusController::setGeometryFallback -- never invented implicitly).
struct FocusNodeSpec {
  QString id;
  QString zoneId;
  QHash<FocusDirection, QString> neighbors;
};

// An opaque, value-comparable capture of everything FocusController needs
// to deterministically restore focus later: the current focus, the full
// modal-return stack, and each zone's last-focused node. Restoring the
// same snapshot any number of times always produces the same result,
// including after the graph has been rebuilt in between (see
// FocusController::restoreSnapshot).
struct FocusSnapshot {
  QString focusedId;
  QVector<QPair<QString, QString>> modalStack; // (modalEntryId, returnToId)
  QHash<QString, QString> zoneLastFocused;

  friend bool operator==(const FocusSnapshot &,
                         const FocusSnapshot &) = default;
};

// Deterministic focus-graph navigation over stable semantic zone/entity
// IDs. This is the *only* thing that ever changes as a result of
// directional-focus/zone-cycle semantic commands; it has no concept of a
// pointer position and never emits one. QML observes currentFocusId() and
// currentZoneChanged()/currentFocusChanged() to drive its own visual
// activeFocus, exactly like any other native Qt focus mechanism.
//
// Adjacency is always explicit (registerNode's |neighbors|), never derived
// from geometry, with one narrow, deliberate exception: an optional
// geometry-fallback callback (see setGeometryFallback) that is only ever
// consulted when (a) the caller has explicitly opted in by installing one,
// and (b) no explicit neighbor and no wrap-around target exist for the
// requested direction. Even then, it only ever *selects a next focus id*;
// it never synthesizes a pointer event or moves a cursor. If no fallback
// is installed, a direction with no explicit neighbor and no eligible
// wrap-around target simply leaves focus unchanged -- this is the default,
// and is what most tests exercise.
class FocusController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(
      QString currentFocusId READ currentFocusId NOTIFY currentFocusChanged)

public:
  explicit FocusController(WrapPolicy wrapPolicy = WrapPolicy::NoWrap,
                           QObject *parent = nullptr);

  void setWrapPolicy(WrapPolicy policy);
  [[nodiscard]] WrapPolicy wrapPolicy() const;

  // A pure function from (currentId, direction, same-zone candidate ids)
  // to the id it considers the deliberate geometric-fallback target, or
  // std::nullopt if none applies. Installing this is fully opt-in (see
  // class comment); passing an empty std::function disables it again.
  using GeometryFallback = std::function<std::optional<QString>(
      const QString &currentId, FocusDirection direction,
      const QVector<QString> &sameZoneCandidateIds)>;
  void setGeometryFallback(GeometryFallback fallback);

  // Registers or replaces |spec|. Replacing an already-registered node id
  // (e.g. re-registering it with different neighbors) deterministically
  // overwrites its previous adjacency/zone -- last registration always
  // wins, never merges -- which is exactly how an explicit-adjacency
  // conflict ("tie") between two registrations for the same id is
  // resolved.
  //
  // Same-zone re-registration (zoneId unchanged): zone last-focused
  // memory is left completely untouched -- if it referred to this id
  // before, it simply continues to refer to the same id, now with its
  // new adjacency; if it referred to some other id, that is unaffected.
  //
  // Cross-zone re-registration (a different zoneId than the node's
  // previous one) has three additional, distinct observable effects:
  //   1. If the previous zone's last-focused memory pointed at this
  //      exact id, that now-stale entry is erased -- the node has moved
  //      away, so the previous zone must stop remembering it as
  //      focused there.
  //   2. If that was the previous zone's last member node, the zone id
  //      is also pruned from the internal zone-cycle order, exactly
  //      like removeNode() below -- re-registering a node into a new
  //      zone must uphold the same "zoneOrder_ never retains an empty
  //      zone" invariant as actually removing it.
  //   3. If this id is the currently-focused node (i.e. it just moved
  //      zones without its identity or focus changing, so
  //      setCurrentFocus()'s own bookkeeping never ran for this move),
  //      the new zone's last-focused memory is set to this id -- so the
  //      new zone's memory reflects the live current focus rather than
  //      staying stale or absent. If the node is not currently focused,
  //      the new zone's last-focused memory is left as-is (no entry is
  //      created for it).
  void registerNode(const FocusNodeSpec &spec);

  // Removes |id| from the graph. If it was the current focus, focus falls
  // back deterministically, trying in order: |fallbackId| (if given and
  // still present after removal), then the removed node's own explicit
  // neighbors in a fixed Up/Down/Left/Right priority order (first one
  // still present after removal wins), then the first remaining node in
  // the same zone (by registration order), then std::nullopt (no node
  // focused) if the zone is now empty. Also purges |id| from the modal
  // stack's return targets (falling back the same way for any modal
  // entry that pointed at it) and from zone-last-focused memory. If this
  // removes the last remaining node of |id|'s zone, that zone id is also
  // pruned from the internal zone-cycle order (see cycleZone) so it no
  // longer takes part in zone cycling and does not grow that order
  // without bound across many dynamically-created/destroyed zones; a
  // later registerNode() for that same zone id treats it as a brand-new
  // zone, appended at the current end of the order.
  void removeNode(const QString &id, const QString &fallbackId = {});

  [[nodiscard]] bool hasNode(const QString &id) const;
  [[nodiscard]] QString zoneOf(const QString &id) const;

  // Sets initial focus directly. Only succeeds (returns true) if |id| is
  // registered and no modal is currently active; used once at startup or
  // whenever the graph is rebuilt from scratch outside of snapshot
  // restoration.
  bool setInitialFocus(const QString &id);

  // Moves focus one step in |direction| from the current focus. Returns
  // true if focus actually changed. No-op (returns false) if there is no
  // current focus, no explicit neighbor, no eligible wrap-around target,
  // and no geometry fallback resolves one.
  bool moveFocus(FocusDirection direction);

  // Cycles to the next/previous zone (by registration order of each
  // zone's first-seen node), restoring that zone's last-focused node if
  // it is still present, otherwise its first node by registration order.
  // This is what drives shoulder-zone switching. No-op if fewer than two
  // zones are registered.
  bool cycleZone(bool forward);

  // Pushes a modal: remembers the current focus as the return target,
  // then focuses |modalEntryId| (which must already be registered).
  // Modals nest: popModal() always returns to exactly the focus that was
  // current immediately before the matching pushModal().
  bool pushModal(const QString &modalEntryId);

  // Pops the innermost modal, restoring the focus that was current
  // immediately before the matching pushModal(). No-op (returns false) if
  // no modal is active.
  bool popModal();

  [[nodiscard]] bool isModalActive() const;
  [[nodiscard]] QString currentFocusId() const;

  // Captures everything needed to deterministically restore focus later.
  // Safe to call repeatedly; each call is a fresh, independent, purely
  // value-based capture with no shared mutable state.
  [[nodiscard]] FocusSnapshot snapshot() const;

  // Restores a previously captured snapshot. Only ids still present in
  // the current graph are actually restored (deterministically ignoring
  // removed/renamed ones); this may be called any number of times,
  // including with graph mutations between calls, and always produces the
  // same result for the same (snapshot, current-graph) pair -- there is
  // no hidden state left over from a previous restoreSnapshot() call that
  // could cause drift.
  void restoreSnapshot(const FocusSnapshot &snapshot);

signals:
  void currentFocusChanged(const QString &id);

private:
  struct Node {
    QString zoneId;
    QHash<FocusDirection, QString> neighbors;
    int registrationOrder = 0;
  };

  QHash<QString, Node> nodes_;
  QVector<QString> zoneOrder_; // first-seen order of each distinct zone id
  QHash<QString, QString> zoneLastFocused_;
  QVector<QPair<QString, QString>> modalStack_; // (modalEntryId, returnToId)
  QString currentFocusId_;
  WrapPolicy wrapPolicy_;
  GeometryFallback geometryFallback_;
  int nextRegistrationOrder_ = 0;

  void setCurrentFocus(const QString &id);
  [[nodiscard]] QVector<QString> nodesInZone(const QString &zoneId) const;
  [[nodiscard]] QString firstRegisteredNodeId() const;
  [[nodiscard]] std::optional<QString>
  resolveRemovalFallback(const QString &removedId, const Node &removedNode,
                         const QString &explicitFallbackId) const;
  // Removes |zoneId| from zoneOrder_ if (and only if) no node currently
  // in |nodes_| belongs to it. Shared by removeNode() (a node's removal
  // may empty its zone) and registerNode() (re-registering an existing
  // node under a *different* zoneId may empty its *old* zone) so both
  // paths uphold the same "zoneOrder_ never retains an empty zone"
  // invariant documented on removeNode().
  void pruneZoneIfEmpty(const QString &zoneId);
};

} // namespace Arkham
