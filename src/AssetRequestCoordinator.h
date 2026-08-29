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
// Concurrent identical requests (same cache key, derived from the SAME
// resolved candidate -- see AssetCache::cacheKeyFor()) are coalesced: a
// second request() for a key already in flight attaches as an additional
// consumer of the same underlying operation rather than issuing a second
// network fetch. Each consumer receives its own opaque RequestHandle --
// including immediate cache-hit and error completions, which are queued
// through the same operation/consumer bookkeeping specifically so that
// handle remains valid for cancel() up until the queued delivery actually
// runs.
//
// Cancellation/destruction semantics:
//   - cancel(handle) detaches exactly that one consumer. Its own callback
//     is invoked exactly once, with AssetErrorCode::Cancelled, and never
//     again afterwards.
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
  // for the SAME first candidate's cache key. `callback` is always invoked
  // exactly once (synchronously-deferred via the event loop), even when
  // the request is rejected before any network I/O (e.g. InvalidIdentifier).
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
};

} // namespace Arkham
