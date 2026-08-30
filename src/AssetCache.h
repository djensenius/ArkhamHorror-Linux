#pragma once

#include "AssetTypes.h"

#include <QByteArray>
#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QSize>
#include <QString>
#include <QUrl>
#include <limits>
#include <memory>
#include <optional>

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
// uniquely-identified GENERATION plus one small mutable pointer:
//   {key}.{generation}.bin        -- raw encoded bytes (generation is a
//                                    unique, unpredictable per-transaction
//                                    identifier -- see
//                                    mintGenerationIdLocked()'s comment --
//                                    NEVER a hash of these bytes; the
//                                    payload's own SHA-256 is instead
//                                    recorded separately, as pure integrity
//                                    metadata, in the .meta.json file
//                                    below).
//   {key}.{generation}.meta.json  -- versioned metadata for that exact
//                                    generation (its "sha256" field is the
//                                    payload's content hash, used only to
//                                    verify integrity on every read/reap --
//                                    it is never the generation identifier
//                                    itself).
//   {key}.manifest.json           -- the ONE mutable file: which
//                                    generation is currently "live" for
//                                    this key.
// Both generation-scoped files are written via QSaveFile (temp file in
// the same directory, fsync'd before the atomic rename that commits it),
// and only once BOTH commit does the manifest itself get rewritten (also
// QSaveFile + fsync) to point at the new generation; the containing
// directory is then fsync'd too, so the rename that publishes the new
// manifest is itself durable. Because a generation's filename identity is
// a fresh, unique mint for every store() transaction -- NEVER derived
// from the payload's own bytes (round-4/5 review item 4: a
// content-addressed generation id would let a same-bytes-but-new-metadata
// replacement silently rewrite an already-live, in-use generation's files
// in place, so a concurrent reader or a failed/partial cleanup of that
// "replacement" could corrupt or delete the still-live generation) --
// publishing a new generation for a key that already has one NEVER
// touches the old generation's files at all until AFTER the manifest swap
// has fully committed -- at every crash boundary before that swap
// commits, the manifest (if it exists) still names the old, completely
// intact generation; at every boundary at or after it, the manifest names
// the new, completely intact generation. A read (or the startup/periodic
// reapAndEnforceQuota() sweep) always re-hashes the generation's payload
// and compares it against that generation's own metadata before trusting
// it, so even a manifest that somehow survives pointing at an
// incomplete/corrupt generation is never served -- and any generation
// whose files exist but are NOT the one the manifest currently names (an
// orphan left by a crash between publishing a new generation and cleaning
// up the old one, or between writing a new generation's files and ever
// reaching the manifest swap) is reclaimed by the reap sweep. A crash
// therefore always resolves deterministically to either the complete old
// generation or the complete new one -- never a half-valid mix of the
// two, and never a same-bytes replacement silently mutating a live file
// in place -- and metadata/manifest commit failure always preserves
// whatever generation was already live.
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
//
// Cross-process authority (cumulative review, PR #18, HIGH,
// "disk-generation/invalidation serialization is instance-local QMutex;
// two cache instances/processes can reap each other's in-progress
// generations or delayed 200 can revive a newer definitive 404"):
// `m_mutex` above only ever serializes threads WITHIN one process --
// it provides no protection at all against a second, genuinely separate
// PROCESS concurrently mutating the exact same on-disk directory: one
// process's reap sweep could delete files a second process's
// in-progress store()/invalidate() transaction still depends on, or a
// slow, already-in-flight revalidation in one process could durably
// resurrect a generation a second process has since authoritatively
// invalidated. Rather than attempt to make two independent processes
// safely cooperate as optimistic-CAS peers over the same directory (an
// approach that would require a durable, globally-ordered mutation
// journal covering every one of store/invalidate/touch/recover/evict,
// fsync'd and interpreted identically by every participant), this
// class instead establishes exactly ONE process-wide coordinator per
// canonical cache root (identified by (device, inode)) at a time -- see
// acquireExclusiveRootOwnershipOrFailClosed()/RootAuthority in the .cpp
// for the full mechanism: the first AssetCache instance for a given
// root in this process takes an exclusive, advisory lock
// (flock(LOCK_EX | LOCK_NB) on a CLOEXEC-flagged dup() of the root
// directory descriptor) the instant the root is opened, and constructs
// a single shared RootAuthority object (holding a genuinely SHARED
// QMutex and a genuinely SHARED LRU/generation-minting access-sequence
// counter) for it; every OTHER instance for that SAME root, in this
// SAME process, joins that SAME authority object via a std::shared_ptr
// (never merely a separate refcount alongside still-private state) --
// this instance's own m_mutex and m_nextAccessSequence are repointed,
// in the constructor, directly AT the shared authority's fields, so
// EVERY disk-mutating operation any same-root sibling performs is
// serialized through the identical mutex and mints access-sequence
// values from the identical counter, closing the exact race a prior
// version of this class had: same-process "cooperating" instances that
// still kept fully private m_mutex/m_nextAccessSequence state could
// interleave real store/invalidate/reap/touch calls against the same
// on-disk files with no mutual exclusion at all, and could each mint
// colliding LRU access-sequence values. Multiple same-process instances
// over one root remain fully supported, exactly as before -- only a
// genuinely DIFFERENT process is ever denied. The underlying lock is
// released the instant the LAST live same-process instance for that
// root (i.e. the last std::shared_ptr reference to its RootAuthority)
// is destroyed (or, on a hard crash, by the kernel's own process-exit
// fd cleanup, so a crashed owner can never leave a stale lock behind
// for another process to wait on indefinitely). A pthread_atfork()
// child-side handler unconditionally clears the process-wide registry
// the instant this process forks: a forked child (before any exec)
// must never "join" an inherited authority object as though it were a
// same-process sibling -- it is a genuinely different process from the
// kernel's (and flock()'s) own perspective the moment fork() returns,
// so any AssetCache it constructs afterward independently re-acquires
// (and, ordinarily, correctly fails to acquire, exactly like any other
// unrelated second process racing the still-live parent) rather than
// silently reusing parent-only mutex/counter state that is unsafe to
// share across a fork boundary at all. If a NEW root's lock cannot be
// acquired for ANY reason -- a different process already holds it, or
// any other failure -- that instance runs memory-only, with disk
// persistence disabled for its entire lifetime exactly as if disk I/O
// were unavailable for any other reason (m_diskCacheDisabled): it never
// reads, writes, deletes, reaps, or otherwise touches this directory. A
// contended or otherwise unprovable lock therefore can never mint a
// durable disk-cache result, and at any moment at most one live PROCESS
// ever has disk authority over a given cache root -- eliminating the
// entire class of cross-process races described above by construction,
// rather than trying to make them individually safe.
class AssetCache {
public:
  struct Config {
    // Round-9+ review (MEDIUM): a negative value here is an invalid
    // configuration, never merely "no limit" or "unlimited" -- passing
    // one disables the corresponding cache tier (memory or disk) for
    // this instance's entire lifetime, fail-closed, rather than driving
    // reapAndEnforceQuota()'s high/low-water-mark eviction math negative
    // (which would otherwise destructively evict every entry on every
    // construction and every periodic sweep). See
    // AssetCache::AssetCache()'s and configHasValidDiskByteLimit()'s
    // comments in the .cpp for the exact failure mode this closes.
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

  // Round-N+ review (MEDIUM, repeat finding, "invalid cache limits
  // publicly constructible"): pure, standalone validation -- matching
  // AssetNetworkFetcher::validateConfiguration()'s own established
  // convention exactly -- so a caller can reject an invalid `config`
  // BEFORE ever constructing anything, with a typed
  // AssetErrorCode::InvalidConfiguration rather than discovering the
  // problem only much later as a confusing, seemingly-unrelated
  // CachePersistenceFailed/CacheCorrupt surprise once disk I/O is
  // already silently disabled. Returns std::nullopt iff `config` is
  // fully valid.
  [[nodiscard]] static std::optional<AssetError>
  validateConfiguration(const Config &config);

  // Round-N+ review (MEDIUM, repeat finding): the preferred way for any
  // REAL caller (in particular AssetRequestCoordinator's own
  // production wiring) to obtain an AssetCache from a `config` that
  // might not be a hardcoded, statically-known-good literal -- fails
  // with a typed AssetErrorCode::InvalidConfiguration (via
  // validateConfiguration() above) rather than ever returning an
  // already-silently-disabled instance. AssetCache is not movable
  // (QMutex/mutable POSIX descriptor members), so a validated instance
  // is heap-allocated and returned by owning std::unique_ptr, mirroring
  // AssetNetworkFetcher::create()'s exact pattern.
  [[nodiscard]] static AssetOutcome<std::unique_ptr<AssetCache>>
  create(Config config = Config());

  // Production/test constructor: an invalid `config` (see
  // validateConfiguration() above) no longer throws or silently
  // disables just ONE tier while leaving the object otherwise
  // seemingly-normal. Instead, this instance enters a permanently
  // invalid configuration state (see isValid()/configurationError()):
  // BOTH the memory and disk tiers are disabled for this instance's
  // entire lifetime, and any AssetRequestCoordinator constructed
  // against it must -- and, in this codebase, does -- refuse to ever
  // touch it, completing every request immediately with
  // AssetErrorCode::InvalidConfiguration instead. Prefer create() over
  // this constructor directly wherever the caller can act on a typed
  // error before ever issuing a request at all; this constructor
  // remains directly usable (fail-closed rather than throwing) so
  // every existing call site that always passes valid,
  // statically-known-good configuration (the overwhelming common
  // case, including every test in this codebase predating this
  // review round) is never forced to unwrap a factory result it knows
  // can never be an error.
  explicit AssetCache(Config config = Config());
  ~AssetCache();

  [[nodiscard]] const Config &config() const { return m_config; }
  [[nodiscard]] QString directory() const { return m_directory; }

  // Round-N+ review (MEDIUM, repeat finding): true iff `config` (as
  // passed to the constructor/create()) was fully valid -- see
  // validateConfiguration(). False here means BOTH cache tiers are
  // permanently disabled for this instance; a caller that owns this
  // instance directly (rather than via AssetRequestCoordinator) should
  // check this before relying on any persistence guarantee at all.
  [[nodiscard]] bool isValid() const noexcept {
    return !m_configurationError.has_value();
  }

  // The specific typed reason isValid() is false, or std::nullopt if
  // it is true.
  [[nodiscard]] const std::optional<AssetError> &
  configurationError() const noexcept {
    return m_configurationError;
  }

  // SHA-256 (hex) over "assetcache-v1\n" + the fully-encoded resolved
  // candidate URL string. Exposed statically so callers/tests can compute
  // the same key independent of any live AssetCache instance.
  [[nodiscard]] static QString cacheKeyFor(const QUrl &resolvedCandidateUrl);

  // Cumulative review (independent re-review, HIGH, "shared root
  // authority incomplete"): sentinel `issuedGeneration` value accepted
  // by store()/touchAfterNotModified()/promoteToMemory()/
  // updateMemoryDecodedImage() meaning "apply unconditionally, entirely
  // outside the cross-instance CAS protocol" -- see issueKeyGeneration()
  // below for the real protocol this opts out of, and each of those
  // methods' own comments for why this is also each one's default.
  static constexpr quint64 kUnconditionalGeneration =
      (std::numeric_limits<quint64>::max)();

  // Mints and returns a fresh, strictly-increasing per-key ISSUANCE
  // token, shared across every same-process, same-root sibling
  // AssetCache instance (via the process-wide RootAuthority -- see its
  // own comment in the .cpp) exactly like m_mutex/m_nextAccessSequence
  // already are. A caller (in production, exclusively
  // AssetRequestCoordinator) must call this exactly once, synchronously,
  // at the moment it BEGINS an operation that may eventually publish a
  // result for `key` (a fresh network fetch or a conditional
  // revalidation), and must thread the returned token through to
  // whichever of store()/touchAfterNotModified()/promoteToMemory()/
  // updateMemoryDecodedImage() that SAME operation may eventually call
  // for `key`.
  //
  // Each of those methods only actually applies its mutation if the
  // token it was given is still >= `key`'s current applied watermark AT
  // THAT MOMENT; invalidate() (called by ANY same-root sibling, not
  // just this instance) advances that watermark strictly past every
  // token issued (by ANY sibling) up to the moment it ran -- see
  // invalidate()'s own comment -- so a token issued strictly before a
  // concurrent invalidate() can never successfully publish afterward,
  // closing the "an older, already-in-flight fetch republishes state a
  // newer, already-applied invalidate() just durably removed" race.
  // Every one of those methods defaults to kUnconditionalGeneration
  // (apply unconditionally, never checking or advancing this watermark
  // at all) for backward compatibility with every pre-existing caller
  // that never participates in this protocol.
  [[nodiscard]] quint64 issueKeyGeneration(const QString &key);

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
  //
  // Cumulative review (independent re-review, HIGH, "shared root
  // authority incomplete"): `issuedGeneration`, when supplied, gates
  // this publish behind the shared cross-instance CAS protocol -- see
  // issueKeyGeneration()'s own comment for the full contract. Defaults
  // to kUnconditionalGeneration (apply unconditionally, exactly this
  // method's entire prior behavior) so every pre-existing call site --
  // in particular, this project's own extensive direct fixture-setup
  // test suite, which seeds cache state directly and never exercises
  // two same-root sibling instances racing over one key at all --
  // continues to work with zero changes required. A caller that DOES
  // need this publish to be safely droppable by a concurrent
  // invalidate() from a same-root sibling (in production, exclusively
  // AssetRequestCoordinator) must capture a real token via
  // issueKeyGeneration() at the moment it begins the operation that
  // produced `entry`, and pass that token here instead.
  void store(const QString &key, CachedEntry entry,
             quint64 issuedGeneration = kUnconditionalGeneration);

  // After a successful conditional (304) response: refresh lastAccess (and
  // optionally a renewed ETag/Last-Modified) without touching the payload
  // bytes at all.
  //
  // See store()'s comment for `issuedGeneration`'s exact contract and
  // default.
  void
  touchAfterNotModified(const QString &key, const QString &newEtag,
                        const QString &newLastModified,
                        quint64 issuedGeneration = kUnconditionalGeneration);

  // Patches an already memory-resident entry's decodedImage in place
  // (recomputing its cost accordingly), if `key` still has one. A no-op if
  // `key` is not currently in memory (e.g. it was evicted between the
  // lookup that produced this decode and this call returning -- the next
  // lookup will simply decode again, never wrongly). Used by
  // AssetRequestCoordinator to publish a just-decoded image back into the
  // memory cache for a disk hit that never carried a decoded QImage (only
  // encodedBytes/metadata are ever persisted to disk), so a later
  // lookupMemory() hit for the same key does not need to redecode.
  //
  // See store()'s comment for `issuedGeneration`'s exact contract and
  // default.
  void
  updateMemoryDecodedImage(const QString &key, const QImage &image,
                           quint64 issuedGeneration = kUnconditionalGeneration);

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
  //
  // See store()'s comment for `issuedGeneration`'s exact contract and
  // default.
  void promoteToMemory(const QString &key, CachedEntry entry,
                       quint64 issuedGeneration = kUnconditionalGeneration);

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

  // Cumulative review (PR #18, HIGH, "same-process cache instances
  // unsynchronized" -- "dup() fd lacks CLOEXEC; exec child retains
  // root"): the exact fd number this instance's process-wide
  // RootAuthority holds for this root's advisory flock(), or -1 if this
  // instance never joined one (disk disabled / a different process
  // already owned the root). Exists purely so a test can spawn a real
  // child process (which, being CLOEXEC-protected, must NOT inherit
  // this descriptor) and independently confirm the exact fd number is
  // absent from the child's own open-file-descriptor table -- the only
  // way to positively prove CLOEXEC actually took effect, rather than
  // merely asserting the absence of an observable symptom.
  [[nodiscard]] int rootLockFileDescriptorForTesting() const;

  // Cumulative review (PR #18, HIGH, "same-process cache instances
  // unsynchronized" -- "fork child inherits registry and falsely joins
  // parent"): true iff this instance's (device, inode) currently has a
  // LIVE entry in the process-wide root-lock registry, per the raw
  // registry state (not this instance's own m_rootAuthorityHandle,
  // which would trivially always be true for any instance with disk
  // enabled). Deliberately does NOT take the registry's own mutex --
  // see this method's .cpp implementation comment for why that is safe
  // and specifically required here: it exists to be called from inside
  // a just-forked child process, immediately after fork() returns,
  // strictly BEFORE that child does anything else (including
  // constructing any further Qt objects) -- the one moment a raw,
  // lock-free snapshot read of already-copy-on-write-duplicated memory
  // is unambiguously safe, and the one moment this accessor is ever
  // used for.
  [[nodiscard]] bool rootLockRegistryHasLiveEntryForTesting() const;

  // Round-7/8 item 7: the number of times invalidate() has actually run
  // since this instance was constructed. Lets a test assert a group of
  // coalesced cache-hit decode waiters sharing a single quarantine-worthy
  // failure genuinely triggers exactly ONE invalidate() call for the
  // whole group -- see
  // AssetRequestCoordinator::completeCacheReadGroupOrQuarantine()'s comment --
  // rather than one per waiter.
  [[nodiscard]] int invalidateCallCountForTesting() const {
    return m_invalidateCallCountForTesting;
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

  // Test-only exposure of the round-9+ home-anchored, arbitrary-depth
  // directory resolver AssetCache::AssetCache() itself now uses (see
  // resolveTrustedDirectoryNoFollow()'s comment in AssetCache.cpp),
  // without needing a full AssetCache construction. Returns true iff
  // `absoluteTargetPath` resolves to a genuine, non-symlink directory
  // walking every component from a trusted starting point, with missing
  // components created only when `allowCreateMissingComponents` is
  // true -- exactly the same policy and code path
  // AssetCache::AssetCache() applies for the default-location vs
  // caller-configured-directory cases respectively.
  [[nodiscard]] static bool
  resolveTrustedDirectoryNoFollowForTesting(const QString &absoluteTargetPath,
                                            bool allowCreateMissingComponents);

  // Test-only, deterministic, UNPRIVILEGED exposure of
  // mountTransitionIsIndependentlyPolicyQualified() (AssetCache.cpp) --
  // the independent "is this mount transition's destination actually
  // policy-qualified" check (real ownership/mode plus, on Linux, a
  // kernel-recorded trusted-local-filesystem-type record) resolveHome-
  // DirectoryNoFollow() consults for EVERY mount transition it
  // considers granting. Lets a test exercise the exact decision
  // function directly against an ordinary, unprivileged directory
  // fixture (e.g. one deliberately chmod()'d group/world-writable),
  // without needing a real mount transition (and therefore without
  // needing real mount privilege) to reach it at all. Returns
  // std::nullopt if `directoryPath` itself could not even be opened
  // no-follow (a test setup failure, distinguishable from the real
  // policy decision itself).
  [[nodiscard]] static std::optional<bool>
  mountTransitionIsIndependentlyPolicyQualifiedForTesting(
      const QString &directoryPath);

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

  // Test-only, UNPRIVILEGED regression witness for the "QFile::atEnd()
  // silently truncates every /proc/self/mountinfo read to zero lines"
  // defect (see readEntireProcFileRaw()'s own comment in AssetCache.cpp
  // for the full mechanism): calls the SAME raw-read primitive
  // mountPointHasTrustedLocalFilesystemType() uses in production and
  // reports how many syntactically valid ("... - fstype ...") mount
  // entries it actually parsed. On any real Linux system this is always
  // > 0 (a process always has at least its own root mount) -- the
  // exact, deterministic, unprivileged, always-reproducible assertion
  // that would have failed under the pre-fix QFile-based
  // implementation (which always parsed exactly 0 entries, regardless
  // of the real mount table's contents) and now passes. Returns
  // std::nullopt on any non-Linux platform.
  [[nodiscard]] static std::optional<int> mountinfoParsedEntryCountForTesting();

  // Test-only, deterministic, UNPRIVILEGED injection of the two
  // degradations a legacy kernel (older than 5.6/5.8, or one built
  // without openat2()/STATX_MNT_ID support) would actually present --
  // see the two std::atomic<bool> globals this sets in AssetCache.cpp's
  // anonymous namespace for the full rationale. Lets a test exercise the
  // fail-closed strict path against perfectly ordinary, unmounted
  // directories, without requiring a real (and often privilege-gated)
  // bind mount. Both parameters independently default to false
  // (ordinary, fully-capable-kernel behaviour) when this is never
  // called; a test MUST reset both back to false before returning
  // (ideally via an RAII scope guard) so this process-wide override
  // never leaks into an unrelated, later test.
  static void
  setMountIdentificationDegradedForTesting(bool forceOpenat2Unavailable,
                                           bool forceMountIdUnavailable);

  // Test-only, deterministic override of authoritativeAccountHomeDirectory()
  // (AssetCache.cpp) -- the independent OS/account-database source
  // resolveHomeDirectoryNoFollow() consults to decide whether $HOME's own
  // mount-transition exception may be trusted (see that function's
  // comment). There is no portable, unprivileged way to make the real
  // getpwuid() return an arbitrary path for the current process's real
  // UID, so this lets a test force either answer against ordinary,
  // unprivileged fixtures: `active=true, value=<some path>` makes the
  // override behave as if the account database's home were exactly
  // `value` (empty `value` simulates "no account-database home matches,
  // ever" -- i.e. forces the strict fallback path unconditionally).
  // `active=false` (the default, and what a test MUST reset back to
  // before returning, ideally via an RAII scope guard) restores the
  // real getpwuid() lookup; production code paths never depend on this
  // outside of a test binary calling this setter.
  static void setAuthoritativeAccountHomeDirectoryOverrideForTesting(
      bool active, const QString &value = QString());

  // Test-only, deterministic, UNPRIVILEGED override of
  // mountTransitionIsIndependentlyPolicyQualified()'s independent
  // "is this mount transition's destination actually policy-qualified"
  // filesystem-type evidence (AssetCache.cpp) -- there is no portable,
  // unprivileged way to make the kernel's own /proc/self/mountinfo (or
  // its non-Linux equivalent, which does not exist in this project at
  // all) report an arbitrary filesystem type for an ordinary,
  // unprivileged test fixture. Lets a test force either answer
  // deterministically: `active=true, qualified=<value>` makes the
  // filesystem-type evidence behave as exactly `qualified`, regardless
  // of the real mount table (this does NOT bypass the real, always-
  // enforced ownership/mode check, which an unprivileged test can
  // already exercise directly via chmod()/real file ownership).
  // `active=false` (the default, and what a test MUST reset back to
  // before returning, ideally via an RAII scope guard) restores the
  // real per-platform lookup; production code paths never depend on
  // this outside of a test binary calling the setter below.
  static void setMountTransitionPolicyQualificationOverrideForTesting(
      bool active, bool qualified = false);

  // Test-only, deterministic, UNPRIVILEGED injection of an INDETERMINATE
  // (never partial) directory-listing failure -- see
  // listAllEntriesRelativeOnce()/listAllEntriesRelative()'s own comments
  // in AssetCache.cpp for the std::nullopt contract this exercises.
  // While active, every call to listAllEntriesRelative() (both attempts
  // of its own single retry, so the fault is never silently masked)
  // fails with std::nullopt, regardless of the real directory's actual
  // contents -- letting a test prove reapAndEnforceQuota() (this
  // function's sole mutating caller) aborts its ENTIRE sweep with zero
  // mutations when the listing it would act on cannot be trusted,
  // rather than proceeding against a partial/empty view. Defaults to
  // false (ordinary behaviour) when never called; a test MUST reset it
  // back to false before returning (ideally via an RAII scope guard) so
  // this process-wide override never leaks into an unrelated, later
  // test.
  static void setListAllEntriesRelativeForcedFailureForTesting(bool active);

  // Test-only, deterministic simulation of "this process has already
  // forked without an intervening exec()" -- see
  // processHasForkedSinceLastExec()'s comment in AssetCache.cpp for the
  // real mechanism this exercises (a lock-free
  // std::atomic<pid_t>, written ONLY by the real pthread_atfork()
  // child-handler in actual production). A real fork() combined with
  // further Qt/heap object construction in the child is independently
  // unsafe on this platform (verified: reliably SIGABRTs), an unrelated
  // general hazard -- this lets a test instead force the EXACT same
  // observable state a real forked-without-exec child would already be
  // in, then construct an ordinary, in-process AssetCache and exercise
  // the real production acquireExclusiveRootOwnershipOrFailClosed()
  // entry point against it, deterministically and without any real
  // fork(). `active=true` forces
  // processHasForkedSinceLastExec() to report true (as if this exact
  // process had already forked); `active=false` (the default, and what
  // a test MUST reset back to before returning, ideally via an RAII
  // scope guard) restores the real, unforced state.
  static void setForkedSinceLastExecForcedStateForTesting(bool active);

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
  // Cumulative review (independent re-review, HIGH, "shared root
  // authority incomplete"): the read half of the shared cross-instance
  // issuance/applied-watermark CAS protocol -- see issueKeyGeneration()'s
  // own (public) comment for the full contract. Callers must already
  // hold m_mutex. A key never yet published or invalidated implicitly
  // starts at generation 0.
  [[nodiscard]] quint64 currentKeyGenerationLocked(const QString &key) const;
  // The single CAS gate every one of store()/touchAfterNotModified()/
  // promoteToMemory()/updateMemoryDecodedImage()'s actual mutations goes
  // through: kUnconditionalGeneration always succeeds (and, matching its
  // own documented contract, never advances the watermark at all -- a
  // caller passing it is, by definition, opting out of this protocol
  // entirely, not merely providing a bypass value that would otherwise
  // corrupt real participants' own bookkeeping); any other value
  // succeeds -- applying it as `key`'s new watermark and returning true
  // -- iff it is not strictly less than `key`'s CURRENT watermark
  // (i.e. no invalidate() or newer-issued, already-applied publish has
  // moved past it yet); otherwise returns false, and the caller must
  // apply NONE of its intended mutation. Callers must already hold
  // m_mutex.
  [[nodiscard]] bool tryApplyKeyGenerationLocked(const QString &key,
                                                 quint64 issuedGeneration);
  // Cumulative review (independent re-review, HIGH, "shared root
  // authority incomplete"): called by invalidate() -- see its own
  // comment -- to advance `key`'s applied watermark strictly past every
  // token issueKeyGeneration() has EVER handed out for `key` up to this
  // exact moment (never merely past the current watermark by one, which
  // would NOT necessarily exceed a token issued -- but not yet
  // applied -- moments before this call), so that no already-issued
  // token can ever successfully tryApplyKeyGenerationLocked() again
  // after this runs. Also advances the issuance counter itself to match,
  // so a FUTURE issueKeyGeneration() call for the same key continues to
  // mint values that are actually capable of satisfying the new
  // watermark (never permanently stuck behind it). Callers must already
  // hold m_mutex.
  void advanceKeyGenerationPastAllIssuedLocked(const QString &key);
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
  // Round-7/8 item 7: see invalidateCallCountForTesting()'s comment.
  int m_invalidateCallCountForTesting{0};
  // Round-N+ review (MEDIUM, repeat finding): set once, at construction,
  // from validateConfiguration(m_config) -- see isValid()/
  // configurationError()'s own comment above. Never cleared afterward:
  // an instance's configuration validity is fixed for its entire
  // lifetime.
  std::optional<AssetError> m_configurationError;
  Config m_config;
  QString m_directory;
  // Cumulative review (PR #18, HIGH, "same-process cache instances
  // unsynchronized"): NEVER this instance's own private mutex when disk
  // authority is shared with same-root siblings -- see
  // m_rootAuthorityHandle's comment. Always non-null: defaults to
  // pointing at m_privateMutexFallback (this instance's own storage,
  // used whenever disk is disabled or a genuinely different process
  // already owns the root) and is repointed, in the constructor body,
  // at the shared per-root RootAuthority's mutex the moment this
  // instance successfully joins one. A raw pointer (never a reference)
  // specifically because which mutex this instance must use is only
  // known partway through the constructor BODY (after root-fd
  // resolution/locking), strictly after the member-initializer list
  // (where a reference member would have to be bound) has already run.
  mutable QMutex *m_mutex{&m_privateMutexFallback};
  mutable QMutex m_privateMutexFallback;
  // Cumulative review (independent re-review, HIGH, "shared root
  // authority incomplete"): NEVER this instance's own private memory
  // cache when disk authority is shared with same-root siblings --
  // mirrors m_mutex/m_nextAccessSequence exactly (see their own
  // comments) and RootAuthority's own comment in the .cpp for the full
  // rationale. Always non-null: defaults to
  // &m_privateMemoryFallback (used whenever disk is disabled or a
  // genuinely different process already owns the root) and is
  // repointed, in the constructor body, at the shared per-root
  // RootAuthority's own memory cache the moment this instance
  // successfully joins one.
  QCache<QString, CachedEntry> *m_memory{&m_privateMemoryFallback};
  QCache<QString, CachedEntry> m_privateMemoryFallback;
  // Cumulative review (independent re-review, HIGH, "shared root
  // authority incomplete"): the shared halves of the issuance/applied-
  // watermark CAS protocol -- see issueKeyGeneration()'s own (public)
  // comment for the full contract. Repointed alongside m_memory above.
  QHash<QString, quint64> *m_keyIssuedGeneration{
      &m_privateKeyIssuedGenerationFallback};
  QHash<QString, quint64> m_privateKeyIssuedGenerationFallback;
  QHash<QString, quint64> *m_keyAppliedGeneration{
      &m_privateKeyAppliedGenerationFallback};
  QHash<QString, quint64> m_privateKeyAppliedGenerationFallback;
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
  // Cumulative review (PR #18, HIGH, "same-process cache instances
  // unsynchronized"): non-null iff this instance successfully joined
  // (or created) the process-wide RootAuthority for
  // `m_rootDevice`/`m_rootInode` -- see
  // acquireExclusiveRootOwnershipOrFailClosed() in the .cpp for the full
  // rationale. Declared as an opaque std::shared_ptr<void> (the
  // RootAuthority type itself is private to AssetCache.cpp's anonymous
  // namespace) purely to keep this instance's OWN share of that
  // authority alive for exactly as long as this instance exists --
  // m_mutex and m_nextAccessSequence below point INTO the object this
  // holds, so their validity is tied directly to this handle's
  // lifetime, never to any separately-tracked boolean/refcount. Reset
  // (in the destructor, implicitly, via ~AssetCache()'s default
  // member-destruction order) the instant this instance goes away;
  // when the LAST same-process instance sharing a given root's
  // authority releases its handle, the authority object itself is
  // destroyed, which is what actually closes the dup'd lock descriptor
  // and releases the interprocess flock -- never gated by any separate
  // manual bookkeeping that could drift out of sync with real object
  // lifetime.
  mutable std::shared_ptr<void> m_rootAuthorityHandle;
  // Review item 11 / cumulative review (PR #18, HIGH, "same-process
  // cache instances unsynchronized"): NEVER this instance's own private
  // counter when disk authority is shared -- see
  // m_rootAuthorityHandle's comment immediately above. Always non-null:
  // defaults to m_privateAccessSequenceFallback and is repointed, in the
  // constructor body, at the shared RootAuthority's own counter the
  // moment this instance joins one, so that two simultaneously-live
  // same-root instances can never independently mint the SAME
  // access-sequence value (which would corrupt LRU ordering across
  // them). Guarded by *m_mutex (now the SAME shared mutex both
  // instances use, closing exactly that race); recovered/advanced
  // monotonically by reapAndEnforceQuota() exactly as before.
  quint64 *m_nextAccessSequence{&m_privateAccessSequenceFallback};
  quint64 m_privateAccessSequenceFallback{1};
};

} // namespace Arkham
