#pragma once

#include <QObject>
#include <QTemporaryDir>
#include <memory>

// Tests for AssetRequestCoordinator: request coalescing (concurrent
// identical requests share exactly one underlying fetch), per-consumer
// cancellation semantics (one consumer cancelling never affects another;
// only the last consumer's cancellation actually aborts the underlying
// fetch), the NotFound-only candidate-fallback advance (a 404 advances,
// but no other error ever does), a cache hit short-circuiting the network
// entirely, and stale-callback/destruction safety.
class AssetRequestCoordinatorTests final : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void coalescesConcurrentIdenticalRequests();
  void cancellingOneConsumerNeverAffectsAnother();
  void lastConsumerCancellationAbortsUnderlyingFetch();
  void advancesToNextCandidateOnlyOnNotFound();
  void nonNotFoundErrorNeverAdvancesCandidate();
  void cacheHitShortCircuitsNetworkEntirely();
  void destructionNeverInvokesStaleCallback();

private:
  QString m_tempDirPath;
  std::unique_ptr<QTemporaryDir> m_tempDir;
};
