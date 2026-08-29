#include "AssetCacheTests.h"

#include "AssetCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTest>

using namespace Arkham;

void AssetCacheTests::init() {
  m_tempDir = std::make_unique<QTemporaryDir>();
  QVERIFY(m_tempDir->isValid());
  m_tempDirPath = m_tempDir->path();
}

void AssetCacheTests::cleanup() { m_tempDir.reset(); }

namespace {

AssetCache::Config configFor(const QString &dir,
                             qint64 diskMaxBytes = 1024 * 1024,
                             qint64 memoryMaxBytes = 8 * 1024 * 1024) {
  AssetCache::Config config;
  config.directory = dir;
  config.memoryMaxCostBytes = memoryMaxBytes;
  config.diskMaxBytes = diskMaxBytes;
  return config;
}

AssetCache::CachedEntry
makeEntry(const QByteArray &bytes,
          const QString &contentType = QStringLiteral("image/png")) {
  AssetCache::CachedEntry entry;
  entry.encodedBytes = bytes;
  entry.contentType = contentType;
  entry.dimensions = QSize(1, 1);
  return entry;
}

// Removes `path` when this guard goes out of scope, including via an
// early QVERIFY-triggered return from within the test body -- used by
// the path-traversal test below so a would-be escape file is always
// cleaned up even if the fix under test regresses and the file actually
// gets created outside the managed QTemporaryDir.
struct ScopedFileRemoval {
  QString path;
  ~ScopedFileRemoval() { QFile::remove(path); }
};

} // namespace

void AssetCacheTests::storeThenLookupMemoryAndDiskRoundTrips() {
  AssetCache cache(configFor(m_tempDirPath));
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/a.png")));
  cache.store(key, makeEntry(QByteArrayLiteral("hello-world-bytes")));

  const auto memoryHit = cache.lookupMemory(key);
  QVERIFY(memoryHit.has_value());
  QCOMPARE(memoryHit->encodedBytes, QByteArrayLiteral("hello-world-bytes"));

  const auto diskHit = cache.lookupDisk(key);
  QVERIFY(diskHit.has_value());
  QCOMPARE(diskHit->encodedBytes, QByteArrayLiteral("hello-world-bytes"));
  QCOMPARE(diskHit->contentType, QStringLiteral("image/png"));
}

void AssetCacheTests::cacheKeyIsNamespacedByFullResolvedUrl() {
  // Two different hosts (or scheme/port/base-path) requesting the exact
  // same category/identifier/side must never collide: the key covers the
  // FULL resolved candidate URL, not just its path.
  const QString keyA = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://a.example.com/cards/01001.jpg")));
  const QString keyB = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://b.example.com/cards/01001.jpg")));
  const QString keyC = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://a.example.com:8443/cards/01001.jpg")));
  const QString keyD = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://a.example.com/base/cards/01001.jpg")));

  QVERIFY(keyA != keyB);
  QVERIFY(keyA != keyC);
  QVERIFY(keyA != keyD);
  QCOMPARE(keyA.size(), 64);
  QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
              .match(keyA)
              .hasMatch());

  AssetCache cache(configFor(m_tempDirPath));
  cache.store(keyA, makeEntry(QByteArrayLiteral("host-a")));
  cache.store(keyB, makeEntry(QByteArrayLiteral("host-b")));

  QCOMPARE(cache.lookupDisk(keyA)->encodedBytes, QByteArrayLiteral("host-a"));
  QCOMPARE(cache.lookupDisk(keyB)->encodedBytes, QByteArrayLiteral("host-b"));
}

void AssetCacheTests::orphanPayloadWithoutMetadataIsRepaired() {
  AssetCache cache(configFor(m_tempDirPath));
  const QString key = QStringLiteral("a").repeated(64);
  QFile payload(m_tempDirPath + u'/' + key + QStringLiteral(".bin"));
  QVERIFY(payload.open(QIODevice::WriteOnly));
  payload.write("orphan");
  payload.close();

  QVERIFY(!cache.lookupDisk(key).has_value());
  QVERIFY(!QFile::exists(payload.fileName())); // repaired: orphan removed
}

void AssetCacheTests::orphanMetadataWithoutPayloadIsRepaired() {
  AssetCache cache(configFor(m_tempDirPath));
  const QString key = QStringLiteral("b").repeated(64);
  QFile metadata(m_tempDirPath + u'/' + key + QStringLiteral(".meta.json"));
  QVERIFY(metadata.open(QIODevice::WriteOnly));
  metadata.write(R"({"formatVersion":1,"key":")" + key.toUtf8() +
                 R"(","sha256":"deadbeef","encodedSize":6})");
  metadata.close();

  QVERIFY(!cache.lookupDisk(key).has_value());
  QVERIFY(!QFile::exists(metadata.fileName()));
}

void AssetCacheTests::mismatchedPayloadMetadataPairIsRepaired() {
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/mismatch.png")));
  {
    AssetCache cache(configFor(m_tempDirPath));
    cache.store(key, makeEntry(QByteArrayLiteral("original-bytes")));
  }

  // Tamper with the payload on disk directly, invalidating the metadata's
  // recorded SHA-256/size without touching the metadata file at all --
  // this must never be served as a "half-valid success."
  QFile payload(m_tempDirPath + u'/' + key + QStringLiteral(".bin"));
  QVERIFY(payload.open(QIODevice::WriteOnly | QIODevice::Truncate));
  payload.write("tampered-bytes-of-different-content");
  payload.close();

  // lookupDisk() deliberately serves an already-resident memory entry
  // without re-validating the disk payload (see AssetCache.cpp): there is
  // no reason to re-read/re-hash a file this process already has correct,
  // known-good bytes for in memory. A fresh AssetCache instance -- exactly
  // as a real process restart would produce, with an empty memory cache --
  // is what actually exercises the on-disk repair path.
  AssetCache freshCache(configFor(m_tempDirPath));
  QVERIFY(!freshCache.lookupDisk(key).has_value());
  QVERIFY(!QFile::exists(payload.fileName()));
}

void AssetCacheTests::strayFileNotMatchingKeyShapeIsRemoved() {
  const QString strayPath =
      m_tempDirPath + QStringLiteral("/not-a-valid-key.tmp");
  {
    QFile stray(strayPath);
    QVERIFY(stray.open(QIODevice::WriteOnly));
    stray.write("leftover");
  }

  AssetCache cache(configFor(m_tempDirPath));
  cache.reapAndEnforceQuota();
  QVERIFY(!QFile::exists(strayPath));
}

void AssetCacheTests::quotaEvictsOldestAccessFirstDownToLowWaterMark() {
  // A tiny quota (5 entries of ~1 KiB fit comfortably below it, but 20
  // don't) forces eviction; the OLDEST-accessed entries must go first,
  // down to the 75% low-water mark.
  AssetCache cache(configFor(m_tempDirPath, 20 * 1024));
  const QByteArray payload(900, 'x');

  QStringList keys;
  for (int i = 0; i < 20; ++i) {
    const QString key = AssetCache::cacheKeyFor(
        QUrl(QStringLiteral("https://example.com/item-%1.png").arg(i)));
    keys.append(key);
    cache.store(key, makeEntry(payload));
    // Ensure strictly increasing lastAccess ordering regardless of clock
    // resolution.
    QTest::qWait(2);
  }

  QVERIFY(cache.diskUsageBytes() <= 20 * 1024);
  // The earliest-stored (and therefore least-recently-accessed) entries
  // must have been evicted first.
  QVERIFY(!cache.lookupDisk(keys.first()).has_value());
  QVERIFY(cache.lookupDisk(keys.last()).has_value());
}

void AssetCacheTests::storeSkipsFullReapSweepWhenComfortablyUnderQuota() {
  // Regression/behavior-pin for a review finding: store() must not
  // unconditionally run the expensive full reap/validate sweep (which
  // re-reads and re-hashes every cached payload) on every single call --
  // only once actual disk usage passes the 90% high-water mark. This
  // proves the guard actually SKIPS the sweep (not merely that skipping
  // it would be harmless): a stray file dropped into the cache directory
  // survives an unrelated, comfortably-under-quota store() call
  // untouched, and is only cleaned up once the sweep is explicitly
  // invoked (the same public reapAndEnforceQuota() escape hatch used at
  // startup, and automatically once quota IS exceeded, per
  // quotaEvictsOldestAccessFirstDownToLowWaterMark above).
  AssetCache cache(configFor(m_tempDirPath, 10 * 1024 * 1024));

  const QString strayPath =
      m_tempDirPath + QStringLiteral("/not-a-valid-key.tmp");
  {
    QFile stray(strayPath);
    QVERIFY(stray.open(QIODevice::WriteOnly));
    stray.write("leftover");
  }

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/tiny.png")));
  cache.store(key, makeEntry(QByteArrayLiteral("tiny-bytes")));

  QVERIFY2(QFile::exists(strayPath),
           "store() must skip the full reap sweep while disk usage is "
           "comfortably under the high-water mark");

  cache.reapAndEnforceQuota();
  QVERIFY(!QFile::exists(strayPath));
}

void AssetCacheTests::touchAfterNotModifiedRefreshesLastAccessAndHeaders() {
  AssetCache cache(configFor(m_tempDirPath));
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/etag.png")));
  AssetCache::CachedEntry entry = makeEntry(QByteArrayLiteral("etag-bytes"));
  entry.etag = QStringLiteral("\"v1\"");
  cache.store(key, entry);

  const qint64 originalAccess =
      cache.lookupDisk(key)->lastAccessMsecsSinceEpoch;
  QTest::qWait(5);
  cache.touchAfterNotModified(key, QStringLiteral("\"v2\""), QString());

  const auto touched = cache.lookupDisk(key);
  QVERIFY(touched.has_value());
  QCOMPARE(touched->etag, QStringLiteral("\"v2\""));
  QVERIFY(touched->lastAccessMsecsSinceEpoch >= originalAccess);
  // The payload itself must be completely untouched by a 304 touch.
  QCOMPARE(touched->encodedBytes, QByteArrayLiteral("etag-bytes"));
}

void AssetCacheTests::restartingWithSameDirectorySeesPriorEntries() {
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/restart.png")));
  {
    AssetCache cache(configFor(m_tempDirPath));
    cache.store(key, makeEntry(QByteArrayLiteral("persisted-across-restart")));
  }
  {
    AssetCache cache(configFor(m_tempDirPath));
    const auto hit = cache.lookupDisk(key);
    QVERIFY(hit.has_value());
    QCOMPARE(hit->encodedBytes, QByteArrayLiteral("persisted-across-restart"));
  }
}

void AssetCacheTests::
    oversizedSelfConsistentPayloadBeyondAbsoluteCapIsRejected() {
  // A payload/metadata pair can be internally self-consistent -- the
  // actual on-disk bytes match both the declared size AND the recorded
  // SHA-256 -- and still be dangerous to trust on read-back, because
  // lookupDisk()/reapAndEnforceQuota() have no way to know whether such a
  // pair actually came from this cache's own store() (which now refuses
  // to write anything this large up front -- see
  // storeRejectsPayloadBeyondAbsoluteCapWithoutTouchingDisk() below)
  // versus a corrupted/tampered file, a file planted by an attacker with
  // filesystem access, or a leftover pair from an older, less strict
  // version of this cache. This test plants exactly such a pair directly
  // on disk -- bypassing store() entirely, hand-writing the payload and
  // metadata JSON in the exact shape writeMetadata() itself produces --
  // and asserts it is rejected and cleaned up rather than ever being
  // served, even though every consistency check that existed before the
  // original fix (SHA-256, size-versus-metadata match) would have passed
  // it. Reading back an entry like that with a single unconditional
  // readAll() would otherwise have no upper bound at all.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/oversized.png")));
  const QByteArray oversized(20 * 1024 * 1024 + 4096, 'x');
  const QString payloadPath =
      m_tempDirPath + u'/' + key + QStringLiteral(".bin");
  const QString metadataPath =
      m_tempDirPath + u'/' + key + QStringLiteral(".meta.json");
  {
    QFile payload(payloadPath);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(oversized);
    payload.close();

    const QString sha256Hex = QString::fromLatin1(
        QCryptographicHash::hash(oversized, QCryptographicHash::Sha256)
            .toHex());
    QJsonObject obj;
    obj[QStringLiteral("formatVersion")] = 1;
    obj[QStringLiteral("key")] = key;
    obj[QStringLiteral("contentType")] = QStringLiteral("image/png");
    obj[QStringLiteral("encodedSize")] = oversized.size();
    obj[QStringLiteral("width")] = 1;
    obj[QStringLiteral("height")] = 1;
    obj[QStringLiteral("sha256")] = sha256Hex;
    obj[QStringLiteral("etag")] = QString();
    obj[QStringLiteral("lastModified")] = QString();
    obj[QStringLiteral("insertedAtMs")] = QDateTime::currentMSecsSinceEpoch();
    obj[QStringLiteral("lastAccessMs")] = QDateTime::currentMSecsSinceEpoch();

    QFile metadata(metadataPath);
    QVERIFY(metadata.open(QIODevice::WriteOnly));
    metadata.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    metadata.close();
  }
  QVERIFY(QFile::exists(payloadPath));
  QVERIFY(QFile::exists(metadataPath));

  // A fresh instance (simulating a restart) runs reapAndEnforceQuota() in
  // its own constructor, which must reject and remove this pair on its
  // own -- no explicit lookupDisk() call should even be necessary to
  // trigger the cleanup.
  AssetCache freshCache(configFor(m_tempDirPath, 64LL * 1024 * 1024));
  QVERIFY(!QFile::exists(payloadPath));
  QVERIFY(!QFile::exists(metadataPath));
  QVERIFY(!freshCache.lookupDisk(key).has_value());
}

void AssetCacheTests::
    storeRejectsPayloadBeyondAbsoluteCapWithoutTouchingDisk() {
  // Regression test: store() itself now enforces the same absolute cap
  // readVerifiedPayload() has always enforced on read-back
  // (kMaxSinglePayloadBytesOnDisk == 20 MiB), so an oversized entry is
  // rejected AT THE POINT OF WRITE and never touches disk at all --
  // rather than being written and only cleaned up later by a reap sweep
  // or a subsequent lookup, which would let an unbounded number of such
  // oversized writes accumulate as real disk usage in the meantime.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/oversized-store.png")));
  const QByteArray oversized(20 * 1024 * 1024 + 1, 'y');
  const QString payloadPath =
      m_tempDirPath + u'/' + key + QStringLiteral(".bin");
  const QString metadataPath =
      m_tempDirPath + u'/' + key + QStringLiteral(".meta.json");

  AssetCache cache(
      configFor(m_tempDirPath, 64LL * 1024 * 1024, 64LL * 1024 * 1024));
  cache.store(key, makeEntry(oversized));

  QVERIFY(!QFile::exists(payloadPath));
  QVERIFY(!QFile::exists(metadataPath));
  // The memory cache is unaffected by this disk-side guard: it still
  // serves the entry via its own independent, cost-based eviction.
  QVERIFY(cache.lookupMemory(key).has_value());
}

void AssetCacheTests::
    metadataWriteFailureAfterPayloadCommitCleansUpOrphanPayload() {
  // Regression test: store() writes the payload first (atomically via
  // QSaveFile) and only then writes metadata, which is the SOLE validity
  // witness for that payload (see the comment in store()). If metadata
  // fails to write AFTER the payload commit already succeeded, the
  // just-committed payload must be deleted immediately -- not left
  // behind as an orphan that only a later reap/lookup sweep happens to
  // notice, silently leaking disk usage in the meantime.
  //
  // Forces writeMetadata() to fail deterministically (rather than
  // relying on filesystem permission bits, which are unreliable across
  // platforms/sandboxes) by pre-creating a DIRECTORY at the exact path
  // metadataPath() would use: QSaveFile::open() cannot open a directory
  // for writing, so writeMetadata() fails exactly like a real disk-full
  // or permission-denied failure would, without depending on any
  // OS-specific permission enforcement.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/metadata-fail.png")));
  const QString payloadPath =
      m_tempDirPath + u'/' + key + QStringLiteral(".bin");
  const QString metadataPath =
      m_tempDirPath + u'/' + key + QStringLiteral(".meta.json");

  QVERIFY(QDir(m_tempDirPath).mkpath(key + QStringLiteral(".meta.json")));
  QVERIFY(QFileInfo(metadataPath).isDir());

  AssetCache cache(configFor(m_tempDirPath));
  cache.store(key, makeEntry(QByteArrayLiteral("orphan-candidate-bytes")));

  // The payload must never be left behind once its sole validity witness
  // (metadata) failed to write.
  QVERIFY(!QFile::exists(payloadPath));
  // The directory planted at the metadata path is untouched: store()
  // must never attempt to delete or otherwise overwrite it.
  QVERIFY(QFileInfo(metadataPath).isDir());
}

void AssetCacheTests::oversizedMetadataFileIsRejectedWithoutUnboundedReadAll() {
  // Copilot review (round 27): readMetadata() called QFile::readAll() on
  // the metadata JSON with no size bound at all. This cache directory is
  // locally writable and can contain a corrupted or maliciously-planted
  // *.meta.json file; a legitimate metadata file is always tiny (a
  // handful of short strings/numbers), so an oversized one -- valid JSON
  // or not -- must be rejected on the cheap stat alone, never read in
  // full. This test plants a metadata file exceeding the absolute cap
  // (kMaxMetadataBytesOnDisk, 64 KiB) directly on disk -- valid JSON,
  // padded via an oversized "etag" field so it would otherwise parse
  // successfully -- and asserts it is rejected and cleaned up rather
  // than ever being served.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/oversized-metadata.png")));
  const QString payloadPath =
      m_tempDirPath + u'/' + key + QStringLiteral(".bin");
  const QString metadataPath =
      m_tempDirPath + u'/' + key + QStringLiteral(".meta.json");
  const QByteArray payloadBytes = QByteArrayLiteral("small-payload-bytes");
  {
    QFile payload(payloadPath);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(payloadBytes);
    payload.close();

    const QString sha256Hex = QString::fromLatin1(
        QCryptographicHash::hash(payloadBytes, QCryptographicHash::Sha256)
            .toHex());
    QJsonObject obj;
    obj[QStringLiteral("formatVersion")] = 1;
    obj[QStringLiteral("key")] = key;
    obj[QStringLiteral("contentType")] = QStringLiteral("image/png");
    obj[QStringLiteral("encodedSize")] = payloadBytes.size();
    obj[QStringLiteral("width")] = 1;
    obj[QStringLiteral("height")] = 1;
    obj[QStringLiteral("sha256")] = sha256Hex;
    // Padding field: still perfectly valid JSON, but pushes the whole
    // file past the 64 KiB metadata cap (128 KiB of filler text).
    obj[QStringLiteral("etag")] = QString(128 * 1024, QLatin1Char('e'));
    obj[QStringLiteral("lastModified")] = QString();
    obj[QStringLiteral("insertedAtMs")] = QDateTime::currentMSecsSinceEpoch();
    obj[QStringLiteral("lastAccessMs")] = QDateTime::currentMSecsSinceEpoch();

    QFile metadata(metadataPath);
    QVERIFY(metadata.open(QIODevice::WriteOnly));
    metadata.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    metadata.close();
  }
  QVERIFY(QFile::exists(payloadPath));
  QVERIFY(QFileInfo(metadataPath).size() > 64 * 1024);

  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.lookupDisk(key).has_value());
  QVERIFY(!QFile::exists(payloadPath));
  QVERIFY(!QFile::exists(metadataPath));
}

void AssetCacheTests::
    malformedKeyWithPathTraversalNeverTouchesFilesystemOutsideCacheDir() {
  // AssetCache is a public API: nothing in the type system stops a
  // caller from forwarding an arbitrary string as `key`, and every
  // disk-touching entry point (lookupDisk(), store(),
  // touchAfterNotModified()) turns `key` directly into a filesystem path
  // via payloadPath()/metadataPath() ("<dir>/<key>.bin",
  // "<dir>/<key>.meta.json"). This test uses a key crafted with "../"
  // segments to escape the cache directory entirely and confirms none of
  // the three entry points ever creates, reads, or deletes anything
  // outside the cache directory -- nor anything unexpected inside it.
  const QString maliciousKey =
      QStringLiteral("../asset-cache-traversal-canary");

  // Compute exactly where the (would-be) escaped payload/metadata paths
  // resolve to, the same way AssetCache itself builds them internally,
  // so the assertions below check the real physical location rather
  // than guessing. The escape target lands one directory above the
  // managed QTemporaryDir (still within the OS temp hierarchy, never a
  // real user path), and ScopedFileRemoval guarantees cleanup even if
  // this test fails because the fix under test regressed.
  const QString escapedPayloadPath = QDir::cleanPath(
      m_tempDirPath + u'/' + maliciousKey + QStringLiteral(".bin"));
  const QString escapedMetadataPath = QDir::cleanPath(
      m_tempDirPath + u'/' + maliciousKey + QStringLiteral(".meta.json"));
  QVERIFY(!escapedPayloadPath.startsWith(m_tempDirPath));
  QVERIFY(!escapedMetadataPath.startsWith(m_tempDirPath));
  const ScopedFileRemoval payloadGuard{escapedPayloadPath};
  const ScopedFileRemoval metadataGuard{escapedMetadataPath};

  AssetCache cache(configFor(m_tempDirPath));

  cache.store(maliciousKey, makeEntry(QByteArrayLiteral("traversal-payload")));
  QVERIFY(!QFile::exists(escapedPayloadPath));
  QVERIFY(!QFile::exists(escapedMetadataPath));

  QVERIFY(!cache.lookupDisk(maliciousKey).has_value());

  cache.touchAfterNotModified(maliciousKey, QStringLiteral("etag"), QString());
  QVERIFY(!QFile::exists(escapedPayloadPath));
  QVERIFY(!QFile::exists(escapedMetadataPath));

  // A rejected key must be a complete no-op, not a partial write under
  // some sanitized-but-still-wrong path inside the cache directory.
  QDir cacheDir(m_tempDirPath);
  QVERIFY(cacheDir.entryList(QDir::Files).isEmpty());
}

void AssetCacheTests::promoteToMemoryRejectsMalformedKeyWithoutInserting() {
  // Copilot review (round 31, suppressed comment): unlike
  // lookupDisk()/store()/touchAfterNotModified(), promoteToMemory()
  // never turns `key` into a filesystem path -- a malformed key can't
  // cause path traversal here -- but it's a public API, and every other
  // entry point enforces the invariant that entries only ever exist in
  // the cache under a cacheKeyFor()-shaped (64-hex) key. This proves
  // promoteToMemory() rejects a malformed key rather than silently
  // breaking that invariant.
  const QString malformedKey = QStringLiteral("../not-a-real-cache-key");

  AssetCache cache(configFor(m_tempDirPath));
  cache.promoteToMemory(malformedKey,
                        makeEntry(QByteArrayLiteral("should-never-be-stored")));

  QVERIFY(!cache.lookupMemory(malformedKey).has_value());
}
