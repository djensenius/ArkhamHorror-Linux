#pragma once

#include "AssetCache.h"
#include "AssetNetworkFetcher.h"
#include "AssetTypes.h"

#include <QHash>
#include <QObject>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

namespace Arkham {

// Orchestrates one logical asset request end-to-end: resolve candidates
// (AssetLocator), consult the cache (AssetCache), and -- on a miss --
// fetch over the network (AssetNetworkFetcher), advancing through the
// candidate list only on a definitive 404 exactly as AssetLocator
// documents, and never on Transport/RedirectRejected/UnexpectedStatus/
// ResponseTooLarge/ContentTypeMismatch/MagicBytesMismatch/DimensionTooLarge/
// PixelBudgetExceeded/UnsupportedCodec/MalformedImage/Cancelled.
//
// A same-process (memory) cache hit short-circuits the network entirely,
// since it was already validated during this process's own lifetime. A
// disk cache hit whose stored metadata carries an ETag and/or
// Last-Modified is instead conditionally revalidated with exactly those
// headers (AssetNetworkFetcher::ConditionalHeaders) before being served:
// a matching 304 refreshes only lastAccess (AssetCache::
// touchAfterNotModified()) and serves the still-valid stale bytes; a 200
// stores and serves the fresh body; any OTHER outcome (including a 404,
// timeout, or transport failure) fails open and serves the stale cached
// entry as-is ("stale-if-error"), so a flaky or briefly-unreachable origin
// can never make previously-cached art disappear. A disk hit with no
// stored validators at all (e.g. an origin that never sent either header)
// is served immediately with no network round trip, exactly as before.
//
// Every one of the above cache-hit paths can hand back a CachedEntry
// whose decodedImage is null (only encodedBytes/metadata are ever
// persisted to disk, so any entry served from disk -- or from memory
// after being promoted from disk -- never carries a decoded QImage until
// something decodes it). ensureDecoded() (see the .cpp) is applied to
// every one of these paths before a result is ever handed to a consumer,
// so a successful outcome's decodedImage is never null: a fresh
// AssetNetworkFetcher decode is performed on demand, through the exact
// same validated codec/dimension path a live fetch uses, and the result
// is published back into the memory cache (AssetCache::
// updateMemoryDecodedImage()) so later hits for the same key need not
// redecode. A decode failure here (e.g. the installed Qt build no longer
// supports a codec that was available when this entry was originally
// cached) is surfaced as the same typed AssetError a live fetch would
// have produced -- never silently served as a null image.
//
// Concurrent identical requests (same canonicalized AssetKey -- every
// field, including assetBase/category/identifier/side/locale/format,
// compares equal; see canonicalOperationKey() in the .cpp) are coalesced:
// a second request() for a key already in flight attaches as an
// additional consumer of the same underlying operation rather than
// issuing a second network fetch. This is deliberately independent of
// AssetCache::cacheKeyFor(), which hashes a resolved *candidate* URL --
// two AssetKeys that differ (e.g. only in locale) are never coalesced
// here even if they would happen to resolve to the same candidate URL.
// Each consumer receives its own opaque RequestHandle --
// including immediate cache-hit and error completions, which are queued
// through the same operation/consumer bookkeeping specifically so that
// handle remains valid for cancel() up until the queued delivery actually
// runs.
//
// Cancellation/destruction semantics:
//   - cancel(handle) detaches exactly that one consumer. While this
//     coordinator is alive, its own callback is invoked exactly once,
//     with AssetErrorCode::Cancelled, and never again afterwards --
//     but that delivery is itself queued via the event loop (like every
//     other completion path here), not invoked synchronously from
//     cancel() itself, and is suppressed entirely if this coordinator
//     is destroyed before the queued delivery runs (the destructor
//     cancels every in-flight operation without invoking any consumer
//     callback at all).
//   - This holds even after the underlying operation has already
//     completed (successfully or not): the handle stays cancel()-able
//     across the whole window until this consumer's own queued delivery
//     actually runs, at which point cancel() substitutes Cancelled for
//     the real result rather than letting it through -- see
//     m_pendingDeliveryCancelled in the .h for the mechanism.
//   - Other consumers of the same coalesced operation are entirely
//     unaffected by one consumer's cancellation: the underlying fetch
//     keeps running and still completes normally for them.
//   - Only when the LAST remaining consumer of an operation cancels (or
//     the coordinator itself is destroyed) is the underlying
//     AssetNetworkFetcher fetch actually aborted.
//   - A reply belonging to an operation that has since been fully
//     cancelled/superseded can never invoke any consumer's callback: the
//     operation is removed from the pending map before the underlying
//     cancel/abort is issued, exactly mirroring
//     NetworkAuthenticationClient's disconnect-before-abort ordering.
class AssetRequestCoordinator final : public QObject {
  Q_OBJECT
public:
  struct RequestHandle {
    quint64 id{0};
    [[nodiscard]] bool isValid() const noexcept { return id != 0; }
  };

  using ResultCallback =
      std::function<void(AssetOutcome<AssetCache::CachedEntry>)>;

  // Takes non-owning references to a cache and fetcher this coordinator
  // does not own; both must outlive it.
  explicit AssetRequestCoordinator(AssetCache &cache,
                                   AssetNetworkFetcher &fetcher,
                                   QObject *parent = nullptr);
  ~AssetRequestCoordinator() override;

  // Resolves `key`'s candidates, serves from cache when possible, and
  // otherwise fetches -- coalescing with any identical in-flight request
  // for the SAME canonicalized AssetKey (see canonicalOperationKey() in
  // the .cpp; this includes both ordinary fetches and disk-hit
  // revalidations). While this coordinator is alive, `callback` is invoked
  // exactly once (synchronously-deferred via the event loop), even when
  // the request is rejected before any network I/O (e.g.
  // InvalidIdentifier). If this AssetRequestCoordinator is destroyed while
  // the request is still in flight, delivery is suppressed entirely (see
  // the destructor) -- callers must not depend on `callback` firing once
  // destruction is possible.
  RequestHandle request(const AssetKey &key, ResultCallback callback);

  // Detaches this one consumer; see the class comment for the exact
  // coalesced-cancellation semantics. A stale/unknown handle is a no-op.
  void cancel(RequestHandle handle);

  [[nodiscard]] int inFlightOperationCountForTesting() const {
    return m_operations.size();
  }

private:
  struct Consumer {
    quint64 handleId{0};
    ResultCallback callback;
  };

  struct Operation {
    AssetKey key;
    QVector<AssetCandidate> candidates;
    int candidateIndex{0};
    AssetNetworkFetcher::FetchHandle fetchHandle;
    QVector<Consumer> consumers;

    // Set only for a disk-cache-hit revalidation (see
    // startRevalidation()): the previously-cached entry being revalidated,
    // served as-is on any revalidation failure ("stale-if-error") and
    // superseded only by an explicit fresh 200 response.
    bool isRevalidation{false};
    QString revalidationCacheKey;
    std::optional<AssetCache::CachedEntry> staleEntry;
  };

  RequestHandle
  registerImmediateCompletion(const AssetKey &key, ResultCallback callback,
                              AssetOutcome<AssetCache::CachedEntry> result);
  // Finds an already in-flight operation (revalidation or ordinary
  // candidate fetch) whose AssetKey canonicalizes to the same opKey, so a
  // new request() call can attach as an additional consumer instead of
  // starting a redundant network operation. Returns nullopt if none.
  [[nodiscard]] std::optional<quint64>
  findInFlightOperation(const QString &opKey) const;
  // A disk-served (or memory-promoted-from-disk) CachedEntry never
  // carries a decoded QImage: only encodedBytes/metadata are ever
  // persisted to disk (see AssetCache::store()/lookupDisk()). Every path
  // that can hand such an entry to a consumer as a SUCCESSFUL outcome
  // (memory hit, disk hit with no validators, and every "serve the stale
  // entry" branch of a revalidation) must route it through this helper
  // first, so a caller can never observe a successful result whose
  // decodedImage is null. A no-op (returns `entry` unchanged) if it
  // already carries a decoded image (the common case: freshly fetched
  // entries, or an entry already patched by a prior call here via
  // AssetCache::updateMemoryDecodedImage()). `cacheKey` is the exact
  // AssetCache key `entry` was retrieved under, so the freshly-decoded
  // image can be published back into the memory cache for subsequent
  // lookupMemory() hits.
  [[nodiscard]] AssetOutcome<AssetCache::CachedEntry>
  ensureDecoded(AssetCache::CachedEntry entry, AssetFormat format,
                const QString &cacheKey);
  void startCandidate(quint64 operationId);
  void startRevalidation(quint64 operationId);
  void completeOperation(quint64 operationId,
                         AssetOutcome<AssetCache::CachedEntry> result);
  void dispatchToConsumers(Operation &operation,
                           AssetOutcome<AssetCache::CachedEntry> result);

  AssetCache &m_cache;
  AssetNetworkFetcher &m_fetcher;
  quint64 m_nextHandle{1};
  quint64 m_nextOperationId{1};
  QHash<quint64, Operation> m_operations; // operationId -> Operation
  QHash<quint64, quint64>
      m_handleToOperation; // consumer handleId -> operationId
  // A consumer's handle remains valid for cancel() from the moment
  // request() returns it until its queued delivery actually runs (see the
  // class comment) -- which spans TWO queued hops for an immediate
  // completion (the queued completeOperation() call, then the queued
  // per-consumer delivery inside dispatchToConsumers()) and one hop for
  // an ordinary fetch completion. m_handleToOperation only covers the
  // first hop (the handle is removed from it the instant
  // completeOperation() runs, before the per-consumer delivery is even
  // queued). This map covers the remaining window: completeOperation()
  // inserts a fresh (false) flag per consumer here instead of simply
  // forgetting the handle, and each consumer's queued lambda in
  // dispatchToConsumers() consults and erases its own entry exactly when
  // it runs, substituting a Cancelled result if cancel() flipped the flag
  // in the meantime. Without this, cancel() would silently no-op on a
  // handle it could not distinguish from a genuinely stale/already-
  // delivered one, delivering the original (non-cancelled) result anyway.
  QHash<quint64, std::shared_ptr<bool>>
      m_pendingDeliveryCancelled; // consumer handleId -> cancelled flag
};

} // namespace Arkham
