#include "AssetCache.h"

#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
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

// A legitimate metadata JSON file (see writeMetadata()) holds only a
// handful of short strings/numbers and is always well under 1 KiB in
// practice. This cache directory is locally writable and can contain a
// corrupted or maliciously-planted *.meta.json file; without an
// independent ceiling here, reading one via readAll() would be an
// unbounded allocation triggered purely by a file's on-disk size, before
// any of its content is even parsed. 64 KiB leaves generous headroom
// over any legitimate metadata payload while still bounding the read.
constexpr qint64 kMaxMetadataBytesOnDisk = 64 * 1024;

// Verifies `payloadFile`'s on-disk size against `expectedSize` (from
// metadata) and an absolute hard ceiling BEFORE ever calling read(), THEN
// reads at most `expectedSize + 1` bytes rather than trusting the
// earlier size() stat and calling readAll(): a corrupted, truncated, or
// locally-planted payload file can change size between the stat above
// and the read below (e.g. concurrent local tampering, or a special
// file whose size() cannot be trusted), so bounding the read itself --
// not just the pre-read stat -- is what actually prevents an unbounded
// allocation here. Reading exactly one byte more than `expectedSize`
// lets a file that grew past the expected size be rejected by comparing
// the actual byte count read against `expectedSize`, without ever
// allocating for more than `expectedSize + 1` bytes regardless of what
// the file claims or how large it has become.
std::optional<QByteArray> readVerifiedPayload(QFile &payloadFile,
                                              qint64 expectedSize) {
  if (expectedSize < 0 || expectedSize > kMaxSinglePayloadBytesOnDisk ||
      payloadFile.size() != expectedSize) {
    return std::nullopt;
  }
  const QByteArray bytes = payloadFile.read(expectedSize + 1);
  if (bytes.size() != expectedSize) {
    // Either short (truncated mid-read) or exactly expectedSize + 1 (the
    // file grew since the size() check above) -- neither is the exact,
    // verified payload this cache promises to serve.
    return std::nullopt;
  }
  return bytes;
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

// All disk-touching public entry points (lookupDisk(), store(),
// touchAfterNotModified()) accept a caller-supplied key and use it,
// unescaped, to form filesystem paths via payloadPath()/metadataPath().
// AssetCache's own callers always pass a key produced by cacheKeyFor()
// (a SHA-256 hex digest), which can never contain a path separator or
// "..", but this is a public API -- nothing in the type system prevents
// a future or misbehaving caller from forwarding an arbitrary string.
// Rejecting anything that doesn't match the exact expected shape here,
// at every entry point that turns a key into a path, closes that off
// completely rather than relying on every call site to keep passing
// keys that happen to be safe.
bool isValidKey(const QString &key) {
  return validKeyPattern().match(key).hasMatch();
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
  // Reject on the cheap stat alone, before ever calling read(): see
  // kMaxMetadataBytesOnDisk's comment above. A corrupted or
  // locally-planted metadata file can claim an arbitrary size regardless
  // of what its (if any) JSON content actually is.
  if (file.size() < 0 || file.size() > kMaxMetadataBytesOnDisk) {
    return std::nullopt;
  }
  // Copilot review: bound the READ itself, not just the preceding stat --
  // readAll() would trust file.size() implicitly, but the file can grow
  // between the stat above and this read (concurrent local tampering, or
  // a special file whose size() cannot be trusted). Reading at most
  // kMaxMetadataBytesOnDisk + 1 bytes means this can never allocate more
  // than that regardless of how large the file has actually become by
  // the time this read runs; a JSON document can never legitimately need
  // the extra byte, so no valid metadata file is ever affected.
  const QByteArray raw = file.read(kMaxMetadataBytesOnDisk + 1);
  if (raw.size() > kMaxMetadataBytesOnDisk) {
    // The file grew past the cap between the stat above and this read --
    // fail closed rather than attempting to parse a file this large as
    // JSON.
    return std::nullopt;
  }
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

  if (!isValidKey(key)) {
    // Never let a malformed key (path separators, "..", etc.) reach
    // payloadPath()/metadataPath() below -- see isValidKey()'s comment.
    return std::nullopt;
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
  if (!isValidKey(key)) {
    // Never let a malformed key reach payloadPath()/metadataPath() below
    // -- see isValidKey()'s comment. Rejecting the whole store() as a
    // no-op (rather than, say, still inserting into the memory cache
    // under an untrusted key) keeps the invariant simple: an entry only
    // ever exists, in memory or on disk, under a key this cache itself
    // considers well-formed.
    return;
  }

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

    // Never write a payload to disk that this cache could never itself
    // read back: readVerifiedPayload() hard-caps any disk read at
    // kMaxSinglePayloadBytesOnDisk, so a larger on-disk file would sit as
    // unreclaimable disk usage (never a valid lookupDisk() hit) until a
    // later reap sweep happens to notice it. Production callers already
    // cap fetched bytes well below this via
    // AssetNetworkFetcher::Limits::maxEncodedBytes before a fetch even
    // completes; this guard is defense-in-depth against a bug or a
    // future caller invoking store() directly with an oversized entry.
    // The memory insert above is unaffected -- QCache's own cost-based
    // eviction already bounds memory usage independently of disk.
    if (entry.encodedBytes.size() <= kMaxSinglePayloadBytesOnDisk) {
      // Publication order matters for crash-consistency: write the
      // payload FIRST via QSaveFile (atomic for that one file), and only
      // once that commit succeeds, write the metadata that "vouches for"
      // it. A crash between the two leaves an orphan payload with no
      // metadata, which lookupDisk()/reapAndEnforceQuota() both already
      // treat as invalid and remove -- never as a half-valid hit.
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
          if (!writeMetadata(key, metadata)) {
            // Metadata is the SOLE validity witness for a payload (see
            // above): without it, the payload just committed can never
            // be read back as valid and would otherwise leak disk usage
            // until the next reap/lookup sweep happens to remove it.
            // Delete it immediately instead of leaving that window open.
            QFile::remove(payloadPath(key));
          }
        }
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
  if (!isValidKey(key)) {
    // Never let a malformed key reach payloadPath()/metadataPath() below
    // -- see isValidKey()'s comment.
    return;
  }
  QMutexLocker locker(&m_mutex);
  const std::optional<DiskMetadata> metadata = readMetadata(key);
  if (!metadata) {
    // Metadata missing or corrupt, exactly as lookupDisk() and
    // reapAndEnforceQuota() both handle: a payload might still be
    // sitting there as an orphan (e.g. a crash between the payload
    // commit and the metadata commit, or external corruption), and
    // this cache would otherwise never reclaim it -- a caller only
    // reaches touchAfterNotModified() after a 304 response to a
    // conditional request it issued for what it believed was a valid
    // disk entry, so missing/corrupt metadata here is itself a repair
    // signal, not merely "already evicted." Clean it up defensively
    // for the same reason lookupDisk() does; a subsequent lookup will
    // simply miss and refetch from scratch.
    deleteEntry(key);
    return;
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
  if (!isValidKey(key)) {
    // Unlike lookupDisk()/store()/touchAfterNotModified(), this method
    // never turns `key` into a filesystem path, so a malformed key can't
    // cause a path-traversal here -- but it's a public API, and every
    // other entry point enforces the invariant that entries only ever
    // exist in the cache under a cacheKeyFor()-shaped (64-hex) key.
    // Silently accepting anything else here would let this one method
    // create an inconsistency future callers/entry points can't rely on
    // -- see isValidKey()'s comment.
    return;
  }
  QMutexLocker locker(&m_mutex);
  auto *heapEntry = new CachedEntry(std::move(entry));
  m_memory->insert(key, heapEntry,
                   static_cast<qsizetype>(heapEntry->costBytes()));
}

void AssetCache::invalidate(const QString &key) {
  if (!isValidKey(key)) {
    return;
  }
  QMutexLocker locker(&m_mutex);
  m_memory->remove(key);
  deleteEntry(key);
}

void AssetCache::reapAndEnforceQuota() {
  QMutexLocker locker(&m_mutex);

  QDir dir(m_directory);
  if (!dir.exists()) {
    return;
  }
  // Copilot review: enumerate BOTH files and directories here, not just
  // QDir::Files -- this directory is exclusively owned by AssetCache
  // (see the comment below), so a stray directory (planted, or left
  // behind by some other means) is exactly as much of a leftover as a
  // stray file, and must not be invisible to this repair sweep. Any
  // directory found is removed recursively below, before the
  // files-only key-shape pass that follows. QDir::Hidden is included
  // too: QDir's filters exclude hidden entries (dotfiles) by default,
  // so a stray hidden file/directory would otherwise be just as
  // invisible to this sweep as a stray directory was before this fix.
  const QStringList allEntries = dir.entryList(
      QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
      QDir::Name);
  QStringList allFiles;
  allFiles.reserve(allEntries.size());
  for (const QString &name : allEntries) {
    const QString fullPath = dir.filePath(name);
    if (QFileInfo(fullPath).isDir()) {
      QDir(fullPath).removeRecursively();
      continue;
    }
    allFiles.append(name);
  }

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
  // Copilot review: recurse into subdirectories here (QDirIterator with
  // Subdirectories), not a flat QDir::Files listing -- this directory is
  // exclusively owned by AssetCache, so a stray directory (e.g. planted,
  // or otherwise left behind) should never be invisible to this usage
  // total. This cheap, stat-only check gates whether the much more
  // expensive reapAndEnforceQuota() sweep (which does recursively remove
  // stray directories) runs at all in store()'s hot path -- if a stray
  // directory's contents weren't counted here, that gate could never
  // trip, and the sweep that would actually clean it up might never run.
  // QDir::Hidden is included too: QDirIterator's filters exclude hidden
  // entries by default, so a stray hidden file would otherwise undercount
  // usage and could prevent the high-water-mark gate from ever tripping.
  qint64 total = 0;
  QDirIterator it(m_directory, QDir::Files | QDir::Hidden,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    total += it.fileInfo().size();
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
