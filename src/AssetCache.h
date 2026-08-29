#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QSize>
#include <QString>
#include <QUrl>
#include <optional>

template <typename Key, typename T> class QCache;

namespace Arkham {

// Thread-confined (single-thread-owner; internally guarded by a mutex so
// tests may exercise it from a worker thread) bounded memory + disk cache
// for decoded/encoded asset payloads.
//
// Cache keys are SHA-256 (hex) over a versioned canonical string built
// from the fully-resolved candidate URL that was actually fetched --
// NEVER the remote path segments used directly as a local filename -- so
// a hosted and a self-hosted server (or two different locale/fallback
// candidates for the same logical AssetKey) can never collide, and no
// remote-controlled string ever becomes a local path component.
//
// On disk (review item 8), each entry is published as an immutable,
// content-addressed GENERATION plus one small mutable pointer:
//   {key}.{generation}.bin        -- raw encoded bytes (generation is the
//                                    SHA-256 hex of those exact bytes).
//   {key}.{generation}.meta.json  -- versioned metadata for that exact
//                                    generation (its own "sha256" field
//                                    always equals `generation`).
//   {key}.manifest.json           -- the ONE mutable file: which
//                                    generation is currently "live" for
//                                    this key.
// Both generation-scoped files are written via QSaveFile (temp file in
// the same directory, fsync'd before the atomic rename that commits it),
// and only once BOTH commit does the manifest itself get rewritten (also
// QSaveFile + fsync) to point at the new generation; the containing
// directory is then fsync'd too, so the rename that publishes the new
// manifest is itself durable. Because a generation's filename is
// content-addressed, publishing generation N+1 for a key that already has
// generation N NEVER touches generation N's files at all until AFTER the
// manifest swap has fully committed -- at every crash boundary before
// that swap commits, the manifest (if it exists) still names the old,
// completely intact generation; at every boundary at or after it, the
// manifest names the new, completely intact generation. A read (or the
// startup/periodic reapAndEnforceQuota() sweep) always re-hashes the
// generation's payload and compares it against that generation's own
// metadata before trusting it, so even a manifest that somehow survives
// pointing at an incomplete/corrupt generation is never served -- and any
// generation whose files exist but are NOT the one the manifest currently
// names (an orphan left by a crash between publishing a new generation
// and cleaning up the old one, or between writing a new generation's
// files and ever reaching the manifest swap) is reclaimed by the reap
// sweep. A crash therefore always resolves deterministically to either
// the complete old generation or the complete new one -- never a
// half-valid mix of the two -- and metadata/manifest commit failure
// always preserves whatever generation was already live.
//
// Metadata (not filesystem atime, which many container/build
// environments mount with atime updates disabled or coarsened) drives
// LRU eviction. Review item 11: eviction order is primarily a per-process
// MONOTONIC ACCESS SEQUENCE (`accessSeq` in the metadata JSON, recovered
// at every construction -- including a real process restart -- as
// (the maximum `accessSeq` found among all valid entries on disk) + 1,
// so a fresh instance's counter always starts strictly after every value
// any prior instance ever persisted for this same directory), not raw
// wall-clock time: `lastAccessMsecsSinceEpoch` is still recorded
// alongside it for diagnostics and as the tie-break should two entries
// somehow carry an identical sequence (only possible for legacy entries
// predating this field), with the cache key itself as the final,
// fully-deterministic tie-break. Every successful access -- a disk hit
// (lookupDisk()), a conditional-request touch (touchAfterNotModified()),
// a fresh store(), AND a MEMORY-only hit (lookupMemory()) -- mints and
// persists a fresh sequence value, so a key kept alive purely by
// repeated in-process memory hits is never mistaken by
// reapAndEnforceQuota() (which only ever consults on-disk metadata) for
// a cold, rarely-used entry. The memory-hit and disk-hit recency bumps
// are deliberately NOT fsync'd before their commit() (unlike every
// crash-consistency-relevant write elsewhere in this class): losing the
// very latest few bumps to a hard power-loss only ever affects eviction
// ORDERING, a heuristic, never the integrity of any entry's actual
// bytes. reapAndEnforceQuota() evicts oldest-access entries once disk
// usage exceeds the 90% high-water mark, down to the 75% low-water
// mark, and only ever counts an entry's bytes as freed once its files
// were actually confirmed removed -- a deletion that fails (e.g. a
// filesystem permission error) leaves that entry counted as still
// occupying its space, rather than silently believing quota was
// reclaimed when it wasn't. Quota accounting always sums payload +
// metadata file sizes together.
//
// Symlink safety: a configured cache directory that is ALREADY a symlink
// at construction time is rejected outright -- disk I/O is disabled for
// this instance's entire lifetime (m_diskCacheDisabled), so nothing is
// ever read, written, or deleted through it. Within an otherwise-genuine
// cache directory (which this class exclusively owns), the periodic
// repair sweep (reapAndEnforceQuota()) never follows a symlink to decide
// whether to recurse into it: a symlink node found directly inside the
// cache root is unlinked itself (its target is never touched), and any
// genuine subdirectory is removed using descriptor-relative, no-follow
// primitives (openat/fstatat.../unlinkat with O_NOFOLLOW/
// AT_SYMLINK_NOFOLLOW; see safeRemoveTree() in the .cpp) so that neither
// the original listing nor a symlink substituted in afterwards (a
// rename/replace race) can ever cause a deletion to escape outside this
// cache's own real files and directories.
class AssetCache {
public:
  struct Config {
    qint64 memoryMaxCostBytes;
    qint64 diskMaxBytes;
    QString directory; // empty = use QStandardPaths::CacheLocation default

    Config()
        : memoryMaxCostBytes(64LL * 1024 * 1024),
          diskMaxBytes(512LL * 1024 * 1024) {}
  };

  struct CachedEntry {
    QByteArray encodedBytes;
    QImage decodedImage;
    QString contentType;
    QSize dimensions;
    QString sha256Hex;
    QString etag;
    QString lastModified;
    qint64 insertedAtMsecsSinceEpoch{0};
    qint64 lastAccessMsecsSinceEpoch{0};

    // Accurate byte cost of BOTH representations this entry may carry:
    // the encoded bytes as downloaded, plus the decoded QImage's own
    // pixel buffer (sizeInBytes()) once/if it has been decoded. A
    // metadata-only or not-yet-decoded entry (decodedImage.isNull())
    // costs only its encoded size.
    [[nodiscard]] qint64 costBytes() const {
      qint64 cost = encodedBytes.size();
      if (!decodedImage.isNull()) {
        cost += decodedImage.sizeInBytes();
      }
      return cost;
    }
  };

  explicit AssetCache(Config config = Config());
  ~AssetCache();

  [[nodiscard]] const Config &config() const { return m_config; }
  [[nodiscard]] QString directory() const { return m_directory; }

  // SHA-256 (hex) over "assetcache-v1\n" + the fully-encoded resolved
  // candidate URL string. Exposed statically so callers/tests can compute
  // the same key independent of any live AssetCache instance.
  [[nodiscard]] static QString cacheKeyFor(const QUrl &resolvedCandidateUrl);

  // Fast path: memory-only lookup. Review item 11: on a hit, this ALSO
  // refreshes `key`'s PERSISTED on-disk recency witness (monotonic
  // access sequence + wall-clock timestamp), non-durably (see the class
  // comment) -- a memory hit is a genuine access and must count for
  // disk-eviction purposes, even though it needs no disk READ. A miss,
  // or a key with no disk record at all yet, does nothing extra. No
  // longer `const` because of this disk side effect.
  [[nodiscard]] std::optional<CachedEntry> lookupMemory(const QString &key);

  // Disk lookup: validates the payload/metadata pair (deleting either or
  // both if corrupt/orphaned), promotes a valid hit into memory (UNLESS it
  // carries a validator -- see lookupDisk()'s .cpp comment for why), and
  // refreshes its on-disk lastAccess. Returns std::nullopt on any miss,
  // including a validation failure -- CacheCorrupt is not surfaced as a
  // distinct return here because a cache miss and a corrupt-then-deleted
  // entry are handled identically by callers (both simply refetch).
  [[nodiscard]] std::optional<CachedEntry> lookupDisk(const QString &key);

  // Publishes a freshly-fetched entry to both memory and disk. `entry`'s
  // insertedAt/lastAccess timestamps are set to "now" if left at zero.
  void store(const QString &key, CachedEntry entry);

  // After a successful conditional (304) response: refresh lastAccess (and
  // optionally a renewed ETag/Last-Modified) without touching the payload
  // bytes at all.
  void touchAfterNotModified(const QString &key, const QString &newEtag,
                             const QString &newLastModified);

  // Patches an already memory-resident entry's decodedImage in place
  // (recomputing its cost accordingly), if `key` still has one. A no-op if
  // `key` is not currently in memory (e.g. it was evicted between the
  // lookup that produced this decode and this call returning -- the next
  // lookup will simply decode again, never wrongly). Used by
  // AssetRequestCoordinator to publish a just-decoded image back into the
  // memory cache for a disk hit that never carried a decoded QImage (only
  // encodedBytes/metadata are ever persisted to disk), so a later
  // lookupMemory() hit for the same key does not need to redecode.
  void updateMemoryDecodedImage(const QString &key, const QImage &image);

  // Unconditionally inserts `entry` into memory (no disk I/O at all --
  // the payload/metadata are assumed already correctly persisted).
  // Unlike lookupDisk()'s conditional promotion (which withholds
  // promoting a validator-carrying entry until it has actually been
  // revalidated), this is for a caller that has ALREADY just performed a
  // real revalidation and confirmed the entry is current: exactly the
  // same trust model lookupDisk()'s doc comment describes for
  // store()/touchAfterNotModified(). Used by AssetRequestCoordinator
  // right after a successful 304 (confirmed-unchanged) revalidation, so
  // a subsequent same-process request short-circuits via lookupMemory()
  // instead of repeating the conditional GET.
  void promoteToMemory(const QString &key, CachedEntry entry);

  // Round-6 item 6: whether invalidate() managed to durably commit
  // `key`'s tombstone -- i.e. whether the manifest naming its live
  // generation is now confirmed unlinked AND that unlink has been
  // fsync-ed to the cache root directory, so the removal survives a
  // crash immediately afterward. A caller that also plans to record a
  // bounded-TTL negative-404 for this key (AssetRequestCoordinator) MUST
  // check this: recording that negative record after a
  // PersistenceFailed result would let a stale, still-live entry
  // resurface once the record's TTL expires, since the entry was never
  // actually confirmed gone.
  enum class InvalidateResult {
    DurablyInvalidated, // No live entry for `key` remains discoverable,
                        // confirmed durable (or none ever existed).
    PersistenceFailed,  // The manifest unlink and/or the directory fsync
                        // that must follow it genuinely failed; `key`'s
                        // on-disk state is NOT confirmed gone.
  };

  // Unconditionally removes `key` from both memory and disk (payload +
  // metadata). Used when a previously-cached candidate is confirmed
  // definitively gone (an authoritative revalidation 404 -- see
  // AssetRequestCoordinator) or definitively invalid (a disk entry that
  // fails an integrity/format/limit re-check on read -- "quarantine").
  // A no-op if `key` is not currently cached anywhere; safe to call
  // repeatedly. Durability ordering (round-6 item 6): the manifest
  // naming `key`'s live generation is unlinked and the cache root
  // directory is fsync-ed BEFORE this cache's own remaining
  // generation-scoped payload/metadata files are best-effort reclaimed
  // -- so even if some of those files fail to unlink (leaving inert
  // orphans the repair sweep will reclaim later), the entry is already
  // durably invisible to every future lookup, which starts from the
  // manifest. See InvalidateResult's comment for why callers must not
  // ignore a PersistenceFailed result.
  [[nodiscard]] InvalidateResult invalidate(const QString &key);

  // Repairs orphan payloads, corrupt entries, and stray temp files, then
  // evicts oldest-access entries if disk usage exceeds the 90% high-water
  // mark, down to the 75% low-water mark. Called once from the
  // constructor; safe and idempotent to call again at any time (e.g. after
  // a simulated restart in tests).
  void reapAndEnforceQuota();

  [[nodiscard]] qint64 memoryCostBytes() const;
  [[nodiscard]] qint64 diskUsageBytes() const;
  [[nodiscard]] int diskEntryCount() const;

  // True if disk I/O has been permanently disabled for this instance
  // because the configured cache directory was already a symlink at
  // construction time (see the constructor / class comment). Exposed
  // only so tests can assert the disabled state directly rather than
  // inferring it indirectly.
  [[nodiscard]] bool isDiskCacheDisabledForTesting() const {
    return m_diskCacheDisabled;
  }

  // Test-only exposure of this cache's on-disk generation/manifest
  // layout (review item 8 -- see the class comment above), so
  // fault-injection tests can construct exact crash-boundary scenarios
  // (a generation's files present but the manifest not yet -- or still
  // -- pointing at them) by hand without duplicating this cache's own
  // path-naming logic, and without any of these three calls counting as
  // production API surface a normal caller would ever need.
  [[nodiscard]] static QString manifestPathForTesting(const QString &directory,
                                                      const QString &key);
  [[nodiscard]] static QString payloadPathForTesting(const QString &directory,
                                                     const QString &key,
                                                     const QString &generation);
  [[nodiscard]] static QString
  metadataPathForTesting(const QString &directory, const QString &key,
                         const QString &generation);

  // Test-only exposure of review item 11's monotonic access-sequence
  // witness for `key`'s current live generation (std::nullopt if `key`
  // has no valid disk record right now), so tests can assert ordering/
  // recovery directly rather than only inferring it indirectly through
  // eviction side effects.
  [[nodiscard]] std::optional<quint64>
  accessSequenceForTesting(const QString &key) const;

  // Test-only exposure of the manifest's current live generation
  // identifier for `key` (std::nullopt if no manifest names one right
  // now). Round-4/5 review item 4 deliberately decouples the generation
  // identifier from the payload's own content hash (see
  // DiskMetadata::generationId's comment) -- this accessor is how a
  // fault-injection test discovers the ACTUAL generation a real
  // store()/replacement produced (to construct
  // payloadPathForTesting()/metadataPathForTesting() paths against it),
  // rather than a test wrongly assuming the generation still equals
  // sha256(payload bytes).
  [[nodiscard]] std::optional<QString>
  currentGenerationForTesting(const QString &key) const;

  // Test-only exposure of the exact no-follow, trusted-anchor +
  // owned-suffix directory chain resolution the constructor uses
  // internally (round-6 item 5 -- see AssetCache::AssetCache()'s and
  // openDirectoryChainNoFollow()'s comments in AssetCache.cpp), without
  // needing a full AssetCache construction against this process's real
  // OS-provided cache directory (which an empty Config::directory would
  // otherwise always resolve to). Returns true iff every component of
  // `ownedSuffixComponents`, walked one at a time from
  // `trustedAnchorPath`, resolved to a genuine, non-symlink directory
  // (false on any failure, including `trustedAnchorPath` itself being
  // unopenable or a symlink). A no-op stub returning false on any
  // non-POSIX platform (this protection is POSIX-only, matching the
  // production code path it tests).
  [[nodiscard]] static bool directoryChainResolvesNoFollowForTesting(
      const QString &trustedAnchorPath,
      const QStringList &ownedSuffixComponents);

  // Test-only, UNPRIVILEGED witness for whether this build actually
  // resolved the kernel's per-mount identifier (Linux 5.8+'s statx()
  // STATX_MNT_ID) for an ordinary directory (`path`) at RUNTIME -- i.e.
  // whether the strengthened same-device-bind-mount detection described
  // by MountIdentity's comment in AssetCache.cpp is actually active,
  // rather than having silently degraded to the weaker st_dev-only
  // comparison. On any real Linux CI/desktop kernel this must be true;
  // false here (without needing any privileged bind mount to observe
  // it) is itself the regression -- see the fix for the header-
  // visibility gap this exists to catch, so it never silently recurs
  // undetected again if some future compiler/include-order change hides
  // the STATX_MNT_ID constant. Returns false unconditionally on any
  // non-Linux platform (the feature does not apply there).
  [[nodiscard]] static bool
  mountIdentificationSupportedForTesting(const QString &path);

private:
  struct DiskMetadata {
    QString key;
    QString contentType;
    qint64 encodedSize{0};
    int width{0};
    int height{0};
    QString sha256Hex;
    // Round-4/5 review item 4: the generation identifier is now a
    // per-store()-transaction unique value, deliberately independent of
    // `sha256Hex` (the payload's own content hash) -- see
    // mintGenerationIdLocked()'s comment. This field is the metadata's
    // own self-consistency witness for that identifier: every reader
    // that resolves a generation via the manifest cross-checks this
    // field against the generation named there (in addition to
    // independently re-hashing the payload against `sha256Hex`), so
    // metadata can never be silently attached to the wrong generation's
    // filename.
    QString generationId;
    QString etag;
    QString lastModified;
    qint64 insertedAtMsecsSinceEpoch{0};
    qint64 lastAccessMsecsSinceEpoch{0};
    // Review item 11: monotonic per-directory access sequence (see the
    // class comment). Defaults to 0 for metadata written by a version of
    // this class that predates this field -- readMetadata() below never
    // trusts a bare 0 as "the earliest possible real access": ties are
    // resolved by lastAccessMsecsSinceEpoch and then the cache key.
    quint64 accessSequence{0};
  };

  // Review round-4/5 item 3: these return a bare filesystem BASENAME
  // (never a directory-joined path) -- every actual disk operation
  // resolves that name relative to the already-open, anchor-verified
  // `m_rootFd` (openat/fstatat/renameat/unlinkat), never by
  // concatenating it onto `m_directory` and re-resolving the result
  // from the filesystem root. `m_directory` itself is retained only for
  // display/config/QStandardPaths-default purposes and the ONE-TIME
  // construction-time open -- never for any I/O after that.
  [[nodiscard]] QString manifestPath(const QString &key) const;
  [[nodiscard]] QString generationPayloadPath(const QString &key,
                                              const QString &generation) const;
  [[nodiscard]] QString generationMetadataPath(const QString &key,
                                               const QString &generation) const;
  // `durable`: whether the write is fsync'd before its commit(). Every
  // crash-consistency-relevant metadata write (the initial publish in
  // store(), or an etag/lastModified refresh in touchAfterNotModified())
  // passes true (the default). A pure recency-only bump with no other
  // semantic change (lookupDisk()'s lastAccess/accessSeq refresh, and
  // touchAccessRecencyLocked() below) passes false -- see the class
  // comment for why that's safe.
  [[nodiscard]] bool writeMetadata(const QString &metadataFilePath,
                                   const DiskMetadata &metadata,
                                   bool durable = true) const;
  [[nodiscard]] std::optional<DiskMetadata>
  readMetadata(const QString &metadataFilePath,
               const QString &expectedKey) const;
  // Publishes `generation` as the current one for `key`: the single
  // atomic pointer swap that makes a freshly-written generation "live"
  // (or, on first insert, live at all). See the class comment.
  [[nodiscard]] bool writeManifest(const QString &key,
                                   const QString &generation) const;
  // Reads and validates `key`'s manifest, returning the generation it
  // names iff the manifest itself is well-formed, matches `key`, and its
  // generation field is a syntactically valid (64-hex) identifier --
  // NOT whether that generation's files actually exist or are valid
  // (callers still verify that separately).
  [[nodiscard]] std::optional<QString>
  readManifestGeneration(const QString &key) const;
  // Round-6 item 6: outcome of deleteEntry() below, split into the two
  // questions its two different call-site families actually need
  // answered, which are NOT the same question:
  //  - quota eviction / corruption repair need "did every byte this key
  //    occupied actually get reclaimed" (allFilesReclaimed);
  //  - invalidate() (and, through it, an authoritative-404 negative-cache
  //    decision) needs the narrower, durability-focused "is this key's
  //    entry now confirmed, crash-durably, gone from every future
  //    lookup's perspective" (manifestDurablyAbsent) -- true as soon as
  //    the manifest itself is unlinked AND that unlink is fsync-ed,
  //    regardless of whether some orphaned generation file underneath it
  //    still failed to unlink (harmless: no lookup ever finds it without
  //    a manifest naming it, and the repair sweep reclaims it later).
  struct DeleteEntryOutcome {
    bool allFilesReclaimed;
    bool manifestDurablyAbsent;
  };

  // Unconditionally reclaims EVERY file associated with `key` --
  // its manifest and every generation-scoped payload/metadata file that
  // exists for it, live or orphaned -- via a name-prefix sweep of this
  // cache's exclusively-owned directory. Used whenever `key` is
  // confirmed to have no valid entry at all (repair, invalidate(),
  // quota eviction); NEVER used mid-replacement in store(), which must
  // keep an old-but-still-live generation's files intact until its
  // successor's manifest swap has actually committed. Ordering (round-6
  // item 6): the manifest is unlinked and fsync-ed to the root directory
  // FIRST, strictly before any other matched file is touched -- see
  // DeleteEntryOutcome's comment.
  [[nodiscard]] DeleteEntryOutcome deleteEntry(const QString &key) const;

  // Review item 11: mints the next value of this cache's per-process
  // monotonic access-sequence counter. Callers must already hold
  // m_mutex (the counter is not independently synchronized) -- see the
  // class comment for the recovery/uniqueness argument.
  [[nodiscard]] quint64 nextAccessSequenceLocked();
  // Round-4/5 review item 4: mints a fresh, unique generation identifier
  // for a single store() transaction -- see DiskMetadata::generationId's
  // comment for why this must be independent of the payload's own
  // content hash. Built from `accessSequence` (already-unique,
  // monotonic, and persisted/recovered across restarts) plus real OS
  // entropy, hashed to the same 64-lowercase-hex shape every other
  // generation identifier already uses, so no on-disk filename-pattern
  // change is required. Callers must already hold m_mutex.
  [[nodiscard]] static QString mintGenerationIdLocked(quint64 accessSequence);
  // Bumps `key`'s on-disk access recency (accessSequence +
  // lastAccessMsecsSinceEpoch) in place, without touching its payload or
  // the manifest at all, tolerating a missing/corrupt disk record as a
  // silent no-op (real repair is lookupDisk()/reapAndEnforceQuota()'s
  // job, not this purely-cosmetic bump's). Called for a memory hit
  // (lookupMemory()) and a disk hit (lookupDisk()); callers must already
  // hold m_mutex.
  void touchAccessRecencyLocked(const QString &key);

  // Review round-3 item 9: re-validates that `m_directory` still names
  // the EXACT filesystem object (device+inode) this cache anchored to a
  // retained, already-open directory descriptor at construction time
  // (see the constructor and m_rootFd's comment). Called at the top of
  // every disk-touching operation. If the path has since been replaced
  // (the directory itself renamed/removed and a new one -- possibly a
  // symlink -- created at the same path, or any ancestor component
  // replaced such that the path no longer resolves to the anchored
  // object). this call permanently disables disk I/O for the remainder
  // of this instance's lifetime (mirroring the constructor's own
  // symlink-at-construction handling) rather than silently continuing
  // to operate against a path string that may now resolve somewhere
  // else entirely. Returns false iff disk I/O is (now, or already was)
  // disabled; callers must already hold m_mutex.
  [[nodiscard]] bool verifyRootAnchorLocked() const;
  // Round-4/5 review item 3/10: fsyncs the retained root directory
  // descriptor (`m_rootFd`) DIRECTLY -- never reopening `m_directory` by
  // path -- so the two-phase publication barrier in store() (fsync
  // after the payload+metadata renames, and again after the manifest
  // rename) can never observe a different directory object than the
  // one every other operation in this class already resolves through.
  // Callers must already hold m_mutex.
  [[nodiscard]] bool fsyncRootLocked() const;
  // Review round-3 item 11: a full, unconditional inventory of every
  // byte this cache's root directory ACTUALLY currently occupies on
  // disk (files, directories, symlink-node entries -- everything,
  // regardless of whether it parses as a valid/recognized entry shape),
  // via a no-follow-symlinks recursive traversal. Unlike summing only
  // the entries reapAndEnforceQuota() has independently validated as
  // live/correct, this can never UNDERCOUNT: a stray/orphan/corrupt
  // entry that a repair pass FAILED to delete (a permission error, a
  // hostile undeletable node, a cross-device mount point it refused to
  // recurse into) still occupies real bytes and must still count
  // against quota, or eviction could stop "successfully" long before
  // actual disk usage has genuinely been brought down. Callers must
  // already hold m_mutex.
  [[nodiscard]] qint64 diskUsageBytesLocked() const;

  mutable bool m_diskCacheDisabled{false};
  Config m_config;
  QString m_directory;
  mutable QMutex m_mutex;
  QCache<QString, CachedEntry> *m_memory;
  // Review round-3 item 9: an already-open, O_DIRECTORY|O_NOFOLLOW|
  // O_CLOEXEC descriptor for `m_directory`, opened once at construction
  // and retained for this instance's entire lifetime, together with the
  // (device, inode) pair it named at that moment (via fstat() on the
  // SAME descriptor, never a second path-based stat). A file descriptor
  // continues to refer to the original filesystem object even if the
  // path that named it is later renamed, removed, or replaced by
  // something else entirely (including a symlink) -- so re-deriving the
  // current (device, inode) for `m_directory` via a fresh path-based
  // stat and comparing it against these retained values
  // (verifyRootAnchorLocked()) detects exactly that class of
  // post-construction root replacement/mount-swap, which a
  // construction-time-only symlink check cannot. -1 when unavailable
  // (disk cache disabled, or a non-POSIX platform).
  mutable int m_rootFd{-1};
  mutable quint64 m_rootDevice{0};
  mutable quint64 m_rootInode{0};
  // Review round-6 item 5: the kernel mount identifier (statx()'s
  // STATX_MNT_ID, Linux 5.8+) for `m_rootFd`'s mount, when the running
  // kernel/libc support reporting it. st_dev alone is NOT sufficient to
  // detect every cross-mount escape: a bind mount of some other
  // directory onto a path underneath the cache root shares the SAME
  // st_dev as its source filesystem (bind mounts don't create a new
  // block device), so an st_dev-only comparison can miss precisely that
  // case. m_rootHasMountId is false (and this field unused) on
  // platforms/kernels where STATX_MNT_ID isn't available; every check
  // that uses this falls back to the pre-existing st_dev-only
  // comparison in that case -- see MountIdentity's comment for the
  // documented limitation this implies on such platforms.
  mutable quint64 m_rootMountId{0};
  mutable bool m_rootHasMountId{false};
  // Review item 11: guarded by m_mutex; recovered in the constructor (via
  // reapAndEnforceQuota()'s directory scan) and re-validated (monotonic,
  // never decreasing) on every subsequent reapAndEnforceQuota() call, so
  // a fresh AssetCache instance pointed at a directory a PRIOR instance
  // already wrote to (a real process restart) never reissues a sequence
  // value any earlier instance already persisted.
  quint64 m_nextAccessSequence{1};
};

} // namespace Arkham
