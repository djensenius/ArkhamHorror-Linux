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

  // Review round-4 item 7: observable sizes of the three per-cache-key
  // maps (m_negative404, m_cacheKeyGeneration, m_cacheKeyIssuedGeneration)
  // that pruneStaleCacheKeyState() bounds -- see its declaration comment
  // for the pruning contract. Test-only; lets a high-cardinality test
  // assert these maps stay bounded (proportional to currently-in-flight
  // + recently-active cache keys) rather than growing without limit for
  // the coordinator's entire process lifetime.
  [[nodiscard]] int negative404RecordCountForTesting() const {
    return m_negative404.size();
  }
  [[nodiscard]] int cacheKeyGenerationStateCountForTesting() const {
    return m_cacheKeyGeneration.size() + m_cacheKeyIssuedGeneration.size();
  }
  // Test-only exposure of the private hasNegative404() predicate itself,
  // for asserting a specific cache key's record was (or was not) evicted
  // by pruning, independent of the aggregate counts above.
  [[nodiscard]] bool hasNegative404ForTesting(const QString &cacheKey) const {
    return hasNegative404(cacheKey);
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
  // compiled-in default (see the constructor).
  void setMaxTrackedNegative404EntriesForTesting(int maxEntries) {
    m_maxTrackedNegative404Entries = maxEntries;
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

  // Authoritative negative-404 record lookup/write for one exact
  // resolved-candidate cache key -- see the class comment above for the
  // full contract (memory-only, per-candidate, 404-only, cleared on a
  // later success). Review round-3 item 13: NEVER permanent -- bounded by
  // kNegative404TtlMs, and additionally invalidated the instant
  // `cacheKey`'s applied generation (see tryApplyCacheKeyMutation()) moves
  // past the value the record was written under (a later success or a
  // later negative recording both count, including via a completely
  // different in-flight operation). `generation` is the exact issuance
  // value (see issueCacheKeyGeneration()) that was just successfully
  // applied via tryApplyCacheKeyMutation() when this 404 was recorded.
  [[nodiscard]] bool hasNegative404(const QString &cacheKey) const;
  void recordNegative404(const QString &cacheKey, quint64 generation);
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
  // `expectedGeneration` is currentCacheKeyGeneration(cacheKey) captured
  // synchronously at the moment of the cache hit in request() -- see the
  // class comment's "Cross-logical-key races" paragraph -- and is
  // threaded through to the eventual completeCacheReadOrQuarantine() call
  // so a quarantine discovered there can be CAS-gated against whatever
  // may have mutated this cache key during the queued-delivery hop.
  RequestHandle
  registerCacheHitCompletion(const AssetKey &key, ResultCallback callback,
                             QVector<AssetCandidate> candidates,
                             int candidateIndex, AssetCache::CachedEntry entry,
                             QString cacheKey, quint64 expectedGeneration);
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
    AssetNetworkFetcher::FetchHandle fetchHandle;
    bool isRevalidation{false};
    QVector<quint64> subscriberOperationIds;
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
  void dispatchCandidateFetchResult(
      const QString &cacheKey, quint64 issuedGeneration,
      const QVector<quint64> &subscribers,
      AssetOutcome<AssetNetworkFetcher::ConditionalFetchResult> result);
  // Identical role for a completed CONDITIONAL (revalidation) fetch
  // outcome -- see startRevalidation()'s comment for the full set of
  // per-subscriber branches (404-advance, stale-if-error,
  // 304-touch-and-promote, fresh-200-replace), each still applied
  // independently per subscriber using THAT subscriber's own staleEntry/
  // candidateIndex even though the network round trip was shared.
  void dispatchRevalidationResult(
      const QString &cacheKey, quint64 issuedGeneration,
      const QVector<quint64> &subscribers,
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
  void completeCacheReadOrQuarantine(quint64 operationId,
                                     AssetCache::CachedEntry entry,
                                     const QString &cacheKey,
                                     quint64 expectedGeneration,
                                     bool promoteOnSuccess);
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
  // the `cacheKey` argument of a future tryApplyCacheKeyMutation()/
  // recordNegative404() call reachable from an operation already present
  // in m_operations. One entry per operation: its current candidate's
  // resolved cache key (or revalidationCacheKey while revalidating).
  // pruneStaleCacheKeyState() treats every key in this set as pinned --
  // never erased from m_negative404/m_cacheKeyGeneration/
  // m_cacheKeyIssuedGeneration -- regardless of TTL/generation state.
  [[nodiscard]] QSet<QString> activeInFlightCacheKeys() const;
  // Review round-4 item 7: bounds m_negative404/m_cacheKeyGeneration/
  // m_cacheKeyIssuedGeneration, which previously retained one entry per
  // DISTINCT cache key ever observed for this coordinator's entire
  // process lifetime (a long play session touching thousands of distinct
  // card arts would grow all three without limit). Called opportunistically
  // at the end of every completeOperation() -- the natural point at which
  // an operation stops being "in-flight" and its cache key becomes a
  // pruning candidate.
  //
  // A key is erased from m_negative404 once its record is expired or
  // superseded (mirrors hasNegative404()'s own "no longer authoritative"
  // condition, just swept here instead of left in place indefinitely)
  // AND it is not in activeInFlightCacheKeys(). A key is erased from
  // m_cacheKeyGeneration/m_cacheKeyIssuedGeneration once it is not in
  // activeInFlightCacheKeys() AND no (still-valid) m_negative404 record
  // for it survives the sweep above.
  //
  // Safety argument ("operation-owned epoch" survives pruning): every
  // completion lambda that could ever call tryApplyCacheKeyMutation()/
  // recordNegative404() for a cache key FIRST looks its own operationId
  // up in m_operations and returns immediately if it is missing (see
  // completeOperation(), which erases the operation the instant it
  // completes, before any further async work). Consequently, once ALL
  // operations referencing a cache key have completed and been erased --
  // exactly the condition activeInFlightCacheKeys() excludes -- no future
  // completion can ever reach these maps for that key again, so resetting
  // its generation counters back to the "never seen" (0) baseline is
  // indistinguishable from having never touched that key at all. A key
  // remaining active is never pruned, so an in-flight operation's own
  // eventual CAS check always sees its true, unbroken generation history.
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

  // Review round-3 item 13: a bounded (kNegative404TtlMs), generation-
  // scoped negative-404 record -- see hasNegative404()'s comment for the
  // full expiry contract. Replaces a bare QSet<QString> (permanent for
  // this coordinator's entire lifetime) with this small struct so a
  // record naturally, deterministically stops being trusted without
  // requiring an active sweep/timer.
  struct Negative404Entry {
    quint64 generation{0};
    qint64 expiresAtMonotonicMs{0};
  };
  // Cache keys (see AssetCache::cacheKeyFor()) for which an exact,
  // authoritative 404 has been observed by THIS process; never
  // persisted to disk. See the class comment for the full contract, and
  // hasNegative404()'s comment for the TTL/generation expiry.
  QHash<QString, Negative404Entry> m_negative404;
  static constexpr qint64 kNegative404TtlMs = 5 * 60 * 1000; // 5 minutes
  // Review round-4 item 7: a hard ceiling on the number of DISTINCT
  // unexpired negative-404 records retained at once, independent of
  // kNegative404TtlMs. pruneStaleCacheKeyState()'s lazy-expiry sweep
  // alone only removes ALREADY-expired records, so it cannot bound the
  // (unrealistic, but not impossible) case of an enormous number of
  // distinct candidates all 404ing within the same TTL window; this cap
  // evicts the soonest-to-expire non-pinned record(s) once exceeded.
  // Chosen generously relative to any real card/asset catalogue's size.
  // A member (not a bare compile-time constant) so
  // setMaxTrackedNegative404EntriesForTesting() can shrink it for a fast,
  // deterministic high-cardinality test; production code never changes
  // it from this default.
  static constexpr int kMaxTrackedNegative404Entries = 4096;
  int m_maxTrackedNegative404Entries = kMaxTrackedNegative404Entries;
  // Review round-3 item 13: the monotonic clock negative-404 expiry is
  // measured against -- steady_clock in production (see the .cpp),
  // replaceable via setMonotonicNowForTesting() above. Deliberately NOT
  // wall-clock time: a system clock adjustment/rollback must never
  // resurrect or prematurely expire a negative-404 record.
  MonotonicNowFn m_monotonicNowMs;

  // Per-cache-key optimistic-concurrency APPLIED-generation watermark
  // (review item 6, refined by round-3 item 14); see
  // currentCacheKeyGeneration()/tryApplyCacheKeyMutation() and the class
  // comment's "Cross-logical-key races" paragraph. Memory-only, like
  // m_negative404.
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
};

} // namespace Arkham
