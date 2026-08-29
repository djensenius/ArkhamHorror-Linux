#pragma once

#include <QObject>
#include <QTemporaryDir>
#include <memory>

// Tests for AssetCache: memory/disk hit ordering, SHA-256 cache-key
// namespace isolation, crash-consistent content-addressed
// generation+manifest publication and crash/corruption repair (orphan
// generation payload/metadata, mismatched pair, stray temp file,
// interrupted-replacement orphan before/after the manifest swap),
// metadata-driven LRU quota/watermark eviction using a monotonic access
// sequence (review item 11) rather than raw wall-clock time -- covering
// same-millisecond ordering, cross-restart sequence recovery, a
// memory-only hit keeping an entry warm over a colder disk-only entry,
// and eviction quota accounting only crediting bytes as freed once
// deletion actually succeeds -- touch-after-304 lastAccess refresh, and
// restart persistence (a fresh AssetCache instance pointed at the same
// directory sees prior entries).
class AssetCacheTests final : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void storeThenLookupMemoryAndDiskRoundTrips();
  void cacheKeyIsNamespacedByFullResolvedUrl();
  void orphanPayloadWithoutMetadataIsRepaired();
  void orphanMetadataWithoutPayloadIsRepaired();
  void mismatchedPayloadMetadataPairIsRepaired();
  void strayFileNotMatchingKeyShapeIsRemoved();
  void strayDirectoryIsRemovedAndCountedTowardDiskUsage();
  void quotaEvictsOldestAccessFirstDownToLowWaterMark();
  void storeSkipsFullReapSweepWhenComfortablyUnderQuota();
  void touchAfterNotModifiedRefreshesLastAccessAndHeaders();
  void touchAfterNotModifiedWithMissingMetadataRepairsOrphanPayload();
  void restartingWithSameDirectorySeesPriorEntries();
  void oversizedSelfConsistentPayloadBeyondAbsoluteCapIsRejected();
  void storeRejectsPayloadBeyondAbsoluteCapWithoutTouchingDisk();
  void metadataWriteFailureAfterPayloadCommitCleansUpOrphanPayload();
  void oversizedMetadataFileIsRejectedWithoutUnboundedReadAll();
  void malformedKeyWithPathTraversalNeverTouchesFilesystemOutsideCacheDir();
  void promoteToMemoryRejectsMalformedKeyWithoutInserting();

  // Review item 8: crash-consistent generation/manifest publication.
  void initialInsertCrashBeforeManifestPublishLeavesNoValidEntry();
  void replacementCrashBeforeManifestSwapPreservesOldGenerationIntact();
  void replacementCrashAfterManifestSwapPromotesNewGenerationAndReclaimsOld();
  void storeReplacementLeavesExactlyOneLiveGenerationOnDisk();

  // Review item 7: symlink cache-root/entry escape prevention.
  void symlinkedCacheRootDisablesDiskIoAndLeavesTargetUntouched();
  void directorySymlinkInsideCacheRootIsUnlinkedNotFollowed();
  void fileSymlinkInsideCacheRootIsUnlinkedNotFollowed();
  void danglingSymlinkInsideCacheRootIsUnlinkedSafely();

  // Review item 11: monotonic-sequence LRU accuracy.
  void
  accessSequenceIsMonotonicAndUniqueEvenForSameMillisecondConsecutiveStores();
  void accessSequenceRecoversPastPriorMaximumAcrossARestart();
  void memoryOnlyHitsKeepAnEntryAliveOverAColderDiskOnlyEntry();
  void failedEvictionDeletionLeavesEntryCountedAsStillOccupyingSpace();

private:
  QString m_tempDirPath;
  std::unique_ptr<QTemporaryDir> m_tempDir;
};
