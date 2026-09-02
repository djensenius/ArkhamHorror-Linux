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
// resolved candidate URL (see AssetCache::snapshotAndIssueGeneration()/
// recordNegative404() -- this record now lives in the shared AssetCache
// authority, not this coordinator, so a same-root sibling's fresher
// success or invalidation is instantly visible to it; see the class-wide
// "shared root authority" note further below) -- recorded only when a
// real fetch or revalidation for that exact candidate received a
// definitive 404, NEVER for a timeout, TLS failure, cancellation, 5xx, or
// any integrity/codec failure (those are transient or ambiguous, not
// proof of absence). This record is memory-only (never persisted to
// disk, so it cannot outlive the process(es) sharing this root) and
// scoped per-candidate (never generalized to a whole identifier or
// logical key); it is cleared if that exact candidate is later observed
// to succeed (defensive: a resource that once 404'd could, in principle,
// reappear).
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
// Cross-logical-key races on a shared cache key (review item 6): because
// coalescing above is keyed on the whole logical AssetKey rather than the
// resolved candidate/cache key, two DIFFERENT AssetKeys that happen to
// resolve to the SAME candidate URL (e.g. an explicit "en" locale and an
// unset/unresolvable locale, both falling back to the same English
// candidate) are NOT coalesced into a single network operation and can
// genuinely run concurrently, each with its own independent fetch or
// revalidation in flight against the identical AssetCache key. Actually
// merging their transport is a larger change (deliberately descoped here
// given delivery constraints -- see the .cpp), but the correctness
// consequence of NOT merging it is fully closed off: every asynchronous
// mutation of a given cache key (a fresh 200 store(), a 304's
// touchAfterNotModified()/memory promotion, or a definitive 404's
// invalidate()+negative-404 record) is guarded by a per-cache-key
// optimistic-concurrency scheme (see currentCacheKeyGeneration()/
// issueCacheKeyGeneration()/tryApplyCacheKeyMutation() in the .cpp):
// every asynchronous operation mints its own strictly-increasing
// ISSUANCE value at the moment it starts (or, for a cache hit, at the
// moment of the hit) -- never merely reading whatever generation happens
// to be currently applied -- and a mutation is actually allowed to apply
// only if no STRICTLY NEWER issuance has already applied one first. A
// mismatch means some OTHER, more recently ISSUED operation has already
// mutated this exact cache key, so this stale callback's mutation is
// silently skipped -- its OWN consumers still receive the outcome it
// genuinely observed (the bytes it fetched, or the fact that its
// candidate really did 404, are both correct for them), it simply never
// gets to publish that outcome into the shared cache/negative-404 state
// that a newer operation has already superseded. Critically, this orders
// mutations by ISSUANCE, not by whichever operation's disk read or
// network fetch happens to COMPLETE first: a slower-to-complete OLDER
// operation can never overwrite a faster-to-complete NEWER one, and vice
// versa a NEWER operation is never blocked merely because an older one
// happened to finish first. This is what prevents a delayed 200 from
// overwriting a newer 200, a delayed 304 from touching or memory-
// promoting a stale body over a newer entry, and a delayed 404 from
// evicting or negative-caching a cache key a newer operation has since
// populated with a genuinely fresh success.
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

  // Review item 9: true for exactly the ensureDecoded()/decodeAndValidate()
  // failure codes that mean the CACHED BYTES THEMSELVES are now known bad
  // against a fresh, current-limits re-check (wrong magic/content-type,
  // failed decode, or exceeding the currently-configured dimension/pixel
  // caps) -- as opposed to UnsupportedCodec, which means the bytes are
  // still perfectly valid but this process currently has no installed
  // decoder for them (e.g. libavif built without a usable AV1 codec
  // backend); those valid bytes must never be quarantined/deleted, since
  // a future process (or later in this same process's life) could still
  // decode them successfully. Exposed publicly (it is a pure, stateless
  // classification with no side effects) specifically so this exact
  // classification can be tested directly and deterministically,
  // independent of whatever byte sequence would be needed to organically
  // provoke each AssetErrorCode through a real decode attempt -- see
  // AssetRequestCoordinatorTests::unsupportedCodecIsNeverQuarantineWorthy().
  [[nodiscard]] static bool isQuarantineWorthy(AssetErrorCode code);

  [[nodiscard]] int inFlightOperationCountForTesting() const {
    return m_operations.size();
  }

  // Round-6 item 8: the number of DISTINCT shared candidate-fetch
  // attempts currently in flight -- lets a test assert that N concurrent
  // requests for DIFFERENT logical AssetKeys resolving to the SAME
  // candidate genuinely share exactly one attempt (this count stays 1
  // regardless of N) rather than merely trusting the coalescing
  // happened.
  [[nodiscard]] int candidateAttemptCountForTesting() const {
    return m_candidateAttempts.size();
  }
  // Round-6 item 8: the number of operations currently subscribed to the
  // shared attempt for `cacheKey` (0 if no such attempt exists), so a
  // test can assert every expected subscriber actually joined the same
  // attempt rather than each starting its own.
  [[nodiscard]] int
  candidateAttemptSubscriberCountForTesting(const QString &cacheKey) const {
    for (auto it = m_candidateAttempts.cbegin();
         it != m_candidateAttempts.cend(); ++it) {
      if (it.value().cacheKey == cacheKey) {
        return it.value().subscriberOperationIds.size();
      }
    }
    return 0;
  }

  // Review round-4 item 7: observable size of the coordinator-local
  // per-cache-key maps (m_cacheKeyGeneration, m_cacheKeyIssuedGeneration)
  // that pruneStaleCacheKeyState() bounds -- see its declaration comment
  // for the pruning contract. Test-only; lets a high-cardinality test
  // assert these maps stay bounded (proportional to currently-in-flight
  // cache keys) rather than growing without limit for the coordinator's
  // entire process lifetime.
  //
  // Cumulative review (independent re-review, HIGH, "negative 404 is
  // coordinator-local and can hide sibling-populated cache"): the
  // negative-404 tombstone record itself no longer lives here at all --
  // see AssetCache::NegativeCacheRecord's own comment -- so
  // negative404RecordCountForTesting()/hasNegative404ForTesting() below
  // now delegate to the shared m_cache instead of a coordinator-local
  // map.
  [[nodiscard]] int negative404RecordCountForTesting() const {
    return m_cache.negative404RecordCountForTesting();
  }
  [[nodiscard]] int cacheKeyGenerationStateCountForTesting() const {
    return m_cacheKeyGeneration.size() + m_cacheKeyIssuedGeneration.size();
  }
  // Test-only exposure of the shared negative-404 record's
  // authoritativeness for a specific cache key, independent of the
  // aggregate count above.
  [[nodiscard]] bool hasNegative404ForTesting(const QString &cacheKey) const {
    return m_cache.hasNegative404ForTesting(cacheKey, m_monotonicNowMs());
  }

  // Round-7/8 item 7: the number of DISTINCT shared cache-hit decode
  // groups (see PendingCacheDecode's declaration comment) currently
  // waiting on their single queued decode -- lets a test assert N
  // concurrent cache-hit requests for aliased logical keys resolving to
  // the same (cacheKey, format) genuinely share one pending group (this
  // count stays 1 regardless of N) before its queued decode actually
  // runs.
  [[nodiscard]] int pendingCacheDecodeGroupCountForTesting() const {
    return m_pendingCacheDecodes.size();
  }
  // The number of waiters currently attached to the pending decode group
  // for `cacheKey`/`format` (0 if no such group exists), so a test can
  // assert every expected aliased request actually joined the same
  // group rather than each scheduling its own independent decode.
  [[nodiscard]] int
  pendingCacheDecodeWaiterCountForTesting(const QString &cacheKey,
                                          AssetFormat format) const {
    return m_pendingCacheDecodes
        .value(cacheDecodeCoalescingKey(cacheKey, format))
        .waiters.size();
  }
  // Round-7/8 item 7: the number of times ensureDecoded() has actually
  // performed a REAL decode (i.e. entry.decodedImage was null on entry,
  // so m_fetcher.decodeAndValidate() genuinely ran) since this
  // coordinator was constructed -- as opposed to its already-decoded
  // short-circuit. Lets a test assert a burst of coalesced requests
  // sharing one cached entry genuinely decoded it exactly once, rather
  // than merely trusting the pending-decode-group bookkeeping above
  // implies it.
  [[nodiscard]] int realDecodeCallCountForTesting() const {
    return m_realDecodeCallCountForTesting;
  }

  // Review round-4 item 7: force an immediate prune (production code
  // only ever prunes opportunistically, at the end of
  // completeOperation()) so a test can assert the post-prune bound
  // deterministically without depending on exactly how many operations
  // happen to have completed.
  void pruneStaleCacheKeyStateForTesting() { pruneStaleCacheKeyState(); }

  // Review round-4 item 7: override the production
  // kMaxTrackedNegative404Entries ceiling so a high-cardinality test can
  // exercise the hard-cap eviction path (as opposed to lazy TTL-expiry
  // pruning) with a small, fast number of distinct candidates instead of
  // needing thousands of real network round trips to reach the
  // production-sized cap. Test-only; production code always uses the
  // compiled-in default.
  //
  // Cumulative review (independent re-review, HIGH, "negative 404 is
  // coordinator-local and can hide sibling-populated cache"): forwards
  // to the shared m_cache, which now owns this ceiling -- see
  // AssetCache::setMaxTrackedNegative404EntriesForTesting()'s own
  // comment.
  void setMaxTrackedNegative404EntriesForTesting(int maxEntries) {
    m_cache.setMaxTrackedNegative404EntriesForTesting(maxEntries);
  }

  // Review round-3 item 13: replaces the real monotonic (steady_clock)
  // clock used to expire negative-404 records with a caller-supplied
  // function, so tests can deterministically simulate TTL expiry without
  // a real sleep and without any sensitivity to wall-clock changes. Test-
  // only; production code never calls this.
  using MonotonicNowFn = std::function<qint64()>;
  void setMonotonicNowForTesting(MonotonicNowFn nowFn) {
    m_monotonicNowMs = std::move(nowFn);
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

    // Round-6 item 8: non-empty exactly while this operation is a
    // subscriber of a shared m_candidateAttempts entry (see
    // candidateAttemptKey()'s comment) -- i.e. its current network fetch
    // or revalidation is coalesced with one or more OTHER operations
    // (necessarily different logical AssetKeys; same-AssetKey coalescing
    // is handled entirely separately by findInFlightOperation()) that
    // resolved to the identical candidate cache key/format/validator
    // snapshot. cancel()'s last-consumer branch consults this to decide
    // whether it may actually abort fetchHandle (only once no attempt
    // subscriber remains) or must merely unsubscribe and leave the
    // shared fetch running for the other subscriber(s).
    QString pendingAttemptKey;
  };

  // Cumulative review (independent re-review, HIGH, "negative 404 is
  // coordinator-local and can hide sibling-populated cache"): the
  // negative-404 record itself (lookup/write/clear) now lives entirely
  // in the shared AssetCache authority -- see AssetCache::
  // NegativeCacheRecord's own comment and snapshotAndIssueGeneration()/
  // recordNegative404()/clearNegative404() there for the full contract
  // (still memory-only, per-candidate, 404-only, cleared on a later
  // success, bounded by kNegative404TtlMs) -- so no coordinator-local
  // lookup/write/clear method remains here at all.

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
  // `expectedGeneration` is currentCacheKeyGeneration(cacheKey) captured
  // synchronously at the moment of the cache hit in request() -- see the
  // class comment's "Cross-logical-key races" paragraph -- and is
  // threaded through to the eventual completeCacheReadOrQuarantine() call
  // so a quarantine discovered there can be CAS-gated against whatever
  // may have mutated this cache key during the queued-delivery hop.
  // `assetCacheGeneration`: see completeCacheReadOrQuarantine()'s own
  // comment -- the separate, AssetCache-level token, minted via
  // AssetCache::issueKeyGeneration() at the exact same moment as
  // `expectedGeneration`, protecting against a same-root SIBLING
  // instance's concurrent invalidate() rather than merely another
  // operation in this same coordinator.
  RequestHandle
  registerCacheHitCompletion(const AssetKey &key, ResultCallback callback,
                             QVector<AssetCandidate> candidates,
                             int candidateIndex, AssetCache::CachedEntry entry,
                             QString cacheKey, quint64 expectedGeneration,
                             quint64 assetCacheGeneration);
  // Finds an already in-flight operation (revalidation or ordinary
  // candidate fetch) whose AssetKey canonicalizes to the same opKey, so a
  // new request() call can attach as an additional consumer instead of
  // starting a redundant network operation. Returns nullopt if none.
  [[nodiscard]] std::optional<quint64>
  findInFlightOperation(const QString &opKey) const;

  // Round-6 item 8 ("coalescing by logical AssetKey duplicates
  // network/decode for aliases resolving to same candidate/cache key"):
  // findInFlightOperation() above only coalesces two request() calls
  // whose full AssetKey canonicalizes identically -- it deliberately
  // never catches two DIFFERENT logical AssetKeys (e.g. an explicit "en"
  // locale and an unset one, both falling back to the same English
  // candidate) that happen to resolve to the SAME candidate URL. Each
  // such Operation previously issued its own independent
  // AssetNetworkFetcher::fetch() call for that candidate -- a real
  // network round trip AND a real image decode duplicated once per
  // distinct logical key, even though only one is ever needed.
  //
  // A CandidateAttempt is the shared unit of transport for exactly one
  // (cacheKey, format, conditional-validator-snapshot) combination:
  // whichever Operation reaches startCandidate()/startRevalidation()
  // FIRST for that combination creates it (minting the one shared
  // issuedGeneration value -- see issueCacheKeyGeneration()'s comment --
  // every subscriber's eventual CAS-guarded mutation is checked against);
  // every subsequent Operation that reaches the identical combination
  // while it is still in flight simply appends its operationId to
  // subscriberOperationIds instead of calling m_fetcher.fetch() again.
  // When the single shared fetch completes, the attempt is immediately
  // erased from m_candidateAttempts (so a later request for the same
  // combination starts a genuinely fresh attempt, never appends to one
  // that already fired) and the CAS-guarded cache/negative-404 mutation
  // is applied AT MOST ONCE for the whole group -- then EVERY subscriber
  // is independently dispatched through its own fallback/completion logic
  // (advance to its own next candidate on a 404, quarantine its own
  // stale entry, complete successfully, etc.) via
  // dispatchCandidateFetchResult()/dispatchRevalidationResult() below.
  // This preserves each subscriber's fully independent
  // fallback/cancellation semantics -- only the underlying HTTP
  // request/decode and the cache mutation are ever shared, never a
  // subscriber's own candidate list, index, or per-consumer cancellation.
  struct CandidateAttempt {
    QString cacheKey;
    quint64 issuedGeneration{0};
    // Cumulative review (independent re-review, HIGH, "shared root
    // authority incomplete", "store has no token"): the SEPARATE,
    // AssetCache-level token for this attempt -- see
    // completeCacheReadOrQuarantine()'s comment for the full contract
    // distinguishing it from `issuedGeneration` above. Minted via
    // AssetCache::issueKeyGeneration() at the exact same moment
    // `issuedGeneration` is minted (see startCandidate()/
    // startRevalidation()), and threaded unchanged through
    // dispatchCandidateFetchResult()/dispatchRevalidationResult() to
    // every store()/touchAfterNotModified() call this attempt's
    // completion may make.
    quint64 assetCacheGeneration{0};
    AssetNetworkFetcher::FetchHandle fetchHandle;
    bool isRevalidation{false};
    QVector<quint64> subscriberOperationIds;
    // Round-9+ review (HIGH): an immutable, monotonic, NEVER-reused
    // identity for THIS specific attempt object, minted once at
    // creation (see m_nextCandidateAttemptToken below) and captured BY
    // VALUE in every completion lambda that closes over this attempt.
    // m_candidateAttempts is keyed by a STRING (candidateAttemptKey()),
    // and that string key is deliberately reused across time -- when a
    // sole subscriber cancels, unsubscribeFromCandidateAttempt() erases
    // the map entry so a fresh request for the identical
    // (cacheKey, format, validators) starts a genuinely new attempt
    // under the SAME key. AssetNetworkFetcher::cancel() does not
    // suppress its own attempt's eventual callback -- it still fires,
    // asynchronously, with AssetErrorCode::Cancelled -- so if that new
    // attempt is created and itself completes BEFORE the old,
    // now-orphaned callback finally runs, the old callback's
    // `m_candidateAttempts.find(attemptKey)` lookup would find the NEW
    // attempt (matching by string key only) and could erase/corrupt it
    // using the OLD, stale/cancelled result. Checking `token` for exact
    // equality before any erase/dispatch closes this completely and
    // unconditionally: a callback whose captured token no longer
    // matches the live map entry's token (including "no entry at all")
    // is uniformly treated as a safe, silent no-op, regardless of what
    // numeric values m_cacheKeyIssuedGeneration happens to hold by the
    // time it runs.
    quint64 token{0};
  };
  // Identifies a CandidateAttempt: distinct cache keys, formats, or
  // conditional-validator snapshots (an unconditional first-try fetch
  // uses empty etag/lastModified and is thus never coalesced with a
  // revalidation of a DIFFERENT stale entry's validators, even for the
  // same cache key) never share one. `format` is included because
  // AssetLocator always resolves one canonical format per candidate, but
  // this keeps the key precise even if that were ever to change.
  [[nodiscard]] static QString candidateAttemptKey(const QString &cacheKey,
                                                   AssetFormat format,
                                                   const QString &etag,
                                                   const QString &lastModified);
  // Applies the CAS-guarded mutation for `cacheKey`/`issuedGeneration`
  // exactly once for `result` (a completed, UNCONDITIONAL fetch outcome),
  // then dispatches every operationId in `subscribers` still present in
  // m_operations through the exact per-operation continuation
  // startCandidate()'s callback used to run inline for a single
  // operation (advance candidates on a definitive 404, complete on
  // success/other error) -- see startCandidate()'s comment.
  // `assetCacheGeneration`: see CandidateAttempt::assetCacheGeneration's
  // comment -- threaded unchanged to the store() call this may make.
  void dispatchCandidateFetchResult(
      const QString &cacheKey, quint64 issuedGeneration,
      quint64 assetCacheGeneration, const QVector<quint64> &subscribers,
      AssetOutcome<AssetNetworkFetcher::ConditionalFetchResult> result);
  // Identical role for a completed CONDITIONAL (revalidation) fetch
  // outcome -- see startRevalidation()'s comment for the full set of
  // per-subscriber branches (404-advance, stale-if-error,
  // 304-touch-and-promote, fresh-200-replace), each still applied
  // independently per subscriber using THAT subscriber's own staleEntry/
  // candidateIndex even though the network round trip was shared.
  // `assetCacheGeneration`: see CandidateAttempt::assetCacheGeneration's
  // comment -- threaded unchanged to whichever of touchAfterNotModified()/
  // store() this may make.
  void dispatchRevalidationResult(
      const QString &cacheKey, quint64 issuedGeneration,
      quint64 assetCacheGeneration, const QVector<quint64> &subscribers,
      AssetOutcome<AssetNetworkFetcher::ConditionalFetchResult> result);
  // Round-6 item 8: removes `operationId` from whichever CandidateAttempt
  // it is currently subscribed to (a no-op if `pendingAttemptKey` is
  // empty, i.e. this operation is not -- or is no longer -- coalesced).
  // Returns the attempt's fetchHandle IFF `operationId` was the last
  // remaining subscriber (in which case the attempt is also erased from
  // m_candidateAttempts) so the caller can actually abort the now-
  // orphaned shared fetch; returns an invalid handle otherwise, meaning
  // the shared fetch must keep running unabated for the remaining
  // subscriber(s). Called from cancel()'s last-consumer branch and from
  // the coordinator's destructor-adjacent cleanup paths.
  [[nodiscard]] AssetNetworkFetcher::FetchHandle
  unsubscribeFromCandidateAttempt(quint64 operationId,
                                  const QString &attemptKey);
  // A disk-served (or memory-promoted-from-disk) CachedEntry never
  // carries a decoded QImage: only encodedBytes/metadata are ever
  // persisted to disk (see AssetCache::store()/lookupDisk()). Every path
  // that can hand such an entry to a consumer as a SUCCESSFUL outcome
  // (memory hit, disk hit with no validators, and every "serve the stale
  // entry" branch of a revalidation) must route it through this helper
  // first, so a caller can never observe a successful result whose
  // decodedImage is null. A no-op (returns `entry` unchanged) if it
  // already carries a decoded image (the common case: freshly fetched
  // entries, or an entry already patched by completeCacheReadOrQuarantine()
  // via AssetCache::updateMemoryDecodedImage()).
  //
  // Review round-3 item 15: deliberately SIDE-EFFECT FREE -- this must
  // never itself call AssetCache::updateMemoryDecodedImage() (or any
  // other cache mutation), because the entry it is decoding may already
  // be stale relative to the cache key by the time this runs (e.g. an
  // older, slower-to-complete disk read racing a newer operation that
  // has already published a fresh generation for the same key -- see the
  // class comment's "Cross-logical-key races" paragraph). Publishing a
  // decode's side effects unconditionally here could patch a NEWER live
  // memory entry's decodedImage with pixels decoded from OLDER encoded
  // bytes. The caller (completeCacheReadOrQuarantine()) is the only place
  // that has both the decoded result AND the CAS generation check needed
  // to decide whether it is still safe to publish it.
  [[nodiscard]] AssetOutcome<AssetCache::CachedEntry>
  ensureDecoded(AssetCache::CachedEntry entry, AssetFormat format);
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
  // success, this method republishes the decoded image back into the
  // memory cache (AssetCache::updateMemoryDecodedImage()) and,
  // additionally when `promoteOnSuccess` is set, publishes the whole
  // entry via AssetCache::promoteToMemory() -- only needed by the
  // post-304-revalidation caller, whose entry was deliberately withheld
  // from memory promotion until this exact revalidation happened (see
  // AssetCache::lookupDisk()'s comment). Review round-3 item 15: BOTH of
  // these publishes are gated by the exact same `expectedGeneration` CAS
  // check, so a decode that raced a newer operation's publish can never
  // mutate the now-current memory entry with stale pixels.
  // `expectedGeneration`: see the class comment's "Cross-logical-key
  // races" paragraph. Gates BOTH the promoteOnSuccess memory promotion and
  // the quarantine-worthy invalidate() below against a cache key already
  // superseded by a more recently issued operation -- either one applied
  // to a now-stale view of this cache key could otherwise resurrect or
  // destroy state a newer operation already established.
  // `assetCacheGeneration`: cumulative review (independent re-review,
  // HIGH, "shared root authority incomplete", "store has no token") --
  // the SEPARATE, AssetCache-level token minted via
  // AssetCache::issueKeyGeneration() at the exact same moment
  // `expectedGeneration` was minted (see the call sites), threaded
  // through unchanged to whichever of promoteToMemory()/
  // updateMemoryDecodedImage() this call may go on to make. This is
  // deliberately a SECOND, independent token from `expectedGeneration`:
  // `expectedGeneration`/tryApplyCacheKeyMutation() only ever protects
  // against a race with ANOTHER OPERATION IN THIS SAME COORDINATOR
  // INSTANCE, while `assetCacheGeneration` protects against a race with
  // ANY same-root AssetCache SIBLING INSTANCE (including ones owned by a
  // different AssetRequestCoordinator entirely) -- see AssetCache::
  // issueKeyGeneration()'s own comment for that layer's full contract.
  void completeCacheReadOrQuarantine(quint64 operationId,
                                     AssetCache::CachedEntry entry,
                                     const QString &cacheKey,
                                     quint64 expectedGeneration,
                                     quint64 assetCacheGeneration,
                                     bool promoteOnSuccess);

  // Round-7/8 item 7 (MEDIUM, "cache-hit read/decode occurs before
  // operation coalescing"): candidateAttemptKey()/CandidateAttempt above
  // already coalesce the NETWORK round trip for aliased logical keys
  // resolving to the same candidate -- but a decode driven by a CACHE
  // HIT (memory or disk, including every stale-served/304/revalidation
  // branch) previously had no equivalent: each Operation independently
  // called ensureDecoded() -- a full, potentially near-32-megapixel
  // image decode -- even when N simultaneous requests (e.g. several QML
  // Image elements for the same card, differing only in a locale that
  // happens to fall back to the same English candidate) resolve to the
  // exact same (cacheKey, format) pair at the exact same moment. A
  // GroupWaiter is one Operation's stake in a shared decode: its own
  // operationId (so a waiter cancelled before delivery is silently
  // skipped -- see completeCacheReadGroupOrQuarantine()'s comment) and
  // its own independently-minted expectedGeneration (see
  // issueCacheKeyGeneration()'s comment) -- CAS-gated exactly as if it
  // had decoded independently, so the final applied generation and any
  // quarantine/invalidate/retry decision is bit-for-bit identical to
  // what fully independent processing in request order would have
  // produced.
  struct GroupWaiter {
    quint64 operationId{0};
    quint64 expectedGeneration{0};
    // Cumulative review (independent re-review, HIGH, "shared root
    // authority incomplete"): the separate, AssetCache-level token --
    // see completeCacheReadOrQuarantine()'s comment -- minted
    // independently by EACH waiter at its own cache-hit moment (see
    // request()'s two lookupMemory()/lookupDisk() call sites), exactly
    // mirroring expectedGeneration's own per-waiter nature: two waiters
    // joining the same decode group may still have observed the shared
    // cache key at genuinely different moments, so each carries its own
    // token, applied independently (in registration order) by
    // completeCacheReadGroupOrQuarantine()'s per-waiter loop.
    quint64 assetCacheGeneration{0};
  };
  // One (cacheKey, format) pair's currently in-flight, not-yet-decoded
  // shared cache read: `entry` is the bytes exactly one Operation (the
  // "leader") looked up and captured at registration time; every LATER
  // registerCacheHitCompletion() call for the identical (cacheKey,
  // format) pair, made before this group's single queued decode actually
  // runs, joins as an additional waiter instead of scheduling its own
  // independent decode -- see completeCoalescedCacheDecode()'s comment.
  struct PendingCacheDecode {
    AssetCache::CachedEntry entry;
    QString cacheKey;
    AssetFormat format{AssetFormat::Png};
    QVector<GroupWaiter> waiters;
    // Independent cumulative re-review (HIGH, repeat finding, "supersession
    // uses highest currently outstanding... Maintain monotonic
    // latestIssued watermark independent of outstanding set"): the
    // single AssetCache-level token this WHOLE group currently uses for
    // its eventual, shared CAS/apply attempt -- the numerically HIGHEST
    // of every member's own independently-minted token seen so far
    // (initially the leader's, at registration; advanced -- never
    // regressed -- as later joiners with a still-higher token arrive;
    // see registerCacheHitCompletion()'s own comment). Every member's own
    // token is minted via snapshotAndIssueGeneration()'s internal,
    // non-committing mint, so a later joiner's own higher-numbered token
    // never, by itself, poisons AssetCache::
    // latestCommittedGenerationLocked()'s shared ceiling against the
    // group's earlier representative (see that method's own comment in
    // AssetCache.cpp for the full contract) -- adopting the highest is
    // therefore not required to avoid spurious rejection by the join
    // itself, but remains strictly safer and free: the group's eventual
    // tryApplyKeyGenerationLocked() check is a simple `>=` comparison, so
    // publishing with the freshest observed token can only ever make
    // that check MORE likely to still succeed if some genuinely
    // separate, actually-committed writer for this exact key advanced
    // the shared ceiling in the interim. Every waiter's own
    // GroupWaiter::assetCacheGeneration is authoritatively re-synced to
    // this exact value immediately before delivery -- see
    // completeCoalescedCacheDecode()'s own comment -- since an earlier
    // waiter's own stored copy can otherwise still reflect a value this
    // field has since moved past.
    quint64 assetCacheGeneration{0};
  };
  // Identifies a PendingCacheDecode group. Distinct from
  // candidateAttemptKey() (which additionally distinguishes conditional-
  // validator snapshots for network-level coalescing): a cache-hit decode
  // has no validators of its own to distinguish -- the SAME already-
  // cached bytes are what every waiter in a group shares.
  [[nodiscard]] static QString cacheDecodeCoalescingKey(const QString &cacheKey,
                                                        AssetFormat format);
  // Decodes `entry` against `expectedFormat` via ensureDecoded() exactly
  // ONCE (regardless of how many waiters share it -- see
  // ensureDecoded()'s comment for why this is safe: it is deliberately
  // side-effect-free, and the resulting QImage is implicitly shared, so
  // copying the outcome into each waiter's own completion below is O(1),
  // never a pixel-buffer duplication), then applies the outcome to every
  // waiter independently and in order:
  //   - success: each waiter's own CAS-gated memory publish/promotion,
  //     exactly as completeCacheReadOrQuarantine() already did for a
  //     single operation -- processed in ascending-generation (i.e.
  //     registration) order so the FINAL applied generation matches
  //     whatever fully independent per-operation processing would have
  //     left behind.
  //   - a non-quarantine-worthy failure: each waiter's own completion
  //     with that same failure, unchanged.
  //   - a quarantine-worthy failure: AssetCache::invalidate(cacheKey) is
  //     called EXACTLY ONCE for the whole group (never once per waiter),
  //     the moment the FIRST waiter whose own CAS still applies is
  //     found; every waiter whose CAS applies then independently retries
  //     its OWN candidate via startCandidate() (which itself coalesces
  //     with any sibling waiter's retry at the network layer via
  //     candidateAttemptKey() -- see the class comment), so the group as
  //     a whole performs one invalidate and, in practice, one shared
  //     refetch, never N of either.
  // A waiter already cancelled (absent from m_operations) by the time
  // this runs -- see cancel()'s comment -- is silently skipped and never
  // participates in the CAS/invalidate/retry decision at all: one
  // consumer cancelling never affects any sibling waiter sharing this
  // same decode.
  void completeCacheReadGroupOrQuarantine(const QVector<GroupWaiter> &waiters,
                                          AssetCache::CachedEntry entry,
                                          const QString &cacheKey,
                                          AssetFormat expectedFormat,
                                          bool promoteOnSuccess);
  // Queued entry point for a PendingCacheDecode group's single shared
  // decode: looks up (and removes) `decodeKey`'s current entry in
  // m_pendingCacheDecodes -- capturing its final waiter list, which may
  // have grown since the leader's registerCacheHitCompletion() call
  // scheduled this closure, if any sibling requests joined in the
  // meantime -- then defers to completeCacheReadGroupOrQuarantine(). A
  // missing entry (the group was already fully pruned away by
  // pruneCancelledPendingCacheDecodeWaiter() below, or -- defensive only
  // -- some other unexpected path) is a silent no-op: the decode this
  // closure was queued for simply never happens.
  void completeCoalescedCacheDecode(const QString &decodeKey);
  // Round-9+ review item 3/7 ("fully cancelled PendingCacheDecode groups
  // retained and still decode"): cancel() calls this for every
  // operationId whose LAST consumer just left (i.e. the Operation itself
  // is being removed from m_operations), so a waiter that will never be
  // delivered to can never keep a shared decode group alive -- and, in
  // particular, a group whose EVERY waiter cancels before its queued
  // completeCoalescedCacheDecode() closure actually runs is erased
  // entirely here, so that closure's later no-op (see its own comment)
  // means the underlying near-32-megapixel decode never happens at all.
  // A linear scan over m_pendingCacheDecodes (never over m_operations)
  // is deliberate and cheap: this map's size is bounded by the number of
  // distinct (cacheKey, format) pairs with a currently in-flight shared
  // decode, not by the number of aliased waiters or historical requests.
  void pruneCancelledPendingCacheDecodeWaiter(quint64 operationId);

  // Per-cache-key optimistic-concurrency counter (review item 6); see the
  // class comment's "Cross-logical-key races" paragraph. A cache key
  // never observed before implicitly starts at generation 0. This is the
  // "last successfully APPLIED" watermark -- see tryApplyCacheKeyMutation()
  // -- never itself minted directly by an issuing call site.
  [[nodiscard]] quint64
  currentCacheKeyGeneration(const QString &cacheKey) const;
  // Review round-3 item 14: mints and returns a NEW, strictly-increasing
  // per-cache-key ISSUANCE sequence number. Called exactly once per
  // operation, at the moment that operation's disk read or network
  // fetch/revalidation for `cacheKey` is actually ISSUED (never at
  // completion time) -- see tryApplyCacheKeyMutation()'s comment for why
  // this must be a distinct counter from the "applied" one
  // currentCacheKeyGeneration() reads.
  [[nodiscard]] quint64 issueCacheKeyGeneration(const QString &cacheKey);
  // Review round-3 item 14: the single CAS gate every cache/negative-404
  // mutation for `cacheKey` must go through, replacing the old "review
  // item 6" equality check (`currentCacheKeyGeneration(cacheKey) ==
  // expectedGeneration`) that ordered mutations by COMPLETION order
  // rather than ISSUANCE order. `issuedGeneration` is the value a call
  // site captured from issueCacheKeyGeneration() when its operation was
  // issued. Succeeds -- applying `issuedGeneration` as the new current
  // generation and returning true -- iff no STRICTLY NEWER issuance has
  // already been applied, i.e. `issuedGeneration >=
  // currentCacheKeyGeneration(cacheKey)`. Deliberately ">=", not ">":
  // several mutations belonging to the SAME operation (e.g. a 304's
  // recency touch immediately followed by completeCacheReadOrQuarantine()'s
  // own promotion) share one issuedGeneration value and must all be able
  // to apply in sequence, while an OLDER operation's issuedGeneration can
  // never "catch up" past whatever a genuinely newer operation already
  // applied. The net effect: a slower-to-complete OLDER operation can
  // never overwrite state a faster-to-complete NEWER operation already
  // published, and a faster NEWER operation is never blocked merely
  // because an older one happened to complete first -- ordering is by
  // ISSUANCE, never by completion race, regardless of which of two
  // concurrent operations' disk reads or network fetches happens to
  // finish first.
  [[nodiscard]] bool tryApplyCacheKeyMutation(const QString &cacheKey,
                                              quint64 issuedGeneration);
  // Review round-4 item 7: the exact set of cache keys some CURRENTLY
  // in-flight operation might still complete against right now -- i.e.
  // every distinct AssetCache::cacheKeyFor() value that could still be
  // the `cacheKey` argument of a future tryApplyCacheKeyMutation() call
  // reachable from an operation already present in m_operations. One
  // entry per operation: its current candidate's resolved cache key (or
  // revalidationCacheKey while revalidating).
  // pruneStaleCacheKeyState() treats every key in this set as pinned --
  // never erased from m_cacheKeyGeneration/m_cacheKeyIssuedGeneration --
  // regardless of generation state.
  [[nodiscard]] QSet<QString> activeInFlightCacheKeys() const;
  // Review round-4 item 7: bounds m_cacheKeyGeneration/
  // m_cacheKeyIssuedGeneration, which previously retained one entry per
  // DISTINCT cache key ever observed for this coordinator's entire
  // process lifetime (a long play session touching thousands of distinct
  // card arts would grow both without limit). Called opportunistically
  // at the end of every completeOperation() -- the natural point at which
  // an operation stops being "in-flight" and its cache key becomes a
  // pruning candidate.
  //
  // Cumulative review (independent re-review, HIGH, "negative 404 is
  // coordinator-local and can hide sibling-populated cache"): the
  // negative-404 record itself no longer lives in this coordinator at
  // all (see AssetCache::recordNegative404()'s own bounded-pruning/
  // hard-cap enforcement), so a key is erased from
  // m_cacheKeyGeneration/m_cacheKeyIssuedGeneration purely once it is
  // not in activeInFlightCacheKeys() -- nothing else pins it.
  //
  // Safety argument ("operation-owned epoch" survives pruning): every
  // completion lambda that could ever call tryApplyCacheKeyMutation()
  // for a cache key FIRST looks its own operationId up in m_operations
  // and returns immediately if it is missing (see completeOperation(),
  // which erases the operation the instant it completes, before any
  // further async work). Consequently, once ALL operations referencing
  // a cache key have completed and been erased -- exactly the condition
  // activeInFlightCacheKeys() excludes -- no future completion can ever
  // reach these maps for that key again, so resetting its generation
  // counters back to the "never seen" (0) baseline is indistinguishable
  // from having never touched that key at all. A key remaining active is
  // never pruned, so an in-flight operation's own eventual CAS check
  // always sees its true, unbroken generation history.
  void pruneStaleCacheKeyState();
  // Review round-3 item 12: the ONE path every candidate-list transition
  // (the very first look at an untried candidate, and every subsequent
  // advance past a definitive 404/quarantine) routes through, so a
  // localized-then-404 candidate can never bypass an already-cached
  // lower-priority candidate (e.g. an English fallback) that a divergent
  // ad hoc "just jump straight to the network for the next index" path
  // could otherwise skip. Walks `operation.candidates` forward from its
  // CURRENT candidateIndex exactly like request()'s own initial scan
  // (negative-404 skip, memory hit, disk hit with/without validators,
  // first genuinely untried candidate falls through to startCandidate()),
  // operating on an EXISTING operationId (never registers a new
  // consumer/operation -- see registerCacheHitCompletion() for that).
  // Completes the operation with NotFound if every remaining candidate is
  // exhausted or authoritatively negative-cached.
  void advanceCandidates(quint64 operationId);
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
  // Round-9+ review (HIGH): see CandidateAttempt::token's comment.
  // Strictly increasing for the entire lifetime of this coordinator,
  // never reused even across many cancel/replace cycles for the same
  // cache key -- this is the ONLY property the token-equality check
  // actually depends on.
  quint64 m_nextCandidateAttemptToken{1};
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

  // Review round-3 item 13: kNegative404TtlMs -- see
  // AssetCache::recordNegative404()'s comment for the full expiry
  // contract this TTL parameterizes. Cumulative review (independent
  // re-review, HIGH, "negative 404 is coordinator-local and can hide
  // sibling-populated cache"): the negative-404 RECORD itself (and its
  // hard cap) now lives entirely in the shared AssetCache authority --
  // see AssetCache::NegativeCacheRecord's own comment -- so only this
  // TTL constant (a pure duration, not state) remains here; it is
  // passed to AssetCache::recordNegative404() at every call site.
  static constexpr qint64 kNegative404TtlMs = 5 * 60 * 1000; // 5 minutes
  // Review round-3 item 13: the monotonic clock negative-404 expiry is
  // measured against -- steady_clock in production (see the .cpp),
  // replaceable via setMonotonicNowForTesting() above. Deliberately NOT
  // wall-clock time: a system clock adjustment/rollback must never
  // resurrect or prematurely expire a negative-404 record.
  MonotonicNowFn m_monotonicNowMs;

  // Per-cache-key optimistic-concurrency APPLIED-generation watermark
  // (review item 6, refined by round-3 item 14); see
  // currentCacheKeyGeneration()/tryApplyCacheKeyMutation() and the class
  // comment's "Cross-logical-key races" paragraph. Memory-only, scoped
  // to THIS coordinator instance only (deliberately distinct from the
  // shared AssetCache-level generation used for cross-instance/sibling
  // protection -- see completeCacheReadOrQuarantine()'s comment for why
  // both layers are needed).
  QHash<QString, quint64> m_cacheKeyGeneration;
  // Review round-3 item 14: per-cache-key ISSUANCE counter -- see
  // issueCacheKeyGeneration()'s comment. Deliberately a SEPARATE counter
  // from m_cacheKeyGeneration (which tracks what has actually been
  // APPLIED): every operation issued against a cache key mints a new,
  // strictly-increasing value here at issue time, independent of
  // whichever operation's mutation eventually gets applied or when.
  QHash<QString, quint64> m_cacheKeyIssuedGeneration;

  // Round-6 item 8: see CandidateAttempt's declaration comment above.
  // Memory-only, like every other in-flight-operation-scoped map here --
  // an attempt only ever exists between the moment its first subscriber
  // issues the shared fetch and the moment that fetch completes (or every
  // subscriber cancels, unsubscribing it back down to empty).
  QHash<QString, CandidateAttempt> m_candidateAttempts;

  // Round-7/8 item 7: see PendingCacheDecode's declaration comment above.
  // Memory-only and inherently transient/bounded, like m_candidateAttempts:
  // an entry only ever exists between a leader's registerCacheHitCompletion()
  // call and the moment its single queued completeCoalescedCacheDecode()
  // call actually runs (at most one Qt event-loop hop later), never
  // persisting across that -- so unlike the shared negative-404 record
  // (AssetCache::NegativeCacheRecord) it needs no separate TTL/pruning
  // logic.
  QHash<QString, PendingCacheDecode> m_pendingCacheDecodes;
  // Round-7/8 item 7: see realDecodeCallCountForTesting()'s comment.
  int m_realDecodeCallCountForTesting = 0;
};

} // namespace Arkham
