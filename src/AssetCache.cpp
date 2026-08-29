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
#include <cmath>
#include <utility>
#include <vector>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// Review round-3 item 8: an independent (deliberately NOT shared with
// AssetNetworkFetcher::Limits -- see kMaxSinglePayloadBytesOnDisk's
// comment for the same "mirror, don't couple" reasoning) sane upper bound
// on a single dimension read back from metadata. Real assets never
// remotely approach this; it exists purely so a corrupted/malicious
// metadata field can never reach the width/height cast below with a
// value that could overflow a later width*height multiplication.
constexpr qint64 kMaxDimensionPixelsOnDisk = 65536;

// The largest double value that can represent every integer exactly
// (2^53): the JSON number type is IEEE-754 double-precision, so ANY
// integer-valued field this cache itself ever legitimately writes (see
// writeMetadata()) that could plausibly exceed this bound must be
// persisted as a decimal STRING instead (see the accessSequence handling
// in readMetadata()/writeMetadata()) rather than a bare JSON number.
// Timestamp fields (insertedAtMs/lastAccessMs) are bounded against this
// same limit below purely as a "finite, integral, in-range" sanity gate
// -- not because a real timestamp could ever legitimately approach it.
constexpr double kMaxExactJsonIntegerDouble = 9007199254740992.0; // 2^53

// Review round-3 item 8: validates that `value` is a JSON number encoding
// a finite, integral, non-negative value no larger than `maxBound` --
// returns std::nullopt for anything else (a fractional value, NaN,
// +/-infinity, negative, oversized, or simply not a number at all).
// QJsonValue::toDouble() happily returns any of those for a corrupted or
// maliciously-planted metadata file; casting such a double DIRECTLY to a
// narrower integer type (as this file used to do for encodedSize/width/
// height) is undefined behaviour for an out-of-range value, not merely
// "wrong" -- this rejects all of that BEFORE any cast is ever performed,
// rather than trusting the cast itself to fail safely.
std::optional<qint64>
readBoundedNonNegativeIntegerField(const QJsonValue &value, qint64 maxBound) {
  if (!value.isDouble()) {
    return std::nullopt;
  }
  const double d = value.toDouble();
  if (!std::isfinite(d) || d < 0.0 || d > static_cast<double>(maxBound) ||
      d != std::trunc(d)) {
    return std::nullopt;
  }
  return static_cast<qint64>(d);
}

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

// A 64-character lowercase hex SHA-256 string -- the shape of both a
// cache key (cacheKeyFor()) and a generation identifier (a payload's own
// content hash): both are validated against this exact same pattern
// before ever becoming part of a filesystem path.
const QRegularExpression &validKeyPattern() {
  static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
  return re;
}

// Review item 8: on-disk filename shapes for the generation/manifest
// layout described in AssetCache.h. Anything found in the cache
// directory during a sweep that matches none of these three shapes (a
// QSaveFile crash artifact, debris from an old format version, etc.) is
// a stray leftover and removed outright -- this directory is exclusively
// owned and fully managed by AssetCache, so that is always safe.
const QRegularExpression &manifestNamePattern() {
  static const QRegularExpression re(
      QStringLiteral("^([0-9a-f]{64})\\.manifest\\.json$"));
  return re;
}
const QRegularExpression &generationPayloadNamePattern() {
  static const QRegularExpression re(
      QStringLiteral("^([0-9a-f]{64})\\.([0-9a-f]{64})\\.bin$"));
  return re;
}
const QRegularExpression &generationMetadataNamePattern() {
  static const QRegularExpression re(
      QStringLiteral("^([0-9a-f]{64})\\.([0-9a-f]{64})\\.meta\\.json$"));
  return re;
}

// Flushes `file`'s buffered writes and, on POSIX, fsyncs its underlying
// descriptor BEFORE commit() performs the atomic rename that publishes
// it -- so the rename that makes a new generation's (or the manifest's)
// file visible can never be reordered, by the OS or a real power loss,
// ahead of that file's own content actually reaching stable storage. On
// a hypothetical non-POSIX build, this degrades to a plain flush (no
// durability guarantee beyond whatever the OS itself provides for a
// rename), which mirrors this codebase's existing safeRemoveTree()
// POSIX-or-no-op precedent above.
bool fsyncSaveFileBeforeCommit(QSaveFile &file) {
  file.flush();
#if defined(Q_OS_UNIX)
  return ::fsync(static_cast<int>(file.handle())) == 0;
#else
  return true;
#endif
}

// fsyncs the directory entry `dirPath` itself: on POSIX, a file rename
// (as QSaveFile::commit() performs) is only durable across a crash once
// the directory's own metadata update is flushed, independent of the
// renamed file's own content already being synced (see
// fsyncSaveFileBeforeCommit() above). Called once after the manifest
// swap -- the single atomic pointer flip that actually publishes a new
// generation -- commits, so that publication itself survives a crash
// immediately following it. Review round-3 item 10: returns whether the
// fsync itself actually succeeded (open+fsync both) -- a caller (store())
// must NEVER proceed to reclaim the previous generation's files as if
// publication were durable when this returns false; the previous
// generation is the only durability hedge available at that point.
bool fsyncDirectory(const QString &dirPath) {
#if defined(Q_OS_UNIX)
  const QByteArray dirUtf8 = QFile::encodeName(dirPath);
  const int fd = ::open(dirUtf8.constData(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return false;
  }
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#else
  Q_UNUSED(dirPath);
  return true;
#endif
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

#if defined(Q_OS_UNIX)
// Descriptor-relative, no-follow recursive delete (review item 7): every
// decision about whether an entry is a directory to recurse into, a file
// to unlink, or a symlink node to unlink-but-never-follow is made via
// fstatat(..., AT_SYMLINK_NOFOLLOW) -- an lstat-equivalent that never
// resolves the final path component -- and every actual filesystem
// mutation (openat/unlinkat) is relative to an already-open directory
// file descriptor rather than a re-resolved path string. Critically,
// descending into a subdirectory uses openat(..., O_NOFOLLOW): if the
// entry was replaced by a symlink between the fstatat() check above and
// this open (a rename/replace TOCTOU race), the open itself fails
// (ELOOP) rather than silently following the attacker-controlled
// indirection -- there is no window in which a path string is
// re-resolved from the filesystem root.
//
// A symlink node encountered anywhere in the tree is unlinked (removed)
// itself; its target, wherever it points, is NEVER opened, stat'd
// through, or touched in any way. A hard-linked regular file is just an
// ordinary directory entry here: unlinkat() drops only that one link,
// never recursing into or otherwise treating a hardlink specially.
bool safeRemoveEntryAt(int parentFd, const char *name);

bool safeRemoveDirectoryContentsAt(int dirFd) {
  DIR *dirStream = fdopendir(dirFd);
  if (!dirStream) {
    ::close(dirFd);
    return false;
  }
  bool allOk = true;
  errno = 0;
  while (struct dirent *entry = readdir(dirStream)) {
    if (qstrcmp(entry->d_name, ".") == 0 || qstrcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (!safeRemoveEntryAt(dirFd, entry->d_name)) {
      allOk = false;
    }
    errno = 0;
  }
  closedir(dirStream); // also closes dirFd
  return allOk;
}

bool safeRemoveEntryAt(int parentFd, const char *name) {
  struct stat st {};
  if (fstatat(parentFd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
    return errno == ENOENT; // already gone: nothing left to do
  }
  if (S_ISDIR(st.st_mode)) {
    const int childFd =
        openat(parentFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (childFd < 0) {
      // Either genuinely not a directory anymore (replaced by a
      // symlink between the fstatat() above and here) or some other
      // open failure -- either way, refuse to proceed rather than
      // guessing.
      return false;
    }
    if (!safeRemoveDirectoryContentsAt(childFd)) {
      return false;
    }
    return unlinkat(parentFd, name, AT_REMOVEDIR) == 0 || errno == ENOENT;
  }
  // A regular file, or a symlink node itself (S_ISLNK): unlinkat()
  // without AT_REMOVEDIR removes the directory ENTRY, never resolving or
  // following it even when it names a symlink.
  return unlinkat(parentFd, name, 0) == 0 || errno == ENOENT;
}
#endif

// Removes `path` (expected to be a directory) and everything under it,
// using the no-follow descriptor-relative primitives above. Refuses
// outright (a safe no-op) if `path` itself is a symlink, is not a
// directory, or does not exist -- see safeRemoveEntryAt()'s comment for
// the full rationale. On a hypothetical platform with no such
// primitives available, this is a safe no-op rather than risking a
// follow-through-symlink recursive delete.
void safeRemoveTree(const QString &path) {
#if defined(Q_OS_UNIX)
  const QByteArray pathUtf8 = QFile::encodeName(path);
  const int fd = ::open(pathUtf8.constData(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  safeRemoveDirectoryContentsAt(fd); // closes fd
  ::rmdir(pathUtf8.constData());
#else
  Q_UNUSED(path);
#endif
}

} // namespace

AssetCache::AssetCache(Config config)
    : m_config(std::move(config)),
      m_memory(new QCache<QString, CachedEntry>()) {
  m_directory = m_config.directory.isEmpty() ? defaultCacheDirectory()
                                             : m_config.directory;
  // Review item 7: if the configured cache directory ALREADY exists as a
  // symlink, refuse to use it at all -- never mkpath() through it (which
  // would silently follow the symlink and start writing at whatever it
  // points to), and never let any later store()/lookupDisk()/reap call
  // touch it either. QFileInfo::isSymLink() (unlike QDir::exists(), which
  // follows) reports the entry's own type without resolving it. The
  // memory cache still works normally -- only disk persistence is
  // disabled for this instance's entire lifetime.
  if (QFileInfo(m_directory).isSymLink()) {
    m_diskCacheDisabled = true;
  } else {
    QDir().mkpath(m_directory);
  }
#if defined(Q_OS_UNIX)
  // Review round-3 item 9: open and retain a directory descriptor for
  // `m_directory` NOW, at construction, and record the (device, inode)
  // pair it names via fstat() on THIS SAME descriptor -- never a second,
  // later path-based stat, which could observe a DIFFERENT filesystem
  // object if the path has since been replaced. O_NOFOLLOW means this
  // open itself fails if the path (already checked above, but this is
  // an independent, TOCTOU-closing re-check via the syscall itself
  // rather than a separate stat) resolves to a symlink. Every later
  // disk-touching operation re-derives the CURRENT (device, inode) for
  // this same path and compares it against these retained values
  // (verifyRootAnchorLocked()) before proceeding, so a root directory
  // renamed away and replaced by a new one (or an ancestor component
  // replaced such that the same path now resolves elsewhere) is
  // detected and disk I/O is permanently disabled rather than silently
  // operating against a different object than the one this instance
  // was constructed against.
  if (!m_diskCacheDisabled) {
    const QByteArray dirUtf8 = QFile::encodeName(m_directory);
    const int fd = ::open(dirUtf8.constData(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st {};
    if (fd < 0 || ::fstat(fd, &st) != 0) {
      if (fd >= 0) {
        ::close(fd);
      }
      m_diskCacheDisabled = true;
    } else {
      m_rootFd = fd;
      m_rootDevice = static_cast<quint64>(st.st_dev);
      m_rootInode = static_cast<quint64>(st.st_ino);
    }
  }
#endif
  m_memory->setMaxCost(m_config.memoryMaxCostBytes);
  reapAndEnforceQuota();
}

AssetCache::~AssetCache() {
  delete m_memory;
#if defined(Q_OS_UNIX)
  if (m_rootFd >= 0) {
    ::close(m_rootFd);
  }
#endif
}

QString AssetCache::cacheKeyFor(const QUrl &resolvedCandidateUrl) {
  const QByteArray canonical =
      ("assetcache-v1\n" + resolvedCandidateUrl.toString(QUrl::FullyEncoded))
          .toUtf8();
  return QString::fromLatin1(
      QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

QString AssetCache::manifestPath(const QString &key) const {
  return m_directory + u'/' + key + ".manifest.json"_L1;
}

QString AssetCache::generationPayloadPath(const QString &key,
                                          const QString &generation) const {
  return m_directory + u'/' + key + u'.' + generation + ".bin"_L1;
}

QString AssetCache::generationMetadataPath(const QString &key,
                                           const QString &generation) const {
  return m_directory + u'/' + key + u'.' + generation + ".meta.json"_L1;
}

QString AssetCache::manifestPathForTesting(const QString &directory,
                                           const QString &key) {
  return directory + u'/' + key + ".manifest.json"_L1;
}

QString AssetCache::payloadPathForTesting(const QString &directory,
                                          const QString &key,
                                          const QString &generation) {
  return directory + u'/' + key + u'.' + generation + ".bin"_L1;
}

QString AssetCache::metadataPathForTesting(const QString &directory,
                                           const QString &key,
                                           const QString &generation) {
  return directory + u'/' + key + u'.' + generation + ".meta.json"_L1;
}

std::optional<AssetCache::CachedEntry>
AssetCache::lookupMemory(const QString &key) {
  QMutexLocker locker(&m_mutex);
  CachedEntry *entry = m_memory->object(key);
  if (!entry) {
    return std::nullopt;
  }
  const CachedEntry hit = *entry;
  // Review item 11: a memory hit is a genuine access and must be
  // reflected in this key's PERSISTED disk recency, not just its
  // in-memory presence -- see touchAccessRecencyLocked()'s comment and
  // the class comment for the full rationale.
  touchAccessRecencyLocked(key);
  return hit;
}

quint64 AssetCache::nextAccessSequenceLocked() {
  return m_nextAccessSequence++;
}

void AssetCache::touchAccessRecencyLocked(const QString &key) {
  if (m_diskCacheDisabled || !isValidKey(key)) {
    return;
  }
  const std::optional<QString> generation = readManifestGeneration(key);
  if (!generation) {
    return; // nothing on disk for this key right now -- nothing to bump
  }
  const QString metadataPath = generationMetadataPath(key, *generation);
  const std::optional<DiskMetadata> metadata = readMetadata(metadataPath, key);
  if (!metadata || metadata->sha256Hex != *generation) {
    // A corrupt/self-inconsistent record here is a real-repair signal,
    // but repairing it is lookupDisk()/reapAndEnforceQuota()'s job (both
    // independently re-verify the payload itself before trusting
    // anything) -- this purely-cosmetic recency bump must not delete a
    // record it hasn't actually read/verified the PAYLOAD of.
    return;
  }
  DiskMetadata refreshed = *metadata;
  refreshed.lastAccessMsecsSinceEpoch = QDateTime::currentMSecsSinceEpoch();
  refreshed.accessSequence = nextAccessSequenceLocked();
  // Review item 11: `durable = false` -- see writeMetadata()'s
  // declaration comment and the class comment for why a lost recency
  // bump on crash only ever affects eviction ordering, never integrity.
  (void)writeMetadata(metadataPath, refreshed, /*durable=*/false);
}

bool AssetCache::verifyRootAnchorLocked() const {
  if (m_diskCacheDisabled) {
    return false;
  }
#if defined(Q_OS_UNIX)
  if (m_rootFd < 0) {
    // Never opened successfully at construction (or on a platform
    // without this protection) -- treat as already disabled rather than
    // silently skipping the check.
    m_diskCacheDisabled = true;
    return false;
  }
  struct stat currentSt {};
  // lstat (never a path resolved THROUGH the retained descriptor, and
  // never following a final symlink component): re-derives what
  // `m_directory` names RIGHT NOW, independent of the object m_rootFd
  // still refers to. If the path has been renamed away, removed, or
  // replaced by anything else (a plain directory, a symlink, a
  // dangling entry), the (device, inode) pair observed here will not
  // match what was captured from m_rootFd at construction.
  const QByteArray dirUtf8 = QFile::encodeName(m_directory);
  if (::lstat(dirUtf8.constData(), &currentSt) != 0 ||
      static_cast<quint64>(currentSt.st_dev) != m_rootDevice ||
      static_cast<quint64>(currentSt.st_ino) != m_rootInode) {
    qWarning() << "AssetCache: root directory anchor mismatch for"
               << m_directory
               << "-- permanently disabling disk I/O for this instance";
    m_diskCacheDisabled = true;
    ::close(m_rootFd);
    m_rootFd = -1;
    return false;
  }
  return true;
#else
  return true;
#endif
}

qint64 AssetCache::diskUsageBytesLocked() const {
  if (!verifyRootAnchorLocked()) {
    return 0;
  }
  // See diskUsageBytes()'s comment (this is its lock-already-held
  // implementation, reused directly by reapAndEnforceQuota() -- review
  // round-3 item 11 -- so its eviction-target math is always driven by
  // a true, unconditional inventory rather than only the subset of
  // entries that happened to validate cleanly).
  qint64 total = 0;
  QDirIterator it(m_directory, QDir::Files | QDir::Hidden,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    total += it.fileInfo().size();
  }
  return total;
}

bool AssetCache::writeMetadata(const QString &metadataFilePath,
                               const DiskMetadata &metadata,
                               bool durable) const {
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
  // Review round-3 item 8: persisted as a decimal STRING, not a bare
  // JSON number -- see kMaxExactJsonIntegerDouble's comment. A JSON
  // number is an IEEE-754 double and can only represent every integer
  // *exactly* up to 2^53; this counter increments once per successful
  // access/store and, over a long enough cache lifetime, could
  // plausibly exceed that threshold, silently losing precision (and
  // therefore LRU ordering correctness) if stored as a number. A
  // quint64-range string has no such ceiling.
  obj[QStringLiteral("accessSeq")] = QString::number(metadata.accessSequence);

  QSaveFile file(metadataFilePath);
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }
  file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
  // Review item 8: fsync the metadata content itself before the commit()
  // rename publishes it -- see fsyncSaveFileBeforeCommit()'s comment.
  // Review item 11: `durable` is false only for a pure recency-only bump
  // with no other semantic change -- see writeMetadata()'s declaration
  // comment for why skipping the fsync there is safe.
  if (durable && !fsyncSaveFileBeforeCommit(file)) {
    return false;
  }
  return file.commit();
}

bool AssetCache::writeManifest(const QString &key,
                               const QString &generation) const {
  QJsonObject obj;
  obj[QStringLiteral("formatVersion")] = kMetadataFormatVersion;
  obj[QStringLiteral("key")] = key;
  obj[QStringLiteral("generation")] = generation;

  QSaveFile file(manifestPath(key));
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }
  file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
  return fsyncSaveFileBeforeCommit(file) && file.commit();
}

std::optional<QString>
AssetCache::readManifestGeneration(const QString &key) const {
  QFile file(manifestPath(key));
  if (!file.open(QIODevice::ReadOnly)) {
    return std::nullopt;
  }
  if (file.size() < 0 || file.size() > kMaxMetadataBytesOnDisk) {
    return std::nullopt;
  }
  const QByteArray raw = file.read(kMaxMetadataBytesOnDisk + 1);
  if (raw.size() > kMaxMetadataBytesOnDisk) {
    return std::nullopt;
  }
  const QJsonDocument doc = QJsonDocument::fromJson(raw);
  if (!doc.isObject()) {
    return std::nullopt;
  }
  const QJsonObject obj = doc.object();
  if (obj[QStringLiteral("formatVersion")].toInt(-1) !=
          kMetadataFormatVersion ||
      obj[QStringLiteral("key")].toString() != key) {
    return std::nullopt;
  }
  const QString generation = obj[QStringLiteral("generation")].toString();
  // A generation identifier is always a payload's own SHA-256 hex
  // digest -- exactly the same shape as a cache key -- and this string
  // becomes part of a filesystem path below (generationPayloadPath()/
  // generationMetadataPath()): never trust it otherwise.
  if (!validKeyPattern().match(generation).hasMatch()) {
    return std::nullopt;
  }
  return generation;
}

std::optional<AssetCache::DiskMetadata>
AssetCache::readMetadata(const QString &metadataFilePath,
                         const QString &expectedKey) const {
  QFile file(metadataFilePath);
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
  // Review round-3 item 8: every numeric field below is independently
  // validated as finite/integral/non-negative/in-range BEFORE any cast
  // to a narrower type -- a corrupted or maliciously-planted metadata
  // file can otherwise supply a fractional, negative, NaN, +/-infinity,
  // or absurdly oversized JSON number, and casting THAT directly to
  // qint64/int/quint64 (as this file used to do) is undefined behaviour,
  // not merely "produces a wrong value". Any field that fails this check
  // renders the whole metadata record untrusted (return std::nullopt
  // below), which the caller then treats as a quarantine-and-refetch
  // candidate exactly like a missing/absent file.
  const std::optional<qint64> encodedSize = readBoundedNonNegativeIntegerField(
      obj[QStringLiteral("encodedSize")], kMaxSinglePayloadBytesOnDisk);
  const std::optional<qint64> width = readBoundedNonNegativeIntegerField(
      obj[QStringLiteral("width")], kMaxDimensionPixelsOnDisk);
  const std::optional<qint64> height = readBoundedNonNegativeIntegerField(
      obj[QStringLiteral("height")], kMaxDimensionPixelsOnDisk);
  const std::optional<qint64> insertedAtMs = readBoundedNonNegativeIntegerField(
      obj[QStringLiteral("insertedAtMs")], kMaxExactJsonIntegerDouble);
  const std::optional<qint64> lastAccessMs = readBoundedNonNegativeIntegerField(
      obj[QStringLiteral("lastAccessMs")], kMaxExactJsonIntegerDouble);
  if (!encodedSize || !width || !height || !insertedAtMs || !lastAccessMs) {
    return std::nullopt;
  }
  metadata.encodedSize = *encodedSize;
  metadata.width = static_cast<int>(*width);
  metadata.height = static_cast<int>(*height);
  metadata.sha256Hex = obj[QStringLiteral("sha256")].toString();
  metadata.etag = obj[QStringLiteral("etag")].toString();
  metadata.lastModified = obj[QStringLiteral("lastModified")].toString();
  metadata.insertedAtMsecsSinceEpoch = *insertedAtMs;
  metadata.lastAccessMsecsSinceEpoch = *lastAccessMs;
  // Review round-3 item 8: accessSeq is persisted as a decimal STRING
  // (see writeMetadata()'s comment), not a bare JSON number -- parse it
  // with QString::toULongLong()'s own strict validation (rejects
  // fractional/non-digit/out-of-quint64-range content via `ok`) rather
  // than a JSON-number cast at all. Absent/malformed defaults to 0
  // (the same default a pre-existing/legacy metadata file with no such
  // field at all would have produced), never an arbitrary or partially-
  // parsed value.
  bool accessSeqOk = false;
  const quint64 accessSeq =
      obj[QStringLiteral("accessSeq")].toString().toULongLong(&accessSeqOk);
  metadata.accessSequence = accessSeqOk ? accessSeq : 0;
  if (metadata.key != expectedKey || metadata.sha256Hex.isEmpty() ||
      metadata.encodedSize < 0) {
    return std::nullopt;
  }
  return metadata;
}

bool AssetCache::deleteEntry(const QString &key) const {
  if (m_diskCacheDisabled || !verifyRootAnchorLocked()) {
    return true; // nothing to delete when disk I/O is disabled entirely
  }
  // Review item 8: `key` no longer maps to a fixed pair of filenames --
  // reclaim EVERY file this cache could ever have written for it (the
  // manifest, plus every generation-scoped payload/metadata file,
  // whether it's the live generation or an orphan left by an
  // interrupted replacement) via a name-prefix sweep, rather than
  // guessing at a single generation. `key` is always validated
  // (isValidKey()) by every public entry point before reaching here, so
  // it can never itself contain a wildcard-special character.
  QDir dir(m_directory);
  const QStringList matches =
      dir.entryList(QStringList{key + QStringLiteral(".*")}, QDir::Files);
  // Review item 11: report whether EVERY matched file was actually
  // removed. QFile::remove() returning false (e.g. a permission error,
  // or -- in tests -- a directory planted at the same path) means this
  // key's disk footprint was NOT fully reclaimed; a caller doing quota
  // accounting must not credit itself with bytes that are still
  // genuinely occupied.
  bool allRemoved = true;
  for (const QString &name : matches) {
    if (!QFile::remove(dir.filePath(name))) {
      allRemoved = false;
    }
  }
  return allRemoved;
}

std::optional<AssetCache::CachedEntry>
AssetCache::lookupDisk(const QString &key) {
  // Check memory first: promote-on-disk-hit still applies, but there is no
  // reason to touch the filesystem at all if the entry is already resident.
  if (auto memoryHit = lookupMemory(key)) {
    return memoryHit;
  }

  if (m_diskCacheDisabled) {
    // Review item 7: the configured root was itself a symlink at
    // construction time -- never read through it.
    return std::nullopt;
  }

  if (!isValidKey(key)) {
    // Never let a malformed key (path separators, "..", etc.) reach
    // manifestPath()/generationPayloadPath()/generationMetadataPath()
    // below -- see isValidKey()'s comment.
    return std::nullopt;
  }

  QMutexLocker locker(&m_mutex);

  if (!verifyRootAnchorLocked()) {
    // Review round-3 item 9: the root has been replaced/removed/mounted
    // over since construction -- refuse to read through it at all.
    return std::nullopt;
  }

  const std::optional<QString> generation = readManifestGeneration(key);
  if (!generation) {
    // Manifest missing or corrupt: nothing for this key can be trusted
    // (see the class comment -- the manifest is the sole pointer to
    // which generation, if any, is live). Reclaim anything left behind
    // for this key -- including any orphaned generation files -- rather
    // than leaking it until the next sweep.
    (void)deleteEntry(key);
    return std::nullopt;
  }

  const std::optional<DiskMetadata> metadata =
      readMetadata(generationMetadataPath(key, *generation), key);
  if (!metadata || metadata->sha256Hex != *generation) {
    // Metadata missing/corrupt, or (defense in depth) it does not even
    // claim to be the generation its own filename says it is.
    (void)deleteEntry(key);
    return std::nullopt;
  }

  QFile payloadFile(generationPayloadPath(key, *generation));
  if (!payloadFile.open(QIODevice::ReadOnly)) {
    // Manifest+metadata present but this generation's payload missing:
    // corrupt/incomplete entry.
    (void)deleteEntry(key);
    return std::nullopt;
  }
  const std::optional<QByteArray> verifiedBytes =
      readVerifiedPayload(payloadFile, metadata->encodedSize);
  payloadFile.close();
  if (!verifiedBytes) {
    // Rejected on size alone before any content was read -- see
    // readVerifiedPayload()'s comment. Never trust a payload whose
    // declared or actual size can't possibly be valid.
    (void)deleteEntry(key);
    return std::nullopt;
  }
  const QByteArray &bytes = *verifiedBytes;

  const QString actualSha256 = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  if (actualSha256 != metadata->sha256Hex || actualSha256 != *generation ||
      bytes.size() != metadata->encodedSize) {
    // Payload does not match the metadata that vouches for it (or the
    // generation identifier its own filename claims): never trust a
    // mismatched pair, no matter which file is "actually" wrong.
    (void)deleteEntry(key);
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
  // atime). This rewrites the SAME generation's metadata file in place
  // (the generation, and therefore the manifest, is unchanged) -- a
  // failure here only loses this one LRU-freshness update, never the
  // entry's validity: writeMetadata() is itself atomic (QSaveFile), so a
  // crash mid-write leaves the prior, still-fully-valid metadata
  // untouched. Review item 11: also mints a fresh monotonic access
  // sequence (the primary eviction-ordering key -- see the class
  // comment), and this bump is deliberately non-durable
  // (`durable=false`): losing it to a crash only affects eviction
  // ordering, never this entry's integrity.
  DiskMetadata refreshed = *metadata;
  refreshed.lastAccessMsecsSinceEpoch = entry.lastAccessMsecsSinceEpoch;
  refreshed.accessSequence = nextAccessSequenceLocked();
  (void)writeMetadata(generationMetadataPath(key, *generation), refreshed,
                      /*durable=*/false);

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
  // Review item 8: ALWAYS (re)compute this from the actual bytes being
  // stored, never trusting a caller-supplied value verbatim -- this
  // string becomes the generation's filename segment below
  // (generationPayloadPath()/generationMetadataPath()), so a malformed
  // or attacker-influenced caller-supplied sha256Hex must never reach a
  // filesystem path. Every legitimate caller already derives this value
  // from a hash of these exact bytes anyway, so recomputing changes
  // nothing for them.
  entry.sha256Hex = QString::fromLatin1(
      QCryptographicHash::hash(entry.encodedBytes, QCryptographicHash::Sha256)
          .toHex());

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
    if (m_diskCacheDisabled || !verifyRootAnchorLocked()) {
      // Review item 7 / round-3 item 9: root was a symlink at
      // construction, or has since been replaced/removed/mounted over.
      // Memory insert above still happened -- only the disk write is
      // skipped.
    } else if (entry.encodedBytes.size() <= kMaxSinglePayloadBytesOnDisk) {
      // Review item 8: content-addressed generation publication. The
      // generation's filename is derived from its own content hash, so
      // writing generation N+1's files can NEVER touch generation N's
      // files (unless the content is byte-identical, in which case
      // there is nothing to actually replace) -- an existing, currently
      // -live generation is therefore fully intact at every point up
      // until the manifest swap below actually commits. See the class
      // comment for the full crash-consistency argument.
      const QString generation = entry.sha256Hex;
      const std::optional<QString> previousGeneration =
          readManifestGeneration(key);
      const QString genPayloadPath = generationPayloadPath(key, generation);
      const QString genMetadataPath = generationMetadataPath(key, generation);

      QSaveFile payloadFile(genPayloadPath);
      if (payloadFile.open(QIODevice::WriteOnly)) {
        payloadFile.write(entry.encodedBytes);
        if (fsyncSaveFileBeforeCommit(payloadFile) && payloadFile.commit()) {
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
          // Review item 11: a fresh store() is a genuine access too --
          // mint a new monotonic sequence so this generation isn't
          // mistaken for stale relative to whatever was live before it.
          metadata.accessSequence = nextAccessSequenceLocked();
          if (writeMetadata(genMetadataPath, metadata)) {
            // Both of this generation's own files are now fully
            // written, fsync'd, and committed -- ATOMICALLY publish it
            // as the live one via the single mutable manifest pointer,
            // then fsync the directory so that publish survives a crash
            // immediately after it (see fsyncDirectory()'s comment).
            if (writeManifest(key, generation)) {
              // Review round-3 item 10: the manifest rename ITSELF is
              // already committed at the filesystem level at this point
              // (QSaveFile::commit() succeeded above) -- there is no
              // reliable way to "roll it back" if the directory fsync
              // that follows fails. What CAN still be controlled is
              // whether the OLD generation's files are reclaimed: if
              // fsyncDirectory() fails, this crash-durability barrier
              // has NOT actually been crossed, so the old generation is
              // deliberately kept around as a durability hedge (a crash
              // right after this point could otherwise leave NEITHER
              // generation recoverable if the rename itself hadn't
              // actually reached stable storage) rather than reclaimed
              // as if publication were guaranteed durable.
              if (fsyncDirectory(m_directory)) {
                // Only now -- strictly after the new generation is
                // confirmed durably live -- reclaim the OLD
                // generation's files, if there was a different one. A
                // crash at any point before this line leaves the OLD
                // generation still fully intact and still the one the
                // manifest names.
                if (previousGeneration && *previousGeneration != generation) {
                  QFile::remove(
                      generationPayloadPath(key, *previousGeneration));
                  QFile::remove(
                      generationMetadataPath(key, *previousGeneration));
                }
              } else {
                qWarning()
                    << "AssetCache: directory fsync failed after publishing"
                    << key
                    << "-- retaining previous generation as a durability "
                       "hedge";
              }
            } else {
              // Manifest publish failed: this generation's files become
              // orphans, safely reclaimed by the next reap sweep (or
              // immediately here, defensively) -- the OLD generation
              // (if any) remains untouched and still live.
              QFile::remove(genPayloadPath);
              QFile::remove(genMetadataPath);
            }
          } else {
            // Metadata is the sole validity witness for a payload (see
            // lookupDisk()): without it, the payload just committed can
            // never be read back as valid. Delete it immediately rather
            // than leaving that window open; the OLD generation (if
            // any) is completely untouched by any of this.
            QFile::remove(genPayloadPath);
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
  if (!m_diskCacheDisabled && diskUsageBytes() > highWaterMark) {
    reapAndEnforceQuota();
  }
}

void AssetCache::touchAfterNotModified(const QString &key,
                                       const QString &newEtag,
                                       const QString &newLastModified) {
  if (m_diskCacheDisabled) {
    return;
  }
  if (!isValidKey(key)) {
    // Never let a malformed key reach manifestPath()/
    // generationMetadataPath() below -- see isValidKey()'s comment.
    return;
  }
  QMutexLocker locker(&m_mutex);
  if (!verifyRootAnchorLocked()) {
    return;
  }
  const std::optional<QString> generation = readManifestGeneration(key);
  const std::optional<DiskMetadata> metadata =
      generation ? readMetadata(generationMetadataPath(key, *generation), key)
                 : std::nullopt;
  if (!generation || !metadata || metadata->sha256Hex != *generation) {
    // Manifest/metadata missing, corrupt, or self-inconsistent, exactly
    // as lookupDisk() and reapAndEnforceQuota() both handle: a caller
    // only reaches touchAfterNotModified() after a 304 response to a
    // conditional request it issued for what it believed was a valid
    // disk entry, so this is itself a repair signal, not merely
    // "already evicted." Clean up defensively for the same reason
    // lookupDisk() does; a subsequent lookup will simply miss and
    // refetch from scratch.
    (void)deleteEntry(key);
    return;
  }
  DiskMetadata refreshed = *metadata;
  refreshed.lastAccessMsecsSinceEpoch = QDateTime::currentMSecsSinceEpoch();
  // Review item 11: a successful conditional revalidation is a genuine
  // access -- mint a fresh monotonic sequence exactly like a full
  // disk/memory hit does.
  refreshed.accessSequence = nextAccessSequenceLocked();
  if (!newEtag.isEmpty()) {
    refreshed.etag = newEtag;
  }
  if (!newLastModified.isEmpty()) {
    refreshed.lastModified = newLastModified;
  }
  // The generation itself (content hash) never changes on a 304 --
  // only its metadata (etag/lastModified/lastAccess) is rewritten in
  // place, atomically, via the same generation-scoped file; the
  // manifest is untouched since the live generation hasn't changed.
  (void)writeMetadata(generationMetadataPath(key, *generation), refreshed);

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
  (void)deleteEntry(key);
}

void AssetCache::reapAndEnforceQuota() {
  if (m_diskCacheDisabled) {
    // Review item 7: root was a symlink at construction -- never
    // enumerate, follow, or delete anything through it.
    return;
  }
  QMutexLocker locker(&m_mutex);
  if (!verifyRootAnchorLocked()) {
    // Review round-3 item 9: root replaced since construction -- never
    // enumerate, follow, or delete anything through it.
    return;
  }

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
  // QDir::System is included so a DANGLING symlink (whose target no
  // longer exists) is not silently invisible to this sweep either --
  // without it, QDir::Files/QDir::Dirs alone only match entries whose
  // resolved TARGET type is a regular file or directory, which excludes
  // a broken symlink node entirely even though it still needs cleanup.
  const QStringList allEntries =
      dir.entryList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System |
                        QDir::NoDotAndDotDot,
                    QDir::Name);
  QStringList allFiles;
  allFiles.reserve(allEntries.size());
  for (const QString &name : allEntries) {
    const QString fullPath = dir.filePath(name);
    // Review item 7: classify with QFileInfo::isSymLink() FIRST, which
    // (unlike isDir()) reports the entry's own type without resolving
    // it. A symlink node found directly inside the cache root -- to a
    // directory, a file, or a dangling target -- is unlinked itself via
    // QFile::remove() (an unlink(), never a recursive follow); its
    // target is never opened, stat'd through, or touched. Only a
    // genuine (non-symlink) directory is recursed into, and even then
    // via safeRemoveTree()'s descriptor-relative no-follow walk rather
    // than QDir::removeRecursively() (which itself follows symlinks
    // internally and would otherwise reintroduce exactly this escape).
    const QFileInfo info(fullPath);
    if (info.isSymLink()) {
      QFile::remove(fullPath);
      continue;
    }
    if (info.isDir()) {
      safeRemoveTree(fullPath);
      continue;
    }
    allFiles.append(name);
  }

  // First pass: discover every manifest (key -> the generation it
  // names) and every generation-scoped payload/metadata file present
  // (key -> the set of generations found for each shape), removing
  // anything that doesn't match one of the three known filename shapes
  // (review item 8) outright.
  QHash<QString, QString> manifestGeneration;
  QHash<QString, QSet<QString>> payloadGenerations;
  QHash<QString, QSet<QString>> metadataGenerations;
  for (const QString &name : allFiles) {
    if (const auto m = manifestNamePattern().match(name); m.hasMatch()) {
      const QString key = m.captured(1);
      if (const std::optional<QString> generation =
              readManifestGeneration(key)) {
        manifestGeneration[key] = *generation;
      } else {
        // Manifest itself unreadable/malformed/self-inconsistent: a
        // stray leftover -- remove outright. Any generation files it
        // might have named are reclaimed below as orphans, since no
        // valid manifest ends up naming this key.
        QFile::remove(dir.filePath(name));
      }
      continue;
    }
    if (const auto m = generationPayloadNamePattern().match(name);
        m.hasMatch()) {
      payloadGenerations[m.captured(1)].insert(m.captured(2));
      continue;
    }
    if (const auto m = generationMetadataNamePattern().match(name);
        m.hasMatch()) {
      metadataGenerations[m.captured(1)].insert(m.captured(2));
      continue;
    }
    // Anything not matching one of the three known shapes is a stray
    // leftover (a QSaveFile temp artifact from an interrupted write, or
    // debris from an old format) -- this directory is exclusively owned
    // by AssetCache, so removing it unconditionally is safe.
    QFile::remove(dir.filePath(name));
  }

  QSet<QString> allKeys;
  for (auto it = manifestGeneration.constBegin();
       it != manifestGeneration.constEnd(); ++it) {
    allKeys.insert(it.key());
  }
  for (auto it = payloadGenerations.constBegin();
       it != payloadGenerations.constEnd(); ++it) {
    allKeys.insert(it.key());
  }
  for (auto it = metadataGenerations.constBegin();
       it != metadataGenerations.constEnd(); ++it) {
    allKeys.insert(it.key());
  }

  struct ValidEntry {
    QString key;
    qint64 totalBytes;
    qint64 lastAccessMs;
    // Review item 11: the primary eviction-ordering key (see the class
    // comment) -- monotonic and unique-at-write-time for any entry ever
    // written by this fix, so lastAccessMs only ever matters as a
    // tie-break for legacy (pre-this-fix) entries that still carry the
    // default 0.
    quint64 accessSequence;
  };
  std::vector<ValidEntry> valid;
  // Review item 11: recovers this process's monotonic access-sequence
  // counter from whatever the highest value any entry on disk already
  // carries, so a fresh AssetCache instance (a real process restart)
  // never reissues a sequence number an earlier instance already
  // persisted. Accumulated across every valid entry found in this same
  // sweep, below.
  quint64 maxObservedAccessSequence = 0;

  for (const QString &key : allKeys) {
    const auto manifestIt = manifestGeneration.constFind(key);
    if (manifestIt == manifestGeneration.constEnd()) {
      // No valid manifest names this key at all: nothing for it can be
      // trusted (see the class comment) -- reclaim every generation
      // file left behind for it.
      (void)deleteEntry(key);
      continue;
    }
    const QString &generation = manifestIt.value();
    const bool payloadPresent =
        payloadGenerations.value(key).contains(generation);
    const bool metadataPresent =
        metadataGenerations.value(key).contains(generation);
    if (!payloadPresent || !metadataPresent) {
      // The manifest names a generation whose files are (partially or
      // wholly) missing -- corrupt/incomplete, never a half-valid hit.
      (void)deleteEntry(key);
      continue;
    }
    const QString genPayloadPath = generationPayloadPath(key, generation);
    const QString genMetadataPath = generationMetadataPath(key, generation);
    const std::optional<DiskMetadata> metadata =
        readMetadata(genMetadataPath, key);
    QFile payloadFile(genPayloadPath);
    const bool payloadReadable = payloadFile.open(QIODevice::ReadOnly);
    const std::optional<QByteArray> verifiedBytes =
        (payloadReadable && metadata)
            ? readVerifiedPayload(payloadFile, metadata->encodedSize)
            : std::nullopt;
    if (payloadReadable) {
      payloadFile.close();
    }
    if (!metadata || metadata->sha256Hex != generation || !payloadReadable ||
        !verifiedBytes) {
      (void)deleteEntry(key); // corrupt pair, or rejected on size alone
      continue;
    }
    const QByteArray &bytes = *verifiedBytes;
    const QString actualSha256 = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    if (actualSha256 != generation || bytes.size() != metadata->encodedSize) {
      (void)deleteEntry(key); // corrupt pair
      continue;
    }

    // The live generation validated cleanly: reclaim any OTHER
    // generation's files still sitting around for this key -- orphans
    // left by a crash between writing a new generation and cleaning up
    // its predecessor, or between writing a new generation's own files
    // and ever reaching the manifest swap (see store()).
    for (const QString &other : payloadGenerations.value(key)) {
      if (other != generation) {
        QFile::remove(generationPayloadPath(key, other));
      }
    }
    for (const QString &other : metadataGenerations.value(key)) {
      if (other != generation) {
        QFile::remove(generationMetadataPath(key, other));
      }
    }

    const qint64 manifestBytes = QFileInfo(manifestPath(key)).size();
    const qint64 payloadBytes = QFileInfo(genPayloadPath).size();
    const qint64 metadataBytes = QFileInfo(genMetadataPath).size();
    valid.push_back(ValidEntry{
        key, manifestBytes + payloadBytes + metadataBytes,
        metadata->lastAccessMsecsSinceEpoch, metadata->accessSequence});
    maxObservedAccessSequence =
        qMax(maxObservedAccessSequence, metadata->accessSequence);
  }

  // Review item 11: never DECREASE the counter -- only ever advance it
  // to strictly past the highest value observed on disk. If this
  // process has already minted sequence values more recent than
  // anything currently on disk (e.g. this sweep ran mid-process after
  // recent memory-only touches whose disk bump is itself in this same
  // batch and therefore already reflected above), m_nextAccessSequence
  // must not regress.
  m_nextAccessSequence =
      qMax(m_nextAccessSequence, maxObservedAccessSequence + 1);

  qint64 totalBytes = diskUsageBytesLocked();
  // Review round-3 item 11: the eviction-target math below is driven by
  // a TRUE, unconditional inventory of every byte still actually present
  // under the root (diskUsageBytesLocked()), not merely the sum of
  // entries that happened to validate cleanly above. A stray/orphan/
  // corrupt entry the cleanup pass above FAILED to remove (a permission
  // error, an undeletable node, or any other reclaim failure) still
  // occupies real, countable disk space; summing only `valid` would
  // silently ignore it and could let quota enforcement report "already
  // under budget" while genuinely undeletable bytes remain resident.

  const qint64 highWaterMark =
      static_cast<qint64>(m_config.diskMaxBytes * kHighWaterMarkFraction);
  const qint64 lowWaterMark =
      static_cast<qint64>(m_config.diskMaxBytes * kLowWaterMarkFraction);

  if (totalBytes <= highWaterMark) {
    return;
  }

  // Oldest access first: evict until at or below the low-water mark.
  // Review item 11: primary key is the monotonic access sequence, NOT
  // raw wall-clock time -- two entries stored/touched within the same
  // millisecond (routine on a fast machine, or in a fast test) are still
  // deterministically ordered by which one was actually accessed first,
  // never by whatever order QHash/QSet happened to iterate `allKeys` in.
  // lastAccessMs and then the cache key itself are tie-breaks, reached
  // only for legacy entries that share the default accessSequence of 0.
  std::sort(valid.begin(), valid.end(),
            [](const ValidEntry &a, const ValidEntry &b) {
              if (a.accessSequence != b.accessSequence) {
                return a.accessSequence < b.accessSequence;
              }
              if (a.lastAccessMs != b.lastAccessMs) {
                return a.lastAccessMs < b.lastAccessMs;
              }
              return a.key < b.key;
            });

  for (const ValidEntry &e : valid) {
    if (totalBytes <= lowWaterMark) {
      break;
    }
    // Review item 11: only credit quota accounting with this entry's
    // bytes as freed if its files were actually confirmed removed. A
    // failed deletion (e.g. a permission error, or a hostile/corrupt
    // entry replaced by an undeletable node) leaves the entry counted as
    // still occupying its space and still resident in memory (it is, in
    // fact, still fully valid on disk) -- moving on to the next
    // candidate rather than looping forever on the one that can't be
    // reclaimed.
    if (deleteEntry(e.key)) {
      m_memory->remove(e.key);
      totalBytes -= e.totalBytes;
    }
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
  // Review round-3 item 9: routed through diskUsageBytesLocked(), which
  // itself re-verifies the root anchor (device+inode) before enumerating
  // anything -- see verifyRootAnchorLocked()'s comment.
  return diskUsageBytesLocked();
}

int AssetCache::diskEntryCount() const {
  QMutexLocker locker(&m_mutex);
  if (!verifyRootAnchorLocked()) {
    return 0;
  }
  QDir dir(m_directory);
  int count = 0;
  for (const QString &name : dir.entryList(QDir::Files)) {
    // One manifest file exists per live entry (review item 8) -- this is
    // a more accurate "how many entries does this cache have" count than
    // counting generation-scoped metadata files, which can transiently
    // include an orphan left by an interrupted replacement until the
    // next reap sweep reclaims it.
    if (manifestNamePattern().match(name).hasMatch()) {
      ++count;
    }
  }
  return count;
}

std::optional<quint64>
AssetCache::accessSequenceForTesting(const QString &key) const {
  QMutexLocker locker(&m_mutex);
  if (m_diskCacheDisabled || !isValidKey(key)) {
    return std::nullopt;
  }
  const std::optional<QString> generation = readManifestGeneration(key);
  if (!generation) {
    return std::nullopt;
  }
  const std::optional<DiskMetadata> metadata =
      readMetadata(generationMetadataPath(key, *generation), key);
  if (!metadata || metadata->sha256Hex != *generation) {
    return std::nullopt;
  }
  return metadata->accessSequence;
}

} // namespace Arkham
