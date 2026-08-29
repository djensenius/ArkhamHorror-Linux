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
// record scoping, and stale-callback/destruction safety.
class AssetRequestCoordinatorTests final : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void coalescesConcurrentIdenticalRequests();
  void keysDifferingOnlyByHostileLocaleContentNeverCoalesce();
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
  void unsupportedCodecOnDecodeOnDemandSurfacesTypedError();

private:
  QString m_tempDirPath;
  std::unique_ptr<QTemporaryDir> m_tempDir;
};
