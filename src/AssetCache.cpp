#include "AssetCache.h"

#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

constexpr int kMetadataFormatVersion = 1;
constexpr double kHighWaterMarkFraction = 0.90;
constexpr double kLowWaterMarkFraction = 0.75;

// The only legitimate writer of a cache payload is store(), which is only
// ever fed encoded bytes that already passed AssetNetworkFetcher's own
// hard incremental cap (`Limits::maxEncodedBytes`, 20 MiB). Mirroring
// that same ceiling here, independent of whatever either on-disk file
// claims about its own size, means a corrupted, truncated, or
// locally-planted payload/metadata pair can never trigger an unbounded
// read-back allocation -- see readVerifiedPayload() below.
constexpr qint64 kMaxSinglePayloadBytesOnDisk = 20LL * 1024 * 1024;

// Verifies `payloadFile`'s on-disk size against `expectedSize` (from
// metadata) and an absolute hard ceiling BEFORE ever calling readAll().
// A corrupted, truncated, or locally-planted payload file can be
// arbitrarily large; blindly loading it into memory just to discover a
// size mismatch afterward is itself an unbounded-allocation / DoS
// surface, so this rejects on the cheap stat alone whenever the file
// cannot possibly be a valid entry, never touching its contents.
std::optional<QByteArray> readVerifiedPayload(QFile &payloadFile,
                                              qint64 expectedSize) {
  if (expectedSize < 0 || expectedSize > kMaxSinglePayloadBytesOnDisk ||
      payloadFile.size() != expectedSize) {
    return std::nullopt;
  }
  return payloadFile.readAll();
}

// Every valid entry filename matches exactly this shape: a 64-character
// lowercase hex SHA-256 key, followed by ".bin" or ".meta.json". Anything
// else found in the cache directory during a sweep (a QSaveFile crash
// artifact, a file from an old format version, etc.) is treated as a
// stray leftover and removed -- this directory is exclusively owned and
// fully managed by AssetCache, so that is always safe.
const QRegularExpression &validKeyPattern() {
  static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
  return re;
}

QString defaultCacheDirectory() {
  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  return base + QStringLiteral("/assets/v1");
}

} // namespace

AssetCache::AssetCache(Config config)
    : m_config(std::move(config)),
      m_memory(new QCache<QString, CachedEntry>()) {
  m_directory = m_config.directory.isEmpty() ? defaultCacheDirectory()
                                             : m_config.directory;
  QDir().mkpath(m_directory);
  m_memory->setMaxCost(m_config.memoryMaxCostBytes);
  reapAndEnforceQuota();
}

AssetCache::~AssetCache() { delete m_memory; }

QString AssetCache::cacheKeyFor(const QUrl &resolvedCandidateUrl) {
  const QByteArray canonical =
      ("assetcache-v1\n" + resolvedCandidateUrl.toString(QUrl::FullyEncoded))
          .toUtf8();
  return QString::fromLatin1(
      QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

QString AssetCache::payloadPath(const QString &key) const {
  return m_directory + u'/' + key + ".bin"_L1;
}

QString AssetCache::metadataPath(const QString &key) const {
  return m_directory + u'/' + key + ".meta.json"_L1;
}

std::optional<AssetCache::CachedEntry>
AssetCache::lookupMemory(const QString &key) const {
  QMutexLocker locker(&m_mutex);
  if (CachedEntry *entry = m_memory->object(key)) {
    return *entry;
  }
  return std::nullopt;
}

bool AssetCache::writeMetadata(const QString &key,
                               const DiskMetadata &metadata) const {
  QJsonObject obj;
  obj[QStringLiteral("formatVersion")] = kMetadataFormatVersion;
  obj[QStringLiteral("key")] = metadata.key;
  obj[QStringLiteral("contentType")] = metadata.contentType;
  obj[QStringLiteral("encodedSize")] = metadata.encodedSize;
  obj[QStringLiteral("width")] = metadata.width;
  obj[QStringLiteral("height")] = metadata.height;
  obj[QStringLiteral("sha256")] = metadata.sha256Hex;
  obj[QStringLiteral("etag")] = metadata.etag;
  obj[QStringLiteral("lastModified")] = metadata.lastModified;
  obj[QStringLiteral("insertedAtMs")] = metadata.insertedAtMsecsSinceEpoch;
  obj[QStringLiteral("lastAccessMs")] = metadata.lastAccessMsecsSinceEpoch;

  QSaveFile file(metadataPath(key));
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }
  file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
  return file.commit();
}

std::optional<AssetCache::DiskMetadata>
AssetCache::readMetadata(const QString &key) const {
  QFile file(metadataPath(key));
  if (!file.open(QIODevice::ReadOnly)) {
    return std::nullopt;
  }
  const QByteArray raw = file.readAll();
  const QJsonDocument doc = QJsonDocument::fromJson(raw);
  if (!doc.isObject()) {
    return std::nullopt;
  }
  const QJsonObject obj = doc.object();
  if (obj[QStringLiteral("formatVersion")].toInt(-1) !=
      kMetadataFormatVersion) {
    return std::nullopt;
  }
  DiskMetadata metadata;
  metadata.key = obj[QStringLiteral("key")].toString();
  metadata.contentType = obj[QStringLiteral("contentType")].toString();
  metadata.encodedSize =
      static_cast<qint64>(obj[QStringLiteral("encodedSize")].toDouble(-1));
  metadata.width = obj[QStringLiteral("width")].toInt(-1);
  metadata.height = obj[QStringLiteral("height")].toInt(-1);
  metadata.sha256Hex = obj[QStringLiteral("sha256")].toString();
  metadata.etag = obj[QStringLiteral("etag")].toString();
  metadata.lastModified = obj[QStringLiteral("lastModified")].toString();
  metadata.insertedAtMsecsSinceEpoch =
      static_cast<qint64>(obj[QStringLiteral("insertedAtMs")].toDouble(0));
  metadata.lastAccessMsecsSinceEpoch =
      static_cast<qint64>(obj[QStringLiteral("lastAccessMs")].toDouble(0));
  if (metadata.key != key || metadata.sha256Hex.isEmpty() ||
      metadata.encodedSize < 0) {
    return std::nullopt;
  }
  return metadata;
}

void AssetCache::deleteEntry(const QString &key) const {
  QFile::remove(payloadPath(key));
  QFile::remove(metadataPath(key));
}

std::optional<AssetCache::CachedEntry>
AssetCache::lookupDisk(const QString &key) {
  // Check memory first: promote-on-disk-hit still applies, but there is no
  // reason to touch the filesystem at all if the entry is already resident.
  if (auto memoryHit = lookupMemory(key)) {
    return memoryHit;
  }

  QMutexLocker locker(&m_mutex);

  const std::optional<DiskMetadata> metadata = readMetadata(key);
  if (!metadata) {
    // Metadata missing or corrupt. A payload might still be sitting there
    // as an orphan (e.g. a crash between the payload commit and the
    // metadata commit) -- clean it up defensively rather than leaving a
    // file this cache can never otherwise reclaim.
    deleteEntry(key);
    return std::nullopt;
  }

  QFile payloadFile(payloadPath(key));
  if (!payloadFile.open(QIODevice::ReadOnly)) {
    // Metadata present but payload missing: corrupt/incomplete entry.
    deleteEntry(key);
    return std::nullopt;
  }
  const std::optional<QByteArray> verifiedBytes =
      readVerifiedPayload(payloadFile, metadata->encodedSize);
  payloadFile.close();
  if (!verifiedBytes) {
    // Rejected on size alone before any content was read -- see
    // readVerifiedPayload()'s comment. Never trust a payload whose
    // declared or actual size can't possibly be valid.
    deleteEntry(key);
    return std::nullopt;
  }
  const QByteArray &bytes = *verifiedBytes;

  const QString actualSha256 = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  if (actualSha256 != metadata->sha256Hex ||
      bytes.size() != metadata->encodedSize) {
    // Payload does not match the metadata that vouches for it: never trust
    // a mismatched pair, no matter which file is "actually" wrong.
    deleteEntry(key);
    return std::nullopt;
  }

  CachedEntry entry;
  entry.encodedBytes = bytes;
  entry.contentType = metadata->contentType;
  entry.dimensions = QSize(metadata->width, metadata->height);
  entry.sha256Hex = metadata->sha256Hex;
  entry.etag = metadata->etag;
  entry.lastModified = metadata->lastModified;
  entry.insertedAtMsecsSinceEpoch = metadata->insertedAtMsecsSinceEpoch;
  entry.lastAccessMsecsSinceEpoch = QDateTime::currentMSecsSinceEpoch();

  // Refresh on-disk lastAccess so LRU eviction reflects this real read,
  // per the class comment's metadata-driven-LRU policy (never filesystem
  // atime).
  DiskMetadata refreshed = *metadata;
  refreshed.lastAccessMsecsSinceEpoch = entry.lastAccessMsecsSinceEpoch;
  (void)writeMetadata(key, refreshed);

  // Promote into memory ONLY when this entry needs no further
  // revalidation. AssetRequestCoordinator's memory-hit path (see
  // AssetRequestCoordinator.cpp) trusts a memory hit unconditionally, with
  // no conditional GET or etag/lastModified check at all -- so promoting
  // an entry that still carries a validator here would let it become a
  // trusted memory hit for every later lookupMemory() call BEFORE this
  // process has ever actually revalidated it against the origin (e.g. two
  // request() calls issued back-to-back, the second landing here while
  // the first's real conditional GET for the very same bytes is still in
  // flight). AssetRequestCoordinator only ever calls
  // AssetCache::store()/touchAfterNotModified() -- which manage the
  // memory entry directly -- once a real revalidation has actually
  // completed, so this is the only path that must withhold promotion.
  if (entry.etag.isEmpty() && entry.lastModified.isEmpty()) {
    auto *heapEntry = new CachedEntry(entry);
    m_memory->insert(key, heapEntry, static_cast<qsizetype>(entry.costBytes()));
  }

  return entry;
}

void AssetCache::store(const QString &key, CachedEntry entry) {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (entry.insertedAtMsecsSinceEpoch == 0) {
    entry.insertedAtMsecsSinceEpoch = now;
  }
  if (entry.lastAccessMsecsSinceEpoch == 0) {
    entry.lastAccessMsecsSinceEpoch = now;
  }
  if (entry.sha256Hex.isEmpty()) {
    entry.sha256Hex = QString::fromLatin1(
        QCryptographicHash::hash(entry.encodedBytes, QCryptographicHash::Sha256)
            .toHex());
  }

  {
    QMutexLocker locker(&m_mutex);
    auto *heapEntry = new CachedEntry(entry);
    m_memory->insert(key, heapEntry, static_cast<qsizetype>(entry.costBytes()));

    // Publication order matters for crash-consistency: write the payload
    // FIRST via QSaveFile (atomic for that one file), and only once that
    // commit succeeds, write the metadata that "vouches for" it. A crash
    // between the two leaves an orphan payload with no metadata, which
    // lookupDisk()/reapAndEnforceQuota() both already treat as invalid and
    // remove -- never as a half-valid hit.
    QSaveFile payloadFile(payloadPath(key));
    if (payloadFile.open(QIODevice::WriteOnly)) {
      payloadFile.write(entry.encodedBytes);
      if (payloadFile.commit()) {
        DiskMetadata metadata;
        metadata.key = key;
        metadata.contentType = entry.contentType;
        metadata.encodedSize = entry.encodedBytes.size();
        metadata.width = entry.dimensions.width();
        metadata.height = entry.dimensions.height();
        metadata.sha256Hex = entry.sha256Hex;
        metadata.etag = entry.etag;
        metadata.lastModified = entry.lastModified;
        metadata.insertedAtMsecsSinceEpoch = entry.insertedAtMsecsSinceEpoch;
        metadata.lastAccessMsecsSinceEpoch = entry.lastAccessMsecsSinceEpoch;
        (void)writeMetadata(key, metadata);
      }
    }
  }

  // A cheap, stat-only usage check (diskUsageBytes(): sums QFileInfo::size()
  // across the directory, no payload reads or re-hashing) gates the much
  // more expensive full reap/validate sweep below: reapAndEnforceQuota()
  // re-reads and re-hashes EVERY cached payload to validate its
  // payload/metadata pair, which makes it O(N * entry_size) over the whole
  // cache -- unconditionally running that on every single store() call
  // would make each individual asset store's cost scale with total cache
  // size, not just that one asset's size. The constructor already runs one
  // unconditional sweep at startup (to repair anything a prior crash left
  // as an orphan/half-written pair), so skipping the sweep here whenever
  // usage is still comfortably under quota never leaves genuine corruption
  // undetected forever -- only until either this check trips or the next
  // process start.
  const qint64 highWaterMark =
      static_cast<qint64>(m_config.diskMaxBytes * kHighWaterMarkFraction);
  if (diskUsageBytes() > highWaterMark) {
    reapAndEnforceQuota();
  }
}

void AssetCache::touchAfterNotModified(const QString &key,
                                       const QString &newEtag,
                                       const QString &newLastModified) {
  QMutexLocker locker(&m_mutex);
  const std::optional<DiskMetadata> metadata = readMetadata(key);
  if (!metadata) {
    return; // nothing to touch; a subsequent lookup will simply miss
  }
  DiskMetadata refreshed = *metadata;
  refreshed.lastAccessMsecsSinceEpoch = QDateTime::currentMSecsSinceEpoch();
  if (!newEtag.isEmpty()) {
    refreshed.etag = newEtag;
  }
  if (!newLastModified.isEmpty()) {
    refreshed.lastModified = newLastModified;
  }
  (void)writeMetadata(key, refreshed);

  if (CachedEntry *entry = m_memory->object(key)) {
    entry->lastAccessMsecsSinceEpoch = refreshed.lastAccessMsecsSinceEpoch;
    if (!newEtag.isEmpty()) {
      entry->etag = newEtag;
    }
    if (!newLastModified.isEmpty()) {
      entry->lastModified = newLastModified;
    }
  }
}

void AssetCache::updateMemoryDecodedImage(const QString &key,
                                          const QImage &image) {
  QMutexLocker locker(&m_mutex);
  CachedEntry *existing = m_memory->object(key);
  if (!existing) {
    return; // evicted since the caller's lookup; a later lookup redecodes
  }
  // A full copy-then-reinsert (rather than mutating *existing in place)
  // is required here, unlike touchAfterNotModified()'s in-place field
  // updates: decodedImage directly affects costBytes(), and QCache's
  // internal cost accounting is only ever updated via insert(), never by
  // mutating an object it already owns.
  auto *updated = new CachedEntry(*existing);
  updated->decodedImage = image;
  m_memory->insert(key, updated, static_cast<qsizetype>(updated->costBytes()));
}

void AssetCache::promoteToMemory(const QString &key, CachedEntry entry) {
  QMutexLocker locker(&m_mutex);
  auto *heapEntry = new CachedEntry(std::move(entry));
  m_memory->insert(key, heapEntry,
                   static_cast<qsizetype>(heapEntry->costBytes()));
}

void AssetCache::reapAndEnforceQuota() {
  QMutexLocker locker(&m_mutex);

  QDir dir(m_directory);
  if (!dir.exists()) {
    return;
  }
  const QStringList allFiles = dir.entryList(QDir::Files, QDir::Name);

  // First pass: discover every key with either file present.
  QHash<QString, bool> hasPayload;
  QHash<QString, bool> hasMetadata;
  for (const QString &name : allFiles) {
    if (name.endsWith(".bin"_L1)) {
      const QString key = name.chopped(4);
      if (validKeyPattern().match(key).hasMatch()) {
        hasPayload[key] = true;
        continue;
      }
    } else if (name.endsWith(".meta.json"_L1)) {
      const QString key = name.chopped(10);
      if (validKeyPattern().match(key).hasMatch()) {
        hasMetadata[key] = true;
        continue;
      }
    }
    // Anything not matching the exact "<64-hex-key>.bin" or
    // "<64-hex-key>.meta.json" shape is a stray leftover (a QSaveFile temp
    // artifact from an interrupted write, or debris from an old format) --
    // this directory is exclusively owned by AssetCache, so removing it
    // unconditionally is safe.
    QFile::remove(dir.filePath(name));
  }

  QSet<QString> allKeys;
  for (auto it = hasPayload.constBegin(); it != hasPayload.constEnd(); ++it) {
    allKeys.insert(it.key());
  }
  for (auto it = hasMetadata.constBegin(); it != hasMetadata.constEnd(); ++it) {
    allKeys.insert(it.key());
  }

  struct ValidEntry {
    QString key;
    qint64 totalBytes;
    qint64 lastAccessMs;
  };
  std::vector<ValidEntry> valid;

  for (const QString &key : allKeys) {
    const bool payloadPresent = hasPayload.value(key, false);
    const bool metadataPresent = hasMetadata.value(key, false);
    if (payloadPresent && !metadataPresent) {
      deleteEntry(key); // orphan payload
      continue;
    }
    if (!payloadPresent && metadataPresent) {
      deleteEntry(key); // metadata with no payload
      continue;
    }
    // Both present: validate the pair.
    const std::optional<DiskMetadata> metadata = readMetadata(key);
    QFile payloadFile(payloadPath(key));
    const bool payloadReadable = payloadFile.open(QIODevice::ReadOnly);
    const std::optional<QByteArray> verifiedBytes =
        (payloadReadable && metadata)
            ? readVerifiedPayload(payloadFile, metadata->encodedSize)
            : std::nullopt;
    if (payloadReadable) {
      payloadFile.close();
    }
    if (!metadata || !payloadReadable || !verifiedBytes) {
      deleteEntry(key); // corrupt pair, or rejected on size alone
      continue;
    }
    const QByteArray &bytes = *verifiedBytes;
    const QString actualSha256 = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    if (actualSha256 != metadata->sha256Hex ||
        bytes.size() != metadata->encodedSize) {
      deleteEntry(key); // corrupt pair
      continue;
    }
    const qint64 payloadBytes = QFileInfo(payloadPath(key)).size();
    const qint64 metadataBytes = QFileInfo(metadataPath(key)).size();
    valid.push_back(ValidEntry{key, payloadBytes + metadataBytes,
                               metadata->lastAccessMsecsSinceEpoch});
  }

  qint64 totalBytes = 0;
  for (const ValidEntry &e : valid) {
    totalBytes += e.totalBytes;
  }

  const qint64 highWaterMark =
      static_cast<qint64>(m_config.diskMaxBytes * kHighWaterMarkFraction);
  const qint64 lowWaterMark =
      static_cast<qint64>(m_config.diskMaxBytes * kLowWaterMarkFraction);

  if (totalBytes <= highWaterMark) {
    return;
  }

  // Oldest lastAccess first: evict until at or below the low-water mark.
  std::sort(valid.begin(), valid.end(),
            [](const ValidEntry &a, const ValidEntry &b) {
              return a.lastAccessMs < b.lastAccessMs;
            });

  for (const ValidEntry &e : valid) {
    if (totalBytes <= lowWaterMark) {
      break;
    }
    deleteEntry(e.key);
    m_memory->remove(e.key);
    totalBytes -= e.totalBytes;
  }
}

qint64 AssetCache::memoryCostBytes() const {
  QMutexLocker locker(&m_mutex);
  return m_memory->totalCost();
}

qint64 AssetCache::diskUsageBytes() const {
  QMutexLocker locker(&m_mutex);
  QDir dir(m_directory);
  qint64 total = 0;
  for (const QFileInfo &info : dir.entryInfoList(QDir::Files)) {
    total += info.size();
  }
  return total;
}

int AssetCache::diskEntryCount() const {
  QMutexLocker locker(&m_mutex);
  QDir dir(m_directory);
  int count = 0;
  for (const QString &name : dir.entryList(QDir::Files)) {
    if (name.endsWith(".meta.json"_L1)) {
      ++count;
    }
  }
  return count;
}

} // namespace Arkham
