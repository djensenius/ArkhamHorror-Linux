#include "AssetCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtDebug>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace Arkham {

namespace {

constexpr qsizetype kMaxHeaderBytes = 4096;
constexpr auto kFileMagic = "ARKHAM-ASSET-CACHE 1";

[[nodiscard]] bool isLowerHexSha256FileName(const QString &fileName) {
  if (fileName.size() != 64) {
    return false;
  }
  for (const QChar ch : fileName) {
    const ushort code = ch.unicode();
    if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f'))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] QByteArray payloadSha256Hex(const QByteArray &payload) {
  return QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
}

[[nodiscard]] QByteArray buildHeader(const QByteArray &payload,
                                     const QString &contentType) {
  QByteArray header;
  header += kFileMagic;
  header += '\n';
  header += "content-type-base64:";
  header += contentType.toUtf8().toBase64(QByteArray::Base64UrlEncoding |
                                          QByteArray::OmitTrailingEquals);
  header += '\n';
  header += "byte-length:";
  header += QByteArray::number(payload.size());
  header += '\n';
  header += "payload-sha256:";
  header += payloadSha256Hex(payload);
  header += '\n';
  header += "stored-at-ms:";
  header += QByteArray::number(QDateTime::currentMSecsSinceEpoch());
  header += "\n\n";
  return header;
}

struct ParsedHeader {
  qsizetype payloadLength{0};
  QByteArray payloadSha256;
};

[[nodiscard]] std::optional<QByteArray>
fieldValue(const QList<QByteArray> &lines, const QByteArray &prefix) {
  for (const QByteArray &line : lines) {
    if (line.startsWith(prefix)) {
      return line.sliced(prefix.size());
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<ParsedHeader>
parseHeader(const QByteArray &header, qsizetype maxPayloadSize) {
  const QList<QByteArray> lines = header.split('\n');
  if (lines.empty() || lines.front() != kFileMagic) {
    return std::nullopt;
  }
  if (!fieldValue(lines, "content-type-base64:")) {
    return std::nullopt;
  }

  const auto lengthValue = fieldValue(lines, "byte-length:");
  const auto shaValue = fieldValue(lines, "payload-sha256:");
  const auto storedAtValue = fieldValue(lines, "stored-at-ms:");
  if (!lengthValue || !shaValue || !storedAtValue) {
    return std::nullopt;
  }

  bool ok = false;
  const qlonglong payloadLength = lengthValue->toLongLong(&ok);
  if (!ok || payloadLength < 0 || payloadLength > maxPayloadSize) {
    return std::nullopt;
  }

  ok = false;
  const qlonglong storedAt = storedAtValue->toLongLong(&ok);
  if (!ok || storedAt < 0) {
    return std::nullopt;
  }

  if (shaValue->size() != 64) {
    return std::nullopt;
  }
  for (const char ch : *shaValue) {
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return std::nullopt;
    }
  }

  return ParsedHeader{static_cast<qsizetype>(payloadLength), *shaValue};
}

} // namespace

AssetCache::AssetCache() : AssetCache(Config{}) {}

AssetCache::AssetCache(Config config) : m_config(std::move(config)) {
  configureMemoryLimit(m_config.memoryMaxCostBytes);
  openDiskCache();
}

AssetCache::~AssetCache() = default;

QString AssetCache::defaultRootDirectory() {
  const QString cacheLocation =
      QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  if (cacheLocation.isEmpty()) {
    return {};
  }
  return QDir(cacheLocation).filePath(QStringLiteral("assets"));
}

QString AssetCache::diskCacheFileNameForUrl(const QUrl &url) {
  return QString::fromLatin1(
      QCryptographicHash::hash(canonicalUrlBytes(url),
                               QCryptographicHash::Sha256)
          .toHex());
}

AssetCache::LookupResult AssetCache::lookup(const QUrl &url) {
  const QString memoryKey = QString::fromLatin1(canonicalUrlBytes(url));
  const QString diskKey = diskCacheFileNameForUrl(url);
  if (const QImage *cached = m_memoryCache.object(memoryKey)) {
    touchDiskEntry(diskKey);
    return LookupResult{LookupSource::Memory, *cached};
  }
  if (m_diskStatus == DiskStatus::Enabled) {
    return lookupDisk(diskKey, memoryKey);
  }
  return {};
}

AssetDecodeOutcome<QImage> AssetCache::store(const QUrl &url,
                                             const QByteArray &encodedBytes,
                                             const QString &contentType) {
  auto decoded = decodeAssetImage(encodedBytes, m_config.decodeLimits);
  if (!decoded) {
    return decoded.error();
  }

  promoteToMemory(QString::fromLatin1(canonicalUrlBytes(url)), *decoded);
  if (m_diskStatus == DiskStatus::Enabled) {
    storeDisk(diskCacheFileNameForUrl(url), encodedBytes, contentType);
  }
  return *decoded;
}

void AssetCache::clearMemory() { m_memoryCache.clear(); }

QByteArray AssetCache::canonicalUrlBytes(const QUrl &url) {
  const QUrl canonical = url.adjusted(QUrl::NormalizePathSegments);
  return canonical.toEncoded(QUrl::FullyEncoded);
}

QString AssetCache::filePathForName(const QString &fileName) const {
  return QDir(m_rootDirectory).filePath(fileName);
}

int AssetCache::imageCost(const QImage &image) const {
  const qsizetype size = image.sizeInBytes();
  if (size <= 0) {
    return 1;
  }
  return static_cast<int>(
      std::min<qsizetype>(size, std::numeric_limits<int>::max()));
}

void AssetCache::configureMemoryLimit(qint64 maxCostBytes) {
  if (maxCostBytes <= 0) {
    m_memoryCache.setMaxCost(0);
    return;
  }
  m_memoryCache.setMaxCost(static_cast<int>(
      std::min<qint64>(maxCostBytes, std::numeric_limits<int>::max())));
}

void AssetCache::openDiskCache() {
  if (m_config.diskMaxBytes <= 0) {
    disableDisk(QStringLiteral("disk cache disabled by zero byte limit"),
                false);
    return;
  }

  m_rootDirectory = m_config.rootDirectory.isEmpty() ? defaultRootDirectory()
                                                     : m_config.rootDirectory;
  if (m_rootDirectory.isEmpty()) {
    disableDisk(QStringLiteral("no writable cache location is available"),
                true);
    return;
  }
  m_rootDirectory = QDir::cleanPath(m_rootDirectory);

  QDir root(m_rootDirectory);
  if (!root.exists() && !QDir().mkpath(m_rootDirectory)) {
    disableDisk(QStringLiteral("could not create asset cache directory: %1")
                    .arg(m_rootDirectory),
                true);
    return;
  }
  const QFileInfo rootInfo(m_rootDirectory);
  if (!rootInfo.isDir()) {
    disableDisk(QStringLiteral("asset cache root is not a directory: %1")
                    .arg(m_rootDirectory),
                true);
    return;
  }

  const QString probePath =
      root.filePath(QStringLiteral(".arkham-cache-write-test"));
  QSaveFile probe(probePath);
  if (!probe.open(QIODevice::WriteOnly) || probe.write("ok", 2) != 2 ||
      !probe.commit()) {
    disableDisk(QStringLiteral("asset cache root is not writable: %1")
                    .arg(m_rootDirectory),
                true);
    return;
  }
  QFile::remove(probePath);

  m_diskStatus = DiskStatus::Enabled;
  m_diagnostic.clear();
  buildIndex();
  evictIfNeeded();
}

void AssetCache::disableDisk(QString diagnostic, bool warn) {
  m_diskStatus = DiskStatus::MemoryOnly;
  m_diagnostic = std::move(diagnostic);
  if (warn && !m_warnedDiskFailure) {
    qWarning().noquote()
        << "AssetCache disk cache unavailable; using memory-only cache:"
        << m_diagnostic;
    m_warnedDiskFailure = true;
  }
}

void AssetCache::buildIndex() {
  m_index.clear();
  m_diskBytes = 0;
  quint64 newestAccess = 0;

  const QFileInfoList files =
      QDir(m_rootDirectory)
          .entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                         QDir::Name);
  for (const QFileInfo &fileInfo : files) {
    const QString fileName = fileInfo.fileName();
    if (!isLowerHexSha256FileName(fileName)) {
      QFile::remove(fileInfo.absoluteFilePath());
      continue;
    }
    const qint64 fileSize = fileInfo.size();
    if (fileSize <= 0) {
      QFile::remove(fileInfo.absoluteFilePath());
      continue;
    }
    const quint64 lastAccess = static_cast<quint64>(
        std::max<qint64>(1, fileInfo.lastModified().toMSecsSinceEpoch()));
    m_index.insert(fileName, DiskEntry{fileSize, lastAccess});
    m_diskBytes += fileSize;
    newestAccess = std::max(newestAccess, lastAccess);
  }
  m_nextAccess = std::max<quint64>(m_nextAccess, newestAccess + 1);
}

void AssetCache::promoteToMemory(const QString &key, const QImage &image) {
  if (image.isNull() || m_memoryCache.maxCost() <= 0) {
    return;
  }
  const int cost = imageCost(image);
  if (cost > m_memoryCache.maxCost()) {
    return;
  }
  m_memoryCache.insert(key, new QImage(image), cost);
}

void AssetCache::touchDiskEntry(const QString &diskKey) {
  if (auto it = m_index.find(diskKey); it != m_index.end()) {
    it->lastAccess = nextAccess();
  }
}

AssetCache::LookupResult AssetCache::lookupDisk(const QString &diskKey,
                                                const QString &memoryKey) {
  if (!m_index.contains(diskKey)) {
    return {};
  }

  const QString path = filePathForName(diskKey);
  const QFileInfo fileInfo(path);
  const qint64 maxFileSize =
      static_cast<qint64>(m_config.decodeLimits.maxEncodedBytes) +
      kMaxHeaderBytes;
  if (!fileInfo.isFile() || fileInfo.size() <= 0 ||
      fileInfo.size() > maxFileSize) {
    removeDiskEntry(diskKey);
    return {};
  }

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  const QByteArray fileBytes = file.read(fileInfo.size() + 1);
  if (fileBytes.size() != fileInfo.size()) {
    removeDiskEntry(diskKey);
    return {};
  }

  const qsizetype separator = fileBytes.indexOf("\n\n");
  if (separator < 0 || separator > kMaxHeaderBytes) {
    removeDiskEntry(diskKey);
    return {};
  }

  const QByteArray header = fileBytes.first(separator);
  const QByteArray payload = fileBytes.sliced(separator + 2);
  const auto parsed =
      parseHeader(header, m_config.decodeLimits.maxEncodedBytes);
  if (!parsed || payload.size() != parsed->payloadLength ||
      payloadSha256Hex(payload) != parsed->payloadSha256) {
    removeDiskEntry(diskKey);
    return {};
  }

  auto decoded = decodeAssetImage(payload, m_config.decodeLimits);
  if (!decoded) {
    removeDiskEntry(diskKey);
    return {};
  }

  if (auto it = m_index.find(diskKey); it != m_index.end()) {
    m_diskBytes += fileInfo.size() - it->fileSize;
    it->fileSize = fileInfo.size();
    it->lastAccess = nextAccess();
  }
  promoteToMemory(memoryKey, *decoded);
  return LookupResult{LookupSource::Disk, *decoded};
}

void AssetCache::storeDisk(const QString &diskKey,
                           const QByteArray &encodedBytes,
                           const QString &contentType) {
  const QString path = filePathForName(diskKey);
  const QByteArray header = buildHeader(encodedBytes, contentType);
  const qint64 entrySize = header.size() + encodedBytes.size();
  if (entrySize > m_config.diskMaxBytes) {
    m_diagnostic =
        QStringLiteral("asset cache entry exceeds disk cache byte limit: %1")
            .arg(path);
    return;
  }

  QSaveFile output(path);
  if (!output.open(QIODevice::WriteOnly) ||
      output.write(header) != header.size() ||
      output.write(encodedBytes) != encodedBytes.size() || !output.commit()) {
    m_diagnostic =
        QStringLiteral("could not write asset cache entry: %1").arg(path);
    QFile stale(path);
    if (stale.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      stale.close();
    }
    removeDiskEntry(diskKey);
    return;
  }

  const qint64 oldSize = m_index.value(diskKey).fileSize;
  const qint64 newSize = QFileInfo(path).size();
  m_diskBytes += newSize - oldSize;
  m_index.insert(diskKey, DiskEntry{newSize, nextAccess()});
  evictIfNeeded();
}

void AssetCache::removeDiskEntry(const QString &key) {
  const QString path = filePathForName(key);
  const qint64 oldSize = m_index.value(key).fileSize;
  if (QFileInfo::exists(path)) {
    QFile::remove(path);
  }
  m_index.remove(key);
  m_diskBytes = std::max<qint64>(0, m_diskBytes - oldSize);
}

void AssetCache::evictIfNeeded() {
  if (m_diskStatus != DiskStatus::Enabled ||
      m_diskBytes <= m_config.diskMaxBytes) {
    return;
  }

  std::vector<QString> keys;
  keys.reserve(static_cast<size_t>(m_index.size()));
  for (auto it = m_index.cbegin(); it != m_index.cend(); ++it) {
    keys.push_back(it.key());
  }
  std::sort(keys.begin(), keys.end(),
            [this](const QString &left, const QString &right) {
              const DiskEntry leftEntry = m_index.value(left);
              const DiskEntry rightEntry = m_index.value(right);
              if (leftEntry.lastAccess != rightEntry.lastAccess) {
                return leftEntry.lastAccess < rightEntry.lastAccess;
              }
              return left < right;
            });

  for (const QString &key : keys) {
    if (m_diskBytes <= m_config.diskMaxBytes) {
      break;
    }
    removeDiskEntry(key);
  }
}

quint64 AssetCache::nextAccess() { return m_nextAccess++; }

} // namespace Arkham
