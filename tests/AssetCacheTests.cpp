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

// Hand-writes a generation-scoped metadata JSON file at `metadataPath` in
// exactly the shape AssetCache's own (private) writeMetadata() produces,
// so fault-injection tests can construct crash-boundary on-disk states
// (a generation's files present, with or without a manifest naming it)
// without needing access to AssetCache's private write path itself.
void writeRawMetadataForTesting(const QString &metadataPath, const QString &key,
                                const QString &generation, qint64 encodedSize) {
  QJsonObject obj;
  obj[QStringLiteral("formatVersion")] = 1;
  obj[QStringLiteral("key")] = key;
  obj[QStringLiteral("contentType")] = QStringLiteral("image/png");
  obj[QStringLiteral("encodedSize")] = encodedSize;
  obj[QStringLiteral("width")] = 1;
  obj[QStringLiteral("height")] = 1;
  obj[QStringLiteral("sha256")] = generation;
  obj[QStringLiteral("etag")] = QString();
  obj[QStringLiteral("lastModified")] = QString();
  obj[QStringLiteral("insertedAtMs")] = QDateTime::currentMSecsSinceEpoch();
  obj[QStringLiteral("lastAccessMs")] = QDateTime::currentMSecsSinceEpoch();
  QFile file(metadataPath);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

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
  // Review item 8: this models a crash between a generation's payload
  // commit and its metadata commit (or, equivalently for a fresh
  // insert, a crash before the manifest that would ever have made it
  // "live" was written at all) -- a fully-formed, content-addressed
  // generation payload file sits on disk with nothing referencing it.
  AssetCache cache(configFor(m_tempDirPath));
  const QString key = QStringLiteral("a").repeated(64);
  const QByteArray bytes = QByteArrayLiteral("orphan");
  const QString generation = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  QFile payload(
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generation));
  QVERIFY(payload.open(QIODevice::WriteOnly));
  payload.write(bytes);
  payload.close();

  QVERIFY(!cache.lookupDisk(key).has_value());
  QVERIFY(!QFile::exists(payload.fileName())); // repaired: orphan removed
}

void AssetCacheTests::orphanMetadataWithoutPayloadIsRepaired() {
  AssetCache cache(configFor(m_tempDirPath));
  const QString key = QStringLiteral("b").repeated(64);
  const QString generation = QStringLiteral("d").repeated(64);
  QFile metadata(
      AssetCache::metadataPathForTesting(m_tempDirPath, key, generation));
  QVERIFY(metadata.open(QIODevice::WriteOnly));
  metadata.write(R"({"formatVersion":1,"key":")" + key.toUtf8() +
                 R"(","sha256":")" + generation.toUtf8() +
                 R"(","encodedSize":6})");
  metadata.close();

  QVERIFY(!cache.lookupDisk(key).has_value());
  QVERIFY(!QFile::exists(metadata.fileName()));
}

void AssetCacheTests::mismatchedPayloadMetadataPairIsRepaired() {
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/mismatch.png")));
  const QByteArray originalBytes = QByteArrayLiteral("original-bytes");
  const QString generation = QString::fromLatin1(
      QCryptographicHash::hash(originalBytes, QCryptographicHash::Sha256)
          .toHex());
  {
    AssetCache cache(configFor(m_tempDirPath));
    cache.store(key, makeEntry(originalBytes));
  }

  // Tamper with the LIVE generation's payload on disk directly,
  // invalidating its own metadata's recorded SHA-256/size without
  // touching the metadata file at all -- this must never be served as a
  // "half-valid success."
  QFile payload(
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generation));
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

void AssetCacheTests::
    initialInsertCrashBeforeManifestPublishLeavesNoValidEntry() {
  // Review item 8: simulates a crash strictly AFTER a brand-new
  // generation's payload+metadata files both committed successfully but
  // BEFORE the manifest that would have made them "live" was ever
  // written -- for a key that had NO prior entry at all. Nothing was
  // ever a valid success here (the manifest is the sole "this exists"
  // witness), so a lookup must miss, and the orphaned pair must be
  // reclaimed rather than leaking disk usage forever.
  const QString key = QStringLiteral("e").repeated(64);
  const QByteArray bytes = QByteArrayLiteral("never-published-bytes");
  const QString generation = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  const QString payloadPath =
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generation);
  const QString metadataPath =
      AssetCache::metadataPathForTesting(m_tempDirPath, key, generation);
  {
    QFile payload(payloadPath);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(bytes);
  }
  writeRawMetadataForTesting(metadataPath, key, generation, bytes.size());
  QVERIFY(QFile::exists(payloadPath));
  QVERIFY(QFile::exists(metadataPath));
  // Deliberately no manifest written: this generation was never
  // published.

  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.lookupDisk(key).has_value());
  QVERIFY(!QFile::exists(payloadPath));
  QVERIFY(!QFile::exists(metadataPath));
}

void AssetCacheTests::
    replacementCrashBeforeManifestSwapPreservesOldGenerationIntact() {
  // Review item 8: after a REAL store() has published generation A
  // (manifest -> A), simulate a crash immediately after a REPLACEMENT
  // generation B's payload+metadata both committed but strictly BEFORE
  // the manifest swap that would have published B ever ran. The
  // manifest still names A, and A's own files were never touched by any
  // of this (content-addressed generations never share a filename) --
  // so a restart must still serve exactly A's original bytes, and B's
  // now-orphaned files must be reclaimed by the repair sweep without
  // touching A at all.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/replace-before-swap.png")));
  const QByteArray bytesA = QByteArrayLiteral("generation-a-bytes");
  const QByteArray bytesB = QByteArrayLiteral("generation-b-bytes-different");
  const QString generationA = QString::fromLatin1(
      QCryptographicHash::hash(bytesA, QCryptographicHash::Sha256).toHex());
  const QString generationB = QString::fromLatin1(
      QCryptographicHash::hash(bytesB, QCryptographicHash::Sha256).toHex());
  {
    AssetCache cache(configFor(m_tempDirPath));
    cache.store(key, makeEntry(bytesA));
  }

  const QString payloadPathB =
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generationB);
  const QString metadataPathB =
      AssetCache::metadataPathForTesting(m_tempDirPath, key, generationB);
  {
    QFile payload(payloadPathB);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(bytesB);
  }
  writeRawMetadataForTesting(metadataPathB, key, generationB, bytesB.size());
  QVERIFY(QFile::exists(payloadPathB));
  QVERIFY(QFile::exists(metadataPathB));
  // The manifest is deliberately left untouched -- still naming A.

  AssetCache freshCache(configFor(m_tempDirPath));
  const auto hit = freshCache.lookupDisk(key);
  QVERIFY(hit.has_value());
  QCOMPARE(hit->encodedBytes, bytesA);
  // B's orphaned files, never published, must be reclaimed; A's own
  // files (a different filename entirely) are untouched by that
  // reclamation.
  QVERIFY(!QFile::exists(payloadPathB));
  QVERIFY(!QFile::exists(metadataPathB));
  QVERIFY(QFile::exists(
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generationA)));
}

void AssetCacheTests::
    replacementCrashAfterManifestSwapPromotesNewGenerationAndReclaimsOld() {
  // Review item 8: the opposite boundary -- generation B's files
  // committed AND the manifest swap to B itself committed, but the
  // crash happens strictly BEFORE the old generation A's cleanup ran.
  // A restart must serve B (the new, now-live generation) and reclaim
  // A's now-superseded files.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/replace-after-swap.png")));
  const QByteArray bytesA = QByteArrayLiteral("generation-a-bytes");
  const QByteArray bytesB = QByteArrayLiteral("generation-b-bytes-different");
  const QString generationA = QString::fromLatin1(
      QCryptographicHash::hash(bytesA, QCryptographicHash::Sha256).toHex());
  const QString generationB = QString::fromLatin1(
      QCryptographicHash::hash(bytesB, QCryptographicHash::Sha256).toHex());
  {
    AssetCache cache(configFor(m_tempDirPath));
    cache.store(key, makeEntry(bytesA));
  }

  const QString payloadPathB =
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generationB);
  const QString metadataPathB =
      AssetCache::metadataPathForTesting(m_tempDirPath, key, generationB);
  {
    QFile payload(payloadPathB);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(bytesB);
  }
  writeRawMetadataForTesting(metadataPathB, key, generationB, bytesB.size());

  // Simulate the manifest swap itself having already committed (the
  // manifest's own JSON shape is documented in AssetCache.h/.cpp; this
  // is the exact shape writeManifest() itself produces).
  {
    QFile manifest(AssetCache::manifestPathForTesting(m_tempDirPath, key));
    QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
    manifest.write(R"({"formatVersion":1,"key":")" + key.toUtf8() +
                   R"(","generation":")" + generationB.toUtf8() + R"("})");
  }

  const QString payloadPathA =
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generationA);
  const QString metadataPathA =
      AssetCache::metadataPathForTesting(m_tempDirPath, key, generationA);
  QVERIFY(QFile::exists(payloadPathA)); // A's files still present

  AssetCache freshCache(configFor(m_tempDirPath));
  const auto hit = freshCache.lookupDisk(key);
  QVERIFY(hit.has_value());
  QCOMPARE(hit->encodedBytes, bytesB);
  QVERIFY(!QFile::exists(payloadPathA));
  QVERIFY(!QFile::exists(metadataPathA));
  QVERIFY(QFile::exists(payloadPathB));
  QVERIFY(QFile::exists(metadataPathB));
}

void AssetCacheTests::storeReplacementLeavesExactlyOneLiveGenerationOnDisk() {
  // Complements the fault-injection tests above with the ordinary,
  // uninterrupted case: two REAL store() calls for the same key (a
  // normal cache refresh) must leave exactly one generation's files on
  // disk afterward -- proving store()'s own post-publish old-generation
  // cleanup actually runs, not just that a crash mid-way would be safe.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/normal-replace.png")));
  AssetCache cache(configFor(m_tempDirPath));
  cache.store(key, makeEntry(QByteArrayLiteral("first-version-bytes")));
  cache.store(key, makeEntry(QByteArrayLiteral("second-version-bytes")));

  const auto hit = cache.lookupDisk(key);
  QVERIFY(hit.has_value());
  QCOMPARE(hit->encodedBytes, QByteArrayLiteral("second-version-bytes"));

  QDir dir(m_tempDirPath);
  int payloadCount = 0;
  int metadataCount = 0;
  for (const QString &name : dir.entryList(QDir::Files)) {
    if (name.startsWith(key) && name.endsWith(QStringLiteral(".bin"))) {
      ++payloadCount;
    } else if (name.startsWith(key) &&
               name.endsWith(QStringLiteral(".meta.json")) &&
               !name.endsWith(QStringLiteral(".manifest.json"))) {
      ++metadataCount;
    }
  }
  QCOMPARE(payloadCount, 1);
  QCOMPARE(metadataCount, 1);
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

void AssetCacheTests::strayDirectoryIsRemovedAndCountedTowardDiskUsage() {
  // Copilot review (round 35): reapAndEnforceQuota() and
  // diskUsageBytes() previously enumerated QDir::Files only, so a
  // planted/stray DIRECTORY inside this cache's exclusively-owned
  // directory was invisible to both cleanup and usage accounting -- a
  // large stray directory could sit there forever, unreachable by the
  // repair sweep and never counted toward the quota gate that decides
  // whether to even run that sweep.
  AssetCache cache(configFor(m_tempDirPath));

  const QString strayDirPath =
      m_tempDirPath + QStringLiteral("/stray-planted-directory");
  QVERIFY(
      QDir(m_tempDirPath).mkpath(QStringLiteral("stray-planted-directory")));
  {
    QFile nested(strayDirPath + QStringLiteral("/nested-file.bin"));
    QVERIFY(nested.open(QIODevice::WriteOnly));
    nested.write(QByteArray(4096, 'z'));
  }
  QVERIFY(QFileInfo(strayDirPath).isDir());

  // diskUsageBytes() must count the nested file's bytes even though they
  // sit inside a directory this cache never created.
  QCOMPARE(cache.diskUsageBytes(), qint64(4096));

  cache.reapAndEnforceQuota();

  // The entire stray directory (and its contents) must be gone, and
  // usage accounting must reflect that it is actually gone.
  QVERIFY(!QFileInfo::exists(strayDirPath));
  QCOMPARE(cache.diskUsageBytes(), qint64(0));
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

void AssetCacheTests::
    touchAfterNotModifiedWithMissingMetadataRepairsOrphanPayload() {
  // Copilot review: touchAfterNotModified() is only ever called after a
  // 304 response to a conditional request the caller issued believing a
  // valid disk entry existed -- if the metadata file has since gone
  // missing or become corrupt (e.g. a crash between payload and metadata
  // commits, or external tampering), this is itself a repair signal, not
  // merely "nothing to touch." It must clean up any orphaned payload
  // left behind, exactly as lookupDisk() and reapAndEnforceQuota() do,
  // rather than silently leaving unreclaimable disk usage behind.
  AssetCache cache(configFor(m_tempDirPath));
  const QString key = QStringLiteral("c").repeated(64);
  const QByteArray bytes = QByteArrayLiteral("orphan-touch");
  const QString generation = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  QFile payload(
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generation));
  QVERIFY(payload.open(QIODevice::WriteOnly));
  payload.write(bytes);
  payload.close();
  QVERIFY(QFile::exists(payload.fileName()));

  cache.touchAfterNotModified(key, QStringLiteral("\"v2\""), QString());

  QVERIFY(!QFile::exists(payload.fileName()));
  QVERIFY(!cache.lookupDisk(key).has_value());
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
  const QString sha256Hex = QString::fromLatin1(
      QCryptographicHash::hash(oversized, QCryptographicHash::Sha256).toHex());
  const QString payloadPath =
      AssetCache::payloadPathForTesting(m_tempDirPath, key, sha256Hex);
  const QString metadataPath =
      AssetCache::metadataPathForTesting(m_tempDirPath, key, sha256Hex);
  const QString manifestPath =
      AssetCache::manifestPathForTesting(m_tempDirPath, key);
  {
    QFile payload(payloadPath);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(oversized);
    payload.close();

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

    QJsonObject manifestObj;
    manifestObj[QStringLiteral("formatVersion")] = 1;
    manifestObj[QStringLiteral("key")] = key;
    manifestObj[QStringLiteral("generation")] = sha256Hex;
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(manifestObj).toJson(QJsonDocument::Compact));
    manifest.close();
  }
  QVERIFY(QFile::exists(payloadPath));
  QVERIFY(QFile::exists(metadataPath));
  QVERIFY(QFile::exists(manifestPath));

  // A fresh instance (simulating a restart) runs reapAndEnforceQuota() in
  // its own constructor, which must reject and remove this pair on its
  // own -- no explicit lookupDisk() call should even be necessary to
  // trigger the cleanup.
  AssetCache freshCache(configFor(m_tempDirPath, 64LL * 1024 * 1024));
  QVERIFY(!QFile::exists(payloadPath));
  QVERIFY(!QFile::exists(metadataPath));
  QVERIFY(!QFile::exists(manifestPath));
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
  const QString generation = QString::fromLatin1(
      QCryptographicHash::hash(oversized, QCryptographicHash::Sha256).toHex());
  const QString payloadPath =
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generation);
  const QString metadataPath =
      AssetCache::metadataPathForTesting(m_tempDirPath, key, generation);
  const QString manifestPath =
      AssetCache::manifestPathForTesting(m_tempDirPath, key);

  AssetCache cache(
      configFor(m_tempDirPath, 64LL * 1024 * 1024, 64LL * 1024 * 1024));
  cache.store(key, makeEntry(oversized));

  QVERIFY(!QFile::exists(payloadPath));
  QVERIFY(!QFile::exists(metadataPath));
  QVERIFY(!QFile::exists(manifestPath));
  // The memory cache is unaffected by this disk-side guard: it still
  // serves the entry via its own independent, cost-based eviction.
  QVERIFY(cache.lookupMemory(key).has_value());
}

void AssetCacheTests::
    metadataWriteFailureAfterPayloadCommitCleansUpOrphanPayload() {
  // Regression test: store() writes a generation's payload first
  // (atomically via QSaveFile) and only then writes that generation's
  // metadata, which is the SOLE validity witness for it (see the
  // comment in store()). If metadata fails to write AFTER the payload
  // commit already succeeded, the just-committed payload must be
  // deleted immediately -- not left behind as an orphan that only a
  // later reap/lookup sweep happens to notice, silently leaking disk
  // usage in the meantime.
  //
  // Forces writeMetadata() to fail deterministically (rather than
  // relying on filesystem permission bits, which are unreliable across
  // platforms/sandboxes) by pre-creating a DIRECTORY at the exact
  // generation-scoped path store() will compute for these exact bytes:
  // QSaveFile::open() cannot open a directory for writing, so
  // writeMetadata() fails exactly like a real disk-full or
  // permission-denied failure would, without depending on any
  // OS-specific permission enforcement.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/metadata-fail.png")));
  const QByteArray bytes = QByteArrayLiteral("orphan-candidate-bytes");
  const QString generation = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  const QString payloadPath =
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generation);
  const QString metadataPath =
      AssetCache::metadataPathForTesting(m_tempDirPath, key, generation);

  // Construct the cache BEFORE planting the blocking directory: the
  // constructor runs an unconditional reapAndEnforceQuota() sweep at
  // startup which (per Copilot review round 35) now recursively removes
  // any stray directory it finds, since this cache directory is
  // exclusively owned by AssetCache and a directory can never
  // legitimately sit at a generation-metadata-shaped path. Planting the
  // directory afterward, immediately before the single store() call
  // under test, ensures that startup sweep cannot race with (and
  // silently invalidate) this test's own deliberately-planted failure
  // trigger.
  AssetCache cache(configFor(m_tempDirPath));

  QVERIFY(QDir().mkpath(metadataPath));
  QVERIFY(QFileInfo(metadataPath).isDir());

  cache.store(key, makeEntry(bytes));

  // The payload must never be left behind once its sole validity witness
  // (metadata) failed to write.
  QVERIFY(!QFile::exists(payloadPath));
  // The directory planted at the metadata path is untouched: store()
  // itself must never attempt to delete or otherwise overwrite it (only
  // the much rarer reapAndEnforceQuota() sweep -- not triggered by this
  // single small store() call, since usage stays well under the
  // configured high water mark -- ever removes stray directories).
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
  const QByteArray payloadBytes = QByteArrayLiteral("small-payload-bytes");
  const QString sha256Hex = QString::fromLatin1(
      QCryptographicHash::hash(payloadBytes, QCryptographicHash::Sha256)
          .toHex());
  const QString payloadPath =
      AssetCache::payloadPathForTesting(m_tempDirPath, key, sha256Hex);
  const QString metadataPath =
      AssetCache::metadataPathForTesting(m_tempDirPath, key, sha256Hex);
  const QString manifestPath =
      AssetCache::manifestPathForTesting(m_tempDirPath, key);
  {
    QFile payload(payloadPath);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(payloadBytes);
    payload.close();

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

    QJsonObject manifestObj;
    manifestObj[QStringLiteral("formatVersion")] = 1;
    manifestObj[QStringLiteral("key")] = key;
    manifestObj[QStringLiteral("generation")] = sha256Hex;
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(manifestObj).toJson(QJsonDocument::Compact));
    manifest.close();
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
  // via manifestPath()/generationPayloadPath()/generationMetadataPath().
  // This test uses a key crafted with "../" segments to escape the
  // cache directory entirely and confirms none of the three entry
  // points ever creates, reads, or deletes anything outside the cache
  // directory -- nor anything unexpected inside it.
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

void AssetCacheTests::
    symlinkedCacheRootDisablesDiskIoAndLeavesTargetUntouched() {
  // Review item 7 (HIGH): a naive AssetCache pointed directly at a
  // symlink would follow it transparently -- QDir::mkpath()/QSaveFile
  // both resolve symlinks -- so a cache "directory" that is itself a
  // symlink (e.g. an attacker-planted or misconfigured deployment) must
  // be refused outright, not silently followed into whatever it points
  // at. Build a genuine external sentinel directory with a real file in
  // it, point AssetCache's configured directory at a symlink to that
  // sentinel (never at the sentinel path directly), and prove: (a) the
  // cache reports disk I/O as disabled, (b) store()/lookupDisk()/reap
  // are all effective no-ops with respect to disk, and (c) the
  // sentinel's own pre-existing file survives completely untouched.
  const QString sentinelDir =
      m_tempDirPath + QStringLiteral("/external-sentinel");
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("external-sentinel")));
  const QString sentinelFile = sentinelDir + QStringLiteral("/precious.txt");
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("do-not-touch"));
  }

  const QString symlinkRoot =
      m_tempDirPath + QStringLiteral("/cache-root-symlink");
  QVERIFY(QFile::link(sentinelDir, symlinkRoot));
  QVERIFY(QFileInfo(symlinkRoot).isSymLink());

  AssetCache cache(configFor(symlinkRoot));
  QVERIFY(cache.isDiskCacheDisabledForTesting());

  const QString key = QString::fromLatin1(
      QCryptographicHash::hash(QByteArrayLiteral("symlink-root-key"),
                               QCryptographicHash::Sha256)
          .toHex());
  cache.store(key, makeEntry(QByteArrayLiteral("payload-bytes")));
  // The memory cache is unaffected by the disabled disk path -- only
  // persistence to (through) the symlink is skipped.
  QVERIFY(cache.lookupMemory(key).has_value());
  QCOMPARE(cache.diskUsageBytes(), qint64(0));
  QCOMPARE(cache.diskEntryCount(), 0);

  cache.reapAndEnforceQuota();

  // The sentinel directory's real, pre-existing file must be completely
  // untouched: no cache files written into it, and its own content
  // unchanged.
  QVERIFY(QFileInfo::exists(sentinelFile));
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("do-not-touch"));
  }
  QDir sentinelListing(sentinelDir);
  QCOMPARE(sentinelListing.entryList(QDir::Files), QStringList{"precious.txt"});
}

void AssetCacheTests::directorySymlinkInsideCacheRootIsUnlinkedNotFollowed() {
  // Review item 7 (HIGH): reapAndEnforceQuota()'s stray-entry sweep
  // previously classified every entry with QFileInfo::isDir() (which
  // FOLLOWS symlinks) and then called QDir::removeRecursively() (which
  // also follows symlinks internally) on anything that looked like a
  // directory -- so a directory SYMLINK planted inside a genuine cache
  // root, pointing at an external sentinel directory OUTSIDE the cache
  // root, would have its entire target recursively deleted. The
  // sentinel here is a SIBLING of the cache root (not nested under it)
  // so that a regression that actually follows the symlink is
  // distinguishable from the pre-existing "a real directory placed
  // directly inside the cache root is stray and removed" behavior.
  // Prove the fixed sweep instead removes only the symlink node itself,
  // and the sentinel's real directory (and the file inside it) survives
  // untouched.
  const QString cacheRoot = m_tempDirPath + QStringLiteral("/cache-root");
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("cache-root")));
  const QString sentinelDir =
      m_tempDirPath + QStringLiteral("/external-sentinel-dir");
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("external-sentinel-dir")));
  const QString sentinelFile = sentinelDir + QStringLiteral("/keepme.bin");
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("external-directory-contents"));
  }

  AssetCache cache(configFor(cacheRoot));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  const QString plantedSymlink =
      cacheRoot + QStringLiteral("/planted-dir-symlink");
  QVERIFY(QFile::link(sentinelDir, plantedSymlink));
  QVERIFY(QFileInfo(plantedSymlink).isSymLink());

  cache.reapAndEnforceQuota();

  // The symlink node itself must be gone (it is a stray entry, exactly
  // like any other unrecognized file), but the external sentinel
  // directory and its contents must be completely untouched.
  QVERIFY(!QFileInfo::exists(plantedSymlink));
  QVERIFY(QFileInfo::exists(sentinelDir));
  QVERIFY(QFileInfo::exists(sentinelFile));
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("external-directory-contents"));
  }
}

void AssetCacheTests::fileSymlinkInsideCacheRootIsUnlinkedNotFollowed() {
  // Review item 7 (HIGH), file-symlink variant: a symlink planted
  // inside the cache root pointing at an external sentinel FILE (not a
  // directory), OUTSIDE the cache root, must also be removed as a
  // stray entry via a plain unlink of the symlink node -- never by
  // opening/truncating/removing whatever it points at.
  const QString cacheRoot = m_tempDirPath + QStringLiteral("/cache-root");
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("cache-root")));
  const QString sentinelFile =
      m_tempDirPath + QStringLiteral("/external-sentinel-file.bin");
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("external-file-contents"));
  }

  AssetCache cache(configFor(cacheRoot));

  const QString plantedSymlink =
      cacheRoot + QStringLiteral("/planted-file-symlink");
  QVERIFY(QFile::link(sentinelFile, plantedSymlink));
  QVERIFY(QFileInfo(plantedSymlink).isSymLink());

  cache.reapAndEnforceQuota();

  QVERIFY(!QFileInfo::exists(plantedSymlink));
  QVERIFY(QFileInfo::exists(sentinelFile));
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("external-file-contents"));
  }
}

void AssetCacheTests::danglingSymlinkInsideCacheRootIsUnlinkedSafely() {
  // A symlink whose target does not exist (or has since been removed)
  // must still be classified as a symlink (via lstat-equivalent
  // fstatat(..., AT_SYMLINK_NOFOLLOW)/QFileInfo::isSymLink(), which
  // never requires the target to exist) and cleanly unlinked, not
  // mishandled as "not found, skip" in a way that could leave it
  // behind forever. This also exercises QDir::System in the entry
  // enumeration: without it, a dangling symlink (matching neither
  // QDir::Files nor QDir::Dirs, since its target cannot be stat'd)
  // would be invisible to the sweep entirely.
  const QString targetThatWillBeRemoved =
      m_tempDirPath + QStringLiteral("/will-be-removed.bin");
  {
    QFile file(targetThatWillBeRemoved);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("temporary"));
  }
  const QString danglingSymlink =
      m_tempDirPath + QStringLiteral("/dangling-symlink");
  QVERIFY(QFile::link(targetThatWillBeRemoved, danglingSymlink));
  QVERIFY(QFile::remove(targetThatWillBeRemoved));
  QVERIFY(QFileInfo(danglingSymlink).isSymLink());
  QVERIFY(!QFileInfo(danglingSymlink).exists()); // target gone: dangling

  AssetCache cache(configFor(m_tempDirPath));
  cache.reapAndEnforceQuota();

  QVERIFY(!QFileInfo(danglingSymlink).isSymLink());
}

void AssetCacheTests::
    accessSequenceIsMonotonicAndUniqueEvenForSameMillisecondConsecutiveStores() {
  // Review item 11: two entries stored back-to-back with no artificial
  // delay (routine on any fast machine, and the norm in a test) must
  // still receive strictly increasing, unique access sequence numbers
  // -- proving ordering never silently degrades to "whatever order a
  // hash/set iterates in" merely because wall-clock milliseconds tied.
  AssetCache cache(configFor(m_tempDirPath));
  const QString keyA = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/seq-a.png")));
  const QString keyB = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/seq-b.png")));
  const QString keyC = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/seq-c.png")));

  cache.store(keyA, makeEntry(QByteArrayLiteral("bytes-a")));
  cache.store(keyB, makeEntry(QByteArrayLiteral("bytes-b")));
  cache.store(keyC, makeEntry(QByteArrayLiteral("bytes-c")));

  const std::optional<quint64> seqA = cache.accessSequenceForTesting(keyA);
  const std::optional<quint64> seqB = cache.accessSequenceForTesting(keyB);
  const std::optional<quint64> seqC = cache.accessSequenceForTesting(keyC);
  QVERIFY(seqA.has_value());
  QVERIFY(seqB.has_value());
  QVERIFY(seqC.has_value());
  QVERIFY(*seqA < *seqB);
  QVERIFY(*seqB < *seqC);

  // A subsequent MEMORY-only hit on the oldest key must mint a fresh
  // sequence past every other entry's, without ever touching
  // lookupDisk().
  QVERIFY(cache.lookupMemory(keyA).has_value());
  const std::optional<quint64> seqAAfterMemoryHit =
      cache.accessSequenceForTesting(keyA);
  QVERIFY(seqAAfterMemoryHit.has_value());
  QVERIFY(*seqAAfterMemoryHit > *seqC);
}

void AssetCacheTests::accessSequenceRecoversPastPriorMaximumAcrossARestart() {
  // Review item 11: m_nextAccessSequence is purely in-memory per
  // AssetCache instance -- a real process restart must recover it from
  // the highest accessSeq value already persisted on disk (via the
  // constructor's initial reapAndEnforceQuota() sweep), never reset to
  // a low default that could let a NEW entry's sequence collide with,
  // or rank below, an old entry's already-persisted sequence.
  const QString keyA = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/restart-a.png")));
  const QString keyB = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/restart-b.png")));
  const QString keyC = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/restart-c.png")));

  quint64 seqBBeforeRestart = 0;
  {
    AssetCache cache(configFor(m_tempDirPath));
    cache.store(keyA, makeEntry(QByteArrayLiteral("bytes-a")));
    cache.store(keyB, makeEntry(QByteArrayLiteral("bytes-b")));
    const std::optional<quint64> seqB = cache.accessSequenceForTesting(keyB);
    QVERIFY(seqB.has_value());
    seqBBeforeRestart = *seqB;
  }

  // A fresh instance over the SAME directory: its constructor's initial
  // reap must recover m_nextAccessSequence to strictly past
  // seqBBeforeRestart before anything new is ever stored.
  AssetCache restarted(configFor(m_tempDirPath));
  restarted.store(keyC, makeEntry(QByteArrayLiteral("bytes-c")));
  const std::optional<quint64> seqCAfterRestart =
      restarted.accessSequenceForTesting(keyC);
  QVERIFY(seqCAfterRestart.has_value());
  QVERIFY(*seqCAfterRestart > seqBBeforeRestart);
}

void AssetCacheTests::memoryOnlyHitsKeepAnEntryAliveOverAColderDiskOnlyEntry() {
  // Review item 11 regression: a memory hit must refresh a key's
  // PERSISTED disk recency, not merely its in-memory presence --
  // otherwise a key kept "alive" purely via repeated memory hits could
  // still be evicted from disk as if it had never been accessed since
  // its original store().
  const QByteArray payload(2000, 'x');
  const QString keptWarmKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/kept-warm.png")));
  const QString neverTouchedAgainKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/never-touched-again.png")));
  QStringList fillerKeys;

  {
    // A generous quota here: nothing is evicted yet in this block. It
    // only establishes the on-disk state -- and, crucially, each
    // entry's persisted access sequence -- that the second,
    // differently (tightly) configured instance below actually evicts
    // from.
    AssetCache cache(configFor(m_tempDirPath, /*diskMaxBytes=*/1'000'000));

    // Stored FIRST, in this order: under a naive wall-clock- or
    // store-order-only LRU scheme, keptWarmKey (stored first) would
    // look strictly OLDER than neverTouchedAgainKey (stored second)
    // forever, with no way to ever change that ranking short of an
    // actual disk re-fetch.
    cache.store(keptWarmKey, makeEntry(payload));
    cache.store(neverTouchedAgainKey, makeEntry(payload));

    for (int i = 0; i < 4; ++i) {
      const QString fillerKey = AssetCache::cacheKeyFor(
          QUrl(QStringLiteral("https://example.com/lru-filler-%1.png").arg(i)));
      fillerKeys.append(fillerKey);
      cache.store(fillerKey, makeEntry(payload));
    }

    // Strictly AFTER every other entry above has already been stored,
    // refresh keptWarmKey via repeated MEMORY-only hits -- never once
    // calling lookupDisk() -- so its persisted access sequence becomes
    // the highest of all six entries.
    for (int i = 0; i < 3; ++i) {
      QVERIFY(cache.lookupMemory(keptWarmKey).has_value());
    }
  }

  // A second instance, configured with a small quota, stands in for a
  // later point at which this directory is discovered to exceed its
  // budget -- its constructor's initial reapAndEnforceQuota() sweep
  // performs the one, fully deterministic eviction pass this test
  // actually checks (no auto-eviction from an earlier store() call in
  // the block above can have fired prematurely, since that block's
  // quota was always generous enough to avoid it).
  AssetCache tightCache(configFor(m_tempDirPath, /*diskMaxBytes=*/9000));

  QVERIFY2(!QFile::exists(AssetCache::manifestPathForTesting(
               m_tempDirPath, neverTouchedAgainKey)),
           "an entry never touched again after its initial store() must "
           "be evicted ahead of one refreshed via later memory hits");
  QVERIFY2(QFile::exists(
               AssetCache::manifestPathForTesting(m_tempDirPath, keptWarmKey)),
           "an entry kept warm purely via memory hits must survive "
           "eviction even though it was stored FIRST (i.e. structurally "
           "older) than the entry above");
}

void AssetCacheTests::
    failedEvictionDeletionLeavesEntryCountedAsStillOccupyingSpace() {
  // Review item 11: quota accounting must only credit an entry's bytes
  // as freed once its files are actually confirmed removed. Simulate a
  // real-world deletion failure (a read-only remount, a permission
  // error) by revoking write permission on the cache directory itself
  // -- QFile::remove() then deterministically fails for every entry
  // (unlink requires write permission on the containing directory, not
  // on the file itself), regardless of platform or of whether the test
  // happens to run as an unprivileged user.
  const QByteArray payload(900, 'x');
  const QString oldestKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/undeletable-oldest.png")));
  const QString newerKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/newer.png")));

  // A generous quota here: both entries are stored without any risk of
  // store()'s OWN post-store auto-eviction check tripping (which would
  // otherwise race ahead of the permission lock below and evict
  // oldestKey normally, before this test ever gets a chance to prove
  // anything about a FAILED deletion).
  qint64 usageBeforeFailedEviction = 0;
  {
    AssetCache cache(configFor(m_tempDirPath, /*diskMaxBytes=*/1'000'000));
    cache.store(oldestKey, makeEntry(payload));
    cache.store(newerKey, makeEntry(payload));
    usageBeforeFailedEviction = cache.diskUsageBytes();
  }
  QVERIFY(usageBeforeFailedEviction > 0);

  // Restores full owner permissions unconditionally when this scope
  // ends, including via an early QVERIFY-triggered return -- otherwise
  // the managed QTemporaryDir could fail to clean itself up afterward.
  struct ScopedDirectoryPermissionLock {
    QString path;
    ~ScopedDirectoryPermissionLock() {
      QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner |
                                      QFile::ExeOwner);
    }
  } permissionGuard{m_tempDirPath};
  QVERIFY(QFile::setPermissions(
      m_tempDirPath, QFile::ReadOwner | QFile::ExeOwner)); // r-x, no write

  // A second instance, configured with a TIGHT quota, is constructed
  // only now -- strictly after the permission lock above is already in
  // place -- so its constructor's initial reapAndEnforceQuota() sweep
  // is the one, fully controlled point at which eviction of oldestKey
  // (the lower-sequence, and therefore normally-first-evicted, entry)
  // is attempted and must fail.
  AssetCache tightCache(configFor(m_tempDirPath, /*diskMaxBytes=*/3000));

  QCOMPARE(tightCache.diskUsageBytes(), usageBeforeFailedEviction);
  QVERIFY2(QFile::exists(
               AssetCache::manifestPathForTesting(m_tempDirPath, oldestKey)),
           "an entry whose deletion failed must remain fully resident, "
           "never counted as freed");
  QVERIFY(tightCache.lookupDisk(oldestKey).has_value());
}
