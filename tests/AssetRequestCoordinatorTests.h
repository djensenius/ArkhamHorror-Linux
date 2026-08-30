#pragma once

#include <QObject>
#include <QTemporaryDir>
#include <memory>

// Tests for AssetRequestCoordinator: request coalescing (concurrent
// identical requests share exactly one underlying fetch), per-consumer
// cancellation semantics (one consumer cancelling never affects another;
// only the last consumer's cancellation actually aborts the underlying
// fetch, and even an immediate cache-hit/error completion can still be
// cancelled before its queued delivery runs), the NotFound-only
// candidate-fallback advance (a 404 advances, but no other error ever
// does), a cache hit short-circuiting the network entirely -- but never
// skipping an untried higher-priority candidate merely because a
// lower-priority one is already cached (review item 5) -- disk-hit
// conditional revalidation using stored ETag/Last-Modified (a 304 serves
// the still-valid stale entry, a fresh 200 replaces it, a definitive 404
// evicts the entry and advances candidates exactly like a first-time
// miss, and any OTHER revalidation failure still serves the stale entry
// rather than erroring or advancing -- "stale-if-error"), negative-404
// record scoping, and stale-callback/destruction safety. Also covers
// review item 9: a disk-cached entry whose bytes fail a fresh
// format/magic/decode/dimension/pixel-budget re-check (never simply
// trusted forever) is quarantined -- evicted from both memory and disk
// -- and the same candidate retried exactly once as a genuine network
// miss, whether that retry succeeds (fresh bytes replace the quarantined
// ones) or fails (the retry's own fresh error surfaces, never a repeat
// or a loop); AssetErrorCode::UnsupportedCodec is the sole carve-out,
// since it means the cached bytes are still valid and must never be
// evicted for lack of an installed decoder. Also covers review item 6
// (cross-logical-key stale resurrection): two DIFFERENT AssetKeys that
// happen to resolve to the SAME candidate/cache key (never coalesced,
// since coalescing keys on the whole logical AssetKey) genuinely race
// when both are in flight at once; a per-cache-key optimistic-
// concurrency generation counter (see AssetRequestCoordinator.h's
// "Cross-logical-key races" paragraph) ensures a callback that arrives
// AFTER some other, more recently issued operation has already mutated
// that exact cache key silently skips its own would-be mutation (never
// overwriting a newer store(), never resurrecting a cache key a newer
// operation has since evicted via a definitive 404) while still
// completing accurately for its own consumers.
class AssetRequestCoordinatorTests final : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void coalescesConcurrentIdenticalRequests();
  void keysDifferingOnlyByHostileLocaleContentNeverCoalesce();
  void keysDifferingOnlyByBackIdentityFieldsNeverCoalesce();
  void keysDifferingOnlyByAssetBaseTrailingSlashStillCoalesce();
  void cancellingOneConsumerNeverAffectsAnother();
  void lastConsumerCancellationAbortsUnderlyingFetch();
  void advancesToNextCandidateOnlyOnNotFound();
  void nonNotFoundErrorNeverAdvancesCandidate();
  void cacheHitShortCircuitsNetworkEntirely();
  void cachedEnglishFallbackNeverSkipsUntriedLocalizedCandidate();
  void confirmedNegative404AuthorizesSkippingCandidate();
  void allCandidatesNegative404CompletesWithoutNetworkRoundTrip();
  void destructionNeverInvokesStaleCallback();
  void cancellingImmediateCacheHitCompletionSuppressesDelivery();
  void cancellingAfterCompletionButBeforeQueuedDeliverySuppressesResult();
  void diskHitWithValidatorsRevalidatesAndServesStaleOn304();
  void notModifiedResponseWithRefreshedValidatorUpdatesCacheEntry();
  void confirmedNotModifiedPromotesEntryToMemoryForSameProcessShortCircuit();
  void diskHitRevalidationReplacesEntryOnFresh200();
  void diskHitRevalidationEvictsEntryAndAdvancesOn404();
  void diskHitRevalidationServesStaleOnNon404Failure();
  void diskHitRevalidationCoalescesConcurrentIdenticalRequests();
  void diskHitAfterRestartDecodesOnDemandAndPublishesToMemory();
  void concurrentIdenticalRequestsForADiskHitCoalesceIntoASingleDecode();
  void malformedDiskEntryIsQuarantinedAndRefetchedFromNetwork();
  void diskMetadataDimensionMismatchIsQuarantinedAndRefetched();
  void diskMetadataContentTypeMismatchIsQuarantinedAndRefetched();
  void diskCachedAvifSequenceIsQuarantinedAndRefetchedFromNetwork();
  void quarantineRefetchFailureSurfacesFreshErrorWithoutLooping();
  void unsupportedCodecIsNeverQuarantineWorthy();
  void delayedStaleFetchSuccessNeverOverwritesNewerCrossLogicalKeyCacheEntry();
  void
  delayedStaleRevalidationSuccessAfterDefinitive404NeverResurrectsEvictedEntry();
  // Review round-3 items 12-15.
  void localized404AdvanceServesAlreadyCachedEnglishCandidateWithoutNetwork();
  void negative404RecordExpiresAfterTtlAndIsRefetched();
  // Round-6 item 8: coalescing by logical AssetKey duplicates
  // network/decode for aliases resolving to the same candidate/cache
  // key. These replace the now-structurally-impossible
  // cross-logical-key completion-ORDER races that
  // laterIssuedOperationPublishesOverEarlierIssuedEvenWhenItCompletesSecond
  // and queuedStaleDiskDecodeNeverMutatesANewerLiveMemoryEntry used to
  // construct, proving the new CandidateAttempt coalescing directly
  // instead.
  void cancellingOneCoalescedCrossLogicalKeySubscriberNeverAbortsAnother();
  void coalescedRevalidationAppliesFreshReplaceOnceForAllSubscribers();
  // Review round-4 items 5, 7.
  void newer404TombstonesOlderCachedEntryAcrossTtlExpiryAndRestart();
  void negative404AndGenerationStateStayBoundedUnderHighCardinality();
  void soleConsumerCancellationPrunesIssuanceStateUnderHighCardinality();
  // Round-6 item 6: a 404 whose durable tombstone cannot be committed
  // must not record a negative-404 that could later let the entry it
  // failed to remove resurface.
  void
  failedDurableInvalidationOnDefinitive404NeverRecordsNegativeAndFailsClosed();
  // Round-7/8 item 6 ("cache-hit read/decode occurs before operation
  // coalescing"): a burst of simultaneous requests for DIFFERENT logical
  // AssetKeys resolving to the same candidate/cache key must share one
  // decode, never one per request, for both a memory hit and a disk hit;
  // a quarantine-worthy decode failure shared by the whole group must
  // invalidate exactly once and let each waiter independently (but
  // network-coalesced) refetch; and a waiter cancelled before the shared
  // decode is delivered must never affect any surviving sibling waiter.
  void concurrentAliasedMemoryHitRequestsCoalesceIntoASingleDecode();
  void concurrentAliasedDiskHitRequestsCoalesceIntoASingleDecode();
  void
  corruptCoalescedCacheHitInvalidatesExactlyOnceAndEachWaiterIndependentlyRefetches();
  void cancellingOneWaiterInACoalescedCacheDecodeGroupNeverAffectsAnother();
  // Round-9+ review item 3/7 ("fully cancelled PendingCacheDecode groups
  // retained and still decode"): when EVERY waiter of a shared cache-hit
  // decode group cancels before its single queued decode has run, the
  // group itself must be pruned entirely -- the decode must never
  // happen at all, not even once, since there is no one left to deliver
  // it to.
  void
  cancellingEveryWaiterInACoalescedCacheDecodeGroupPreventsTheDecodeEntirely();
  // Round-9+ review item 3/7 ("aliases coalesce network but not cached/
  // 304 decode"): two aliased logical keys revalidating the identical
  // disk entry (same cacheKey/etag, so the same coalesced conditional
  // GET -- see diskHitRevalidationCoalescesConcurrentIdenticalRequests())
  // must ALSO share exactly one decode of the served-stale (or
  // confirmed-304) entry -- never one independent decode per aliased
  // subscriber, which would otherwise multiply a near-32-megapixel
  // decode by the number of aliases sharing one revalidation.
  void
  concurrentAliasedRevalidationStaleIfErrorRequestsCoalesceIntoASingleDecode();
  void
  concurrentAliasedRevalidationNotModifiedRequestsCoalesceIntoASingleDecode();
  // Round-9+ review (HIGH): CandidateAttempt::token closes a race where
  // a sole consumer's cancellation synchronously removes/cancels its
  // CandidateAttempt, and a fresh request for the IDENTICAL candidate
  // issued immediately afterward (same call stack, before the old
  // fetch's now-queued Cancelled callback has run) reuses the exact same
  // string-keyed attemptKey. Without a per-attempt token, the OLD,
  // stale, orphaned callback -- when it eventually runs -- finds the
  // NEW attempt under that same key, erases it, and wrongly dispatches
  // its own stale Cancelled result to the NEW request's subscriber,
  // silently discarding the new attempt's real, later, correct
  // completion. These two tests reproduce that exact interleaving for
  // both the unconditional network-fetch path (startCandidate()) and
  // the conditional revalidation path (startRevalidation()), and assert
  // the replacement request always observes its OWN real, correct
  // result.
  void staleCancelledAttemptCallbackNeverCorruptsReplacementNetworkFetch();
  void staleCancelledAttemptCallbackNeverCorruptsReplacementRevalidation();
  // Round-N+ review (MEDIUM, repeat finding, "invalid cache limits
  // publicly constructible"): a coordinator constructed against a cache
  // whose Config failed AssetCache::validateConfiguration() (see
  // AssetCache::isValid()/configurationError()) must refuse to ever
  // touch it -- every request() completes immediately, synchronously-
  // deferred exactly like every other immediate-completion path, with
  // AssetErrorCode::InvalidConfiguration, and no network request is
  // ever issued at all.
  void
  requestAgainstAnInvalidlyConfiguredCacheFailsImmediatelyWithoutNetworkAccess();

private:
  QString m_tempDirPath;
  std::unique_ptr<QTemporaryDir> m_tempDir;
};
