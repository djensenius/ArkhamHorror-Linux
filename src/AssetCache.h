#pragma once

#include "AssetTypes.h"

#include <QCache>
#include <QHash>
#include <QImage>
#include <QSize>
#include <QString>
#include <QUrl>

#include <map>

namespace Arkham {

// Thread-confined decoded-image asset cache. Memory entries are keyed by the
// canonical URL plus the requested decode bound ("@WxH", or "@full" for an
// invalid or empty bound), so each cell size holds only its own downscaled
// decode. The disk cache stores the raw encoded bytes keyed by URL only, and a
// disk hit decodes at the bound being looked up. AssetCache is intended to be
// used from one owner thread (in production, AssetImageRequestCoordinator's
// "arkham-card-image-provider" worker thread); it does not start background
// work and does not synchronize concurrent callers. Disk operations
// are deliberately small and synchronous: lookup reads at most one cache file,
// store atomically writes at most one cache file, and eviction uses the
// in-memory index built once from the cache root's immediate directory entries.
// Runtime LRU recency is updated in memory only; a restarted cache seeds that
// recency from file mtimes (store times) rather than touching mtimes on hits.
class AssetCache final {
public:
  struct Config {
    qint64 memoryMaxCostBytes{128LL * 1024LL * 1024LL};
    qint64 diskMaxBytes{512LL * 1024LL * 1024LL};
    QString rootDirectory;
    AssetDecodeLimits decodeLimits{};
  };

  enum class DiskStatus {
    Enabled,
    MemoryOnly,
  };

  enum class LookupSource {
    Miss,
    Memory,
    Disk,
  };

  struct LookupResult {
    LookupSource source{LookupSource::Miss};
    QImage image;

    [[nodiscard]] bool hit() const noexcept {
      return source != LookupSource::Miss;
    }
  };

  AssetCache();
  explicit AssetCache(Config config);
  ~AssetCache();

  [[nodiscard]] static QString defaultRootDirectory();
  [[nodiscard]] static QString diskCacheFileNameForUrl(const QUrl &url);

  [[nodiscard]] LookupResult lookup(const QUrl &url,
                                    const QSize &bound = QSize());
  // Decodes encodedBytes at bound, caches the decode in memory, and writes
  // the encoded bytes to disk. Bytes that fail to decode never reach disk.
  // Memory entries for the same URL at other bounds are dropped because they
  // were decoded from the bytes this call replaces.
  [[nodiscard]] AssetDecodeOutcome<QImage>
  store(const QUrl &url, const QByteArray &encodedBytes,
        const QString &contentType = {}, const QSize &bound = QSize());
  // Decodes encodedBytes at bound and caches the decode in memory only. Used
  // for additional bounds of bytes that store() has already written to disk.
  [[nodiscard]] AssetDecodeOutcome<QImage>
  storeMemoryOnly(const QUrl &url, const QByteArray &encodedBytes,
                  const QSize &bound);

  void clearMemory();

  [[nodiscard]] DiskStatus diskStatus() const noexcept { return m_diskStatus; }
  [[nodiscard]] QString diagnostic() const { return m_diagnostic; }
  [[nodiscard]] QString rootDirectory() const { return m_rootDirectory; }
  [[nodiscard]] qint64 indexedDiskBytes() const noexcept { return m_diskBytes; }
  // Total cost (decoded image bytes) of the in-memory entries.
  [[nodiscard]] qint64 memoryCostBytes() const {
    return m_memoryCache.totalCost();
  }

private:
  struct DiskEntry {
    qint64 fileSize{0};
    quint64 lastAccess{0};
  };

  [[nodiscard]] static QByteArray canonicalUrlBytes(const QUrl &url);
  [[nodiscard]] static QString memoryKeyFor(const QUrl &url,
                                            const QSize &bound);
  [[nodiscard]] QString filePathForName(const QString &fileName) const;
  [[nodiscard]] int imageCost(const QImage &image) const;

  void configureMemoryLimit(qint64 maxCostBytes);
  void openDiskCache();
  void disableDisk(QString diagnostic, bool warn);
  void buildIndex();
  void promoteToMemory(const QString &key, const QImage &image);
  void removeMemoryEntriesForUrl(const QUrl &url);
  void touchDiskEntry(const QString &diskKey);
  [[nodiscard]] LookupResult lookupDisk(const QString &diskKey,
                                        const QString &memoryKey,
                                        const QSize &bound);
  void storeDisk(const QString &diskKey, const QByteArray &encodedBytes,
                 const QString &contentType);
  void removeDiskEntry(const QString &key);
  void setDiskEntryAccess(const QString &key, quint64 access);
  void evictIfNeeded();
  [[nodiscard]] quint64 nextAccess();

  Config m_config;
  QCache<QString, QImage> m_memoryCache;
  QHash<QString, DiskEntry> m_index;
  std::multimap<quint64, QString> m_lruQueue;
  std::map<QString, std::multimap<quint64, QString>::iterator> m_lruPositions;
  DiskStatus m_diskStatus{DiskStatus::MemoryOnly};
  QString m_rootDirectory;
  QString m_diagnostic;
  qint64 m_diskBytes{0};
  quint64 m_nextAccess{1};
  bool m_warnedDiskFailure{false};
};

} // namespace Arkham
