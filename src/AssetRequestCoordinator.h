#pragma once

#include "AssetCache.h"
#include "AssetNetworkFetcher.h"
#include "AssetTypes.h"

#include <QHash>
#include <QObject>
#include <QSet>
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
// Candidate priority order is enforced STRICTLY, including when consulting
// the cache: a lower-priority candidate (e.g. the English fallback) that
// happens to already be cached can never be served ahead of a
// higher-priority candidate (e.g. a localized variant) that has not yet
// been tried at all. The ONLY thing that authorizes skipping an untried
// candidate is an exact, authoritative negative-404 record for that exact
// resolved candidate URL (see hasNegative404()/recordNegative404() in the
// .cpp) -- recorded only when a real fetch or revalidation for that exact
// candidate received a definitive 404, NEVER for a timeout, TLS failure,
// cancellation, 5xx, or any integrity/codec failure (those are transient
// or ambiguous, not proof of absence). This record is memory-only (never
// persisted to disk, so it cannot outlive this process) and scoped
// per-candidate (never generalized to a whole identifier or logical key);
// it is cleared if that exact candidate is later observed to succeed
// (defensive: a resource that once 404'd could, in principle, reappear).
// A revalidation (see startRevalidation()) that itself receives a 404
// applies the identical rule: the previously-cached entry is evicted (see
// AssetCache::invalidate()), a negative record is written, and the
// request advances to the NEXT candidate exactly as a first-time miss
// would -- this is the one revalidation outcome that does NOT fall back
// to "stale-if-error"; every other revalidation failure (transport,
// timeout, TLS, 5xx, cancellation, integrity/codec) still serves the
// still-valid stale cached entry unchanged.
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

  // Authoritative negative-404 record lookup/write for one exact
  // resolved-candidate cache key -- see the class comment above for the
  // full contract (memory-only, per-candidate, 404-only, cleared on a
  // later success).
  [[nodiscard]] bool hasNegative404(const QString &cacheKey) const;
  void recordNegative404(const QString &cacheKey);
  void clearNegative404(const QString &cacheKey);

  RequestHandle
  registerImmediateCompletion(const AssetKey &key, ResultCallback callback,
                              AssetOutcome<AssetCache::CachedEntry> result);
  // Registers a real Operation (with the full candidate vector and the
  // exact index the cache hit was found at, exactly like the ordinary
  // network-fetch path) for a cache-hit result that still needs
  // ensureDecoded() to run, then defers to completeCacheReadOrQuarantine()
  // via a queued invocation -- see that method's comment for why a cache
  // hit cannot simply be resolved synchronously via
  // registerImmediateCompletion() (review item 9: a decode/integrity
  // failure discovered here must be able to quarantine the entry and
  // retry the SAME candidate over the network, which registerImmediate
  // Completion's fixed pre-computed AssetOutcome cannot express).
  RequestHandle registerCacheHitCompletion(const AssetKey &key,
                                           ResultCallback callback,
                                           QVector<AssetCandidate> candidates,
                                           int candidateIndex,
                                           AssetCache::CachedEntry entry,
                                           QString cacheKey);
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
  // Review item 9: true for exactly the ensureDecoded()/decodeAndValidate()
  // failure codes that mean the CACHED BYTES THEMSELVES are now known bad
  // against a fresh, current-limits re-check (wrong magic/content-type,
  // failed decode, or exceeding the currently-configured dimension/pixel
  // caps) -- as opposed to UnsupportedCodec, which means the bytes are
  // still perfectly valid but this process currently has no installed
  // decoder for them (e.g. an AVIF plugin not present); those valid bytes
  // must never be quarantined/deleted, since a future process (or later
  // in this same process's life, if plugins are ever discovered late)
  // could still decode them successfully.
  [[nodiscard]] static bool isQuarantineWorthy(AssetErrorCode code);
  // Shared by every code path that hands a CACHE-SOURCED entry (never a
  // freshly-fetched one, which always already carries a decoded image --
  // see ensureDecoded()'s comment) to a consumer: runs ensureDecoded(),
  // and on a quarantine-worthy failure (see isQuarantineWorthy()), evicts
  // BOTH the memory and disk generation for `cacheKey` via
  // AssetCache::invalidate() and retries the SAME candidate
  // (operation.candidates[operation.candidateIndex]) as a genuine network
  // miss via startCandidate() -- exactly once: startCandidate()'s own
  // fetch-completion handler does not call back into this method, so a
  // decode failure on the freshly-refetched bytes (if any) completes via
  // the ordinary network-fetch-failure path instead of looping. Any other
  // failure (including UnsupportedCodec) completes the operation with
  // that error, unchanged, without touching the cache entry at all. On
  // success, `promoteOnSuccess` additionally publishes the now-decoded
  // entry into the memory cache via AssetCache::promoteToMemory() -- only
  // needed by the post-304-revalidation caller, whose entry was
  // deliberately withheld from memory promotion until this exact
  // revalidation happened (see AssetCache::lookupDisk()'s comment); the
  // plain cache-hit callers pass promoteOnSuccess=false because
  // ensureDecoded() already republishes the decoded image in place via
  // AssetCache::updateMemoryDecodedImage() for an entry already resident
  // in memory.
  void completeCacheReadOrQuarantine(quint64 operationId,
                                     AssetCache::CachedEntry entry,
                                     const QString &cacheKey,
                                     bool promoteOnSuccess);
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

  // Cache keys (see AssetCache::cacheKeyFor()) for which an exact,
  // authoritative 404 has been observed by THIS process; never
  // persisted to disk. See the class comment for the full contract.
  QSet<QString> m_negative404;
};

} // namespace Arkham
