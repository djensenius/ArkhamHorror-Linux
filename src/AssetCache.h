#pragma once

#include "AssetTypes.h"

#include <QByteArray>
#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QSet>
#include <QSize>
#include <QString>
#include <QUrl>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <unistd.h>

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

  // Independent cumulative re-review (HIGH, "root authority... Create
  // atomic success/304/404 authority methods returning Applied/
  // SkippedStale/PersistenceFailed"): the typed outcome every CAS-gated
  // mutating method below (store()/touchAfterNotModified()/
  // promoteToMemory()/updateMemoryDecodedImage()) now returns, replacing
  // their previous `void` return -- a caller (in production, exclusively
  // AssetRequestCoordinator) MUST inspect this and never treat its own
  // raw network/candidate result as authoritative once it sees
  // SkippedStaleGeneration: a same-root sibling already applied
  // something strictly newer for this exact key, so this call's own
  // publish was correctly discarded, and the caller must re-derive
  // whatever it delivers to its own consumers from the cache's CURRENT
  // state instead (see AssetRequestCoordinator::advanceCandidates()),
  // never from the stale result it already has in hand. This closes the
  // exact defect class the review identified: previously every one of
  // these methods silently no-op'd on a failed CAS with no way for the
  // caller to tell "genuinely applied" apart from "silently discarded as
  // stale", so a coordinator dispatch routine could (and did) go on to
  // deliver its own already-known-stale network result to its
  // subscribers, or drive a 404-triggered candidate-fallback decision,
  // regardless of whether the shared authority actually accepted the
  // publish at all.
  enum class MutationOutcome {
    Applied,                // The CAS succeeded and this mutation is now
                            // this key's current, authoritative state
                            // (memory always; disk too, unless disk
                            // persistence is disabled/degraded for this
                            // instance, which is not itself a failure --
                            // see PersistenceFailed's own comment for the
                            // distinction).
    SkippedStaleGeneration, // `issuedGeneration` was superseded by a
                            // strictly newer generation some same-root
                            // sibling already applied for this key
                            // BEFORE this call could take effect; NEITHER
                            // memory NOR disk state was touched at all.
    PersistenceFailed,      // The CAS itself succeeded (this IS now the
                            // authoritative in-memory state for this
                            // key), but the accompanying durable disk
                            // write genuinely failed (an fsync/rename/
                            // metadata-write failure, never merely "disk
                            // persistence is intentionally disabled for
                            // this instance") -- the in-memory publish
                            // this exact call produced is still safe for
                            // a caller to use for ITS OWN, currently-in-
                            // flight request, but must not be assumed
                            // durable across a restart.
  };

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
  //
  // Independent cumulative re-review (HIGH, repeat finding -- see
  // latestCommittedGenerationLocked()'s own .cpp-side comment for the
  // full rationale): unlike snapshotAndIssueGeneration()'s own internal
  // mint (used purely for atomic, may-be-immediately-discarded
  // cache/negative-404 probes), THIS bare entry point also immediately
  // commits its returned token as `key`'s new supersession ceiling --
  // because every caller of this exact method, by this comment's own
  // contract above, retains and eventually threads the token through to
  // a real mutation, never discards it. This is what makes cancelling
  // or abandoning that operation without ever publishing still
  // permanently foreclose any OLDER token for the same key, while a
  // merely-probed-and-discarded snapshotAndIssueGeneration() token never
  // does.
  [[nodiscard]] quint64 issueKeyGeneration(const QString &key);

  // Independent cumulative re-review (HIGH, "root authority... Prune
  // issued==applied while older token outstanding resets watermark"):
  // marks a token issueKeyGeneration()/snapshotAndIssueGeneration()
  // previously minted for `key` as RESOLVED -- its owning operation has
  // reached its own terminal outcome (delivered a result, been
  // cancelled, or otherwise given up on ever calling any of
  // store()/touchAfterNotModified()/promoteToMemory()/
  // updateMemoryDecodedImage()/recordNegative404()/
  // invalidateAndRecordNegative404() for this token). A caller must call
  // this EXACTLY ONCE for every token it was ever handed, regardless of
  // whether that token ever actually reached a CAS-guarded mutation call
  // or was rejected/never attempted one at all -- this is the only
  // ground-truth signal touchAndPruneKeyGenerationMapsLocked() has for
  // "no one still needs `key`'s watermark to remain in memory," and
  // omitting it for even one token would leave that key's tracking
  // state permanently unprunable (a bounded-memory regression), while
  // calling it BEFORE the token's owner is genuinely done (a
  // use-after-release) would let the exact same class of bug this
  // method exists to close (a stale-but-still-outstanding token
  // wrongly resurrected against a pruned/reset watermark) recur. A
  // no-op for `key == kUnconditionalGeneration` tokens (never tracked as
  // outstanding in the first place -- see issueKeyGeneration()'s own
  // comment on that constant) and for any token already released
  // (idempotent, so a defensive caller-side double-release, e.g. from
  // two different failure paths racing to unwind the same operation, is
  // always safe).
  void releaseKeyGeneration(const QString &key, quint64 issuedGeneration);

  // Cumulative review (independent re-review, HIGH, "shared authority
  // remains non-linearizable... negative 404 is coordinator-local and
  // can hide sibling-populated cache"): a shared, per-key negative-404
  // tombstone record -- versioned against the EXACT SAME issuance/
  // applied-watermark this file already uses for store()/invalidate()
  // (see issueKeyGeneration()'s comment) -- so an authoritative "confirmed
  // absent" record minted by one AssetRequestCoordinator/AssetCache
  // sibling instance can never survive, nor hide, a strictly newer
  // success or invalidation applied by ANY other same-root sibling.
  // Previously this bookkeeping lived entirely inside
  // AssetRequestCoordinator, keyed against that ONE coordinator
  // instance's own private generation counters -- a second coordinator
  // (or a second AssetCache instance entirely) sharing this exact root
  // had no way to observe, or be observed by, the first one's negative
  // records at all.
  struct NegativeCacheRecord {
    quint64 generation{0};
    qint64 expiresAtMonotonicMs{0};
  };

  // The result of atomically reading `key`'s current state (an
  // authoritative negative-404 tombstone, a memory/disk cache hit, or
  // neither) and minting a fresh issuance token for it, all within ONE
  // locked critical section. Cumulative review (independent re-review,
  // HIGH, "cache snapshot lookup then issuance in separate critical
  // sections allows stale v1 to receive newer token"): previously a
  // caller (AssetRequestCoordinator::request()/advanceCandidates())
  // called lookupMemory()/lookupDisk() and issueKeyGeneration()
  // SEPARATELY -- two independent lock acquisitions -- so a same-root
  // sibling's invalidate()+store() could run in between them: the token
  // minted by the second call would legitimately be current, but the
  // ENTRY already captured by the first call would be the now-stale
  // snapshot from before that concurrent mutation, and threading that
  // stale entry through a subsequent promoteToMemory()/
  // updateMemoryDecodedImage() call gated only by the (now current)
  // token would incorrectly let it publish. snapshotAndIssueGeneration()
  // closes this by construction: the read this returns and the token it
  // mints are observed under the exact same mutex acquisition, so no
  // sibling mutation can possibly be interleaved between them.
  struct KeyGenerationSnapshot {
    // A memory or disk cache hit for `key`, or std::nullopt on a genuine
    // miss. Always std::nullopt whenever `authoritativeNegative404`
    // below is true (an authoritatively-tombstoned key is never also
    // looked up for a stale positive hit).
    std::optional<CachedEntry> hit;
    // True iff `hit` was served directly from the in-process memory
    // cache (as opposed to a disk read). Callers that distinguish "a
    // same-process memory hit is trusted for this process's remaining
    // lifetime with no revalidation, regardless of any validators it
    // happens to carry" from "a disk-only hit with validators must be
    // conditionally revalidated" (see AssetRequestCoordinator::request()/
    // advanceCandidates()) need this to preserve that distinction: a
    // stored entry's etag/lastModified fields may be non-empty in EITHER
    // location (store() always populates both memory and disk from the
    // same CachedEntry), so `hit.etag`/`hit.lastModified` alone cannot
    // tell the two cases apart.
    bool hitFromMemory{false};
    // True iff a currently-authoritative (matching generation, unexpired)
    // shared negative-404 tombstone exists for `key` at the moment this
    // snapshot was taken.
    bool authoritativeNegative404{false};
    // A freshly minted issuance token for `key`, captured in the exact
    // same locked critical section as `hit`/`authoritativeNegative404`
    // above -- pass this to whichever of store()/touchAfterNotModified()/
    // promoteToMemory()/updateMemoryDecodedImage()/recordNegative404()
    // this snapshot's caller may eventually call for `key`.
    quint64 issuedGeneration{0};
  };

  // Atomically reads `key`'s current negative-404/cache-hit state and
  // mints a fresh issuance token for it -- see KeyGenerationSnapshot's
  // comment for the exact race this closes. `nowMonotonicMs` is supplied
  // by the caller (in production, AssetRequestCoordinator's own
  // injectable monotonic clock) rather than read internally, so this
  // stays deterministic under test-controlled time exactly like the
  // negative-404 TTL check already was before this record moved here.
  [[nodiscard]] KeyGenerationSnapshot
  snapshotAndIssueGeneration(const QString &key, qint64 nowMonotonicMs);

  // Records a shared negative-404 tombstone for `key`, gated by the
  // SAME cross-instance CAS protocol store()/invalidate() already use:
  // a no-op if `issuedGeneration` is no longer >= key's current shared
  // applied watermark (a same-root sibling's fresher success or
  // invalidation already superseded it) -- see issueKeyGeneration()'s
  // comment. `ttlMs` from now (`nowMonotonicMs`) bounds how long this
  // tombstone remains authoritative even if never explicitly cleared.
  // Performs an opportunistic, bounded sweep of every already-expired
  // record (across ALL keys, not just this one) each time it is called,
  // so this shared map's size stays proportional to the number of
  // currently-unexpired negative records rather than growing without
  // bound for the cache's entire lifetime.
  void recordNegative404(const QString &key, quint64 issuedGeneration,
                         qint64 nowMonotonicMs, qint64 ttlMs);

  // Unconditionally (i.e. NOT gated by any generation/CAS check at all)
  // clears any shared negative-404 tombstone for `key`.
  //
  // Independent cumulative re-review (HIGH, "root authority... Unversioned
  // clearNegative404 before CAS"): production code must NEVER call this
  // directly any more. store()'s own successful-CAS path now clears the
  // tombstone itself, from WITHIN the exact same locked critical section
  // its own tryApplyKeyGenerationLocked() CAS check just passed -- see
  // store()'s own comment. Calling this SEPARATELY (as
  // AssetRequestCoordinator's dispatch routines previously did,
  // immediately before invoking store()) is unsound: it clears the
  // tombstone unconditionally, with no generation check of its own,
  // strictly BEFORE store()'s own CAS has even run -- so a caller whose
  // OWN coordinator-local CAS happened to pass, but whose subsequent
  // store() call is then itself rejected as stale (a same-root sibling
  // applied something newer in between), would still have already
  // erased a legitimate, currently-authoritative tombstone a sibling's
  // fresher 404 had just recorded, with nothing left to restore it.
  // Retained here only as a low-level, explicitly ungated primitive for
  // direct test fixtures that need to manipulate negative-404 state
  // without going through a full store() publish at all.
  void clearNegative404(const QString &key);

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
  //
  // Independent cumulative re-review (HIGH, "root authority... store has
  // no token" / "Unversioned clearNegative404 before CAS"): returns
  // MutationOutcome (see its own comment) instead of the previous
  // `void` -- a real caller MUST inspect this before treating `entry` as
  // this key's new authoritative state. On Applied/PersistenceFailed,
  // this call ALSO clears any shared negative-404 tombstone for `key`,
  // from within the exact same locked, CAS-guarded critical section that
  // just confirmed `issuedGeneration` is current -- never as a separate,
  // ungated call (see clearNegative404()'s own comment for why that was
  // unsound).
  [[nodiscard]] MutationOutcome
  store(const QString &key, CachedEntry entry,
        quint64 issuedGeneration = kUnconditionalGeneration);

  // After a successful conditional (304) response: refresh lastAccess (and
  // optionally a renewed ETag/Last-Modified) without touching the payload
  // bytes at all.
  //
  // See store()'s comment for `issuedGeneration`'s exact contract,
  // default, and MutationOutcome return.
  [[nodiscard]] MutationOutcome
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
  // See store()'s comment for `issuedGeneration`'s exact contract,
  // default, and MutationOutcome return (this method never touches disk,
  // so it can only ever return Applied or SkippedStaleGeneration, never
  // PersistenceFailed).
  [[nodiscard]] MutationOutcome
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
  // See store()'s comment for `issuedGeneration`'s exact contract,
  // default, and MutationOutcome return (this method never touches disk
  // either, so it too can only ever return Applied or
  // SkippedStaleGeneration).
  [[nodiscard]] MutationOutcome
  promoteToMemory(const QString &key, CachedEntry entry,
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
    // Cumulative review (independent re-review round-5, HIGH, "old 404
    // can invalidate newer-issued/finished 200"): returned ONLY by
    // invalidateAndRecordNegative404() below, NEVER by the unconditional
    // invalidate() above -- `issuedGeneration` was superseded by a
    // strictly newer generation already applied for this key (e.g. a
    // same-root sibling's fresher success) BEFORE this call could take
    // effect. Neither memory/disk state nor the negative-404 record was
    // touched at all: this stale 404 is treated as if it had never been
    // observed, exactly preserving whatever the newer operation already
    // published.
    SkippedStaleGeneration,
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
  //
  // NEVER gated by a generation token -- this method ALWAYS invalidates
  // unconditionally and, via advanceKeyGenerationPastAllIssuedLocked(),
  // deliberately poisons EVERY currently-issued token for `key` (only a
  // token minted strictly AFTER this call can publish again). Callers
  // that need to gate the invalidate itself on a specific token (e.g. an
  // authoritative-404 confirmation that must never clobber a fresher
  // sibling success) must use invalidateAndRecordNegative404() instead,
  // which performs its own CAS check exactly once, atomically covering
  // both the invalidate and the negative-404 record.
  [[nodiscard]] InvalidateResult invalidate(const QString &key);

  // Cumulative review (independent re-review round-5, HIGH, "old 404 can
  // invalidate newer-issued/finished 200" / "negative 404 is
  // coordinator-local and can hide sibling-populated cache"): the
  // combined, CAS-gated equivalent of invalidate() immediately followed
  // by recordNegative404() -- but critically, as ONE atomic operation
  // under ONE mutex acquisition, gated by a SINGLE CAS check against
  // `issuedGeneration` (the same tryApplyKeyGenerationLocked() gate
  // store()/touchAfterNotModified()/promoteToMemory()/
  // updateMemoryDecodedImage() already use), never invalidate()'s own
  // unconditional advanceKeyGenerationPastAllIssuedLocked() poisoning.
  //
  // This exists because calling the separate invalidate() +
  // recordNegative404() in sequence is UNSOUND: invalidate()
  // unconditionally poisons every token issued so far for `key`
  // (including `issuedGeneration` itself, if it was minted before this
  // call, which it always is for an authoritative-404 confirmation) --
  // so a subsequent recordNegative404(key, issuedGeneration, ...) call
  // would ALWAYS be rejected by its own CAS check, silently failing to
  // ever record a negative-404 at all. Routing both through ONE CAS
  // check here avoids this self-defeating poisoning entirely: on
  // success, `issuedGeneration` becomes the new applied watermark
  // (exactly like store() does), then the entry is invalidated and the
  // negative-404 record is written under that SAME now-current
  // generation.
  //
  // On a stale `issuedGeneration` (superseded by a strictly newer
  // generation already applied -- e.g. a same-root sibling's fresher
  // success completing first), returns SkippedStaleGeneration and
  // touches NEITHER memory/disk state NOR the negative-404 record at
  // all: this stale 404 is treated as if it had never been observed,
  // exactly preserving whatever the newer operation already published.
  [[nodiscard]] InvalidateResult
  invalidateAndRecordNegative404(const QString &key, quint64 issuedGeneration,
                                 qint64 nowMonotonicMs, qint64 ttlMs);

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

  // Independent cumulative re-review (MEDIUM, "Pre-fork live AssetCache
  // objects remain usable in child" -- "Real inherited object test
  // while parent registry/mutex active"): lets a test deterministically
  // hold this instance's real m_mutex from one thread while a SEPARATE
  // real fork() happens on another, so the forked child's copy of
  // m_mutex is guaranteed (not merely probabilistically) captured in a
  // locked state -- exactly the scenario hasForkedSinceConstruction()'s
  // guards on every public accessor/mutator (checked BEFORE ever
  // touching m_mutex) must survive without ever deadlocking. Test-only:
  // a normal caller has no legitimate reason to manually lock/unlock
  // this instance's own mutex from outside its own methods.
  void lockMutexForTesting() { m_mutex->lock(); }
  void unlockMutexForTesting() { m_mutex->unlock(); }

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

  // Cumulative review (independent re-review, HIGH, "negative 404 is
  // coordinator-local... bounded pruning"): test-only exposure of the
  // shared negative-404 map's size and a specific key's current
  // authoritativeness, plus a settable hard-cap override -- mirrors the
  // equivalent test hooks AssetRequestCoordinator used to own directly
  // before this record moved into the shared authority. `nowMonotonicMs`
  // is supplied by the caller exactly like every other negative-404
  // method here, so a test's own fake clock stays authoritative.
  [[nodiscard]] int negative404RecordCountForTesting() const;
  [[nodiscard]] bool hasNegative404ForTesting(const QString &key,
                                              qint64 nowMonotonicMs) const;
  // Overrides the production ceiling (kMaxTrackedNegative404Entries) on
  // the shared negative-404 map so a high-cardinality test can exceed it
  // with a small, fast number of local round trips. 0 restores the
  // production default.
  void setMaxTrackedNegative404EntriesForTesting(int maxEntries) {
    m_maxTrackedNegative404Entries =
        maxEntries > 0 ? maxEntries : kMaxTrackedNegative404Entries;
  }

  // Independent cumulative re-review (HIGH, "root authority... m_key
  // issued/applied maps never prune"): the shared issuance/applied-
  // watermark maps (keyIssuedGeneration/keyAppliedGeneration -- see
  // issueKeyGeneration()'s own comment) previously grew, entirely
  // unbounded, by one entry per distinct cache key EVER seen for this
  // root's entire process lifetime -- unlike m_negative404, which
  // already bounds itself via recordNegative404()'s own opportunistic
  // sweep+hard-cap. pruneKeyGenerationMapsLocked() (see the .cpp) now
  // performs the equivalent bounded eviction here, opportunistically,
  // whenever a fresh issuance/apply pushes the map over
  // kMaxTrackedKeyGenerationEntries -- but, unlike negative404 (which
  // has no ordering-correctness dependency on any individual record's
  // survival), this eviction is restricted to only ever remove a key
  // whose issued and applied watermarks are CURRENTLY EQUAL (i.e.
  // nothing is known to be outstanding for it at the moment of the
  // sweep), oldest-touched first, so a key with a genuinely in-flight
  // (issued-but-not-yet-applied) token is never evicted regardless of
  // how large the map grows or how long ago that key was last touched.
  [[nodiscard]] int trackedKeyGenerationEntryCountForTesting() const;
  // Overrides the production ceiling (kMaxTrackedKeyGenerationEntries)
  // the same way setMaxTrackedNegative404EntriesForTesting() does. 0
  // restores the production default.
  void setMaxTrackedKeyGenerationEntriesForTesting(int maxEntries) {
    m_maxTrackedKeyGenerationEntries =
        maxEntries > 0 ? maxEntries : kMaxTrackedKeyGenerationEntries;
  }
  // Independent cumulative re-review (MEDIUM, repeat finding, "release
  // prunes but 15-minute idle threshold leaves >4096 young entries
  // forever when activity stops... Hard cap must immediately evict
  // eligible non-outstanding entries regardless soft idle"): overrides
  // the unconditional backstop ceiling (kMaxTrackedKeyGenerationEntriesHardCap
  // -- see its own comment on the private field) the same way
  // setMaxTrackedKeyGenerationEntriesForTesting() overrides the soft
  // cap, so a test can force the unconditional (never idle-gated)
  // backstop to trigger with a small, fast number of local round trips
  // instead of thousands, using the PRODUCTION idle-eviction threshold
  // unchanged. 0 restores the production default.
  void setMaxTrackedKeyGenerationEntriesHardCapForTesting(int maxEntries);
  // Independent cumulative re-review (HIGH, "root authority... Track
  // in-flight tokens and bounded prune"):
  // touchAndPruneKeyGenerationMapsLocked() (see its own .cpp comment)
  // additionally requires a key to have been quiescent for at least this many
  // milliseconds, by a real std::chrono::steady_clock reading (see
  // keyGenerationIdleNowMs()), before it becomes eviction-eligible -- a large
  // default (see kDefaultKeyGenerationIdleEvictionThresholdMs) makes it
  // overwhelmingly implausible for any genuinely still-outstanding real network
  // operation (bounded by this project's own request timeouts, always
  // far shorter) to still be in flight for a key by the time it is
  // considered idle enough to evict its bookkeeping. Settable to a tiny
  // value (e.g. 0) purely so a test can exercise the eviction path
  // itself deterministically, without a real sleep.
  static constexpr qint64 kDefaultKeyGenerationIdleEvictionThresholdMs =
      15 * 60 * 1000;
  void setKeyGenerationIdleEvictionThresholdMsForTesting(qint64 thresholdMs) {
    m_keyGenerationIdleEvictionThresholdMs =
        thresholdMs >= 0 ? thresholdMs
                         : kDefaultKeyGenerationIdleEvictionThresholdMs;
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
  //
  // `isFinalAccountHomeTransition` (round-6, MEDIUM, "position-
  // sensitive ownership"): true (the default every existing caller of
  // this test hook already assumes) exercises the FINAL, account-home
  // ownership check (current-uid); false exercises the ANCESTOR
  // ownership check (root-owned) instead -- see
  // mountTransitionIsIndependentlyPolicyQualified()'s own comment.
  //
  // `parentDirectoryPath` (independent cumulative re-review, MEDIUM,
  // "arbitrary same-device bind mount still passes"): the directory
  // this transition is modelled as being grafted ONTO, whose own mount
  // identity is looked up and passed through to the exact same
  // same-device rejection and mountinfo parent-id authentication the
  // real resolveHomeDirectoryNoFollow() walk now performs. When
  // omitted (the default every existing caller of this test hook
  // already assumes, all of which are concerned with orthogonal
  // ownership/mode/filesystem-type behaviour, never this specific
  // check), a SYNTHETIC parent identity with a device deliberately
  // guaranteed to differ from `directoryPath`'s own is used instead,
  // so this newer check can never incidentally mask what those
  // existing tests actually exercise.
  [[nodiscard]] static std::optional<bool>
  mountTransitionIsIndependentlyPolicyQualifiedForTesting(
      const QString &directoryPath, bool isFinalAccountHomeTransition = true,
      const QString &parentDirectoryPath = QString());

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

  // Test-only, deterministic, UNPRIVILEGED override of
  // componentPassesOwnershipModePolicy()'s raw fstat()-based
  // ANCESTOR-position ownership/mode decision (AssetCache.cpp) -- there
  // is no portable, unprivileged way to make a real directory
  // genuinely owned by root (uid 0) for an ordinary, unprivileged test
  // fixture, so the "an ordinary, legitimately root-provisioned
  // ancestor passes" branch is otherwise untestable hermetically. Lets
  // a test force EITHER answer deterministically for every ANCESTOR
  // component (`active=true, passes=<value>`), regardless of that
  // component's real owning uid/mode. This deliberately does NOT
  // affect the FINAL-account-home-component decision at all, which
  // ALWAYS uses the real, unmodified check -- an unprivileged test
  // process genuinely owns its own final-home fixture directories
  // already, and can make one group/world-writable via a real
  // chmod(), so that decision needs no override to prove either
  // acceptance or rejection for real. This also does NOT need to be
  // used at all to prove ANCESTOR rejection (wrong owner,
  // group/world-writable) -- an unprivileged test process's own
  // directories already genuinely fail the "owned by root" / "never
  // writable by group or other" ancestor requirement without any
  // override. `active=false` (the default, and what a test MUST reset
  // back to before returning, ideally via an RAII scope guard) restores
  // the real, unmodified fstat()-based ancestor check; production code
  // paths never depend on this outside of a test binary calling the
  // setter below.
  static void
  setHomeComponentOwnershipModePolicyOverrideForTesting(bool active,
                                                        bool passes = false);

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

  // Test-only, deterministic simulation of "THIS already-live instance
  // (constructed before some fork()) is now being called through by a
  // just-forked child" -- see hasForkedSinceConstruction()'s comment for
  // the real, per-instance mechanism (a lock-free comparison of the pid
  // captured at construction against the current ::getpid()) this
  // exercises. Unlike setForkedSinceLastExecForcedStateForTesting()
  // above (which simulates the state a NEW instance's construction path
  // would observe), this lets a test exercise the full, ALREADY-LIVE
  // production object's public methods (lookupMemory()/store()/
  // invalidate()/etc.) as they would actually behave in a real forked
  // child -- deterministically and without needing a real (and,
  // verified elsewhere in this file, SIGABRT-hazardous when it goes on
  // to construct further Qt/heap state) fork() of this Qt-using test
  // binary. A process-wide override (mirrors
  // setForkedSinceLastExecForcedStateForTesting()'s own design): when
  // active, EVERY AssetCache instance's hasForkedSinceConstruction()
  // reports true, regardless of its own actual m_constructionPid.
  // `active=false` (the default, and what a test MUST reset back to
  // before returning, ideally via an RAII scope guard) restores each
  // instance's own real, per-instance comparison.
  static void setPreForkLiveInstanceForcedStateForTesting(bool active);

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

  // Cumulative review (independent re-review round-5, HIGH, "old 404 can
  // invalidate newer-issued/finished 200"): the shared body of
  // recordNegative404()'s write-and-prune logic (writing the record
  // itself, then the bounded TTL sweep and hard-cap eviction), factored
  // out so invalidateAndRecordNegative404() can reuse it from within its
  // own single mutex acquisition without duplicating the pruning logic.
  // Callers must already hold m_mutex and must have already validated
  // `issuedGeneration` via tryApplyKeyGenerationLocked() themselves.
  void writeNegative404RecordLocked(const QString &key,
                                    quint64 issuedGeneration,
                                    qint64 nowMonotonicMs, qint64 ttlMs);

  // Review item 11: mints the next value of this cache's per-process
  // monotonic access-sequence counter. Callers must already hold
  // m_mutex (the counter is not independently synchronized) -- see the
  // class comment for the recovery/uniqueness argument.
  [[nodiscard]] quint64 nextAccessSequenceLocked();
  // The locked body of the public issueKeyGeneration() above, factored
  // out so snapshotAndIssueGeneration() can mint a token from WITHIN its
  // own single mutex acquisition. Callers must already hold m_mutex.
  [[nodiscard]] quint64 issueKeyGenerationLocked(const QString &key);
  // The locked body of the public releaseKeyGeneration() above. Callers
  // must already hold m_mutex.
  void releaseKeyGenerationLocked(const QString &key, quint64 issuedGeneration);
  // Cumulative review (independent re-review, HIGH, "shared root
  // authority incomplete"): the read half of the shared cross-instance
  // issuance/applied-watermark CAS protocol -- see issueKeyGeneration()'s
  // own (public) comment for the full contract. Callers must already
  // hold m_mutex. A key never yet published or invalidated implicitly
  // starts at generation 0.
  [[nodiscard]] quint64 currentKeyGenerationLocked(const QString &key) const;
  // Independent cumulative re-review (HIGH, repeat finding: "supersession
  // uses highest currently outstanding; cancel/release gen2 makes
  // delayed gen1 authoritative again... Maintain monotonic latestIssued
  // watermark independent of outstanding set", then, on the SAME finding
  // recurring against the first fix, "PendingCacheDecode's shared
  // representative... second or third joiner's own mere issuance would
  // permanently and spuriously fail the whole group's eventual publish"
  // and "delayedStaleRevalidationSuccess...", "coalescedRevalidation..."
  // regressions caused by literally treating m_keyIssuedGeneration
  // itself -- which every mere READ (snapshotAndIssueGeneration(), used
  // for mint-then-immediately-release probe-and-defer decisions such as
  // "check cache, decide to join an existing CandidateAttempt/revalidate
  // instead") also advances -- as the ceiling): `key`'s highest token
  // ever explicitly COMMITTED as a real, competing write-intending
  // attempt's own designated token via commitKeyGenerationLocked() (see
  // its own comment), or 0 if none has. This is deliberately NARROWER
  // than "highest ever issued": issueKeyGenerationLocked() itself mints
  // a strictly-increasing value for EVERY caller, including a caller
  // that immediately releases it without ever using it for anything
  // (e.g. AssetRequestCoordinator::advanceCandidates()'s own cache-hit/
  // negative-404 probe, which frequently discovers it should defer
  // entirely to an already-in-flight CandidateAttempt bearing a LOWER,
  // still-valid token) -- committing on every mere issuance made EVERY
  // such harmless, unused probe permanently poison every genuinely
  // still-outstanding, LOWER real attempt for the same key, which is
  // exactly the regression this narrower design fixes. Only
  // issueKeyGeneration() (the bare public entry point -- used
  // exclusively where the resulting token IS actually retained as a
  // real attempt's own designated token: AssetRequestCoordinator's
  // CandidateAttempt creation in startCandidate()/startRevalidation(),
  // and any direct caller such as this class's own tests) commits
  // automatically; snapshotAndIssueGeneration()'s own internal mint
  // deliberately does not.
  //
  // This remains a value that can only ever grow for a given live key --
  // unlike the outstanding-token-set-based ceiling a previous fix used,
  // releasing (or even destroying) some OTHER, strictly newer COMMITTED
  // token's operation can never make this value regress, so a strictly
  // newer real attempt, once its token is committed, permanently
  // forecloses every older token's ability to ever apply again for this
  // key -- even if that newer attempt is later cancelled or abandoned
  // without ever publishing anything itself. This is exactly the
  // semantics the finding requires: cancellation must never
  // retroactively re-authorize an older, already-superseded operation --
  // while a mere, never-retained probe committing NOTHING means it can
  // never falsely supersede a genuinely still-live, lower-numbered real
  // attempt either.
  //
  // The still-earlier outstanding-set-based ceiling existed specifically
  // to avoid a livelock a still-earlier "highest ever issued" attempt
  // supposedly caused: "a rejected, retried operation that mints a
  // fresh token on every retry would otherwise ratchet the watermark up
  // forever." That concern does not apply here either: every rejection
  // (`SkippedStaleGeneration`) is handled by
  // AssetRequestCoordinator::advanceCandidates()/completeCacheReadGroupOrQuarantine(),
  // which ALWAYS resnapshots the cache (a fresh, atomic,
  // non-committing snapshotAndIssueGeneration()/lookup) BEFORE ever
  // minting a new, committed token, and a fresh network attempt for the
  // SAME resource only ever mints (and commits) a new token via
  // startCandidate()/startRevalidation() if no CandidateAttempt is
  // already in flight for that exact (cacheKey, format, etag,
  // lastModified) identity (see candidateAttemptKey()'s own comment) --
  // an already-in-flight attempt is joined as an additional subscriber
  // instead, never given a second, independently-committing token. So
  // this ceiling can only ever advance in step with a genuinely NEW,
  // distinct real attempt actually starting for a key, never as an
  // unbounded artifact of retrying the same one, and never as an
  // artifact of an unrelated, never-retained probe read either.
  [[nodiscard]] quint64
  latestCommittedGenerationLocked(const QString &key) const;
  // Independent cumulative re-review (HIGH, repeat finding, same as
  // latestCommittedGenerationLocked()'s own comment): explicitly records
  // `token` as `key`'s designated real write-intending attempt token,
  // advancing m_keyCommittedGeneration[key] to
  // std::max(current, token) -- monotonic, exactly like
  // m_keyIssuedGeneration/m_keyAppliedGeneration, and safe to call
  // redundantly (e.g. once per PendingCacheDecode joiner-adopts event)
  // with a value that is not strictly greater than the current one: a
  // no-op in that case. Called automatically by issueKeyGeneration()
  // (never by issueKeyGenerationLocked()'s raw mint alone, and never by
  // snapshotAndIssueGeneration()) -- see issueKeyGeneration()'s own
  // comment -- and also by advanceKeyGenerationPastAllIssuedLocked() (so
  // invalidate() continues to permanently foreclose every already-
  // issued token exactly as before, now via this ceiling instead of the
  // raw issuance counter). Callers must already hold m_mutex.
  void commitKeyGenerationLocked(const QString &key, quint64 token);
  // The single CAS gate every one of store()/touchAfterNotModified()/
  // promoteToMemory()/updateMemoryDecodedImage()'s actual mutations goes
  // through: kUnconditionalGeneration always succeeds (and, matching its
  // own documented contract, never advances the watermark at all -- a
  // caller passing it is, by definition, opting out of this protocol
  // entirely, not merely providing a bypass value that would otherwise
  // corrupt real participants' own bookkeeping); any other value
  // succeeds -- applying it as `key`'s new watermark and returning true
  // -- iff it is STRICTLY neither less than `key`'s CURRENT applied
  // watermark (i.e. no invalidate() or newer-issued, already-applied
  // publish has moved past it yet) NOR less than the highest token EVER
  // COMMITTED for `key` other than itself (see
  // latestCommittedGenerationLocked()'s own comment: a strictly newer
  // real attempt for this exact key, once its token is committed, must
  // never have its supersession of an older one undone merely because
  // that newer attempt was itself later cancelled or abandoned) --
  // otherwise returns false, and the caller must apply NONE of its
  // intended mutation, and must treat this exact result as "some other
  // operation for this key is more authoritative than mine; resnapshot
  // and defer to it, never deliver my own now-superseded result."
  // Callers must already hold m_mutex.
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
  // after this runs. Also advances the issuance counter AND the
  // committed-generation ceiling themselves to match, so a FUTURE
  // issueKeyGeneration() call for the same key continues to mint values
  // that are actually capable of satisfying the new watermark (never
  // permanently stuck behind it), and so this invalidate() itself
  // permanently forecloses every already-issued token exactly like a
  // committed newer attempt would, via commitKeyGenerationLocked().
  // Callers must already hold m_mutex.
  void advanceKeyGenerationPastAllIssuedLocked(const QString &key);
  // Independent cumulative re-review (HIGH, "root authority... Prune
  // issued==applied while older token outstanding resets watermark"):
  // records `key`'s entry in the shared "last touched" real
  // steady-clock-ms map (m_keyGenerationLastTouchSteadyMs) as touched
  // right now, then, only once the tracked-key count exceeds
  // kMaxTrackedKeyGenerationEntries, evicts the oldest-touched keys down
  // to a low-water mark -- but ONLY keys that satisfy BOTH of:
  //   (1) m_keyOutstandingGeneration[key] is empty -- no token
  //       issueKeyGeneration() has ever minted for `key` remains
  //       un-released (see releaseKeyGeneration()'s own comment). This
  //       is genuine ground truth, not a heuristic: a previous version
  //       of this method instead checked "issued == applied watermark,"
  //       which is NOT a reliable proxy for "nothing outstanding" -- a
  //       token minted long before the current applied watermark can
  //       still be genuinely in flight (its holder simply hasn't
  //       finished yet) even while a LATER token has already been
  //       issued AND applied for the same key, making
  //       issued == applied true despite that earlier token's holder
  //       still being able to call tryApplyKeyGenerationLocked() at any
  //       moment; evicting `key`'s watermark in that state resets it to
  //       0 (via the default-0 lookup every reader here uses), letting
  //       that stale-but-still-outstanding token wrongly satisfy the
  //       CAS against an amnesia'd watermark instead of the real,
  //       already-superseded one it should be compared against. AND
  //   (2) idle for at least m_keyGenerationIdleEvictionThresholdMs by a
  //       REAL std::chrono::steady_clock reading (keyGenerationIdleNowMs()
  //       -- never the caller-supplied, test-fakeable "nowMonotonicMs"
  //       negative404 uses, since that value is under a TEST's control
  //       and could otherwise be used to defeat this safeguard).
  // Condition (1) alone is sufficient to prove no operation can still
  // successfully publish against `key`'s current watermark once evicted
  // (every token that could ever call tryApplyKeyGenerationLocked() for
  // it has already been released); condition (2) exists purely to bound
  // how eagerly an otherwise-idle key's bookkeeping is reclaimed, not to
  // paper over any remaining correctness gap.
  //
  // Independent cumulative re-review (MEDIUM, repeat finding, "release
  // prunes but 15-minute idle threshold leaves >4096 young entries
  // forever when activity stops... Hard cap must immediately evict
  // eligible non-outstanding entries regardless soft idle"): condition
  // (2) above is DROPPED once the tracked map exceeds the strictly
  // larger, unconditional m_maxTrackedKeyGenerationEntriesHardCap
  // ceiling (see its own comment) -- only condition (1) still applies in
  // that case -- so this very sweep, triggered synchronously by
  // whichever issue/apply/release call pushed the map over the hard
  // cap, can bound the map's size entirely on its own, with no
  // dependence on any future call ever occurring, regardless of how
  // recently every excess key happened to be last touched. This closes
  // the exact gap a single burst of activity across many distinct keys
  // -- each released before any later traffic ever arrives -- left open
  // under the production (15-real-minute) idle threshold: previously,
  // every one of those young entries could remain tracked forever.
  // Called from issueKeyGenerationLocked() and
  // tryApplyKeyGenerationLocked() -- the only two places that ever touch
  // a key's issued/applied watermark -- so every key that is ever
  // issued or applied at all is covered. Callers must already hold
  // m_mutex.
  void touchAndPruneKeyGenerationMapsLocked(const QString &key);
  // Real (never test-fakeable) std::chrono::steady_clock reading in
  // milliseconds, used exclusively by touchAndPruneKeyGenerationMapsLocked()'s
  // idle-eviction-eligibility check -- see its own comment for why this
  // must be independent of any caller-suppliable clock.
  [[nodiscard]] static qint64 keyGenerationIdleNowMs();
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
  // Cumulative review (independent re-review, HIGH, "cache snapshot
  // lookup then issuance in separate critical sections"): the locked
  // bodies of lookupMemory()/lookupDisk() above, factored out so
  // snapshotAndIssueGeneration() can call them from WITHIN its own
  // single mutex acquisition (never re-entering/re-acquiring m_mutex,
  // which QMutex does not support). lookupMemory()/lookupDisk()
  // themselves now trivially acquire m_mutex once and delegate here,
  // preserving their exact previous externally-observable behavior.
  // Callers must already hold m_mutex.
  [[nodiscard]] std::optional<CachedEntry>
  lookupMemoryLocked(const QString &key);
  [[nodiscard]] std::optional<CachedEntry> lookupDiskLocked(const QString &key);
  // The disk-only portion of lookupDiskLocked() (i.e. everything after
  // its own leading memory check), factored out so
  // snapshotAndIssueGeneration() can perform its OWN explicit memory
  // check first (to accurately populate
  // KeyGenerationSnapshot::hitFromMemory -- see its comment) and, only
  // on a memory miss, fall through to exactly the disk read
  // lookupDiskLocked() itself would have reached, without redundantly
  // re-checking memory a second time. Callers must already hold
  // m_mutex.
  [[nodiscard]] std::optional<CachedEntry>
  lookupDiskOnlyLocked(const QString &key);

  // Cumulative review (independent re-review round-6, MEDIUM, "Pre-fork
  // live AssetCache objects remain usable in child"): checked at the
  // TOP of every public operation, strictly BEFORE any QMutexLocker or
  // any other touch of m_memory/disk state -- see m_constructionPid's
  // own comment for the full rationale. A lock-free, single-field
  // comparison; safe to call at any time, including concurrently from
  // multiple threads (though only ever meaningfully "true" for every
  // thread simultaneously the instant a fork() has actually happened).
  // Implemented in the .cpp (rather than inline here) so it can consult
  // the same test-only forced-override mechanism
  // setPreForkLiveInstanceForcedStateForTesting() below installs --
  // exercising a genuinely already-live instance's full public-method
  // fail-closed behavior deterministically, without needing a real (and
  // independently verified elsewhere in this file to be SIGABRT-
  // hazardous) fork() of this Qt-using test binary.
  [[nodiscard]] bool hasForkedSinceConstruction() const noexcept;

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
  // Cumulative review (independent re-review round-6, MEDIUM, "Pre-fork
  // live AssetCache objects remain usable in child"): the PID captured
  // the instant THIS object's constructor ran, compared against
  // ::getpid() at the top of EVERY public operation below via
  // hasForkedSinceConstructionLocked()/rejectIfForkedSinceConstruction().
  // Unlike processHasForkedSinceLastExec() (a process-wide, atfork-
  // handler-driven flag guarding NEW instance construction/root
  // acquisition -- see registerForkSafetyOnce()'s comment), this is a
  // simple, per-instance, lock-free comparison: a bare fork() always
  // produces a child with a genuinely different PID from its parent
  // (POSIX-guaranteed, no atfork registration required to observe this
  // correctly), so an ALREADY-CONSTRUCTED instance -- one that predates
  // the fork and is still being called through by the child, which the
  // process-wide flag alone does not guard against -- can reliably
  // detect "I am now running in a different process than the one that
  // constructed me" without ever needing to touch m_mutex (whose state
  // is undefined post-fork if any other thread held it at fork time) or
  // any inherited disk/memory state to make that determination.
  const ::pid_t m_constructionPid{::getpid()};
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
  // Independent cumulative re-review (HIGH, repeat finding: "highest
  // currently outstanding" / "highest ever issued" both proven unsound
  // -- see latestCommittedGenerationLocked()'s own comment for the full
  // rationale): the shared ceiling that ONLY a real, write-intending
  // attempt's own designated token (never a discarded probe) ever
  // advances. Repointed alongside m_keyIssuedGeneration/
  // m_keyAppliedGeneration above (same RootAuthority, same sharing
  // rules) -- it must be shared cross-instance for exactly the same
  // reason those two are: a sibling AssetCache instance for the same
  // physical root committing a newer attempt's token must permanently
  // foreclose an older, still-outstanding attempt in THIS instance too.
  QHash<QString, quint64> *m_keyCommittedGeneration{
      &m_privateKeyCommittedGenerationFallback};
  QHash<QString, quint64> m_privateKeyCommittedGenerationFallback;
  // Independent cumulative re-review (HIGH, "root authority... m_key
  // issued/applied maps never prune"): the shared "last touched" real
  // steady-clock-ms map touchAndPruneKeyGenerationMapsLocked() uses to
  // decide both eviction ORDER (oldest-touched first) and eviction
  // ELIGIBILITY (only once idle at least
  // m_keyGenerationIdleEvictionThresholdMs) -- see its own comment.
  // Repointed alongside m_keyIssuedGeneration/m_keyAppliedGeneration
  // above (same RootAuthority, same sharing rules).
  QHash<QString, qint64> *m_keyGenerationLastTouchSteadyMs{
      &m_privateKeyGenerationLastTouchSteadyMsFallback};
  QHash<QString, qint64> m_privateKeyGenerationLastTouchSteadyMsFallback;
  // Independent cumulative re-review (HIGH, "root authority... Prune
  // issued==applied while older token outstanding resets watermark"):
  // the actual, ground-truth set of tokens issueKeyGeneration() has
  // handed out for a key that have NOT yet reached releaseKeyGeneration()
  // -- see both methods' own comments. `issued == applied` is NOT a
  // reliable proxy for "nothing outstanding": a token minted long before
  // the current applied watermark can still be genuinely in flight (its
  // holder simply hasn't finished yet) even while a LATER token has
  // already been issued AND applied for the same key, making
  // issued == applied true despite that earlier token's holder still
  // being able to call tryApplyKeyGenerationLocked() at any moment.
  // touchAndPruneKeyGenerationMapsLocked() now consults this set
  // directly instead. Repointed alongside m_keyIssuedGeneration/
  // m_keyAppliedGeneration above (same RootAuthority, same sharing
  // rules).
  QHash<QString, QSet<quint64>> *m_keyOutstandingGeneration{
      &m_privateKeyOutstandingGenerationFallback};
  QHash<QString, QSet<quint64>> m_privateKeyOutstandingGenerationFallback;
  // Cumulative review (independent re-review, HIGH, "negative 404 is
  // coordinator-local and can hide sibling-populated cache"): the
  // shared negative-404 tombstone map -- see NegativeCacheRecord's own
  // comment. Repointed alongside m_memory/m_keyIssuedGeneration/
  // m_keyAppliedGeneration above.
  QHash<QString, NegativeCacheRecord> *m_negative404{
      &m_privateNegative404Fallback};
  QHash<QString, NegativeCacheRecord> m_privateNegative404Fallback;
  // Cumulative review (independent re-review, HIGH, "bounded pruning"):
  // hard ceiling on m_negative404's size enforced by recordNegative404()
  // -- see its own comment. A per-instance (never shared) threshold,
  // consistent with every other configured limit on this class (e.g.
  // Config's own byte/count limits) never being shared via
  // RootAuthority either.
  static constexpr int kMaxTrackedNegative404Entries = 4096;
  int m_maxTrackedNegative404Entries{kMaxTrackedNegative404Entries};
  // Independent cumulative re-review (HIGH, "root authority... m_key
  // issued/applied maps never prune"): per-instance (never shared, same
  // rationale as kMaxTrackedNegative404Entries above) ceiling on the
  // shared keyIssuedGeneration/keyAppliedGeneration/
  // keyGenerationLastTouchSteadyMs maps' combined size, enforced by
  // touchAndPruneKeyGenerationMapsLocked().
  static constexpr int kMaxTrackedKeyGenerationEntries = 4096;
  int m_maxTrackedKeyGenerationEntries{kMaxTrackedKeyGenerationEntries};
  // Independent cumulative re-review (MEDIUM, repeat finding, "release
  // prunes but 15-minute idle threshold leaves >4096 young entries
  // forever when activity stops... Hard cap must immediately evict
  // eligible non-outstanding entries regardless soft idle"): a SECOND,
  // strictly larger ceiling touchAndPruneKeyGenerationMapsLocked()
  // additionally enforces UNCONDITIONALLY -- i.e. WITHOUT requiring
  // m_keyGenerationIdleEvictionThresholdMs to have elapsed at all --
  // the instant the tracked map size exceeds it, for any key whose
  // outstanding set is empty (see condition (1) in that method's own
  // comment; this hard cap never touches the correctness requirement,
  // only the throttle). This closes the exact gap the soft,
  // idle-gated cap alone left open: a single burst of activity for
  // many distinct keys, each released before any later traffic ever
  // arrives, would otherwise leave every one of those young (not yet
  // idle for m_keyGenerationIdleEvictionThresholdMs -- which defaults
  // to 15 real minutes in production) entries permanently tracked, with
  // no future call ever guaranteed to occur that could sweep them.
  // Deliberately a generous multiple of the soft cap (never the SAME
  // value) so ordinary, gradually-idling traffic still gets the softer,
  // idle-throttled eviction first in the common case -- this hard
  // ceiling exists purely as an unconditional backstop, never as the
  // primary eviction path. Overridable for testing via
  // setMaxTrackedKeyGenerationEntriesHardCapForTesting() (public,
  // above).
  static constexpr int kMaxTrackedKeyGenerationEntriesHardCap =
      kMaxTrackedKeyGenerationEntries * 4;
  int m_maxTrackedKeyGenerationEntriesHardCap{
      kMaxTrackedKeyGenerationEntriesHardCap};
  qint64 m_keyGenerationIdleEvictionThresholdMs{
      kDefaultKeyGenerationIdleEvictionThresholdMs};
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
