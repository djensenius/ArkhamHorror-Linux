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

  // Round-3 item 9: root replaced (renamed away + a new directory
  // created at the same path) strictly AFTER construction.
  void rootReplacedAfterConstructionPermanentlyDisablesDiskIoForBothTargets();
  // Round-4/5 review item 3: a MEMORY hit's recency bump must be exactly
  // as anchor-verified as any other disk-touching operation.
  void
  memoryHitRecencyBumpAfterRootReplacementNeverTouchesReplacementDirectory();

  // Round-4/5 review item 4: the on-disk generation identifier is an
  // independently-minted token, decoupled from the payload's own
  // content hash -- two byte-identical stores must not collapse onto
  // the same generation filename.
  void identicalPayloadReplacementMintsANewGenerationEachTime();

  // Round-3 item 8: metadata numeric-field cast safety.
  void metadataWithImpossibleNumericFieldsIsRejectedAndQuarantined();
  void accessSequenceAbove2Pow53RoundTripsExactlyAcrossARestart();

  // Review item 11: monotonic-sequence LRU accuracy.
  void
  accessSequenceIsMonotonicAndUniqueEvenForSameMillisecondConsecutiveStores();
  void accessSequenceRecoversPastPriorMaximumAcrossARestart();
  void memoryOnlyHitsKeepAnEntryAliveOverAColderDiskOnlyEntry();
  void failedEvictionDeletionLeavesEntryCountedAsStillOccupyingSpace();

  // Round-6 item 5: root/owned-subtree symlink-during-creation escape
  // prevention -- see AssetCache::AssetCache()'s and
  // openDirectoryChainNoFollow()'s comments in AssetCache.cpp.
  void
  ownedSuffixComponentThatIsASymlinkIsRejectedEvenWhenTrustedAnchorIsPlain();
  void ownedSuffixOfPlainDirectoriesUnderTrustedAnchorResolvesSuccessfully();
  void crossMountBindMountDirectoryDuringCleanupIsNeverDescendedIntoOrDeleted();
  void mountIdentificationIsActuallySupportedOnThisLinuxBuildUnprivileged();

  // Round-7/8 item 2/5: path-based QDir::mkpath() is entirely removed
  // from the constructor -- owned-suffix components are now created
  // (never merely verified) directly by openDirectoryChainNoFollow()
  // itself, via mkdirat() relative to an already-open parent
  // descriptor, and every step of that walk must also be
  // mount-identity-continuous with the trusted anchor above it.
  void ownedSuffixMissingComponentsAreCreatedViaMkdiratNeverPathBasedMkpath();
  void
  constructingWithIntermediateConfiguredDirectorySymlinkNeverAutoCreatesOrRecoversForeignDirectory();
  void bindMountOverOwnedSuffixComponentIsRejectedDuringChainResolution();

  // Round-9+ review (HIGH): a configured directory whose full path
  // ALREADY EXISTS on disk (even by way of an attacker-planted
  // symlink for an intermediate ancestor two or more levels above the
  // final leaf) must still be rejected -- the "longest existing
  // prefix is a single trusted anchor" shortcut previously used for
  // Config::directory could never detect this, since the whole path
  // resolved successfully. Also proves the sentinel directory the
  // symlink points at is left completely untouched.
  void
  precreatedLeafBehindDeepIntermediateSymlinkInConfiguredDirectoryIsRejectedAndSentinelUntouched();

  // Round-N+ review (HIGH, repeat finding): the SAME "precreated leaf
  // behind an intermediate symlink" attack as the test above, but for a
  // configured directory OUTSIDE the user's home directory -- the
  // uncommon-case fallback branch that, before this round, used a
  // separate, weaker "longest existing prefix via symlink-following
  // QFileInfo::exists()" shortcut instead of walking every component
  // no-follow. Proves that shortcut is gone and this uncommon case now
  // gets the SAME per-component no-follow walk (anchored at "/" instead
  // of home) as every other configured directory.
  void
  precreatedLeafBehindIntermediateSymlinkOutsideHomeIsRejectedAndSentinelUntouched();

  // Round-9+ review: the default cache location (an OS-provided
  // parent, e.g. `~/.cache` or `~/Library/Caches`) may not exist AT
  // ALL on a genuinely clean install -- every one of its own missing
  // ancestor components, not just this application's fixed
  // "assets/v1" suffix, must be created (never merely assumed
  // pre-existing) via the same secure, no-follow walker.
  void cleanInstallWithEntirelyMissingCacheHierarchyIsCreatedSecurely();

  // Round-9+ review: for a caller-supplied Config::directory, this
  // resolver must still never auto-create ANY missing component, even
  // when every component up to the last is already present -- exactly
  // preserving the pre-existing "never creates any part of a
  // caller-supplied custom cache directory" guarantee for the new,
  // arbitrary-depth home-anchored walker.
  void configuredDirectoryWithMissingLeafUnderHomeIsNeverAutoCreated();

  // Round-6 item 6: invalidate() must report a typed failure -- rather
  // than silently succeeding -- when the manifest unlink it depends on
  // for a durable tombstone genuinely cannot be committed.
  void invalidateReportsPersistenceFailedWhenManifestUnlinkFails();

  // Round-7/8 item 6: deleteEntry()'s manifest unlink must never be
  // gated on a prefix enumeration that could itself fail -- and the
  // manifest's removal must be crash-durable (fsync'd) even when that
  // enumeration cannot be completed at all, so the entry can never
  // revive after a restart or an expired negative-cache TTL purely
  // because a transient, unrelated directory-listing failure occurred.
  void
  deleteEntryUnlinksManifestDurablyEvenWhenPrefixEnumerationCannotBeCompleted();

  // Round-9+ review (MEDIUM): a negative Config byte limit is an invalid
  // configuration, never merely "unlimited" -- see Config's own doc
  // comment in AssetCache.h.
  void negativeDiskMaxBytesDisablesDiskCacheInsteadOfDestructivelyEvicting();
  void negativeMemoryMaxCostBytesDisablesMemoryCacheRatherThanCrashing();

  // Round-9+ review (HIGH): invalidate()/deleteEntry() must never
  // report a key as durably, authoritatively gone when root
  // verification failing on THIS call is the only reason nothing was
  // actually deleted -- a real manifest can still be sitting there,
  // untouched, discoverable by a different instance later.
  void invalidateAfterRootReplacementReportsPersistenceFailedNotDurable();

  // Round-N+ review (HIGH, repeat finding): the SAME "revive after
  // restart" risk as the test above, but for the PRE-latched case --
  // m_diskCacheDisabled already true BEFORE invalidate() is even
  // called (an EARLIER call on the same instance already detected the
  // root was replaced), rather than root verification failing freshly
  // during THIS call. Proves invalidate() reports PersistenceFailed
  // here too, and that the untouched manifest genuinely survives.
  void
  invalidateWithAlreadyLatchedDiskDisabledReportsPersistenceFailedNotDurable();

  // Round-N+ review (MEDIUM, repeat finding): an unreadable nested
  // subtree (a subdirectory whose contents cannot be listed/opened,
  // simulated the same deterministic, permission-based way other tests
  // in this file already force enumeration/fstatat/open failures) must
  // make diskUsageBytes() report a genuinely indeterminate result --
  // never a numeric zero a caller could mistake for "this cache is
  // empty" -- and must disable disk persistence for this instance
  // rather than silently under-reporting real, resident usage.
  void unreadableNestedSubtreeDisablesPersistenceRatherThanReportingZeroUsage();

private:
  QString m_tempDirPath;
  std::unique_ptr<QTemporaryDir> m_tempDir;
};
