#include "AssetCacheTests.h"

#include "AssetCache.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
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
                             qint64 diskMaxBytes = 1024 * 1024) {
  AssetCache::Config config;
  config.directory = dir;
  config.memoryMaxCostBytes = 8 * 1024 * 1024;
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
  // pair actually came from this cache's own store() (which is only ever
  // fed encoded bytes that already passed AssetNetworkFetcher's own hard
  // 20 MiB incremental cap) versus a corrupted or locally-planted file
  // that simply happens to be internally consistent at some much larger
  // size. Reading back an entry like that with a single unconditional
  // readAll() has no upper bound at all. This test constructs exactly
  // such a pair directly via store() (bypassing the network fetcher's own
  // cap entirely, which store() itself does not enforce -- verification
  // happens only on the read-back path) and asserts it is rejected and
  // cleaned up rather than ever being served, even though every
  // consistency check that existed before this fix (SHA-256, size-versus-
  // metadata match) would have passed it.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/oversized.png")));
  const QByteArray oversized(20 * 1024 * 1024 + 4096, 'x');
  const QString payloadPath =
      m_tempDirPath + u'/' + key + QStringLiteral(".bin");
  const QString metadataPath =
      m_tempDirPath + u'/' + key + QStringLiteral(".meta.json");
  {
    AssetCache cache(configFor(m_tempDirPath, 64LL * 1024 * 1024));
    cache.store(key, makeEntry(oversized));
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
