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
// does), a cache hit short-circuiting the network entirely, disk-hit
// conditional revalidation using stored ETag/Last-Modified (a 304 serves
// the still-valid stale entry, a fresh 200 replaces it, and any OTHER
// revalidation failure still serves the stale entry rather than erroring
// -- "stale-if-error"), and stale-callback/destruction safety.
class AssetRequestCoordinatorTests final : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void coalescesConcurrentIdenticalRequests();
  void keysDifferingOnlyByHostileLocaleContentNeverCoalesce();
  void cancellingOneConsumerNeverAffectsAnother();
  void lastConsumerCancellationAbortsUnderlyingFetch();
  void advancesToNextCandidateOnlyOnNotFound();
  void nonNotFoundErrorNeverAdvancesCandidate();
  void cacheHitShortCircuitsNetworkEntirely();
  void destructionNeverInvokesStaleCallback();
  void cancellingImmediateCacheHitCompletionSuppressesDelivery();
  void cancellingAfterCompletionButBeforeQueuedDeliverySuppressesResult();
  void diskHitWithValidatorsRevalidatesAndServesStaleOn304();
  void notModifiedResponseWithRefreshedValidatorUpdatesCacheEntry();
  void confirmedNotModifiedPromotesEntryToMemoryForSameProcessShortCircuit();
  void diskHitRevalidationReplacesEntryOnFresh200();
  void diskHitRevalidationServesStaleOnAnyFailure();
  void diskHitRevalidationCoalescesConcurrentIdenticalRequests();
  void diskHitAfterRestartDecodesOnDemandAndPublishesToMemory();
  void unsupportedCodecOnDecodeOnDemandSurfacesTypedError();

private:
  QString m_tempDirPath;
  std::unique_ptr<QTemporaryDir> m_tempDir;
};
