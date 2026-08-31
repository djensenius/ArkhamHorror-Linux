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
  // Cumulative review (PR #18, MEDIUM, "planted FIFO under a known
  // manifest/metadata filename hangs forever before S_ISREG"): a
  // hostile/broken concurrent process (or attacker) planting a FIFO
  // (named pipe) at a manifest/metadata/payload filename must never
  // block construction/lookup indefinitely -- a blocking open() of a
  // FIFO with no writer hangs forever, well before any type check ever
  // runs. Each covers a DIFFERENT one of the three read call sites,
  // all funnelling through the same openRegularNoFollowRelative().
  void manifestPlantedAsFifoNeverBlocksConstructionOrLookup();
  void metadataPlantedAsFifoNeverBlocksConstructionOrLookup();
  void payloadPlantedAsFifoNeverBlocksConstructionOrLookup();
  // "socket too" -- a UNIX domain socket special file planted at a
  // metadata filename must be rejected identically (never blocks,
  // never mistaken for a regular file).
  void metadataPlantedAsUnixSocketNeverBlocksConstructionOrLookup();
  // Cumulative review (PR #18, MEDIUM, "listAllEntriesRelative returns
  // partial vector on traversal errors ... reapAndEnforceQuota mutates
  // based on it and may delete a valid generation whose manifest was
  // omitted"): proves the entire repair/quota sweep aborts with ZERO
  // mutations when the directory listing it would act on cannot be
  // trusted, rather than proceeding against a partial/empty view.
  void reapSweepAbortsAllMutationWhenDirectoryListingIsIndeterminate();
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
  void
  realBindMountWithOpenat2ForcedUnavailableIsStillConfirmedAndSkippedNeverDisablingDiskCache();
  void mountIdentificationIsActuallySupportedOnThisLinuxBuildUnprivileged();
  void mountinfoRawReadParsesAtLeastOneEntryOnThisLinuxBuildUnprivileged();

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

  // Round-N+ review (MEDIUM, repeat finding, "default cache still
  // trusts an already-resolved multi-component home path"): unlike
  // every OTHER symlink test above (which plants the symlink inside the
  // CONFIGURED suffix beneath an already-trusted anchor), this plants
  // the symlink INSIDE the user's own HOME path itself -- an ancestor
  // of home, not home's final component -- which the pre-fix single
  // leaf-only O_NOFOLLOW open() of the complete home path string could
  // never see at all. Overrides $HOME (restored via RAII guard) to a
  // multi-component fake home whose own ancestor is a symlink pointing
  // at an external sentinel, and proves resolution is rejected before
  // ever reaching the caller's owned suffix.
  void
  intermediateSymlinkWithinTheHomePathItselfIsRejectedEvenForAPreexistingLeaf();
  // Positive control for the test above: an entirely ordinary,
  // symlink-free fake home (still multi-component, still overriding
  // $HOME) must resolve successfully -- proving the rejection above is
  // specific to the planted symlink, not a general regression in home
  // resolution whenever $HOME happens to differ from this test
  // process's own real home.
  void ordinaryMultiComponentHomePathWithNoSymlinksResolvesSuccessfully();

  // Independent cumulative re-review (MEDIUM, "Validate owner/mode for
  // EVERY opened component regardless mount transition ... Production
  // same-mount test wrong owner/world-writable"): a group/world-
  // writable FINAL account-home directory must be rejected even when
  // NO mount transition occurs anywhere in the walk at all (an
  // entirely ordinary same-mount fake home, real chmod()'d 0777 --
  // no privilege, no override needed). Proves the new unconditional
  // per-component ownership/mode check actually runs for the
  // final-home position even without a transition, not merely at
  // transition boundaries as before this fix.
  void
  sameMountGroupWorldWritableFinalHomeDirectoryIsRejectedWithoutAnyMountTransition();
  // Sibling of the test above: an ORDINARY, non-writable final home
  // directory (still same-mount, still no transition) must keep
  // resolving successfully -- proving the rejection above is specific
  // to the writable mode, not a general regression whenever no
  // transition occurs.
  void
  sameMountOrdinaryFinalHomeDirectoryStillResolvesSuccessfullyWithoutAnyMountTransition();

  // Cumulative review (PR #18, MEDIUM): an UNAUTHENTICATED mount
  // transition landing exactly on a fake $HOME (i.e. the account
  // database, forced via the test-only override, does NOT agree this
  // is the current user's real home) that resolves onto a genuinely
  // DIFFERENT mount than its own parent directory must be rejected --
  // exactly the same strict same-mount-throughout policy an
  // outside-home configured path already gets. Requires a real bind
  // mount (root-privileged); QSKIP when unavailable.
  void unauthenticatedHomeMountTransitionOntoADifferentMountIsRejected();
  // Positive control for the test above: the SAME bind-mount shape,
  // but with the account database (forced via the test-only override)
  // agreeing this exact fake $HOME is the current user's own real
  // home -- the mount transition MUST be permitted, exactly modelling
  // a real SteamOS-style dedicated "/home/deck" mount. Requires a real
  // bind mount (root-privileged); QSKIP when unavailable.
  void authenticatedHomeMountTransitionOntoADifferentMountIsPermitted();
  // Cumulative review (PR #18, MEDIUM, "home mount auth wrong
  // boundary"): the two tests above only ever exercise a mount
  // transition landing on home's own FINAL path component. This test
  // (and its rejected companion below) instead bind-mounts an
  // ANCESTOR of home's final component -- modelling a real, ordinary
  // dedicated "/home" partition (distinct from a SteamOS-style
  // "/home/deck" split) -- and proves the resolver now correctly
  // permits it when authenticated, closing the exact gap where a
  // prior version of this resolver hard-coded the permitted
  // transition point to home's own final component and rejected this
  // equally-legitimate topology outright. Requires a real bind mount
  // (root-privileged); QSKIP when unavailable.
  void
  authenticatedAncestorMountTransitionModellingADedicatedHomePartitionIsPermitted();
  // Negative control for the test above: the identical ancestor
  // bind-mount shape, but unauthenticated -- must be rejected exactly
  // like every other unauthenticated mount transition, proving the
  // fix's "only when authenticated AND independently policy-qualified"
  // rule applies uniformly regardless of WHERE in home's path the
  // transition organically falls. Requires a real bind mount
  // (root-privileged); QSKIP when unavailable.
  void
  unauthenticatedAncestorMountTransitionModellingADedicatedHomePartitionIsRejected();
  // Independent cumulative re-review (MEDIUM, repeat finding, "home
  // trust... arbitrary same-device bind mount still passes"): a bind
  // mount landing at an ancestor position that reports the IDENTICAL
  // device as its parent must be refused even when fully authenticated
  // and ownership/mode-qualified -- a real dedicated partition is, by
  // definition, backed by a distinct device; a same-device "mount" can
  // only be a redirect of some arbitrary directory. Requires a real
  // bind mount (root-privileged); QSKIP when unavailable.
  void
  arbitrarySameDeviceBindMountOntoAncestorPositionIsRejectedEvenWhenFullyAuthenticatedAndOwnershipQualifies();
  // Independent cumulative re-review (MEDIUM, repeat finding, "home
  // trust... even tests arbitrary /dev/shm bind as accepted"): a
  // GENUINE, freshly-created, TOP-LEVEL tmpfs mount (mountinfo root
  // == "/", so the new root-field check alone cannot explain a
  // rejection) landing exactly on home's own final component, fully
  // authenticated against the account database and correctly owned/
  // moded, must STILL be refused -- tmpfs is no longer a trusted local
  // filesystem type at all (see trustedLocalMountFilesystemTypes()'s
  // own comment), regardless of how pristine/legitimate-shaped the
  // mount otherwise is. Requires real tmpfs mount privilege; QSKIP
  // when unavailable.
  void
  authenticatedFreshTopLevelTmpfsHomeMountTransitionIsRejectedDespiteRootBeingSlash();
  // Independent cumulative re-review (MEDIUM, repeat finding, "home
  // trust... still discards mount root... authenticate exact
  // descriptor mount id against position-specific expected...
  // root..."): a bind mount of an ORDINARY SUBDIRECTORY of an
  // otherwise genuinely trusted, distinct-device ext4 loopback
  // filesystem (never the filesystem's own root) landing on home's
  // final component, fully authenticated and correctly owned/moded,
  // must still be refused -- mountinfo's own "root" field for such a
  // mount is never "/", proving this is merely an arbitrary directory
  // bind mount of SOME already-existing filesystem, never a genuine
  // dedicated whole-partition mount, regardless of the underlying
  // filesystem type's own trust status. Requires a real loopback ext4
  // filesystem and bind-mount privilege; QSKIP when unavailable.
  void
  authenticatedBindMountOfASubdirectoryOfATrustedFilesystemIsRejectedDespiteTrustedFstype();
  // An unauthenticated $HOME whose mount-identification itself is
  // degraded (forced via setMountIdentificationDegradedForTesting(),
  // no privilege required) must fail closed exactly like an
  // outside-home configured path already does -- never silently fall
  // back to treating an unproven mount boundary as safe.
  void
  unauthenticatedHomeWithDegradedMountIdentificationFailsClosedEvenUnmounted();
  // Cumulative review (independent re-review, MEDIUM, "home trust
  // still pathname-only"): direct, deterministic, UNPRIVILEGED proof of
  // mountTransitionIsIndependentlyPolicyQualified()'s real ownership/
  // mode enforcement, exercised via
  // mountTransitionIsIndependentlyPolicyQualifiedForTesting() against
  // an ordinary directory this test itself owns and chmod()s -- no real
  // mount transition (and therefore no mount privilege) is needed to
  // reach this exact decision function at all. A group-writable
  // destination must be refused even though ownership matches and (via
  // the deterministic filesystem-type override) the kernel-recorded
  // filesystem type would otherwise qualify.
  void
  mountTransitionPolicyRejectsGroupWritableDestinationEvenWhenFilesystemTypeQualifies();
  // Sibling of the test above: a world-writable destination must be
  // refused for the identical reason.
  void
  mountTransitionPolicyRejectsWorldWritableDestinationEvenWhenFilesystemTypeQualifies();
  // Positive control for the two tests above: an ordinary directory
  // owned by this process's own real uid, with neither group- nor
  // world-write bits set, passes the ownership/mode half of the policy
  // -- combined with the deterministic filesystem-type override
  // reporting "qualified", the overall decision is granted.
  void
  mountTransitionPolicyAcceptsOwnedNonWritableDestinationWhenFilesystemTypeQualifies();
  // Cumulative review (independent re-review, MEDIUM, "trusted
  // deployment/mount identity independently established"): even with
  // PERFECT ownership/mode, an untrusted/unrecognized filesystem-type
  // verdict (forced via
  // setMountTransitionPolicyQualificationOverrideForTesting(), modelling
  // what a real kernel-recorded network/FUSE-backed filesystem type
  // would report) must still refuse the overall policy decision --
  // ownership/mode alone is never sufficient on its own.
  void
  mountTransitionPolicyRejectsDestinationWhenFilesystemTypeOverrideReportsUnqualified();
  // Cumulative review (independent re-review round-6, MEDIUM, "Home
  // policy rejects normal root-owned `/home` but accepts arbitrary
  // user-owned binds" -- "position-sensitive ownership"): an ordinary,
  // current-uid-owned, non-writable destination -- exactly the shape
  // mountTransitionPolicyAcceptsOwnedNonWritableDestinationWhenFilesystemTypeQualifies()
  // above proves PASSES for a FINAL (account-home) transition -- must
  // instead be REJECTED when evaluated as an ANCESTOR-position
  // transition (isFinalAccountHomeTransition=false): an ancestor must
  // be root-owned, never merely owned by whichever uid happens to be
  // running this process (an unprivileged attacker sharing that uid
  // could otherwise satisfy this exact check with a bind mount of a
  // directory they themselves own). Proves the fail-before/pass-after
  // contract directly: this exact destination fixture was accepted
  // under the OLD, position-blind policy and must now be refused.
  void
  mountTransitionPolicyRejectsCurrentUidOwnedDestinationForAncestorPositionEvenWhenFilesystemTypeQualifies();
  // Cumulative review (independent re-review round-6, MEDIUM, same
  // finding): the complementary positive control -- a genuinely
  // root-owned, non-writable directory ("/" itself, present and
  // root-owned mode 0755 on every POSIX system this project targets,
  // needing no privileged setup at all) correctly QUALIFIES as an
  // ANCESTOR-position transition destination, modelling a real
  // distribution's own root-provisioned dedicated "/home" partition --
  // proving the fix does not merely reject everything, but correctly
  // accepts the legitimate topology the review demands ("legitimate
  // dedicated /home ... transitions pass").
  void
  mountTransitionPolicyAcceptsRootOwnedDestinationForAncestorPositionWhenFilesystemTypeQualifies();
  // Cumulative review (independent re-review, MEDIUM, "only one
  // transition allowed"): unlike every test above (which permitted at
  // most a single mount transition across the whole home-path walk),
  // this bind-mounts BOTH an ancestor of home's final component AND
  // home's own final component onto two SEPARATE, independent real
  // mounts -- modelling a genuine SteamOS-style topology with more than
  // one legitimate transition in the same walk (e.g. a dedicated
  // "/home" partition AND a further per-user data mount beneath it) --
  // and proves the resolver now permits BOTH, closing the prior
  // artificial "at most one, ever" cap. Requires two real bind mounts
  // (root-privileged); QSKIP when unavailable.
  void
  multipleIndependentlyQualifiedMountTransitionsInTheSameHomeWalkAreAllPermitted();

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
  void bothNegativeDiskAndMemoryLimitsDisableBothTiersWithZeroMutation();

  // Round-N+ review (MEDIUM, repeat finding, "invalid cache limits
  // publicly constructible"): AssetCache::validateConfiguration()/
  // create() give a caller a way to reject an invalid Config with a
  // typed AssetErrorCode::InvalidConfiguration BEFORE ever constructing
  // anything, mirroring AssetNetworkFetcher::validateConfiguration()/
  // create()'s own already-established convention exactly.
  void validateConfigurationAndCreateAcceptEveryOrdinaryValidConfig();
  void
  validateConfigurationAndCreateRejectNegativeDiskOrMemoryLimitsAsInvalidConfiguration();

  // Round-7 review item 4 ("quota uses logical st_size and omits root
  // allocation, while policy claims physical bytes"): diskUsageBytes()
  // must report REAL, physical (block-rounded) on-disk allocation --
  // never a tiny entry's merely-logical byte count, which would badly
  // undercount actual space consumed by many small entries.
  void
  diskUsageBytesReflectsPhysicalAllocationRatherThanLogicalSizeForATinyEntry();

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

  // Round-N+ review (HIGH, repeat finding): cleanup/quota descent used
  // to reach a same-device bind mount through a bare openat() plus the
  // PERMISSIVE mountIdentityMatches() comparator at three separate
  // runtime call sites (safeRemoveEntryAt()'s recursive delete,
  // safeRemoveTreeRelative()'s own initial open, and
  // sumUsageRelative()'s recursive descent), none of which were routed
  // through the fail-closed policy construction-time hardening already
  // used. Deterministically forces BOTH legacy-kernel degradations
  // (openat2() unavailable AND STATX_MNT_ID unavailable) against an
  // entirely ordinary, unprivileged, unmounted directory tree -- no
  // real bind mount, no sudo, never skipped -- and proves cleanup and
  // quota accounting now both fail closed rather than silently treating
  // it as safe to descend.
  void
  cleanupAndQuotaDescentFailClosedWhenMountIdentificationIsUnavailableWithoutAnyRealMount();
  // Negative control for the test above: with NEITHER degradation
  // forced (the ordinary, fully-capable-kernel path this project
  // actually targets), the exact same directory tree must still clean
  // up and account normally -- proving the fail-closed behaviour above
  // is specific to the forced degradation, not a general regression.
  void
  cleanupAndQuotaDescentSucceedNormallyWhenMountIdentificationIsFullyAvailable();

  // Cumulative review (PR #18, HIGH, "disk-generation/invalidation
  // serialization is instance-local QMutex; two cache instances/
  // processes can reap each other's in-progress generations or delayed
  // 200 can revive a newer definitive 404"): a REAL, separate OS
  // process (see tests/helpers/AssetCacheLockHolderMain.cpp) is
  // spawned first and holds the cache root's exclusive cross-process
  // lock; this test process's OWN AssetCache instance over the exact
  // same root must be denied disk authority and run memory-only --
  // proving the cross-PROCESS denial path this class's header comment
  // ("Cross-process authority") describes actually holds against a
  // genuine second process, not merely a simulated/in-process stand-in.
  void secondProcessHoldingRootLockForcesThisProcessMemoryOnlyUntilReleased();
  // Same-process instances over the SAME root must instead cooperate
  // (never be denied by each other) -- the positive-control companion
  // to the test above, proving the denial above really is specific to
  // a genuinely different process, not an overzealous lock that would
  // also break ordinary same-process usage (multiple live AssetCache
  // instances over one root within a single process, exactly like
  // several existing tests in this very file already rely on).
  void
  sameProcessMultipleInstancesOverSameRootAllCooperateWithFullDiskAuthority();
  // Cumulative review (PR #18, HIGH, "same-process cache instances
  // unsynchronized" -- "dup() fd lacks CLOEXEC; exec child retains
  // root"): with the lock held by a live AssetCache instance in THIS
  // process, spawns a real child process (via QProcess, which uses
  // fork()+exec() on POSIX) and independently inspects the child's OWN
  // open-file-descriptor table for the exact fd number the parent's
  // RootAuthority holds -- proving CLOEXEC actually prevented
  // inheritance, rather than merely asserting the absence of an
  // indirect symptom.
  void execChildProcessNeverInheritsTheRootLockFileDescriptor();
  // Cumulative review (PR #18, HIGH, "same-process cache instances
  // unsynchronized" -- "duplicate LRU sequences"): two SIMULTANEOUSLY
  // LIVE same-process instances over the same root, each storing a
  // number of DIFFERENT keys, must never mint colliding
  // access-sequence values -- a prior version of this class gave each
  // instance its own private, independently-recovered counter, so two
  // live instances would both recover the SAME starting value and then
  // independently increment, guaranteeing collisions under exactly this
  // scenario. This is a deterministic outcome check (not a timing race)
  // that fails under the old per-instance-counter design and passes
  // once the counter is genuinely shared.
  void concurrentSameProcessInstancesNeverMintCollidingAccessSequenceValues();
  // Cumulative review (PR #18, HIGH, "same-process cache instances
  // unsynchronized" -- "fork child inherits registry and falsely joins
  // parent"): a real, raw fork() (never QProcess/exec) while this
  // process already holds a live root lock; the CHILD reports (via a
  // pipe, then _exit()s immediately, never running inherited C++
  // destructors) whether the raw process-wide root-lock registry
  // still appears to hold a live entry for this root from its own
  // post-fork perspective. It must NOT: this exercises the exact,
  // narrow mechanism the fix actually relies on
  // (processHasForkedSinceLastExec(), consulted by
  // rootLockRegistryHasLiveEntryForTesting() itself) via a minimal,
  // single-threaded-window read that is safe immediately post-fork.
  // Deliberately does NOT construct a further Qt object (e.g. a new
  // AssetCache) in the forked child: a live Qt process is not
  // generally safe to fork() without an immediate exec() at all
  // (verified independently -- doing so reliably SIGABRTs on this
  // platform), a hazard unrelated to this fix.
  // See constructingAssetCacheAfterSimulatedForkFailsDiskAuthorityClosed()
  // for the deterministic, non-hazardous way this file instead proves
  // the stronger "a brand-new AssetCache constructed after a fork()
  // fails disk authority closed" contract.
  void forkedChildProcessNeverJoinsParentsInheritedRootAuthority();
  // Independent cumulative re-review (MEDIUM, "fork destruction... real
  // inherited stack object normal-scope destruction under held parent
  // mutex must be addressed, not `_exit`-only test"): the test above
  // deliberately never destroys anything in its forked child (its
  // child calls ::_exit() immediately, bypassing every destructor) --
  // this test instead lets a genuinely stack-scoped, pre-fork
  // AssetCache instance be destroyed via ORDINARY C++ scope-exit
  // semantics in a real forked child, and proves ~AssetCache()'s own
  // forked-child branch terminates that process deterministically
  // (via a distinguishable ::_exit() code) from WITHIN the destructor
  // call itself, rather than letting any further code -- including any
  // other member's own destructor -- run at all.
  void
  forkedChildDestroyingInheritedStackAssetCacheTerminatesProcessDeterministically();
  // Cumulative review (independent re-review, MEDIUM, "atfork child
  // handler unsafe"): proves the review's specific demand --
  // "continuing child must fail disk authority closed on first use"
  // -- end-to-end through the REAL production entry point
  // (acquireExclusiveRootOwnershipOrFailClosed(), via a normal,
  // in-process AssetCache construction), WITHOUT a real fork() (which
  // independently SIGABRTs when combined with further Qt/heap
  // construction on this platform -- an unrelated, general hazard, not
  // a defect in this fix; see the comment above). Uses a test-only
  // hook to force processHasForkedSinceLastExec() to observe exactly
  // the same state a real atfork child handler would have left behind
  // (this process's own current pid recorded as "already forked"),
  // then restores it afterward -- deterministic, no real fork(),
  // exercises the identical guard the real forked path relies on.
  // Also proves the complementary "exec() is the only way back"
  // recovery contract: after simulating an exec() (clearing the
  // forced-fork marker), a fresh AssetCache over a fresh root regains
  // full disk authority normally.
  void constructingAssetCacheAfterSimulatedForkFailsDiskAuthorityClosed();
  // Cumulative review (independent re-review round-6, MEDIUM, "Pre-fork
  // live AssetCache objects remain usable in child"): proves the
  // per-instance hasForkedSinceConstruction() guard's actual, full
  // effect on an ALREADY-LIVE, previously-fully-functional instance --
  // using setPreForkLiveInstanceForcedStateForTesting() (see its own
  // comment) to deterministically put it into exactly the state a real
  // just-forked child's inherited copy would observe, then exercising
  // EVERY mutating/reading public method against it and asserting each
  // one fails closed as a safe no-op/miss, touching neither m_mutex,
  // m_memory, nor disk -- while the instance's OWN pre-existing entry
  // (stored before the simulated fork) remains completely untouched,
  // and the instance fully recovers to normal behavior once the forced
  // state is cleared (mirroring "require exec for fresh disk authority"
  // -- clearing the marker here plays the role a real exec() would).
  void
  preForkLiveInstanceRejectsEveryInheritedOperationBeforeTouchingStateForTesting();
  // Independent cumulative re-review (MEDIUM, "Pre-fork live AssetCache
  // objects remain usable in child" -- "Real inherited object test
  // while parent registry/mutex active"): unlike the simulated-flag
  // test immediately above, this uses a REAL fork() while a real
  // background thread genuinely holds this instance's m_mutex (via
  // lockMutexForTesting(), synchronized deterministically -- no sleep-
  // based timing race) at the exact moment of the fork(), so the
  // forked child's copy of m_mutex is guaranteed captured in a locked
  // state. The child touches ONLY the already-inherited, pre-existing
  // AssetCache object (no further Qt/heap construction of its own --
  // seeforkedChildProcessNeverJoinsParentsInheritedRootAuthority()'s
  // comment for why that would be an unrelated, independent hazard on
  // this platform) and must return the documented safe sentinel from
  // every read-only accessor without ever deadlocking, proving
  // hasForkedSinceConstruction()'s guard runs strictly before any
  // m_mutex acquisition rather than merely usually finishing fast
  // enough not to matter.
  void realForkedChildAccessorsNeverDeadlockOnMutexHeldByParentAtForkTime();
  // Cumulative review (independent re-review, HIGH, "shared root
  // authority incomplete"): two SIMULTANEOUSLY LIVE same-process
  // instances over the same root -- one sibling's invalidate() must be
  // INSTANTLY visible in another sibling's OWN memory tier (no
  // independent, staler private copy left behind at all), proving
  // memory is genuinely one shared object, not merely
  // independently-synchronized copies.
  void siblingInvalidateImmediatelyClearsAnotherSiblingsMemoryView();
  // Cumulative review (independent re-review, HIGH, "shared root
  // authority incomplete" -- "store has no token"): a token issued via
  // issueKeyGeneration() BEFORE a concurrent sibling's invalidate() for
  // the exact same key must be rejected by every one of store()/
  // touchAfterNotModified()/promoteToMemory()/updateMemoryDecodedImage()
  // afterward -- proving the exact "older pre-404 fetch can republish"
  // race the review describes is now closed for all four mutating
  // entry points, not merely one.
  void
  staleIssuedGenerationTokenCannotPublishThroughAnyMutatingEntryPointAfterConcurrentInvalidate();
  // Positive control / backward-compatibility proof: every existing
  // call site in this suite (and in production, until
  // AssetRequestCoordinator is updated) omits the new trailing
  // generation parameter entirely, relying on its default
  // (kUnconditionalGeneration) to keep behaving exactly as before --
  // unconditionally applying regardless of any concurrent invalidate.
  // This is the fail-before/pass-after pair to the test above: proves
  // the CAS protocol is opt-in per caller, never silently mandatory in
  // a way that would break every pre-existing test in this file.
  void
  unconditionalGenerationDefaultAlwaysPublishesEvenAfterConcurrentInvalidate();
  // Cumulative review (independent re-review, HIGH, "shared root
  // authority incomplete"): invalidate()'s watermark-advance must never
  // PERMANENTLY poison a key -- a freshly issued token, minted strictly
  // AFTER the invalidate(), must still succeed at every one of store()/
  // touchAfterNotModified()/promoteToMemory()/updateMemoryDecodedImage(),
  // proving advanceKeyGenerationPastAllIssuedLocked() only rejects
  // tokens that predate it, never blocks all future publication for
  // that key.
  void freshlyIssuedTokenAfterInvalidateCanStillPublishNormally();
  // Cumulative review (independent re-review round-5, HIGH, "Shared
  // authority remains non-linearizable across coordinators" -- "Old 404
  // can invalidate newer-issued/finished 200"): an OLDER-issued
  // generation's recordNegative404() attempt, arriving strictly AFTER a
  // NEWER-issued generation's store() has already published and applied
  // for the exact same key, must be rejected by the same CAS gate
  // store()/invalidate() already use -- never silently overwrite a
  // fresher success with a stale negative tombstone.
  void oldIssuedNegative404CannotClobberNewerAppliedSuccess();
  // Cumulative review (independent re-review round-5, HIGH, "negative
  // 404 is coordinator-local and can hide sibling-populated cache"): a
  // negative-404 record written under an OLDER issued generation must
  // stop being authoritative the instant a NEWER generation's store()
  // applies for the same key -- proving a sibling's fresher success is
  // never hidden behind a stale tombstone recorded earlier, even though
  // the record itself is not actively cleared (only its authoritativeness
  // check, gated on an exact generation match, changes).
  void negative404BecomesNonAuthoritativeOnceANewerSuccessAppliesForSameKey();
  // Cumulative review (independent re-review round-5, HIGH, "old 404 can
  // invalidate newer-issued/finished 200"): invalidateAndRecordNegative404()
  // is the ONE combined call dispatchCandidateFetchResult()/
  // dispatchRevalidationResult() actually use for an authoritative 404 --
  // this proves its stale-token-skip contract directly at the AssetCache
  // layer: an older-issued token's invalidate-and-record attempt, arriving
  // strictly after a newer generation has already applied a success for
  // the exact same key, must return SkippedStaleGeneration and leave
  // BOTH the newer entry AND the (absent) negative record completely
  // untouched -- proving the exact composition bug this method exists to
  // avoid (a plain invalidate() followed by a separately CAS-gated
  // recordNegative404() using the SAME already-stale token, which would
  // always self-reject and silently never write a record at all) cannot
  // recur.
  void
  invalidateAndRecordNegative404SkipsStaleGenerationLeavingNewerSuccessIntact();
  // Cumulative review (independent re-review round-5, HIGH, "cache
  // snapshot lookup then issuance in separate critical sections"): two
  // INDEPENDENT AssetCache instances sharing one root (see
  // sameProcessMultipleInstancesOverSameRootAllCooperateWithFullDiskAuthority()
  // above for the same sharing setup) -- one instance's
  // snapshotAndIssueGeneration() call must observe a fresher store()
  // another sibling instance already applied, atomically, in the SAME
  // call that mints its own fresh issuance token (never a stale read
  // paired, non-atomically, with a fresh token that could otherwise be
  // used to gate publishing that stale read).
  void
  siblingInstanceSnapshotAndIssueGenerationAtomicallyObservesOtherSiblingsPriorStore();
  // Companion to the test above, from the opposite direction: a sibling
  // instance's snapshotAndIssueGeneration() call for a key currently
  // carrying an authoritative negative-404 record (written by ANOTHER
  // sibling instance) must itself see authoritativeNegative404 == true
  // and hit == std::nullopt -- proving the negative record, like the
  // positive cache state, is genuinely shared across sibling instances
  // sharing one root, not merely readable-by-coincidence via one
  // instance's own private state.
  void
  siblingInstanceSnapshotAndIssueGenerationObservesOtherSiblingsNegative404();
  // Independent cumulative re-review (HIGH, "root authority... Prune
  // issued==applied while older token outstanding resets watermark"):
  // an OLDER token can still be genuinely outstanding (never released)
  // even after a strictly NEWER token for the identical key has already
  // been issued AND applied -- making issued==applied true for that
  // key. Before this fix, touchAndPruneKeyGenerationMapsLocked() used
  // exactly that issued==applied condition (rather than the ground-
  // truth outstanding-token set) as its eviction-eligibility check, so
  // an aggressive prune sweep (forced here via a zero tracked-entry cap
  // and a zero idle-eviction threshold) could reset this key's tracked
  // issued/applied watermark back to its 0 default WHILE the older
  // token was still outstanding -- letting that older token's own,
  // later store() attempt wrongly succeed against the reset watermark
  // instead of correctly failing its CAS against the real, newer-
  // applied generation. This test fails against that old behavior and
  // passes once eviction eligibility is gated on the outstanding-token
  // set instead.
  void
  outstandingTokenKeepsCasWatermarkAliveAcrossAggressivePruningUntilReleased();
  // Independent cumulative re-review (HIGH, repeat finding, "tryApply
  // compares highest APPLIED, not latest issued... Gen1 404 after Gen2
  // issuance is accepted and can install tombstone/advance fallback"):
  // an older token's store() must be rejected -- never wrongly
  // succeed -- while a strictly newer token for the same key is still
  // OUTSTANDING (issued, not yet applied, not yet released), even
  // though the applied watermark itself has not moved at all (still
  // its 0 default). This exact scenario passed against the pre-fix
  // code (which compared ONLY against the applied watermark) and must
  // fail once tryApplyKeyGenerationLocked() also consults the
  // outstanding-token ceiling.
  void
  oldTokenStoreCannotApplyWhileANewerTokenForTheSameKeyIsStillOutstanding();
  // Independent cumulative re-review (HIGH, repeat finding, exact
  // scenario named by the review, "Gen1 404 after Gen2 issuance is
  // accepted and can install tombstone/advance fallback"): a
  // NEGATIVE-404 tombstone attempt using an older, still-unresolved
  // token must never become authoritative while a strictly newer
  // token for the same key is still outstanding -- a subsequent
  // authoritativeness check must observe no tombstone at all, proving
  // no fallback-advancing decision could have been made from it.
  void
  oldTokenNegative404CannotBecomeAuthoritativeWhileANewerTokenIsStillOutstanding();
  // Independent cumulative re-review (HIGH, repeat finding, "Cover
  // old-404-first, both completion orders, third request..."): drives
  // BOTH adversarial completion orders end-to-end against one shared
  // key using the real coordinator-facing primitives
  // (recordNegative404()/store()), then proves a THIRD, entirely
  // independent reader (a fresh snapshotAndIssueGeneration() call,
  // modelling a concurrent third request arriving in the exact race
  // window) never observes the superseded verdict in either order --
  // only ever the genuinely newer, still-current one.
  void
  bothOld404FirstAndOldSuccessFirstOrdersLeaveOnlyTheNewerTokenVisibleToAThirdReader();
  // Independent cumulative re-review (HIGH, repeat finding,
  // "supersession uses highest currently outstanding... Maintain
  // monotonic latestIssued watermark independent of outstanding set.
  // Cancellation never retroactively authorizes older 200/304/404/
  // tombstone/fallback/delivery... Reverse test 5790-5834; cover gen1,
  // gen2 cancel, gen1 404, third request"): this test's own name and
  // final assertion are the exact reverse of its predecessor. The
  // PRIOR version proved a since-removed, now-incorrect behavior --
  // that releasing a blocking newer token without ever applying it
  // lets an older token's retry succeed again. That behavior is
  // exactly the bug this finding requires fixing:
  // latestCommittedGenerationLocked() (see its own comment in
  // AssetCache.cpp) is monotonic for every real, write-intending
  // attempt's own token and never shrinks on release, so cancellation
  // must never retroactively re-authorize an older, already-superseded
  // token for ANY mutation -- store(), the 404 tombstone, or a third
  // reader's own snapshot.
  void
  oldTokenNeverBecomesAuthoritativeAgainAfterBlockingNewerTokenIsReleasedWithoutEverApplying();
  // Independent cumulative re-review (MEDIUM, repeat finding, "release
  // does not prune... remains unbounded if no later activity"):
  // touchAndPruneKeyGenerationMapsLocked() was previously invoked ONLY
  // from issueKeyGenerationLocked()/advanceKeyGenerationPastAllIssuedLocked(),
  // never from releaseKeyGenerationLocked() -- meaning if traffic for
  // every tracked key simply STOPS forever right after each key's own
  // final outstanding token is released, no future call could ever
  // trigger a sweep to reclaim them. This test issues and releases
  // (never applies) tokens for more keys than the tracked-entry cap
  // allows, then -- with NO further issuance for ANY key at all --
  // proves the tracked-entry count is bounded, failing against the
  // pre-fix code (which left every entry behind forever) and passing
  // once release() itself can also trigger the bounded sweep.
  void
  releasingTheLastOutstandingTokenForAKeyMakesItPrunableWithNoFurtherActivity();
  // Independent cumulative re-review (MEDIUM, repeat finding, "release
  // prunes but 15-minute idle threshold leaves >4096 young entries
  // forever when activity stops... Hard cap must immediately evict
  // eligible non-outstanding entries regardless soft idle... Test
  // default production threshold, burst/release/no later activity
  // bounded"): unlike its predecessor above, this test deliberately
  // NEVER overrides the idle-eviction threshold -- it uses the exact
  // production default (15 real minutes) -- so it cannot rely on the
  // softer, idle-gated eviction path at all within any plausible test
  // runtime. It instead overrides ONLY the tracked-entry cap and the
  // separate, strictly larger unconditional hard-cap backstop (see
  // setMaxTrackedKeyGenerationEntriesHardCapForTesting()'s own comment)
  // down to small values, issues and releases (never applies) tokens
  // for more distinct keys than the hard cap allows in one burst, then
  // -- with NO further issuance for any key, and NO idle sleep either
  // -- proves the tracked map is still bounded, purely as a consequence
  // of the hard-cap backstop itself. Fails against a fix that only
  // adds condition-(2)-gated eviction to release() (Finding #2's FIRST,
  // insufficient fix) -- which would leave every one of these young,
  // just-touched entries stuck at the burst's peak size, since none of
  // them can ever satisfy the production 15-minute idle gate within
  // this test's own real running time.
  void
  releaseBurstAgainstProductionIdleThresholdIsStillBoundedByTheUnconditionalHardCap();
  // Independent cumulative re-review (MEDIUM, repeat finding, "remove
  // pathname fallback when STATX_MNT_ID unavailable; fail closed"):
  // an ordinary, otherwise perfectly owned/moded local directory must
  // be REFUSED as a qualified mount transition once this descriptor's
  // own STATX_MNT_ID is forced unavailable
  // (MountIdentificationDegradationGuard), never silently degrade to the
  // removed pathname-based mountinfo correlation. This exact fixture passed
  // under the pre-fix fallback (a genuine local directory, on this
  // environment's own real, trusted local filesystem) and must fail closed
  // post-fix.
  void
  mountTransitionFailsClosedWhenMountIdentificationIsUnavailableEvenWithOrdinaryOwnershipAndModeOnARealLocalMount();

private:
  QString m_tempDirPath;
  std::unique_ptr<QTemporaryDir> m_tempDir;
};
