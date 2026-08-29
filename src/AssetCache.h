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
// On disk, each entry is two separate files under `directory()`:
//   {key}.bin        -- the raw encoded bytes, written via QSaveFile.
//   {key}.meta.json  -- versioned metadata, also written via QSaveFile,
//                       AFTER the payload commit succeeds.
// QSaveFile only makes a single file's write atomic; it does not make the
// pair transactional. Crash-consistency across the pair is achieved by
// treating metadata as the sole "this entry is valid" witness: a read (or
// the startup/periodic reapAndEnforceQuota() sweep) always re-hashes the
// payload and compares it against metadata's recorded SHA-256 before
// trusting an entry, deletes a payload-without-metadata as an orphan, and
// deletes a metadata-without-payload (or metadata whose payload hash
// mismatches) as corrupt. A crash between the two writes therefore always
// resolves deterministically to "entry absent" on the next read/sweep,
// never to a half-valid success.
//
// Metadata (not filesystem atime, which many container/build
// environments mount with atime updates disabled or coarsened) drives
// LRU eviction: `lastAccessMsecsSinceEpoch` is refreshed on every disk
// hit (rewritten via the same atomic metadata-write path) and consulted
// by reapAndEnforceQuota(), which evicts oldest-access entries once disk
// usage exceeds the 90% high-water mark, down to the 75% low-water mark.
// Quota accounting always sums payload + metadata file sizes together.
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

  // Fast path: memory-only lookup. Does not touch disk or update
  // lastAccess metadata on disk (a memory hit is not persisted as a
  // separate disk access -- disk lastAccess is only meaningful for disk
  // eviction, and a memory hit by definition did not need disk I/O).
  [[nodiscard]] std::optional<CachedEntry>
  lookupMemory(const QString &key) const;

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

  // Repairs orphan payloads, corrupt entries, and stray temp files, then
  // evicts oldest-access entries if disk usage exceeds the 90% high-water
  // mark, down to the 75% low-water mark. Called once from the
  // constructor; safe and idempotent to call again at any time (e.g. after
  // a simulated restart in tests).
  void reapAndEnforceQuota();

  [[nodiscard]] qint64 memoryCostBytes() const;
  [[nodiscard]] qint64 diskUsageBytes() const;
  [[nodiscard]] int diskEntryCount() const;

private:
  struct DiskMetadata {
    QString key;
    QString contentType;
    qint64 encodedSize{0};
    int width{0};
    int height{0};
    QString sha256Hex;
    QString etag;
    QString lastModified;
    qint64 insertedAtMsecsSinceEpoch{0};
    qint64 lastAccessMsecsSinceEpoch{0};
  };

  [[nodiscard]] QString payloadPath(const QString &key) const;
  [[nodiscard]] QString metadataPath(const QString &key) const;
  [[nodiscard]] bool writeMetadata(const QString &key,
                                   const DiskMetadata &metadata) const;
  [[nodiscard]] std::optional<DiskMetadata>
  readMetadata(const QString &key) const;
  void deleteEntry(const QString &key) const;

  Config m_config;
  QString m_directory;
  mutable QMutex m_mutex;
  QCache<QString, CachedEntry> *m_memory;
};

} // namespace Arkham
