#include "AssetCacheTests.h"

#include "AssetCache.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTest>

#include <atomic>
#include <thread>

#if defined(Q_OS_UNIX)
#include <csignal>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace Arkham;

void AssetCacheTests::init() {
  // Round-9+ review: AssetCache's directory-resolution hardening now
  // walks no-follow starting from the process's own home directory
  // (see resolveTrustedDirectoryNoFollow()'s comment in AssetCache.cpp)
  // rather than the filesystem root, specifically so it never has to
  // reject legitimate OS-bootstrap symlinks like macOS's `/var` ->
  // `/private/var` -- which sits between "/" and this project's OWN
  // test fixtures, since QTemporaryDir's default (parameterless)
  // constructor resolves under the OS temp location (`/var/folders/...`
  // on macOS), not under home. Constructing every test's temporary
  // directory explicitly UNDER home instead means these tests exercise
  // the SAME, real, home-anchored code path a genuine deployment uses
  // for both its default cache location (also normally under home:
  // `~/.cache` on Linux, `~/Library/Caches` on macOS) and any
  // caller-configured custom directory a user places under their own
  // home directory -- rather than every single test run incidentally
  // falling into this resolver's narrower, uncommon-case fallback
  // branch for a path outside home instead.
  m_tempDir = std::make_unique<QTemporaryDir>(
      QDir::homePath() + QStringLiteral("/.arkham-asset-cache-test-XXXXXX"));
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

#if defined(Q_OS_UNIX)
// Round-7 review item 4 ("quota uses logical st_size and omits root
// allocation, while policy claims physical bytes"): diskUsageBytes()
// now reports PHYSICAL (st_blocks*512), not logical, byte usage, and
// now includes the cache root directory's own on-disk allocation --
// see physicalBytesOverflowSafe()'s comment in AssetCache.cpp. Every
// test comparing an expected total against a real path must read that
// path's ACTUAL physical allocation back the same way (never assume it
// equals the logical size a test happened to write), exactly as
// strayDirectoryIsRemovedAndCountedTowardDiskUsage() already did for a
// directory's own entry size before this round.
qint64 physicalBytesOnDiskForTesting(const QString &path) {
  struct stat st {};
  const QByteArray pathUtf8 = QFile::encodeName(path);
  if (::stat(pathUtf8.constData(), &st) != 0) {
    return 0;
  }
  return static_cast<qint64>(st.st_blocks) * 512;
}
#endif

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
  // Round-4/5 review item 4: `generationId` is the SEPARATE
  // self-consistency witness cross-checked against the generation named
  // by this filename/the manifest (see DiskMetadata::generationId's
  // comment) -- callers of this helper always pass the exact generation
  // this synthetic file is meant to represent, so it belongs here too,
  // not just in `sha256`.
  obj[QStringLiteral("generationId")] = generation;
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
  QString generation;
  {
    AssetCache cache(configFor(m_tempDirPath));
    cache.store(key, makeEntry(originalBytes));
    // Round-4/5 review item 4: the generation identifier is an
    // independently-minted opaque token, no longer sha256(payload
    // bytes) -- discover the REAL one store() actually produced rather
    // than assuming it.
    const std::optional<QString> actualGeneration =
        cache.currentGenerationForTesting(key);
    QVERIFY(actualGeneration.has_value());
    generation = *actualGeneration;
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
  QString generationA;
  {
    AssetCache cache(configFor(m_tempDirPath));
    cache.store(key, makeEntry(bytesA));
    // Round-4/5 review item 4: the generation identifier is an
    // independently-minted opaque token, no longer sha256(payload
    // bytes) -- discover the REAL one store() actually produced rather
    // than assuming it.
    const std::optional<QString> actualGenerationA =
        cache.currentGenerationForTesting(key);
    QVERIFY(actualGenerationA.has_value());
    generationA = *actualGenerationA;
  }
  // An arbitrary (but validly-shaped) generation identifier for the
  // synthetic "replacement" pair planted directly below -- content
  // hashes have no special meaning to the generation namespace anymore,
  // this is just a convenient 64-hex string, guaranteed distinct from
  // whatever store() minted for A above.
  const QString generationB = QString::fromLatin1(
      QCryptographicHash::hash(bytesB, QCryptographicHash::Sha256).toHex());
  QVERIFY(generationB != generationA);

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
  QString generationA;
  {
    AssetCache cache(configFor(m_tempDirPath));
    cache.store(key, makeEntry(bytesA));
    // Round-4/5 review item 4: discover the REAL generation identifier
    // store() minted for A -- see the previous test's identical comment.
    const std::optional<QString> actualGenerationA =
        cache.currentGenerationForTesting(key);
    QVERIFY(actualGenerationA.has_value());
    generationA = *actualGenerationA;
  }
  // An arbitrary (but validly-shaped) generation identifier for the
  // synthetic replacement pair planted directly below, guaranteed
  // distinct from whatever store() minted for A above.
  const QString generationB = QString::fromLatin1(
      QCryptographicHash::hash(bytesB, QCryptographicHash::Sha256).toHex());
  QVERIFY(generationB != generationA);

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

void AssetCacheTests::
    metadataWithImpossibleNumericFieldsIsRejectedAndQuarantined() {
  // Review round-3 item 8: readMetadata() must reject a fractional,
  // negative, non-numeric, or out-of-bound JSON number for encodedSize/
  // width/height BEFORE ever casting it to a narrower integer type --
  // casting an out-of-range double directly (as this file used to do)
  // is undefined behaviour, not merely "produces a wrong value". Each
  // variant below is otherwise a fully self-consistent payload/metadata/
  // manifest triple (correct SHA-256, matching declared size) -- only
  // the single named field is impossible -- isolating readMetadata()'s
  // own per-field validation from the other integrity checks already
  // covered elsewhere in this file (oversized payload, mismatched hash,
  // etc). A fresh AssetCache's own constructor-time reapAndEnforceQuota()
  // sweep must quarantine (fully remove) every variant on its own.
  struct Variant {
    const char *label;
    const char *fieldName;
    QJsonValue badValue;
  };
  const QVector<Variant> variants = {
      {"fractional-width", "width", QJsonValue(4.5)},
      {"negative-height", "height", QJsonValue(-10)},
      {"width-above-dimension-bound", "width", QJsonValue(999999999.0)},
      {"encodedSize-above-absolute-cap", "encodedSize",
       QJsonValue(50.0 * 1024 * 1024)},
      {"non-numeric-height", "height", QJsonValue(QStringLiteral("nope"))},
  };

  int index = 0;
  for (const Variant &variant : variants) {
    const QUrl url(QStringLiteral("https://example.com/impossible-field-%1.png")
                       .arg(index++));
    const QString key = AssetCache::cacheKeyFor(url);
    const QByteArray bytes = QByteArrayLiteral("impossible-field-payload");
    const QString sha256Hex = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QString payloadPath =
        AssetCache::payloadPathForTesting(m_tempDirPath, key, sha256Hex);
    const QString metadataPath =
        AssetCache::metadataPathForTesting(m_tempDirPath, key, sha256Hex);
    const QString manifestPath =
        AssetCache::manifestPathForTesting(m_tempDirPath, key);

    {
      QFile payload(payloadPath);
      QVERIFY2(payload.open(QIODevice::WriteOnly), variant.label);
      payload.write(bytes);
    }

    QJsonObject obj;
    obj[QStringLiteral("formatVersion")] = 1;
    obj[QStringLiteral("key")] = key;
    obj[QStringLiteral("contentType")] = QStringLiteral("image/png");
    obj[QStringLiteral("encodedSize")] = bytes.size();
    obj[QStringLiteral("width")] = 1;
    obj[QStringLiteral("height")] = 1;
    obj[QStringLiteral("sha256")] = sha256Hex;
    obj[QStringLiteral("etag")] = QString();
    obj[QStringLiteral("lastModified")] = QString();
    obj[QStringLiteral("insertedAtMs")] = QDateTime::currentMSecsSinceEpoch();
    obj[QStringLiteral("lastAccessMs")] = QDateTime::currentMSecsSinceEpoch();
    obj[QStringLiteral("accessSeq")] = QStringLiteral("1");
    obj[QLatin1String(variant.fieldName)] = variant.badValue;

    {
      QFile metadata(metadataPath);
      QVERIFY2(metadata.open(QIODevice::WriteOnly), variant.label);
      metadata.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }

    QJsonObject manifestObj;
    manifestObj[QStringLiteral("formatVersion")] = 1;
    manifestObj[QStringLiteral("key")] = key;
    manifestObj[QStringLiteral("generation")] = sha256Hex;
    {
      QFile manifest(manifestPath);
      QVERIFY2(manifest.open(QIODevice::WriteOnly), variant.label);
      manifest.write(QJsonDocument(manifestObj).toJson(QJsonDocument::Compact));
    }

    AssetCache cache(configFor(m_tempDirPath));
    QVERIFY2(!cache.lookupDisk(key).has_value(), variant.label);
    QVERIFY2(!QFile::exists(payloadPath), variant.label);
    QVERIFY2(!QFile::exists(metadataPath), variant.label);
    QVERIFY2(!QFile::exists(manifestPath), variant.label);
  }
}

void AssetCacheTests::
    accessSequenceAbove2Pow53RoundTripsExactlyAcrossARestart() {
  // Review round-3 item 8: accessSeq is persisted as a decimal STRING
  // specifically so a value beyond 2^53 (the largest integer a JSON
  // double can represent exactly) survives a write/read round trip with
  // no precision loss -- a bare JSON number would silently round such a
  // value to the nearest representable double. Hand-plants a fully
  // valid, self-consistent generation whose accessSeq field is set to a
  // quint64 value comfortably past 2^53, then constructs a fresh
  // AssetCache (a real restart) and reads it back via
  // accessSequenceForTesting(), asserting the EXACT value survives.
  constexpr quint64 kAbove2Pow53 = 9007199254741101ULL; // 2^53 + 109
  const QUrl url(
      QStringLiteral("https://example.com/huge-access-sequence.png"));
  const QString key = AssetCache::cacheKeyFor(url);
  const QByteArray bytes = QByteArrayLiteral("huge-sequence-payload");
  const QString sha256Hex = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  const QString payloadPath =
      AssetCache::payloadPathForTesting(m_tempDirPath, key, sha256Hex);
  const QString metadataPath =
      AssetCache::metadataPathForTesting(m_tempDirPath, key, sha256Hex);
  const QString manifestPath =
      AssetCache::manifestPathForTesting(m_tempDirPath, key);

  {
    QFile payload(payloadPath);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(bytes);
  }

  QJsonObject obj;
  obj[QStringLiteral("formatVersion")] = 1;
  obj[QStringLiteral("key")] = key;
  obj[QStringLiteral("contentType")] = QStringLiteral("image/png");
  obj[QStringLiteral("encodedSize")] = bytes.size();
  obj[QStringLiteral("width")] = 1;
  obj[QStringLiteral("height")] = 1;
  obj[QStringLiteral("sha256")] = sha256Hex;
  // Round-4/5 review item 4: the separate self-consistency witness,
  // matching this metadata's own filename-embedded generation -- see
  // DiskMetadata::generationId's comment.
  obj[QStringLiteral("generationId")] = sha256Hex;
  obj[QStringLiteral("etag")] = QString();
  obj[QStringLiteral("lastModified")] = QString();
  obj[QStringLiteral("insertedAtMs")] = QDateTime::currentMSecsSinceEpoch();
  obj[QStringLiteral("lastAccessMs")] = QDateTime::currentMSecsSinceEpoch();
  obj[QStringLiteral("accessSeq")] = QString::number(kAbove2Pow53);

  {
    QFile metadata(metadataPath);
    QVERIFY(metadata.open(QIODevice::WriteOnly));
    metadata.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
  }

  QJsonObject manifestObj;
  manifestObj[QStringLiteral("formatVersion")] = 1;
  manifestObj[QStringLiteral("key")] = key;
  manifestObj[QStringLiteral("generation")] = sha256Hex;
  {
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(manifestObj).toJson(QJsonDocument::Compact));
  }

  AssetCache cache(configFor(m_tempDirPath));

  // Read the persisted value BEFORE any lookup: accessSequenceForTesting()
  // is a pure read with no touch/bump side effect, but lookupDisk()
  // deliberately mints and persists a FRESH sequence on every real hit
  // (see its "Refresh on-disk lastAccess" comment) -- calling it first
  // would overwrite the exact value this test needs to verify survived
  // the write/read round trip with zero precision loss.
  const std::optional<quint64> readSeq = cache.accessSequenceForTesting(key);
  QVERIFY(readSeq.has_value());
  QCOMPARE(*readSeq, kAbove2Pow53);

  const auto hit = cache.lookupDisk(key);
  QVERIFY(hit.has_value());
  QCOMPARE(hit->encodedBytes, bytes);

  // The real disk hit above already minted a fresh sequence strictly
  // past the recovered maximum -- never silently starting back over
  // from a small default because the on-disk value could not be
  // represented.
  const std::optional<quint64> afterHit = cache.accessSequenceForTesting(key);
  QVERIFY(afterHit.has_value());
  QVERIFY(*afterHit > kAbove2Pow53);
}

void AssetCacheTests::
    rootReplacedAfterConstructionPermanentlyDisablesDiskIoForBothTargets() {
  // Review round-3 item 9 (HIGH): a constructor-only symlink check plus
  // absolute paths is insufficient -- the cache root can be renamed
  // away and a NEW filesystem object (a plain directory, here) created
  // at the exact same path strictly AFTER construction. The retained
  // root directory descriptor (O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC) plus
  // its captured (device, inode) pair, re-verified via
  // verifyRootAnchorLocked() at the top of every disk-touching method,
  // must detect this and permanently disable disk I/O for this
  // instance -- rather than silently continuing to operate against
  // whatever object the path now resolves to (splitting/leaking into
  // the replacement) or reopening the ORIGINAL (renamed-away) directory
  // by path (which could just as easily have been deleted entirely).
  const QString cacheRoot = m_tempDirPath + QStringLiteral("/cache-root");
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("cache-root")));

  AssetCache cache(configFor(cacheRoot));
  const QString existingKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/pre-replacement.png")));
  cache.store(existingKey,
              makeEntry(QByteArrayLiteral("pre-replacement-bytes")));
  QVERIFY(cache.lookupDisk(existingKey).has_value());
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  // Rename the ORIGINAL root away (still containing the entry just
  // stored above), then create a brand-new, empty directory at the
  // EXACT original path -- simulating a root replacement/mount-swap
  // race that happens after this AssetCache instance is already alive.
  const QString renamedAwayRoot =
      m_tempDirPath + QStringLiteral("/cache-root-renamed-away");
  QVERIFY(QDir().rename(cacheRoot, renamedAwayRoot));
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("cache-root")));
  const QString replacementSentinel =
      cacheRoot + QStringLiteral("/replacement-sentinel.txt");
  {
    QFile file(replacementSentinel);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("attacker-or-race-controlled"));
  }

  // Any further disk-touching call must detect the mismatch and
  // permanently disable disk I/O -- proven here via a fresh store()
  // for a brand-new key, not merely a re-lookup of the old one.
  const QString newKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/post-replacement.png")));
  cache.store(newKey, makeEntry(QByteArrayLiteral("post-replacement-bytes")));
  QVERIFY(cache.isDiskCacheDisabledForTesting());
  // The memory cache is unaffected: the new entry is still observable
  // in-process even though nothing was persisted for it.
  QVERIFY(cache.lookupMemory(newKey).has_value());

  // The REPLACEMENT directory at the original path must remain
  // completely untouched: no cache files written into it at all, only
  // the sentinel this test itself planted.
  QDir replacementListing(cacheRoot);
  QCOMPARE(replacementListing.entryList(QDir::Files),
           QStringList{"replacement-sentinel.txt"});
  {
    QFile file(replacementSentinel);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("attacker-or-race-controlled"));
  }

  // The ORIGINAL (renamed-away) directory must also remain untouched --
  // still holding exactly the entry stored before the replacement, with
  // no attempt made to write, delete, or otherwise reopen it by path.
  AssetCache::Config renamedConfig = configFor(renamedAwayRoot);
  AssetCache freshCacheOverOriginal(renamedConfig);
  QVERIFY(freshCacheOverOriginal.lookupDisk(existingKey).has_value());
  QCOMPARE(freshCacheOverOriginal.lookupDisk(existingKey)->encodedBytes,
           QByteArrayLiteral("pre-replacement-bytes"));
}

void AssetCacheTests::
    memoryHitRecencyBumpAfterRootReplacementNeverTouchesReplacementDirectory() {
  // Round-4/5 review item 3: touchAccessRecencyLocked() (reached via a
  // pure MEMORY hit through lookupMemory(), never lookupDisk()) was
  // previously missing its own verifyRootAnchorLocked() call entirely,
  // so a memory-hit-triggered "bump this key's persisted recency"
  // write could proceed even after the root directory had been
  // replaced post-construction -- writing into whatever NEW object now
  // sits at the same path. This proves the fix: a memory hit still
  // succeeds (the memory cache itself is never anchor-gated), but the
  // recency bump it triggers must detect the replacement and touch
  // nothing in the replacement directory at all.
  const QString cacheRoot =
      m_tempDirPath + QStringLiteral("/memory-hit-cache-root");
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("memory-hit-cache-root")));

  AssetCache cache(configFor(cacheRoot));
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/memory-hit-recency.png")));
  cache.store(key, makeEntry(QByteArrayLiteral("memory-hit-recency-bytes")));
  QVERIFY(cache.lookupDisk(key).has_value()); // resident in memory now too
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  // Replace the root exactly as
  // rootReplacedAfterConstructionPermanentlyDisablesDiskIoForBothTargets()
  // does: rename the original away, create an empty directory at the
  // exact same path, plant a sentinel file in it.
  const QString renamedAwayRoot =
      m_tempDirPath + QStringLiteral("/memory-hit-cache-root-renamed-away");
  QVERIFY(QDir().rename(cacheRoot, renamedAwayRoot));
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("memory-hit-cache-root")));
  const QString replacementSentinel =
      cacheRoot + QStringLiteral("/replacement-sentinel.txt");
  {
    QFile file(replacementSentinel);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("untouched-by-any-memory-hit-recency-bump"));
  }

  // A pure memory hit: the entry is still resident in-process, so this
  // must succeed and return the correct bytes regardless of what has
  // happened to the disk root.
  const auto memoryHit = cache.lookupMemory(key);
  QVERIFY(memoryHit.has_value());
  QCOMPARE(memoryHit->encodedBytes,
           QByteArrayLiteral("memory-hit-recency-bytes"));

  // The recency bump this memory hit triggers must have detected the
  // anchor mismatch and disabled disk I/O -- never having written
  // anything into the replacement directory.
  QVERIFY(cache.isDiskCacheDisabledForTesting());
  QDir replacementListing(cacheRoot);
  QCOMPARE(replacementListing.entryList(QDir::Files),
           QStringList{"replacement-sentinel.txt"});
  {
    QFile file(replacementSentinel);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(),
             QByteArrayLiteral("untouched-by-any-memory-hit-recency-bump"));
  }

  // The ORIGINAL (renamed-away) directory must also remain exactly as
  // it was: still holding the entry with its ORIGINAL access sequence,
  // never touched by the memory hit's (correctly aborted) recency bump.
  AssetCache freshCacheOverOriginal(configFor(renamedAwayRoot));
  QVERIFY(freshCacheOverOriginal.lookupDisk(key).has_value());
  QCOMPARE(freshCacheOverOriginal.lookupDisk(key)->encodedBytes,
           QByteArrayLiteral("memory-hit-recency-bytes"));
}

void AssetCacheTests::identicalPayloadReplacementMintsANewGenerationEachTime() {
  // Round-4/5 review item 4 (HIGH): the generation identifier is now an
  // independently-minted opaque token (mintGenerationIdLocked()), never
  // derived from the payload's own content hash. Two SEPARATE store()
  // calls for the same key with BYTE-IDENTICAL content must therefore
  // still produce two DIFFERENT generation filenames -- proving a
  // same-bytes refresh can never collapse onto (and thus risk
  // overwriting/rewriting) whatever generation filename an EARLIER
  // store of the exact same content already published, which would
  // otherwise let a failed second publish's cleanup delete what is
  // actually still the first, live, already-served generation.
  AssetCache cache(configFor(m_tempDirPath));
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/identical-payload.png")));
  const QByteArray bytes = QByteArrayLiteral("byte-identical-payload-content");

  cache.store(key, makeEntry(bytes));
  const std::optional<QString> generationAfterFirstStore =
      cache.currentGenerationForTesting(key);
  QVERIFY(generationAfterFirstStore.has_value());
  const QString payloadPathAfterFirstStore = AssetCache::payloadPathForTesting(
      m_tempDirPath, key, *generationAfterFirstStore);
  QVERIFY(QFile::exists(payloadPathAfterFirstStore));

  // A second, entirely independent store() call for the SAME key with
  // the EXACT same bytes -- an ordinary "re-fetched and re-validated
  // the same content" cache refresh.
  cache.store(key, makeEntry(bytes));
  const std::optional<QString> generationAfterSecondStore =
      cache.currentGenerationForTesting(key);
  QVERIFY(generationAfterSecondStore.has_value());

  QVERIFY2(*generationAfterFirstStore != *generationAfterSecondStore,
           "identical payload bytes must never collapse onto the same "
           "generation identifier");

  // Normal post-publish cleanup still reclaims the now-superseded FIRST
  // generation's files -- exactly the ordinary replacement path, not a
  // crash scenario -- leaving exactly the second generation live.
  QVERIFY(!QFile::exists(payloadPathAfterFirstStore));
  QVERIFY(QFile::exists(AssetCache::payloadPathForTesting(
      m_tempDirPath, key, *generationAfterSecondStore)));

  const auto hit = cache.lookupDisk(key);
  QVERIFY(hit.has_value());
  QCOMPARE(hit->encodedBytes, bytes);
}

namespace {
// RAII guard for AssetCache::setListAllEntriesRelativeForcedFailureForTesting()
// -- mirrors MountIdentificationDegradationGuard's own pattern so a test
// can never accidentally leave this process-wide override active for a
// later, unrelated test even if an assertion fails partway through.
struct ListAllEntriesRelativeFailureGuard {
  explicit ListAllEntriesRelativeFailureGuard(bool active) {
    AssetCache::setListAllEntriesRelativeForcedFailureForTesting(active);
  }
  ListAllEntriesRelativeFailureGuard(
      const ListAllEntriesRelativeFailureGuard &) = delete;
  ListAllEntriesRelativeFailureGuard &
  operator=(const ListAllEntriesRelativeFailureGuard &) = delete;
  ~ListAllEntriesRelativeFailureGuard() {
    AssetCache::setListAllEntriesRelativeForcedFailureForTesting(false);
  }
};
} // namespace

void AssetCacheTests::
    reapSweepAbortsAllMutationWhenDirectoryListingIsIndeterminate() {
  AssetCache cache(configFor(m_tempDirPath));

  // A perfectly valid, live entry -- must survive untouched regardless
  // of what happens below.
  const QString liveKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/still-live.png")));
  cache.store(liveKey, makeEntry(QByteArrayLiteral("still-live-bytes")));
  const auto liveGeneration = cache.currentGenerationForTesting(liveKey);
  QVERIFY(liveGeneration.has_value());
  const QString livePayloadPath = AssetCache::payloadPathForTesting(
      m_tempDirPath, liveKey, *liveGeneration);
  const QString liveMetadataPath = AssetCache::metadataPathForTesting(
      m_tempDirPath, liveKey, *liveGeneration);
  const QString liveManifestPath =
      AssetCache::manifestPathForTesting(m_tempDirPath, liveKey);
  QVERIFY(QFile::exists(livePayloadPath));
  QVERIFY(QFile::exists(liveMetadataPath));
  QVERIFY(QFile::exists(liveManifestPath));

  // A stray file that an ordinary, un-degraded sweep WOULD remove --
  // included so this test also proves the injected fault is genuinely
  // gating something real, not merely a no-op every sweep would have
  // been anyway.
  const QString strayPath =
      m_tempDirPath + QStringLiteral("/not-a-valid-key-indeterminate.tmp");
  {
    QFile stray(strayPath);
    QVERIFY(stray.open(QIODevice::WriteOnly));
    stray.write("leftover");
  }

  {
    ListAllEntriesRelativeFailureGuard guard(true);
    cache.reapAndEnforceQuota();
  }

  // While the directory listing was forced INDETERMINATE, the sweep
  // must have aborted before touching anything at all -- the live
  // entry's files are untouched, AND the stray file (which a healthy
  // sweep would have deleted) is STILL there, proving zero mutation
  // happened rather than merely "the live entry got lucky".
  QVERIFY(QFile::exists(livePayloadPath));
  QVERIFY(QFile::exists(liveMetadataPath));
  QVERIFY(QFile::exists(liveManifestPath));
  QVERIFY(QFile::exists(strayPath));
  const auto liveHit = cache.lookupDisk(liveKey);
  QVERIFY(liveHit.has_value());
  QCOMPARE(liveHit->encodedBytes, QByteArrayLiteral("still-live-bytes"));

  // Positive control: with the fault no longer injected, an ordinary
  // sweep still behaves exactly as before -- the stray file is finally
  // removed, and the live entry remains live -- proving the guard
  // above was genuinely gating this behavior, not permanently broken.
  cache.reapAndEnforceQuota();
  QVERIFY(!QFile::exists(strayPath));
  QVERIFY(QFile::exists(livePayloadPath));
  QVERIFY(QFile::exists(liveMetadataPath));
  QVERIFY(QFile::exists(liveManifestPath));
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
  // Round-7 review item 4: diskUsageBytes() now also credits the cache
  // root directory's own physical allocation (see
  // physicalBytesOnDiskForTesting()'s comment above) -- captured here,
  // on a pristine, freshly-constructed, still-empty cache, rather than
  // ever assumed to be a fixed/portable constant (it is filesystem- and
  // platform-dependent) or hardcoded as literal 0.
  const qint64 emptyCacheBaselineBytes = cache.diskUsageBytes();

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
  // Round-N+ review (MEDIUM, repeat finding, "directory storage
  // uncounted"): a directory node itself now also contributes its own
  // on-disk entry size to the total -- read it back directly (physical,
  // filesystem/platform-dependent, never a fixed constant) rather than
  // assuming the nested file's bytes are the ONLY contribution, exactly
  // like the cross-mount case already did before this round. Round-7
  // review item 4: both the directory's own allocation AND the nested
  // file's allocation are now PHYSICAL (st_blocks*512), not logical --
  // read back via physicalBytesOnDiskForTesting() rather than assumed
  // to equal the 4096 logical bytes actually written.
  const qint64 strayDirEntryPhysicalBytes =
      physicalBytesOnDiskForTesting(strayDirPath);
  const qint64 nestedFilePhysicalBytes = physicalBytesOnDiskForTesting(
      strayDirPath + QStringLiteral("/nested-file.bin"));

  // diskUsageBytes() must count the nested file's bytes even though they
  // sit inside a directory this cache never created, PLUS that
  // directory's own on-disk entry size, PLUS the pre-existing empty-
  // cache baseline (the root directory's own allocation).
  QCOMPARE(cache.diskUsageBytes(), emptyCacheBaselineBytes +
                                       nestedFilePhysicalBytes +
                                       strayDirEntryPhysicalBytes);

  cache.reapAndEnforceQuota();

  // The entire stray directory (and its contents) must be gone, and
  // usage accounting must reflect that it is actually gone -- back down
  // to (never below) the same empty-cache baseline as before anything
  // was planted, never a hardcoded literal 0.
  QVERIFY(!QFileInfo::exists(strayDirPath));
  QCOMPARE(cache.diskUsageBytes(), emptyCacheBaselineBytes);
}

void AssetCacheTests::quotaEvictsOldestAccessFirstDownToLowWaterMark() {
  // A tiny quota (a few entries fit comfortably below it, but 20 don't)
  // forces eviction; the OLDEST-accessed entries must go first, down to
  // the 75% low-water mark.
  //
  // Round-7 review item 4 ("quota uses logical st_size ... policy
  // claims physical bytes"): diskUsageBytes() now reports real,
  // block-rounded PHYSICAL allocation, so a quota literal calibrated
  // against a purely logical per-entry payload size (e.g. "20 entries
  // of ~900 bytes fit under a 20*1024-byte quota") can be wildly wrong
  // once each entry's three files (manifest+payload+metadata) each
  // independently round up to at least one whole filesystem block --
  // on a filesystem with a large block size, that quota could then be
  // too small to ever hold even ONE entry, which would falsely turn
  // this into an "everything gets evicted immediately" test rather
  // than the "oldest evicted first, newest survives" test it's meant
  // to be. Measure the real per-entry physical cost at runtime instead
  // of hardcoding an assumed logical total -- the same pattern already
  // established by memoryOnlyHitsKeepAnEntryAliveOverAColderDiskOnlyEntry
  // and
  // diskUsageBytesReflectsPhysicalAllocationRatherThanLogicalSizeForATinyEntry.
  const QByteArray payload(900, 'x');
  constexpr int kEntryCount = 20;

  AssetCache generousCache(configFor(m_tempDirPath, 512 * 1024 * 1024));
  const qint64 emptyBaselineBytes = generousCache.diskUsageBytes();
  QStringList probeKeys;
  for (int i = 0; i < kEntryCount; ++i) {
    const QString key = AssetCache::cacheKeyFor(
        QUrl(QStringLiteral("https://example.com/item-%1.png").arg(i)));
    probeKeys.append(key);
    generousCache.store(key, makeEntry(payload));
  }
  const qint64 establishedTotalBytes = generousCache.diskUsageBytes();
  const qint64 perEntryCost =
      (establishedTotalBytes - emptyBaselineBytes + kEntryCount - 1) /
      kEntryCount;
  QVERIFY(perEntryCost > 0);

  // A quota that comfortably fits ~5 entries (plus the empty-root
  // baseline) but never all 20 -- enough headroom for the low-water
  // mark's 75% target to always land strictly between "0 entries" and
  // "kEntryCount entries" regardless of exact block-rounding.
  const qint64 tightDiskMaxBytes = emptyBaselineBytes + 5 * perEntryCost;

  const QString tightCacheRoot = m_tempDirPath + QStringLiteral("/tight");
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("tight")));
  AssetCache cache(configFor(tightCacheRoot, tightDiskMaxBytes));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  QStringList keys;
  for (int i = 0; i < kEntryCount; ++i) {
    const QString key = AssetCache::cacheKeyFor(
        QUrl(QStringLiteral("https://example.com/tight-item-%1.png").arg(i)));
    keys.append(key);
    cache.store(key, makeEntry(payload));
    // Ensure strictly increasing lastAccess ordering regardless of clock
    // resolution.
    QTest::qWait(2);
  }

  QVERIFY(cache.diskUsageBytes() <= tightDiskMaxBytes);
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

#if defined(Q_OS_UNIX)
namespace {
// Creates a FIFO (named pipe) special file at `path` via mkfifo(2) --
// opening it O_RDONLY with a plain blocking open() (the pre-fix
// behaviour openRegularNoFollowRelative() used) blocks until some OTHER
// process opens the same path for writing, which never happens in
// these tests -- proving the fix (O_NONBLOCK) is what stands between a
// passing test and one that hangs until QtTest's own global watchdog
// kills the whole binary.
void createFifoForTesting(const QString &path) {
  const QByteArray pathUtf8 = QFile::encodeName(path);
  QCOMPARE(::mkfifo(pathUtf8.constData(), 0600), 0);
}
} // namespace
#endif

void AssetCacheTests::manifestPlantedAsFifoNeverBlocksConstructionOrLookup() {
#if !defined(Q_OS_UNIX)
  QSKIP("FIFOs are a POSIX-specific concept; not applicable on this "
        "platform");
#else
  // Cumulative review (PR #18, MEDIUM): a FIFO planted at a MANIFEST
  // filename must be rejected -- both during the constructor's own
  // startup reap sweep (reapAndEnforceQuota() itself reads every
  // manifest it finds) and during an explicit lookupDisk() call --
  // WITHOUT ever blocking, in bounded, real wall-clock time. There is
  // no legitimate reader/writer for this FIFO anywhere in this test, so
  // a plain blocking open() would hang until QtTest's own global
  // watchdog eventually aborts the entire test binary -- there is no
  // "wait a little and see" available here; only O_NONBLOCK prevents
  // the hang from ever starting at all.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/manifest-fifo.png")));
  const QString manifestPath =
      AssetCache::manifestPathForTesting(m_tempDirPath, key);
  createFifoForTesting(manifestPath);
  QVERIFY(QFileInfo(manifestPath).isFile() == false);

  QElapsedTimer timer;
  timer.start();
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.lookupDisk(key).has_value());
  // A generous, but still tightly bounded, upper limit -- construction
  // plus one lookup here involves nothing but a handful of syscalls; if
  // the fix ever regressed back to a blocking open(), this would hang
  // indefinitely rather than merely running slowly, so ANY finite bound
  // well under QtTest's own multi-minute default timeout reliably
  // distinguishes fixed from broken.
  QVERIFY(timer.elapsed() < 5000);
#endif
}

void AssetCacheTests::metadataPlantedAsFifoNeverBlocksConstructionOrLookup() {
#if !defined(Q_OS_UNIX)
  QSKIP("FIFOs are a POSIX-specific concept; not applicable on this "
        "platform");
#else
  // A real manifest names a generation whose METADATA file is instead a
  // FIFO -- readMetadata() must reject it (via the same
  // openRegularNoFollowRelative()) without ever blocking, and the
  // repair sweep must treat this exactly like any other corrupt/
  // orphaned generation.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/metadata-fifo.png")));
  const QByteArray payloadBytes = QByteArrayLiteral("metadata-fifo-payload");
  const QString generation = QString::fromLatin1(
      QCryptographicHash::hash(payloadBytes, QCryptographicHash::Sha256)
          .toHex());
  const QString payloadPath =
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generation);
  const QString metadataPath =
      AssetCache::metadataPathForTesting(m_tempDirPath, key, generation);
  const QString manifestPath =
      AssetCache::manifestPathForTesting(m_tempDirPath, key);
  {
    QFile payload(payloadPath);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(payloadBytes);
  }
  createFifoForTesting(metadataPath);
  {
    QJsonObject manifestObj;
    manifestObj[QStringLiteral("formatVersion")] = 1;
    manifestObj[QStringLiteral("key")] = key;
    manifestObj[QStringLiteral("generation")] = generation;
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(manifestObj).toJson(QJsonDocument::Compact));
  }

  QElapsedTimer timer;
  timer.start();
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.lookupDisk(key).has_value());
  QVERIFY(timer.elapsed() < 5000);
#endif
}

void AssetCacheTests::payloadPlantedAsFifoNeverBlocksConstructionOrLookup() {
#if !defined(Q_OS_UNIX)
  QSKIP("FIFOs are a POSIX-specific concept; not applicable on this "
        "platform");
#else
  // A fully self-consistent manifest+metadata pair naming a generation
  // whose PAYLOAD file is instead a FIFO -- readExactSizeVerifiedRelative()
  // must reject it without ever blocking.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/payload-fifo.png")));
  const QByteArray payloadBytes = QByteArrayLiteral("payload-fifo-bytes");
  const QString generation = QString::fromLatin1(
      QCryptographicHash::hash(payloadBytes, QCryptographicHash::Sha256)
          .toHex());
  const QString payloadPath =
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generation);
  const QString metadataPath =
      AssetCache::metadataPathForTesting(m_tempDirPath, key, generation);
  const QString manifestPath =
      AssetCache::manifestPathForTesting(m_tempDirPath, key);
  createFifoForTesting(payloadPath);
  writeRawMetadataForTesting(metadataPath, key, generation,
                             payloadBytes.size());
  {
    QJsonObject manifestObj;
    manifestObj[QStringLiteral("formatVersion")] = 1;
    manifestObj[QStringLiteral("key")] = key;
    manifestObj[QStringLiteral("generation")] = generation;
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(manifestObj).toJson(QJsonDocument::Compact));
  }

  QElapsedTimer timer;
  timer.start();
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.lookupDisk(key).has_value());
  QVERIFY(timer.elapsed() < 5000);
#endif
}

void AssetCacheTests::
    metadataPlantedAsUnixSocketNeverBlocksConstructionOrLookup() {
#if !defined(Q_OS_UNIX)
  QSKIP("UNIX domain sockets are a POSIX-specific concept; not "
        "applicable on this platform");
#else
  // "socket too": a UNIX domain socket special file (created via
  // socket()+bind(), fully unprivileged) planted at a metadata filename
  // must be rejected identically to a FIFO -- S_ISREG is false for a
  // socket node exactly as it is for a FIFO, and a blocking open() of a
  // socket special file with AF_UNIX semantics behaves like opening any
  // other non-regular file (it does not itself block the way a FIFO
  // does on most platforms, but must still be rejected as non-regular,
  // never silently accepted).
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/metadata-socket.png")));
  const QByteArray payloadBytes = QByteArrayLiteral("metadata-socket-payload");
  const QString generation = QString::fromLatin1(
      QCryptographicHash::hash(payloadBytes, QCryptographicHash::Sha256)
          .toHex());
  const QString payloadPath =
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generation);
  const QString metadataPath =
      AssetCache::metadataPathForTesting(m_tempDirPath, key, generation);
  const QString manifestPath =
      AssetCache::manifestPathForTesting(m_tempDirPath, key);
  {
    QFile payload(payloadPath);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(payloadBytes);
  }
  {
    const int sockFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(sockFd >= 0);
    // sockaddr_un::sun_path is a short fixed-size buffer (108 bytes on
    // Linux, 104 on macOS/BSD), too short even for just this cache's own
    // <64-hex-key>.<64-hex-generation>.meta.json LEAF filename alone
    // (139 bytes), let alone the full path under this test's own
    // deliberately home-anchored, multi-component m_tempDirPath (see
    // init()'s comment for why every test constructs its temp directory
    // under QDir::homePath() rather than the OS's normal, much shorter
    // temp location). bind() is therefore done against a SHORT
    // temporary relative name in the metadata file's own parent
    // directory (chdir()'d into first, so bind()'s own path length
    // limit never applies at all), then the resulting socket special
    // file is renamed onto the real, long metadata leaf name -- a plain
    // filesystem rename() has no such length restriction; only the
    // kernel's bind(2) does.
    const QByteArray originalCwd = QDir::currentPath().toUtf8();
    const QFileInfo metadataInfo(metadataPath);
    QVERIFY(QDir::setCurrent(metadataInfo.absolutePath()));
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    const QByteArray shortTempNameUtf8 =
        QByteArrayLiteral(".socket-tmp-for-testing");
    std::memcpy(addr.sun_path, shortTempNameUtf8.constData(),
                static_cast<size_t>(shortTempNameUtf8.size()));
    const int bindResult = ::bind(
        sockFd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    int renameResult = -1;
    if (bindResult == 0) {
      renameResult =
          ::rename(shortTempNameUtf8.constData(),
                   QFile::encodeName(metadataInfo.fileName()).constData());
    }
    QVERIFY(QDir::setCurrent(QString::fromUtf8(originalCwd)));
    QCOMPARE(bindResult, 0);
    QCOMPARE(renameResult, 0);
    ::close(sockFd);
  }
  QVERIFY(QFileInfo(metadataPath).isFile() == false);
  {
    QJsonObject manifestObj;
    manifestObj[QStringLiteral("formatVersion")] = 1;
    manifestObj[QStringLiteral("key")] = key;
    manifestObj[QStringLiteral("generation")] = generation;
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(manifestObj).toJson(QJsonDocument::Compact));
  }

  QElapsedTimer timer;
  timer.start();
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.lookupDisk(key).has_value());
  QVERIFY(timer.elapsed() < 5000);
#endif
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
  qint64 emptyBaselineBytes = 0;
  qint64 establishedTotalBytes = 0;

  {
    // A generous quota here: nothing is evicted yet in this block. It
    // only establishes the on-disk state -- and, crucially, each
    // entry's persisted access sequence -- that the second,
    // differently (tightly) configured instance below actually evicts
    // from.
    AssetCache cache(configFor(m_tempDirPath, /*diskMaxBytes=*/1'000'000));
    // Round-7 review item 4: diskUsageBytes() now reports PHYSICAL
    // (st_blocks*512) bytes, including the cache root directory's own
    // allocation -- both filesystem- and platform-dependent, so the
    // tight quota below is DERIVED from real, measured usage rather
    // than a hardcoded literal that assumed logical byte sums.
    emptyBaselineBytes = cache.diskUsageBytes();

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

    establishedTotalBytes = cache.diskUsageBytes();
  }

  // Round-7 review item 4: with physical (block-rounded) accounting,
  // per-entry overhead is no longer a small, precisely-known constant
  // (payload+manifest+metadata can each independently round up to a
  // whole filesystem block) -- so the tight quota below is derived from
  // the ACTUAL measured six-entry total rather than a literal constant
  // that assumed pure logical-byte sums. `perEntryCost` is the average
  // physical cost of one of the six (structurally identical: same
  // payload size, similarly-short URL) entries above.
  const qint64 perEntryCost =
      (establishedTotalBytes - emptyBaselineBytes + 5) / 6;
  QVERIFY(perEntryCost > 0);
  // Chosen so that: (a) the high-water mark is strictly below the
  // established six-entry total, guaranteeing the tight cache's own
  // constructor-time reap actually triggers eviction at all; and (b)
  // the low-water mark is at or above "baseline + exactly one entry's
  // cost" (i.e. everything evicted except keptWarmKey), guaranteeing
  // eviction can never need to reach keptWarmKey (the newest, by
  // persisted access sequence, of all six) to satisfy its target --
  // regardless of exactly how many of the four filler entries also get
  // reclaimed along the way, which this test does not otherwise care
  // about.
  const qint64 tightDiskMaxBytes = emptyBaselineBytes + 3 * perEntryCost;

  // A second instance, configured with a small quota, stands in for a
  // later point at which this directory is discovered to exceed its
  // budget -- its constructor's initial reapAndEnforceQuota() sweep
  // performs the one, fully deterministic eviction pass this test
  // actually checks (no auto-eviction from an earlier store() call in
  // the block above can have fired prematurely, since that block's
  // quota was always generous enough to avoid it).
  AssetCache tightCache(configFor(m_tempDirPath, tightDiskMaxBytes));

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

void AssetCacheTests::
    ownedSuffixComponentThatIsASymlinkIsRejectedEvenWhenTrustedAnchorIsPlain() {
  // Round-6 review item 5: a component-by-component, no-follow open of
  // this cache's own reserved sub-path (the "owned suffix" -- e.g.
  // "assets/v1" beneath an OS-provided cache base) must reject an
  // attacker pre-planting ANY of those components as a symlink, even
  // though the trusted anchor directory ABOVE it is perfectly ordinary
  // and was never itself under attacker control. This is exactly the
  // realistic attack the finding describes: a symlink placed at
  // <anchor>/assets pointing somewhere else, well before this process
  // ever runs.
  const QString anchorDir = m_tempDirPath + QStringLiteral("/anchor");
  QVERIFY(QDir().mkpath(anchorDir));
  const QString elsewhereDir = m_tempDirPath + QStringLiteral("/elsewhere");
  QVERIFY(QDir().mkpath(elsewhereDir));

  QVERIFY(QFile::link(elsewhereDir, anchorDir + QStringLiteral("/assets")));
  QVERIFY(QFileInfo(anchorDir + QStringLiteral("/assets")).isSymLink());

  QVERIFY(!AssetCache::directoryChainResolvesNoFollowForTesting(
      anchorDir, {QStringLiteral("assets"), QStringLiteral("v1")}));

  // The symlink itself must be left completely alone -- this check is a
  // pure read-only reject, never a "helpfully clean it up" mutation.
  QVERIFY(QFileInfo(anchorDir + QStringLiteral("/assets")).isSymLink());
}

void AssetCacheTests::
    ownedSuffixOfPlainDirectoriesUnderTrustedAnchorResolvesSuccessfully() {
  // Negative control for the test above: an ordinary, non-symlinked
  // owned-suffix chain (whether pre-existing or freshly created by
  // mkpath, both are exercised identically by this helper) must still
  // resolve -- the no-follow check must not be so strict it breaks the
  // overwhelmingly common, non-hostile case.
  const QString anchorDir = m_tempDirPath + QStringLiteral("/anchor2");
  QVERIFY(QDir().mkpath(anchorDir + QStringLiteral("/assets/v1")));

  QVERIFY(AssetCache::directoryChainResolvesNoFollowForTesting(
      anchorDir, {QStringLiteral("assets"), QStringLiteral("v1")}));
}

void AssetCacheTests::
    crossMountBindMountDirectoryDuringCleanupIsNeverDescendedIntoOrDeleted() {
  // Round-6 review item 5's second half: a same-device bind mount
  // planted inside this cache's exclusively-owned directory must be
  // treated exactly like a different-device mount (never descended
  // into, never deleted, its bytes still counted so quota accounting
  // cannot be starved by an undeletable/unreadable entry). A bind mount
  // shares its host filesystem's st_dev, so this specifically exercises
  // the STATX_MNT_ID-based mountIdentityMatches() check rather than the
  // pre-existing (and separately-tested-by-construction) st_dev-only
  // comparison. Creating a real mount needs privilege this environment
  // may not grant (e.g. this local macOS dev machine has no Linux-style
  // bind mounts at all, and a CI runner might lack passwordless sudo),
  // so this test fails closed by skipping rather than failing when that
  // privilege is unavailable -- the reviewer's own explicit allowance
  // ("mount tests where CI permits/fail-closed otherwise").
#if !defined(__linux__)
  QSKIP("bind mounts are a Linux-specific concept; not applicable on this "
        "platform");
#else
  const QString decoyMountPoint =
      m_tempDirPath + QStringLiteral("/decoy-mount");
  QVERIFY(QDir().mkpath(decoyMountPoint));
  // The bind mount's SOURCE directory is deliberately a completely
  // separate QTemporaryDir, NOT a sibling reachable from inside
  // `m_tempDirPath` itself. A real attacker's bind-mount source (the
  // scenario this test exercises: something foreign planted onto a
  // subdirectory of this cache's exclusively-owned root) always lives
  // somewhere else entirely on the filesystem -- it is never itself a
  // path this cache's own cleanup sweep would otherwise enumerate and
  // legitimately reclaim as an ordinary, unrecognized same-mount stray
  // directory (see strayDirectoryIsRemovedAndCountedTowardDiskUsage()'s
  // test just above, which relies on exactly that cleanup behaviour
  // being correct and unconditional for genuine same-mount strays).
  // Putting the source INSIDE the cache root would make this test
  // self-defeating: reapAndEnforceQuota()'s own (deliberately
  // unconditional) top-level stray-directory removal would recurse
  // into and delete the source's real content THROUGH ITS OWN PATH
  // (never through the mount, and never violating the cross-mount
  // guard this test exists to verify) the moment the constructor's
  // automatic reap sweep runs, collapsing the mount point's own
  // observed size to zero and starving the very assertion below for a
  // reason that has nothing to do with the cross-mount guard itself.
  QTemporaryDir bindSourceDir;
  QVERIFY(bindSourceDir.isValid());
  const QString bindSource = bindSourceDir.path();
  {
    QFile sentinel(bindSource + QStringLiteral("/sentinel.bin"));
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    sentinel.write(QByteArray(2048, 's'));
  }

  QProcess mountProc;
  mountProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("mount"),
                   QStringLiteral("--bind"), bindSource, decoyMountPoint});
  const bool mounted =
      mountProc.waitForFinished(5000) && mountProc.exitCode() == 0;
  if (!mounted) {
    QSKIP("passwordless bind-mount privilege unavailable in this "
          "environment; see the finding's own fail-closed allowance");
  }

  struct UnmountGuard {
    QString mountPoint;
    ~UnmountGuard() {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPoint});
    }
  } unmountGuard{decoyMountPoint};

  AssetCache cache(configFor(m_tempDirPath));

  // The cross-mount directory can never be descended into, so its
  // CONTENTS (the 2048-byte sentinel living on the other side of the
  // mount) are deliberately NOT what gets counted here -- but its own
  // directory-entry size must still be added as an irreducible
  // placeholder rather than silently vanishing from the total (see
  // sumUsageRelative()'s comment), so usage must be nonzero.
  QVERIFY(cache.diskUsageBytes() > qint64(0));

  cache.reapAndEnforceQuota();

  // The mount point directory itself, and the sentinel file behind it,
  // must both survive untouched -- the cross-mount guard must refuse to
  // recurse into or unlink it even though it looks exactly like an
  // ordinary stray directory the reap sweep would otherwise remove.
  QVERIFY(QFileInfo::exists(decoyMountPoint));
  QVERIFY(QFileInfo::exists(bindSource + QStringLiteral("/sentinel.bin")));
#endif
}

void AssetCacheTests::
    mountIdentificationIsActuallySupportedOnThisLinuxBuildUnprivileged() {
  // Regression for the actual root cause behind the bind-mount test
  // above ever failing at all on real CI: `STATX_MNT_ID` is declared by
  // the Linux kernel UAPI header, but was previously only reachable
  // through glibc's <sys/stat.h> feature-test-macro-gated extension --
  // invisible under this project's own `CMAKE_CXX_EXTENSIONS OFF`
  // strict `-std=c++23` build even on a brand-new glibc/kernel that
  // fully supports the feature at runtime. That silent compile-time
  // degradation made every same-device bind-mount comparison fall back
  // to a bare st_dev check, which wrongly treats a bind mount as
  // ordinary same-mount content -- exactly what the test above exists
  // to catch, but only when passwordless sudo is actually available.
  // This assertion needs no privilege at all (an ordinary, unmodified
  // directory already has its own real mount id on any Linux kernel new
  // enough to run this project at all), so it fails fast and
  // unconditionally on Linux CI even in an environment where the
  // bind-mount test above can only QSKIP.
#if !defined(__linux__)
  QSKIP("STATX_MNT_ID support is a Linux-specific concept; not "
        "applicable on this platform");
#else
  QVERIFY(AssetCache::mountIdentificationSupportedForTesting(m_tempDirPath));
#endif
}

void AssetCacheTests::
    mountinfoRawReadParsesAtLeastOneEntryOnThisLinuxBuildUnprivileged() {
  // Regression for the actual root cause behind all 3 real bind-mount
  // "transition is permitted" tests above failing on real CI, entirely
  // independent of the mount transition itself: reading
  // /proc/self/mountinfo via QFile's buffered QIODevice::atEnd()
  // machinery silently parsed EXACTLY ZERO lines on every real Linux
  // system, always -- fstat() reports a procfs pseudo-file's size as 0,
  // and QFile derives a random-access (non-sequential) device's
  // bytesAvailable()/atEnd() from that same stale size, so atEnd()
  // reported true immediately after open(), before a single byte was
  // ever actually read. This silently turned
  // mountPointHasTrustedLocalFilesystemType()'s real production check
  // into an unconditional "no entry found" no-op -- exactly why every
  // one of the 3 tests above that depends on it (not merely the
  // ownership/mode half of the check) could never pass on any real
  // Linux system, bind mount or not.
  //
  // This assertion needs no privilege and no bind mount at all: a
  // process ALWAYS has at least one real mountinfo entry (its own root
  // mount), so mountinfoParsedEntryCountForTesting() -- which exercises
  // the exact same raw-read-and-parse code path
  // mountPointHasTrustedLocalFilesystemType() itself uses in production
  // -- must report a strictly positive count on every real Linux
  // kernel. Under the pre-fix QFile-based implementation this would
  // deterministically have reported exactly 0 instead.
#if !defined(__linux__)
  QSKIP("/proc/self/mountinfo is a Linux-specific concept; not "
        "applicable on this platform");
#else
  const std::optional<int> parsedEntries =
      AssetCache::mountinfoParsedEntryCountForTesting();
  QVERIFY(parsedEntries.has_value());
  QVERIFY(*parsedEntries > 0);
#endif
}

namespace {
// RAII guard around AssetCache::setMountIdentificationDegradedForTesting()
// -- guarantees the process-wide override is always reset back to false
// even when a QVERIFY inside the test body triggers an early return, so
// this never leaks into an unrelated, later test.
struct MountIdentificationDegradationGuard {
  MountIdentificationDegradationGuard(bool forceOpenat2Unavailable,
                                      bool forceMountIdUnavailable) {
    AssetCache::setMountIdentificationDegradedForTesting(
        forceOpenat2Unavailable, forceMountIdUnavailable);
  }
  MountIdentificationDegradationGuard(
      const MountIdentificationDegradationGuard &) = delete;
  MountIdentificationDegradationGuard &
  operator=(const MountIdentificationDegradationGuard &) = delete;
  ~MountIdentificationDegradationGuard() {
    AssetCache::setMountIdentificationDegradedForTesting(false, false);
  }
};
} // namespace

void AssetCacheTests::
    cleanupAndQuotaDescentFailClosedWhenMountIdentificationIsUnavailableWithoutAnyRealMount() {
  // Round-N+ review (HIGH, repeat finding, "cleanup can traverse
  // same-device bind mounts when mount IDs are unavailable"): unlike
  // crossMountBindMountDirectoryDuringCleanupIsNeverDescendedIntoOrDeleted()
  // above (which needs a REAL bind mount and QSKIPs without passwordless
  // sudo, so it can never run deterministically on every CI runner),
  // this test forces the exact two conditions a legacy kernel would
  // present -- openat2() itself unavailable AND STATX_MNT_ID
  // unavailable -- against a perfectly ordinary, unprivileged, entirely
  // unmounted stray directory, and proves the fixed cleanup/quota code
  // now fails closed instead of silently treating it as safe (the
  // pre-fix bug: those three call sites called the PERMISSIVE
  // mountIdentityMatches() directly, which degrades to a bare st_dev
  // check the instant a mount id is unavailable on either side, and
  // would have wrongly accepted this exact scenario).
#if !defined(__linux__)
  QSKIP("mount-identity hardening is a Linux-specific concept; not "
        "applicable on this platform");
#else
  AssetCache cache(configFor(m_tempDirPath));

  // An entirely ordinary same-mount stray directory with nested
  // content -- exactly the shape
  // strayDirectoryIsRemovedAndCountedTowardDiskUsage() already proves
  // cleans up fine in the ordinary (non-degraded) case.
  const QString strayDirPath =
      m_tempDirPath + QStringLiteral("/stray-mount-degraded");
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("stray-mount-degraded")));
  {
    QFile nested(strayDirPath + QStringLiteral("/nested-file.bin"));
    QVERIFY(nested.open(QIODevice::WriteOnly));
    nested.write(QByteArray(4096, 'z'));
  }

  MountIdentificationDegradationGuard guard(
      /*forceOpenat2Unavailable=*/true, /*forceMountIdUnavailable=*/true);

  // diskUsageBytes() can no longer prove the stray directory remains on
  // the cache's own mount -- neither the kernel-native openat2()/
  // RESOLVE_NO_XDEV guarantee nor a real STATX_MNT_ID comparison is
  // available -- so this must now be treated as genuinely
  // indeterminate: disk persistence disables itself rather than
  // reporting a false, safe-looking usage total.
  QCOMPARE(cache.diskUsageBytes(), qint64(0));
  QVERIFY(cache.isDiskCacheDisabledForTesting());

  // The stray directory itself, and its nested file, must be left
  // completely untouched: a fail-closed refusal to descend must never
  // be confused with "safe to delete".
  QVERIFY(QFileInfo::exists(strayDirPath));
  QVERIFY(QFileInfo::exists(strayDirPath + QStringLiteral("/nested-file.bin")));
#endif
}

void AssetCacheTests::
    cleanupAndQuotaDescentSucceedNormallyWhenMountIdentificationIsFullyAvailable() {
  // Negative control for the test above: with NEITHER degradation
  // forced (the ordinary, fully-capable-kernel path this project
  // actually targets and the default state whenever the test-only
  // setter above is never called), the exact same directory shape must
  // still account and clean up completely normally -- proving the
  // fail-closed behaviour above is specific to the forced degradation
  // and not a general regression in ordinary operation.
#if !defined(__linux__)
  QSKIP("mount-identity hardening is a Linux-specific concept; not "
        "applicable on this platform");
#else
  MountIdentificationDegradationGuard guard(
      /*forceOpenat2Unavailable=*/false, /*forceMountIdUnavailable=*/false);

  AssetCache cache(configFor(m_tempDirPath));
  // Round-7 review item 4: see
  // strayDirectoryIsRemovedAndCountedTowardDiskUsage()'s matching comment above
  // -- captured on a pristine, empty cache rather than assumed to be 0.
  const qint64 emptyCacheBaselineBytes = cache.diskUsageBytes();

  const QString strayDirPath =
      m_tempDirPath + QStringLiteral("/stray-mount-normal");
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("stray-mount-normal")));
  {
    QFile nested(strayDirPath + QStringLiteral("/nested-file.bin"));
    QVERIFY(nested.open(QIODevice::WriteOnly));
    nested.write(QByteArray(4096, 'z'));
  }
  const qint64 strayDirEntryPhysicalBytes =
      physicalBytesOnDiskForTesting(strayDirPath);
  const qint64 nestedFilePhysicalBytes = physicalBytesOnDiskForTesting(
      strayDirPath + QStringLiteral("/nested-file.bin"));

  QCOMPARE(cache.diskUsageBytes(), emptyCacheBaselineBytes +
                                       nestedFilePhysicalBytes +
                                       strayDirEntryPhysicalBytes);
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  cache.reapAndEnforceQuota();
  QVERIFY(!QFileInfo::exists(strayDirPath));
  QCOMPARE(cache.diskUsageBytes(), emptyCacheBaselineBytes);
#endif
}

void AssetCacheTests::
    realBindMountWithOpenat2ForcedUnavailableIsStillConfirmedAndSkippedNeverDisablingDiskCache() {
  // Independent cumulative re-review: root-cause fix for a real bug
  // this exact test suite's own
  // crossMountBindMountDirectoryDuringCleanupIsNeverDescendedIntoOrDeleted()
  // test only ever happened to catch on a kernel that genuinely lacks
  // openat2() at all (e.g. this project's own containerized
  // verification environment) -- and even there, only ever surfaced as
  // an intermittent-looking failure rather than a deterministic,
  // environment-independent regression test. Root cause: when
  // openat2()'s kernel-native RESOLVE_NO_XDEV guarantee is unavailable,
  // openSubdirectoryNoFollowMountChecked() falls back to comparing
  // STATX_MNT_ID on both sides -- but a DEFINITIVE mismatch there (both
  // sides report a real mount id, and they genuinely differ, exactly
  // proving a real bind mount) was previously reported to its caller
  // identically to a genuinely INDETERMINATE result (mount id missing
  // entirely), so sumUsageRelative() disabled disk I/O for the whole
  // instance instead of performing the deliberate, already-tested skip
  // every OTHER proven-cross-mount case already receives.
  //
  // This test forces the exact "openat2 unavailable" condition
  // deterministically (via setMountIdentificationDegradedForTesting(),
  // no reliance on this build's actual kernel) while still requiring a
  // REAL bind mount so STATX_MNT_ID genuinely, definitively disagrees
  // between the two sides -- reproducing the precise fixed code path
  // regardless of whether the kernel this test happens to run on
  // natively supports openat2() or not.
#if !defined(__linux__)
  QSKIP("bind mounts are a Linux-specific concept; not applicable on this "
        "platform");
#else
  const QString decoyMountPoint =
      m_tempDirPath + QStringLiteral("/degraded-decoy-mount");
  QVERIFY(QDir().mkpath(decoyMountPoint));

  QTemporaryDir bindSourceDir;
  QVERIFY(bindSourceDir.isValid());
  const QString bindSource = bindSourceDir.path();
  {
    QFile sentinel(bindSource + QStringLiteral("/sentinel.bin"));
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    sentinel.write(QByteArray(2048, 's'));
  }

  QProcess mountProc;
  mountProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("mount"),
                   QStringLiteral("--bind"), bindSource, decoyMountPoint});
  const bool mounted =
      mountProc.waitForFinished(5000) && mountProc.exitCode() == 0;
  if (!mounted) {
    QSKIP("passwordless bind-mount privilege unavailable in this "
          "environment; see the finding's own fail-closed allowance");
  }

  struct UnmountGuard {
    QString mountPoint;
    ~UnmountGuard() {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPoint});
    }
  } unmountGuard{decoyMountPoint};

  // Force the exact no-openat2 fallback path -- mount id support
  // itself stays genuinely available, so STATX_MNT_ID can (and, over
  // a real bind mount, definitively will) disagree. Constructed BEFORE
  // AssetCache below so that its constructor's own automatic
  // reapAndEnforceQuota() sweep -- which walks this exact decoy mount
  // point -- exercises the fixed fallback path too, not just a later
  // explicit call.
  MountIdentificationDegradationGuard guard(
      /*forceOpenat2Unavailable=*/true, /*forceMountIdUnavailable=*/false);

  AssetCache cache(configFor(m_tempDirPath));

  // The fix under test: this must be treated exactly like the
  // openat2-available case above -- a confirmed cross-mount skip, never
  // an indeterminate disable of the whole instance's disk I/O. This is
  // asserted directly via isDiskCacheDisabledForTesting() (the exact
  // regression: before the fix, a definitive same-device mount-id
  // mismatch reached via this fallback path was indistinguishable from
  // a genuinely indeterminate one, and disabled disk I/O for the whole
  // instance) rather than solely via a physical-byte-count comparison,
  // since some filesystems (e.g. certain overlayfs configurations, as
  // seen in this project's own containerized verification environment)
  // legitimately report zero allocated blocks for every directory node,
  // which would make a strict "> 0" byte-count assertion unreliable
  // there even though the underlying fix (not disabling the instance)
  // is unaffected by that filesystem quirk. diskUsageBytes() is still
  // required to complete without disabling the instance and to return a
  // valid non-negative total (never treated as indeterminate).
  QVERIFY(!cache.isDiskCacheDisabledForTesting());
  QVERIFY(cache.diskUsageBytes() >= qint64(0));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  cache.reapAndEnforceQuota();

  QVERIFY(QFileInfo::exists(decoyMountPoint));
  QVERIFY(QFileInfo::exists(bindSource + QStringLiteral("/sentinel.bin")));
#endif
}

void AssetCacheTests::
    ownedSuffixMissingComponentsAreCreatedViaMkdiratNeverPathBasedMkpath() {
  // Round-7/8 item 2/5 (HIGH): the constructor no longer calls
  // QDir::mkpath() at all -- openDirectoryChainNoFollow() itself must
  // now create any MISSING owned-suffix component, via mkdirat()
  // relative to an already-open, already no-follow-verified parent
  // descriptor. Prove this directly against the exact function under
  // test: neither "assets" nor "v1" exist under `anchorDir` beforehand
  // (unlike
  // ownedSuffixOfPlainDirectoriesUnderTrustedAnchorResolvesSuccessfully()'s
  // negative control, which pre-creates them with QDir::mkpath() itself
  // purely as harness setup), yet resolution still succeeds and leaves
  // genuine, non-symlink directories behind on disk.
  const QString anchorDir = m_tempDirPath + QStringLiteral("/anchor3");
  QVERIFY(QDir().mkpath(anchorDir));
  QVERIFY(!QFileInfo::exists(anchorDir + QStringLiteral("/assets")));

  QVERIFY(AssetCache::directoryChainResolvesNoFollowForTesting(
      anchorDir, {QStringLiteral("assets"), QStringLiteral("v1")}));

  QVERIFY(QFileInfo(anchorDir + QStringLiteral("/assets")).isDir());
  QVERIFY(!QFileInfo(anchorDir + QStringLiteral("/assets")).isSymLink());
  QVERIFY(QFileInfo(anchorDir + QStringLiteral("/assets/v1")).isDir());
  QVERIFY(!QFileInfo(anchorDir + QStringLiteral("/assets/v1")).isSymLink());

  // Idempotent: resolving again against the now-already-existing chain
  // must still succeed (mkdirat()'s EEXIST is treated as success, not a
  // failure), matching mkdir -p's own "already there is fine" semantics
  // without ever falling back to a path-based call to get it.
  QVERIFY(AssetCache::directoryChainResolvesNoFollowForTesting(
      anchorDir, {QStringLiteral("assets"), QStringLiteral("v1")}));
}

void AssetCacheTests::
    constructingWithIntermediateConfiguredDirectorySymlinkNeverAutoCreatesOrRecoversForeignDirectory() {
  // Round-7/8 item 2 (HIGH): previously, QDir::mkpath(m_directory) ran
  // UNCONDITIONALLY before any fd-based validation, and -- being a
  // plain path-based `mkdir -p` with no symlink-awareness -- would
  // silently create the configured leaf directory THROUGH an
  // attacker-planted symlink for an INTERMEDIATE ancestor (one the
  // single leaf-only QFileInfo::isSymLink() check could never see),
  // "destructively recovering" a directory at wherever that symlink
  // happened to point. Construct with Config::directory pointing at a
  // NOT-YET-EXISTING leaf, one of whose ANCESTORS is a symlink to a
  // completely separate external sentinel location, and prove: (a) the
  // cache disables disk I/O entirely, (b) NOTHING is ever created at
  // the configured path or anywhere beneath the symlink target, and (c)
  // the sentinel directory the symlink points at is left with exactly
  // its own pre-existing contents, untouched.
  const QString sentinelDir =
      m_tempDirPath + QStringLiteral("/intermediate-sentinel");
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("intermediate-sentinel")));
  const QString sentinelFile =
      sentinelDir + QStringLiteral("/pre-existing.txt");
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("must-not-be-touched"));
  }

  // `configured-symlink-ancestor` is a symlink to the sentinel; the
  // actual configured cache directory is a LEAF underneath it
  // ("cache-leaf") that does not exist yet on either side.
  const QString symlinkAncestor =
      m_tempDirPath + QStringLiteral("/configured-symlink-ancestor");
  QVERIFY(QFile::link(sentinelDir, symlinkAncestor));
  QVERIFY(QFileInfo(symlinkAncestor).isSymLink());
  const QString configuredDirectory =
      symlinkAncestor + QStringLiteral("/cache-leaf");
  // Confirm the leaf genuinely does not exist through EITHER path
  // before construction -- otherwise this test would not actually
  // exercise the "must never be auto-created" guarantee at all.
  QVERIFY(!QFileInfo::exists(configuredDirectory));
  QVERIFY(!QFileInfo::exists(sentinelDir + QStringLiteral("/cache-leaf")));

  AssetCache cache(configFor(configuredDirectory));
  QVERIFY(cache.isDiskCacheDisabledForTesting());

  // Neither side of the symlink ever gained a "cache-leaf" directory:
  // no directory creation ever happened at all, through the symlink or
  // otherwise.
  QVERIFY(!QFileInfo::exists(configuredDirectory));
  QVERIFY(!QFileInfo::exists(sentinelDir + QStringLiteral("/cache-leaf")));

  // The memory cache still works; only disk persistence is disabled.
  const QString key = QString::fromLatin1(
      QCryptographicHash::hash(
          QByteArrayLiteral("intermediate-symlink-ancestor-key"),
          QCryptographicHash::Sha256)
          .toHex());
  cache.store(key, makeEntry(QByteArrayLiteral("payload-bytes")));
  QVERIFY(cache.lookupMemory(key).has_value());
  QCOMPARE(cache.diskUsageBytes(), qint64(0));

  cache.reapAndEnforceQuota();

  // The sentinel's own pre-existing file must still be exactly what it
  // was -- nothing was ever written into, through, or alongside it.
  QVERIFY(QFileInfo::exists(sentinelFile));
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("must-not-be-touched"));
  }
  QDir sentinelListing(sentinelDir);
  QCOMPARE(sentinelListing.entryList(QDir::Files | QDir::Dirs |
                                     QDir::NoDotAndDotDot),
           QStringList{"pre-existing.txt"});
}

void AssetCacheTests::
    bindMountOverOwnedSuffixComponentIsRejectedDuringChainResolution() {
  // Round-7/8 item 2 (HIGH), construction-time half of the round-6
  // cleanup-time bind-mount guard above: openDirectoryChainNoFollow()'s
  // per-step walk must now ALSO reject an owned-suffix component that
  // resolves onto a different mount than its trusted anchor -- not just
  // detect it later, after the fact, during a cleanup sweep. Exercises
  // the exact same mountIdentityMatches()-based guard added to the
  // walk's loop this round, directly against
  // directoryChainResolvesNoFollowForTesting() rather than a full
  // AssetCache (which would require redirecting the real
  // QStandardPaths::CacheLocation, an unrelated and riskier harness
  // change) -- fails closed (QSKIP) wherever passwordless bind-mount
  // privilege is unavailable, exactly like the pre-existing cleanup-
  // time test above.
#if !defined(__linux__)
  QSKIP("bind mounts are a Linux-specific concept; not applicable on this "
        "platform");
#else
  const QString anchorDir = m_tempDirPath + QStringLiteral("/anchor4");
  QVERIFY(QDir().mkpath(anchorDir + QStringLiteral("/assets")));

  QTemporaryDir bindSourceDir;
  QVERIFY(bindSourceDir.isValid());

  QProcess mountProc;
  mountProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("mount"),
                   QStringLiteral("--bind"), bindSourceDir.path(),
                   anchorDir + QStringLiteral("/assets")});
  const bool mounted =
      mountProc.waitForFinished(5000) && mountProc.exitCode() == 0;
  if (!mounted) {
    QSKIP("passwordless bind-mount privilege unavailable in this "
          "environment; see the finding's own fail-closed allowance");
  }
  struct UnmountGuard {
    QString mountPoint;
    ~UnmountGuard() {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPoint});
    }
  } unmountGuard{anchorDir + QStringLiteral("/assets")};

  QVERIFY(!AssetCache::directoryChainResolvesNoFollowForTesting(
      anchorDir, {QStringLiteral("assets"), QStringLiteral("v1")}));
#endif
}

void AssetCacheTests::
    precreatedLeafBehindDeepIntermediateSymlinkInConfiguredDirectoryIsRejectedAndSentinelUntouched() {
  // Round-9+ review (HIGH): the CORE demonstrated attack -- a
  // configured directory whose full path already exists via a
  // symlinked ancestor TWO levels above the leaf ("outer-symlink" ->
  // sentinel, then a plain "inner-normal" directory, then the leaf
  // "cache-leaf" itself, precreated under the sentinel so it exists
  // through BOTH the symlink path and directly). The pre-round-9
  // "longest existing prefix is a single trusted anchor" shortcut
  // could never catch this: the WHOLE configured path resolves
  // successfully (through the symlink), so nothing was ever identified
  // as "still needing a no-follow walk" at all. The fixed resolver must
  // instead walk every component -- home, ..., "outer-symlink",
  // "inner-normal", "cache-leaf" -- individually, and reject the moment
  // it reaches "outer-symlink" itself.
  const QString sentinelDir =
      m_tempDirPath + QStringLiteral("/deep-intermediate-sentinel");
  QVERIFY(QDir().mkpath(sentinelDir + QStringLiteral("/inner-normal")));
  QVERIFY(
      QDir().mkpath(sentinelDir + QStringLiteral("/inner-normal/cache-leaf")));
  const QString sentinelFile =
      sentinelDir + QStringLiteral("/pre-existing.txt");
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("must-not-be-touched"));
  }

  const QString outerSymlink = m_tempDirPath + QStringLiteral("/outer-symlink");
  QVERIFY(QFile::link(sentinelDir, outerSymlink));
  QVERIFY(QFileInfo(outerSymlink).isSymLink());

  const QString configuredDirectory =
      outerSymlink + QStringLiteral("/inner-normal/cache-leaf");
  // Confirm the leaf genuinely exists THROUGH the symlinked path --
  // this is precisely what defeats a "does the whole thing already
  // exist" shortcut.
  QVERIFY(QFileInfo::exists(configuredDirectory));
  QVERIFY(!QFileInfo(configuredDirectory).isSymLink());

  AssetCache cache(configFor(configuredDirectory));
  QVERIFY(cache.isDiskCacheDisabledForTesting());

  // The sentinel's own pre-existing content is exactly as it was --
  // this rejection is a pure read-only refusal, never a "helpfully
  // clean it up" mutation, and nothing was ever created or touched
  // anywhere along or beneath the symlinked path.
  QVERIFY(QFileInfo::exists(sentinelFile));
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("must-not-be-touched"));
  }

  // The memory cache still works; only disk persistence is disabled.
  const QString key = QString::fromLatin1(
      QCryptographicHash::hash(
          QByteArrayLiteral("deep-intermediate-symlink-key"),
          QCryptographicHash::Sha256)
          .toHex());
  cache.store(key, makeEntry(QByteArrayLiteral("payload-bytes")));
  QVERIFY(cache.lookupMemory(key).has_value());
  QCOMPARE(cache.diskUsageBytes(), qint64(0));
}

void AssetCacheTests::
    precreatedLeafBehindIntermediateSymlinkOutsideHomeIsRejectedAndSentinelUntouched() {
  // The OUTSIDE-home counterpart to the test immediately above. Uses a
  // base directory rooted at the canonicalized system temp location
  // (NEVER under home, so this genuinely exercises the "/"-anchored
  // fallback branch) rather than `m_tempDirPath` (which `init()`
  // deliberately places under home to exercise the common,
  // home-anchored branch instead -- see its own comment). The base is
  // canonicalized FIRST specifically so any legitimate OS-bootstrap
  // symlink an ancestor of the system temp directory might itself be
  // (e.g. macOS's `/tmp` -> `/private/tmp`, `/var` -> `/private/var`)
  // is already resolved away before this test plants its OWN, entirely
  // deliberate intermediate symlink -- isolating exactly the one
  // symlink this test means to exercise from any incidental OS one.
  const QString canonicalBase = QFileInfo(QDir::tempPath()).canonicalFilePath();
  QVERIFY(!canonicalBase.isEmpty());
  const QString home = QDir::cleanPath(QDir::homePath());
  QVERIFY2(canonicalBase != home &&
               !canonicalBase.startsWith(home + QLatin1Char('/')),
           "test precondition: the canonicalized system temp directory must "
           "be outside home for this test to exercise the outside-home "
           "fallback branch at all");

  const QString scratchDir =
      canonicalBase + QStringLiteral("/arkham-outside-home-symlink-test-%1")
                          .arg(QRandomGenerator::global()->generate());
  QVERIFY(QDir().mkpath(scratchDir));
  struct ScopedScratchRemoval {
    QString outerSymlink;
    QString base;
    ~ScopedScratchRemoval() {
      // Remove the symlink node itself FIRST (QFile::remove never
      // follows it) so the subsequent recursive removal of `base`
      // cannot possibly descend through it into the sentinel a second
      // time via two different paths.
      QFile::remove(outerSymlink);
      QDir(base).removeRecursively();
    }
  };

  const QString sentinelDir = scratchDir + QStringLiteral("/sentinel");
  QVERIFY(QDir().mkpath(sentinelDir + QStringLiteral("/inner-normal")));
  QVERIFY(
      QDir().mkpath(sentinelDir + QStringLiteral("/inner-normal/cache-leaf")));
  const QString sentinelFile =
      sentinelDir + QStringLiteral("/pre-existing.txt");
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("must-not-be-touched"));
  }

  const QString outerSymlink = scratchDir + QStringLiteral("/outer-symlink");
  ScopedScratchRemoval cleanupGuard{outerSymlink, scratchDir};
  QVERIFY(QFile::link(sentinelDir, outerSymlink));
  QVERIFY(QFileInfo(outerSymlink).isSymLink());

  const QString configuredDirectory =
      outerSymlink + QStringLiteral("/inner-normal/cache-leaf");
  // The leaf genuinely exists THROUGH the symlinked path -- exactly
  // what defeats a "longest existing prefix" shortcut, and exactly why
  // this test's base is deliberately OUTSIDE home (only that fallback
  // branch ever used such a shortcut).
  QVERIFY(QFileInfo::exists(configuredDirectory));
  QVERIFY(!QFileInfo(configuredDirectory).isSymLink());
  QVERIFY(!configuredDirectory.startsWith(home + QLatin1Char('/')));

  AssetCache cache(configFor(configuredDirectory));
  QVERIFY(cache.isDiskCacheDisabledForTesting());

  // Purely a read-only refusal: the sentinel's own pre-existing content
  // is exactly as it was, and nothing was ever created or touched
  // anywhere along or beneath the symlinked path.
  QVERIFY(QFileInfo::exists(sentinelFile));
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("must-not-be-touched"));
  }

  // The memory cache still works; only disk persistence is disabled.
  const QString key = QString::fromLatin1(
      QCryptographicHash::hash(
          QByteArrayLiteral("outside-home-intermediate-symlink-key"),
          QCryptographicHash::Sha256)
          .toHex());
  cache.store(key, makeEntry(QByteArrayLiteral("payload-bytes")));
  QVERIFY(cache.lookupMemory(key).has_value());
  QCOMPARE(cache.diskUsageBytes(), qint64(0));
}

void AssetCacheTests::
    cleanInstallWithEntirelyMissingCacheHierarchyIsCreatedSecurely() {
  // Round-9+ review (MEDIUM, "clean install"): the default cache
  // location's own ancestor components (everything the OS/desktop
  // environment would normally already have created) must be created
  // securely on demand when NONE of them exist yet -- not just this
  // application's fixed "assets/v1" suffix beneath an assumed
  // pre-existing OS parent. Exercise this directly against the same
  // home-anchored resolver AssetCache::AssetCache() itself now uses,
  // with an entirely fresh, multi-level, nowhere-yet-existing target
  // path under this test's own temp-under-home directory.
  const QString entirelyMissingHierarchy =
      m_tempDirPath + QStringLiteral("/does-not-exist-yet/nested/assets/v1");
  QVERIFY(!QFileInfo::exists(m_tempDirPath +
                             QStringLiteral("/does-not-exist-yet")));

  QVERIFY(AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      entirelyMissingHierarchy, /*allowCreateMissingComponents=*/true));

  QVERIFY(QFileInfo(entirelyMissingHierarchy).isDir());
  QVERIFY(!QFileInfo(entirelyMissingHierarchy).isSymLink());
  QVERIFY(
      QFileInfo(m_tempDirPath + QStringLiteral("/does-not-exist-yet")).isDir());
}

namespace {
// RAII guard around a temporary $HOME override -- QDir::homePath()
// reads $HOME fresh from the environment on every call on Unix (never
// cached at static-init time), so overriding it here is sufficient to
// redirect resolveTrustedDirectoryNoFollow()'s home-anchored branch at
// an entirely fake, test-constructed directory tree. Restores the
// PRIOR value (or unsets it, if it was never set to begin with) in the
// destructor so this never leaks into an unrelated, later test -- most
// importantly init()'s own QTemporaryDir construction, which itself
// depends on QDir::homePath() to place every test's temp directory.
struct HomeEnvOverrideGuard {
  explicit HomeEnvOverrideGuard(const QString &fakeHome)
      : m_hadOriginal(qEnvironmentVariableIsSet("HOME")),
        m_originalHome(qgetenv("HOME")) {
    qputenv("HOME", fakeHome.toUtf8());
  }
  HomeEnvOverrideGuard(const HomeEnvOverrideGuard &) = delete;
  HomeEnvOverrideGuard &operator=(const HomeEnvOverrideGuard &) = delete;
  ~HomeEnvOverrideGuard() {
    if (m_hadOriginal) {
      qputenv("HOME", m_originalHome);
    } else {
      qunsetenv("HOME");
    }
  }
  bool m_hadOriginal;
  QByteArray m_originalHome;
};

// RAII guard around
// AssetCache::setHomeComponentOwnershipModePolicyOverrideForTesting() --
// used ONLY to prove the otherwise-unreachable-without-real-root
// "ordinary, legitimately root-provisioned ancestor / legitimately
// account-owned home passes" branch hermetically; tests proving
// rejection use the real, unmodified fstat()-based checks instead (see
// the override's own declaration comment in AssetCache.h).
struct HomeComponentOwnershipModePolicyOverrideGuard {
  explicit HomeComponentOwnershipModePolicyOverrideGuard(bool passes) {
    AssetCache::setHomeComponentOwnershipModePolicyOverrideForTesting(
        /*active=*/true, passes);
  }
  HomeComponentOwnershipModePolicyOverrideGuard(
      const HomeComponentOwnershipModePolicyOverrideGuard &) = delete;
  HomeComponentOwnershipModePolicyOverrideGuard &
  operator=(const HomeComponentOwnershipModePolicyOverrideGuard &) = delete;
  ~HomeComponentOwnershipModePolicyOverrideGuard() {
    AssetCache::setHomeComponentOwnershipModePolicyOverrideForTesting(
        /*active=*/false);
  }
};
} // namespace

void AssetCacheTests::
    intermediateSymlinkWithinTheHomePathItselfIsRejectedEvenForAPreexistingLeaf() {
  // Round-N+ review (MEDIUM, repeat finding, "default cache still
  // trusts an already-resolved multi-component home path"): the fake
  // home's OWN path is
  // "<tempDir>/fake-home-parent/ancestor-link/actual-home", where
  // "ancestor-link" -- an ANCESTOR of home, not home's own final
  // component -- is a symlink to a completely separate sentinel
  // location. "actual-home" is precreated UNDER the sentinel so the
  // complete home path resolves successfully via ordinary,
  // symlink-following path resolution (QFileInfo::exists() on the full
  // path would report true) -- exactly the shape that defeated the
  // pre-fix single leaf-only O_NOFOLLOW open() of the whole home path
  // string, which only ever inspected home's OWN final component.
  const QString sentinelDir =
      m_tempDirPath + QStringLiteral("/home-attack-sentinel");
  QVERIFY(QDir().mkpath(sentinelDir + QStringLiteral("/actual-home")));
  const QString sentinelFile =
      sentinelDir + QStringLiteral("/pre-existing.txt");
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("must-not-be-touched"));
  }

  const QString fakeHomeParent =
      m_tempDirPath + QStringLiteral("/fake-home-parent");
  QVERIFY(QDir().mkpath(fakeHomeParent));
  const QString ancestorLink =
      fakeHomeParent + QStringLiteral("/ancestor-link");
  QVERIFY(QFile::link(sentinelDir, ancestorLink));
  QVERIFY(QFileInfo(ancestorLink).isSymLink());

  const QString fakeHome = ancestorLink + QStringLiteral("/actual-home");
  // Confirm the attack shape is real: the leaf exists (through the
  // symlink) before this test ever overrides $HOME or calls the
  // resolver under test.
  QVERIFY(QFileInfo::exists(fakeHome));
  QVERIFY(!QFileInfo(fakeHome).isSymLink());

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  QVERIFY(!AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));

  // Purely a read-only refusal: nothing was ever created through or
  // alongside the symlink, and the sentinel's own pre-existing content
  // is exactly as it was.
  QVERIFY(!QFileInfo::exists(fakeHome + QStringLiteral("/assets")));
  QVERIFY(QFileInfo::exists(sentinelFile));
  {
    QFile file(sentinelFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("must-not-be-touched"));
  }
  QVERIFY(QFileInfo(ancestorLink).isSymLink());
}

void AssetCacheTests::
    ordinaryMultiComponentHomePathWithNoSymlinksResolvesSuccessfully() {
  // Negative control for the test above: an entirely ordinary,
  // symlink-free fake home (still multi-component, still reached only
  // via a $HOME override) must resolve and even auto-create its owned
  // suffix normally -- proving the rejection above is specific to the
  // planted symlink, not a general regression whenever $HOME simply
  // differs from this test process's own real home.
  const QString fakeHome =
      m_tempDirPath + QStringLiteral("/ordinary-fake-home-parent/actual-home");
  QVERIFY(QDir().mkpath(fakeHome));

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));

  // Independent cumulative re-review (MEDIUM, "Validate owner/mode for
  // EVERY opened component regardless mount transition"): every
  // ANCESTOR component of $HOME is now checked for real, root
  // ownership even without a transition -- but this fixture's fake
  // home tree lives entirely beneath this TEST PROCESS'S OWN temp
  // directory, so every one of its ancestor components is genuinely
  // owned by this (unprivileged, non-root) test process, not root,
  // exactly like the "wrong owner ancestor" case the fix now correctly
  // refuses. There is no portable, unprivileged way to make a real
  // directory genuinely root-owned, so this override is used ONLY to
  // simulate "every ancestor legitimately passes the real-world
  // ownership/mode policy" for this specific positive control -- it
  // does not weaken or bypass the real check in production, never
  // affects the FINAL-home decision (which this fixture's own,
  // genuinely-owned, non-writable directory already satisfies for
  // real, unconditionally), and no mount transition is even in play
  // here (this whole fake tree lives on one mount throughout).
  HomeComponentOwnershipModePolicyOverrideGuard ownershipGuard(
      /*passes=*/true);

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  QVERIFY(AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));

  QVERIFY(QFileInfo(configuredUnderFakeHome).isDir());
  QVERIFY(!QFileInfo(configuredUnderFakeHome).isSymLink());
}

void AssetCacheTests::
    sameMountGroupWorldWritableFinalHomeDirectoryIsRejectedWithoutAnyMountTransition() {
  // Independent cumulative re-review (MEDIUM, "Validate owner/mode for
  // EVERY opened component regardless mount transition ... Production
  // same-mount test wrong owner/world-writable"): before this fix,
  // ownership/mode was consulted ONLY inside resolveHomeDirectoryNoFollow()'s
  // `!sameMount` branch -- i.e. only when a component's open() actually
  // crossed onto a DIFFERENT mount than its parent. This fake home has
  // NO mount transition anywhere in its walk at all (its entire tree
  // lives beneath this test process's own temp directory, on one
  // mount, throughout) -- exactly the shape an ordinary, unprivileged,
  // entirely unmounted misconfiguration (or attacker who can influence
  // permissions but not plant an actual mount) would produce. The
  // FINAL account-home directory itself is real-chmod()'d to 0777
  // (group AND world-writable) -- no privilege needed, since this
  // process already owns it -- and this decision deliberately is NEVER
  // subject to the ancestor-only test override (see
  // componentPassesOwnershipModePolicy()'s own comment), so this
  // exercises the REAL, unmodified fstat()-based final-home check.
  const QString fakeHome =
      m_tempDirPath +
      QStringLiteral("/world-writable-final-home-parent/actual-home");
  QVERIFY(QDir().mkpath(fakeHome));
  QVERIFY(::chmod(QFile::encodeName(fakeHome).constData(),
                  S_IRWXU | S_IRWXG | S_IRWXO) == 0);

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));
  // Ancestors above this fixture's own final component still need the
  // override (they can never be genuinely root-owned in an
  // unprivileged test) -- but the final-home decision under test here
  // is never affected by it.
  HomeComponentOwnershipModePolicyOverrideGuard ownershipGuard(
      /*passes=*/true);

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  QVERIFY(!AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));
  QVERIFY(!QFileInfo::exists(configuredUnderFakeHome));
}

void AssetCacheTests::
    sameMountOrdinaryFinalHomeDirectoryStillResolvesSuccessfullyWithoutAnyMountTransition() {
  // Negative control for the test above: the IDENTICAL same-mount, no-
  // transition shape, but the final home directory keeps its ordinary,
  // non-group/world-writable mode -- proving the rejection above is
  // specific to the writable mode, not a general regression whenever
  // no mount transition occurs anywhere in the walk.
  const QString fakeHome =
      m_tempDirPath +
      QStringLiteral("/ordinary-final-home-no-transition-parent/actual-home");
  QVERIFY(QDir().mkpath(fakeHome));
  QVERIFY(::chmod(QFile::encodeName(fakeHome).constData(), S_IRWXU) == 0);

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));
  HomeComponentOwnershipModePolicyOverrideGuard ownershipGuard(
      /*passes=*/true);

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  QVERIFY(AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));
  QVERIFY(QFileInfo(configuredUnderFakeHome).isDir());
}

namespace {
// RAII guard around
// AssetCache::setAuthoritativeAccountHomeDirectoryOverrideForTesting() --
// guarantees the process-wide override is always reset back to
// inactive even when a QVERIFY inside the test body triggers an early
// return, so this never leaks into an unrelated, later test.
struct AuthoritativeAccountHomeOverrideGuard {
  explicit AuthoritativeAccountHomeOverrideGuard(const QString &value) {
    AssetCache::setAuthoritativeAccountHomeDirectoryOverrideForTesting(
        /*active=*/true, value);
  }
  AuthoritativeAccountHomeOverrideGuard(
      const AuthoritativeAccountHomeOverrideGuard &) = delete;
  AuthoritativeAccountHomeOverrideGuard &
  operator=(const AuthoritativeAccountHomeOverrideGuard &) = delete;
  ~AuthoritativeAccountHomeOverrideGuard() {
    AssetCache::setAuthoritativeAccountHomeDirectoryOverrideForTesting(
        /*active=*/false);
  }
};

// RAII guard around
// AssetCache::setMountTransitionPolicyQualificationOverrideForTesting() --
// guarantees the process-wide override is always reset back to inactive
// even when a QVERIFY inside the test body triggers an early return.
struct MountTransitionPolicyQualificationOverrideGuard {
  explicit MountTransitionPolicyQualificationOverrideGuard(bool qualified) {
    AssetCache::setMountTransitionPolicyQualificationOverrideForTesting(
        /*active=*/true, qualified);
  }
  MountTransitionPolicyQualificationOverrideGuard(
      const MountTransitionPolicyQualificationOverrideGuard &) = delete;
  MountTransitionPolicyQualificationOverrideGuard &
  operator=(const MountTransitionPolicyQualificationOverrideGuard &) = delete;
  ~MountTransitionPolicyQualificationOverrideGuard() {
    AssetCache::setMountTransitionPolicyQualificationOverrideForTesting(
        /*active=*/false);
  }
};

// RAII guard around
// AssetCache::setMountSourceBackingIdentityOverrideForTesting() --
// guarantees the process-wide override is always reset back to inactive
// even when a QVERIFY inside the test body triggers an early return.
// Unlike the guard above, this leaves every OTHER real mount-identity
// check (root=="/", mount-id/device/parent-id correlation, fstype
// allowlist) genuinely unmodified -- see
// mountSourceIsTrustedBackingIdentity()'s own comment.
struct MountSourceBackingIdentityOverrideGuard {
  explicit MountSourceBackingIdentityOverrideGuard(bool trusted) {
    AssetCache::setMountSourceBackingIdentityOverrideForTesting(
        /*active=*/true, trusted);
  }
  MountSourceBackingIdentityOverrideGuard(
      const MountSourceBackingIdentityOverrideGuard &) = delete;
  MountSourceBackingIdentityOverrideGuard &
  operator=(const MountSourceBackingIdentityOverrideGuard &) = delete;
  ~MountSourceBackingIdentityOverrideGuard() {
    AssetCache::setMountSourceBackingIdentityOverrideForTesting(
        /*active=*/false);
  }
};
} // namespace

namespace {
// Independent cumulative re-review (MEDIUM, repeat finding, "home
// trust... arbitrary same-device bind mount still passes... even
// tests arbitrary /dev/shm bind as accepted"): production code no
// longer trusts tmpfs/overlay at all (see
// trustedLocalMountFilesystemTypes()'s own comment in AssetCache.cpp)
// and now additionally requires mountinfo's own "root" field to be
// exactly "/" (see mountIdHasTrustedLocalFilesystemType()'s own
// comment) -- a genuine dedicated partition mounts an ENTIRE
// filesystem, never merely bind-mounts some arbitrary already-existing
// SUBDIRECTORY of one. Every "legitimate mount transition" positive
// control in this file therefore needs a real, disk-backed (ext4)
// filesystem, freshly created via a loopback device and mounted
// WHOLESALE (never bind-mounted as a subdirectory) -- `/dev/shm`
// (tmpfs) is no longer suitable for this purpose at all, on either
// count. Requires `mkfs.ext4`/`losetup`/passwordless sudo `mount`;
// gracefully returns std::nullopt (never QSKIPs itself -- callers must
// do so, exactly like every other privilege-dependent step in these
// tests) when any step is unavailable or fails, since real CI
// runners/dev environments are expected to have these (e2fsprogs,
// util-linux) preinstalled, but this must never be assumed absolutely.
// `keepAliveOut` receives ownership of the RAII cleanup object (backing
// file, loop device, and the mount itself) so nothing is torn down out
// from under the caller until the caller itself goes out of scope.
struct LoopbackExt4Mount {
  QString mountPointPath;
  QString loopDevicePath;
  QString backingFilePath;
  QString mountPointDirToRemove;

  LoopbackExt4Mount(const LoopbackExt4Mount &) = delete;
  LoopbackExt4Mount &operator=(const LoopbackExt4Mount &) = delete;
  LoopbackExt4Mount(LoopbackExt4Mount &&) = default;
  LoopbackExt4Mount &operator=(LoopbackExt4Mount &&) = default;
  explicit LoopbackExt4Mount(QString mountPoint, QString loopDevice,
                             QString backingFile, QString mountPointDir)
      : mountPointPath(std::move(mountPoint)),
        loopDevicePath(std::move(loopDevice)),
        backingFilePath(std::move(backingFile)),
        mountPointDirToRemove(std::move(mountPointDir)) {}
  ~LoopbackExt4Mount() {
    if (!mountPointPath.isEmpty()) {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPointPath});
    }
    if (!loopDevicePath.isEmpty()) {
      QProcess::execute(QStringLiteral("sudo"),
                        {QStringLiteral("-n"), QStringLiteral("losetup"),
                         QStringLiteral("-d"), loopDevicePath});
    }
    if (!mountPointDirToRemove.isEmpty()) {
      QDir().rmdir(mountPointDirToRemove);
    }
    if (!backingFilePath.isEmpty()) {
      QFile::remove(backingFilePath);
    }
  }
};

std::optional<QString> createLoopbackExt4BindSourceDirectory(
    std::unique_ptr<LoopbackExt4Mount> &keepAliveOut) {
  QTemporaryDir scratchDir;
  if (!scratchDir.isValid()) {
    return std::nullopt;
  }
  scratchDir.setAutoRemove(false);
  const QString backingFile =
      scratchDir.path() + QStringLiteral("/backing.img");
  const QString mountPointDir = scratchDir.path() + QStringLiteral("/mnt");
  if (!QDir().mkpath(mountPointDir)) {
    return std::nullopt;
  }

  QProcess truncateProc;
  truncateProc.start(
      QStringLiteral("truncate"),
      {QStringLiteral("-s"), QStringLiteral("16M"), backingFile});
  if (!truncateProc.waitForFinished(5000) || truncateProc.exitCode() != 0) {
    return std::nullopt;
  }

  QProcess mkfsProc;
  mkfsProc.start(QStringLiteral("mkfs.ext4"),
                 {QStringLiteral("-q"), QStringLiteral("-F"), backingFile});
  if (!mkfsProc.waitForFinished(15000) || mkfsProc.exitCode() != 0) {
    QFile::remove(backingFile);
    return std::nullopt;
  }

  QProcess losetupProc;
  losetupProc.start(QStringLiteral("sudo"),
                    {QStringLiteral("-n"), QStringLiteral("losetup"),
                     QStringLiteral("-f"), QStringLiteral("--show"),
                     backingFile});
  if (!losetupProc.waitForFinished(5000) || losetupProc.exitCode() != 0) {
    QFile::remove(backingFile);
    return std::nullopt;
  }
  const QString loopDevice =
      QString::fromUtf8(losetupProc.readAllStandardOutput()).trimmed();
  if (loopDevice.isEmpty()) {
    QFile::remove(backingFile);
    return std::nullopt;
  }

  // A whole-filesystem mount (never `--bind`) so mountinfo's own
  // "root" field reads "/" -- see this function's own top comment.
  QProcess mountProc;
  mountProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("mount"),
                   QStringLiteral("-t"), QStringLiteral("ext4"), loopDevice,
                   mountPointDir});
  if (!mountProc.waitForFinished(5000) || mountProc.exitCode() != 0) {
    QProcess::execute(QStringLiteral("sudo"),
                      {QStringLiteral("-n"), QStringLiteral("losetup"),
                       QStringLiteral("-d"), loopDevice});
    QFile::remove(backingFile);
    return std::nullopt;
  }

  // A freshly-formatted ext4 filesystem's own root directory is owned
  // by root:root with mode 0755 by construction -- chown it to THIS
  // (unprivileged) test process's own uid/gid, mirroring the default
  // ownership QTemporaryDir() always had (the tmpfs-based helper this
  // replaces). This is exactly the shape the FINAL-account-home-
  // position tests need out of the box (current-uid ownership, see
  // directoryDescriptorPassesOwnerAndModePolicy()); the ANCESTOR-
  // position tests separately chown this back to root:root themselves
  // afterward (and restore it before this helper's own destructor
  // tries to unmount/remove it), exactly as they already did for the
  // tmpfs-based helper.
  QProcess chownProc;
  chownProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("chown"),
                   QString::number(::getuid()) + QLatin1Char(':') +
                       QString::number(::getgid()),
                   mountPointDir});
  chownProc.waitForFinished(5000);
  QProcess chmodProc;
  chmodProc.start(QStringLiteral("chmod"),
                  {QStringLiteral("755"), mountPointDir});
  chmodProc.waitForFinished(5000);

  keepAliveOut = std::make_unique<LoopbackExt4Mount>(
      mountPointDir, loopDevice, backingFile, mountPointDir);
  return mountPointDir;
}
} // namespace

namespace {
// Independent cumulative re-review (MEDIUM, repeat finding, "home
// trust... arbitrary same-device bind mount still passes... Test
// arbitrary same-device bind rejection and real expected SteamOS
// home"): every bind-mount "legitimate mount transition" test in this
// file must model a GENUINELY distinct-device source, now that the
// production code itself refuses any transition landing on the SAME
// device as its parent (see
// mountTransitionIsIndependentlyPolicyQualified()'s own comment in
// AssetCache.cpp for the full rationale). The default QTemporaryDir()
// location (this process's own system temp directory) is USELESS for
// that purpose on a typical single-partition CI runner, where $HOME,
// the system temp directory, and everything else this test process
// creates all live on the exact same root filesystem device --
// bind-mounting from there would incidentally exercise the NEW
// same-device rejection instead of the legitimate-transition
// acceptance path these tests actually intend to verify.
//
// Independent cumulative re-review (MEDIUM, "even tests arbitrary
// /dev/shm bind as accepted"): `/dev/shm` (tmpfs) is NO LONGER a
// suitable distinct-device source at all -- production code trusts
// neither tmpfs as a filesystem type, nor a bind-mounted subdirectory
// (mountinfo root != "/") of anything, genuine partition or otherwise
// (see createLoopbackExt4BindSourceDirectory()'s own comment just
// above, which every "legitimate transition" positive control now uses
// instead). This function is kept ONLY for the dedicated negative test
// proving a tmpfs source is correctly REJECTED even when otherwise
// fully authenticated.
//
// Returns std::nullopt (never QSKIPs itself -- callers must do so,
// exactly like every other privilege-dependent step in these tests)
// when `/dev/shm` is unavailable or not writable in this environment.
// `keepAliveOut` receives ownership of the underlying QTemporaryDir so
// its directory (and everything bind-mounted from it) is not removed
// out from under the caller until the caller itself goes out of
// scope.
std::optional<QString> createDeviceDistinctBindSourceDirectory(
    std::unique_ptr<QTemporaryDir> &keepAliveOut) {
  const QString shmRoot = QStringLiteral("/dev/shm");
  if (!QFileInfo::exists(shmRoot) || !QFileInfo(shmRoot).isWritable()) {
    return std::nullopt;
  }
  auto dir = std::make_unique<QTemporaryDir>(
      shmRoot + QStringLiteral("/arkham-asset-cache-test-XXXXXX"));
  if (!dir->isValid()) {
    return std::nullopt;
  }
  const QString path = dir->path();
  keepAliveOut = std::move(dir);
  return path;
}
} // namespace

void AssetCacheTests::
    unauthenticatedHomeMountTransitionOntoADifferentMountIsRejected() {
  // Cumulative review (PR #18, MEDIUM, "arbitrary mount exactly on
  // /home/deck is accepted without independent identity"): before this
  // fix, resolveHomeDirectoryNoFollow() permitted a mount transition
  // landing on home's own final component UNCONDITIONALLY -- an
  // attacker (or a hostile/misconfigured mount namespace) arranging a
  // foreign mount to sit at exactly whatever path $HOME names would
  // have been silently trusted. This test forces the account-database
  // override to report NO match for the fake $HOME below (simulating
  // "this is not really the current account's registered home"), then
  // bind-mounts a completely separate source directory exactly onto
  // the fake home's own final path component, and proves the resolver
  // now refuses it -- exactly the strict same-mount policy an
  // outside-home configured path already gets.
#if !defined(__linux__)
  QSKIP("bind mounts are a Linux-specific concept; not applicable on this "
        "platform");
#else
  const QString fakeHomeParent =
      m_tempDirPath + QStringLiteral("/unauth-mount-home-parent");
  QVERIFY(QDir().mkpath(fakeHomeParent));
  const QString fakeHome = fakeHomeParent + QStringLiteral("/actual-home");
  QVERIFY(QDir().mkpath(fakeHome));

  QTemporaryDir bindSourceDir;
  QVERIFY(bindSourceDir.isValid());
  const QString bindSource = bindSourceDir.path();
  {
    QFile sentinel(bindSource + QStringLiteral("/sentinel.bin"));
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    sentinel.write(QByteArrayLiteral("must-not-be-trusted"));
  }

  QProcess mountProc;
  mountProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("mount"),
                   QStringLiteral("--bind"), bindSource, fakeHome});
  const bool mounted =
      mountProc.waitForFinished(5000) && mountProc.exitCode() == 0;
  if (!mounted) {
    QSKIP("passwordless bind-mount privilege unavailable in this "
          "environment; see the finding's own fail-closed allowance");
  }
  struct UnmountGuard {
    QString mountPoint;
    ~UnmountGuard() {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPoint});
    }
  } unmountGuard{fakeHome};

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));
  // Empty override value: the account database is forced to agree with
  // NO path at all, i.e. this fake $HOME can never be authenticated.
  AuthoritativeAccountHomeOverrideGuard accountGuard(QString());

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  QVERIFY(!AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));
  QVERIFY(!QFileInfo::exists(configuredUnderFakeHome));
#endif
}

void AssetCacheTests::
    authenticatedHomeMountTransitionOntoALoopbackBackedMountRequiresAuthenticatedBackingIdentity() {
  // Positive control for the test above: the IDENTICAL bind-mount
  // shape, and the account-database override now agrees this exact
  // fake $HOME path IS the current account's own registered home --
  // modelling a real SteamOS-style dedicated "/home/deck" mount.
  //
  // Independent review (MEDIUM, repeat finding, "whole-filesystem mount
  // substitution still accepted... reverse test; full-fs bind/move/
  // loopback rejected unless explicitly configured/authenticated
  // expected home volume"): a loopback ext4 filesystem is, by
  // construction, backed by a loop device -- exactly the kind of
  // trivially attacker-fabricable "device"
  // mountSourceIsTrustedBackingIdentity() now refuses regardless of how
  // perfectly every OTHER check (root==
  // "/", distinct device, mount-id/parent-id correlation, trusted
  // fstype, ownership/mode, account-database authentication) is
  // satisfied. Phase 1 below proves that REAL, unmodified refusal;
  // phase 2 proves the accept path still functions once an independent
  // backing-identity authority (modelled here via the test-only
  // override) actually vouches for this exact volume -- exactly what a
  // real, non-loopback SteamOS partition would need to present.
#if !defined(__linux__)
  QSKIP("bind mounts are a Linux-specific concept; not applicable on this "
        "platform");
#else
  const QString fakeHomeParent =
      m_tempDirPath + QStringLiteral("/auth-mount-home-parent");
  QVERIFY(QDir().mkpath(fakeHomeParent));
  const QString fakeHome = fakeHomeParent + QStringLiteral("/actual-home");
  QVERIFY(QDir().mkpath(fakeHome));

  // Independent cumulative re-review (MEDIUM, "arbitrary same-device
  // bind mount still passes"; MEDIUM, "even tests arbitrary /dev/shm
  // bind as accepted"): the bind source must live on a GENUINELY
  // distinct device from `fakeHomeParent`, AND now be a genuine,
  // disk-backed, whole-filesystem mount (root=="/") -- see
  // createLoopbackExt4BindSourceDirectory()'s own comment -- so this
  // positive control keeps exercising a real, legitimate DIFFERENT-
  // mount transition rather than incidentally tripping either the
  // same-device rejection or the tmpfs/subdirectory-root rejection
  // this fix adds.
  std::unique_ptr<LoopbackExt4Mount> bindSourceDirKeepAlive;
  const std::optional<QString> bindSourceOpt =
      createLoopbackExt4BindSourceDirectory(bindSourceDirKeepAlive);
  if (!bindSourceOpt.has_value()) {
    QSKIP("a loopback ext4 filesystem (mkfs.ext4/losetup/passwordless sudo "
          "mount) is unavailable in this environment; cannot model a "
          "genuinely distinct-device, whole-filesystem mount transition "
          "without it");
  }
  const QString bindSource = *bindSourceOpt;

  QProcess mountProc;
  mountProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("mount"),
                   QStringLiteral("--bind"), bindSource, fakeHome});
  const bool mounted =
      mountProc.waitForFinished(5000) && mountProc.exitCode() == 0;
  if (!mounted) {
    QSKIP("passwordless bind-mount privilege unavailable in this "
          "environment; see the finding's own fail-closed allowance");
  }
  struct UnmountGuard {
    QString mountPoint;
    ~UnmountGuard() {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPoint});
    }
  } unmountGuard{fakeHome};

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));
  // Override value EXACTLY matches the fake $HOME: the account
  // database is forced to agree this is genuinely the current
  // account's own home, authenticating the mount transition.
  AuthoritativeAccountHomeOverrideGuard accountGuard(QDir::cleanPath(fakeHome));
  // Independent cumulative re-review (MEDIUM, "Validate owner/mode for
  // EVERY opened component regardless mount transition"): this test's
  // fake $HOME tree lives beneath this TEST PROCESS'S OWN temp
  // directory, so every ANCESTOR component above the real bind-mounted
  // transition this test actually exercises is genuinely owned by this
  // unprivileged process, not root -- there is no portable,
  // unprivileged way to make those scaffolding ancestors genuinely
  // root-owned. This override isolates the test to what it actually
  // verifies (the mount-transition-specific authentication/identity
  // logic, exercised for real via the genuine bind mount above), not
  // the separately-tested raw ownership/mode policy itself.
  HomeComponentOwnershipModePolicyOverrideGuard ownershipGuard(
      /*passes=*/true);

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  // Phase 1: no backing-identity override active -- the REAL,
  // unmodified check must refuse this loopback-backed mount despite
  // every other check passing.
  QVERIFY(!AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));
  QVERIFY(!QFileInfo::exists(configuredUnderFakeHome));

  // Phase 2: an independent backing-identity authority now vouches for
  // this exact volume (modelling a real, non-loopback SteamOS
  // partition) -- every other check remains genuinely, unmodifiedly
  // exercised, and the transition is now permitted.
  MountSourceBackingIdentityOverrideGuard backingIdentityGuard(
      /*trusted=*/true);
  QVERIFY(AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));
  QVERIFY(QFileInfo(configuredUnderFakeHome).isDir());
#endif
}

void AssetCacheTests::
    authenticatedAncestorMountTransitionOntoALoopbackBackedMountRequiresAuthenticatedBackingIdentity() {
  // Cumulative review (PR #18, MEDIUM, "home mount auth wrong
  // boundary"): unlike the two tests above (which bind-mount home's
  // own FINAL component), this bind-mounts fakeHome's PARENT directory
  // -- "actual-home" itself remains an ORDINARY subdirectory, on the
  // SAME mount as its bind-mounted parent, with no further transition
  // at the leaf at all. This is exactly the shape of a real, ordinary
  // dedicated "/home" partition (as opposed to a SteamOS-style
  // "/home/deck" split): the transition happens at an ANCESTOR of
  // home's final component, not at home's own final component.
  //
  // Independent review (MEDIUM, repeat finding, "whole-filesystem mount
  // substitution still accepted... reverse test"): this bind source is
  // a loopback ext4 filesystem (necessarily loop-backed), which
  // mountSourceIsTrustedBackingIdentity() now refuses regardless of
  // every other check (including the chowned root ownership this
  // ancestor position requires) passing. Phase 1 below proves that
  // REAL, unmodified refusal; phase 2 proves the accept path still
  // functions once an independent backing-identity authority
  // authenticates this exact volume, exactly modelling a real,
  // non-loopback dedicated "/home" partition.
#if !defined(__linux__)
  QSKIP("bind mounts are a Linux-specific concept; not applicable on this "
        "platform");
#else
  const QString fakeHomeGrandparent =
      m_tempDirPath + QStringLiteral("/auth-ancestor-mount-grandparent");
  QVERIFY(QDir().mkpath(fakeHomeGrandparent));
  const QString fakeHomeParent =
      fakeHomeGrandparent + QStringLiteral("/mounted-ancestor");
  QVERIFY(QDir().mkpath(fakeHomeParent));

  // Independent cumulative re-review (MEDIUM, "arbitrary same-device
  // bind mount still passes"; MEDIUM, "even tests arbitrary /dev/shm
  // bind as accepted"): see createLoopbackExt4BindSourceDirectory()'s
  // own comment -- this positive control's bind source must live on a
  // genuinely distinct device from `fakeHomeGrandparent`, AND now be a
  // genuine, disk-backed, whole-filesystem mount (root=="/").
  std::unique_ptr<LoopbackExt4Mount> bindSourceDirKeepAlive;
  const std::optional<QString> bindSourceOpt =
      createLoopbackExt4BindSourceDirectory(bindSourceDirKeepAlive);
  if (!bindSourceOpt.has_value()) {
    QSKIP("a loopback ext4 filesystem (mkfs.ext4/losetup/passwordless sudo "
          "mount) is unavailable in this environment; cannot model a "
          "genuinely distinct-device, whole-filesystem mount transition "
          "without it");
  }
  const QString bindSource = *bindSourceOpt;
  QVERIFY(QDir().mkpath(bindSource + QStringLiteral("/actual-home")));

  // Cumulative review (independent re-review round-6, MEDIUM,
  // "position-sensitive ownership"): this transition lands at an
  // ANCESTOR position (fakeHome's own PARENT), which now requires
  // root ownership -- see
  // directoryDescriptorPassesAncestorOwnerAndModePolicy()'s own
  // comment. QTemporaryDir's own directory is created with a
  // restrictive 0700 mode; relaxed to 0755 BEFORE the chown so this
  // (non-root) test process retains traversal access to the mounted
  // view afterward, exactly like multipleIndependentlyQualified...'s
  // own identical fix below.
  QProcess bindSourceChmodProc;
  bindSourceChmodProc.start(QStringLiteral("chmod"),
                            {QStringLiteral("755"), bindSource});
  QVERIFY(bindSourceChmodProc.waitForFinished(5000));
  QCOMPARE(bindSourceChmodProc.exitCode(), 0);

  QProcess bindSourceChownProc;
  bindSourceChownProc.start(QStringLiteral("sudo"),
                            {QStringLiteral("-n"), QStringLiteral("chown"),
                             QStringLiteral("root:root"), bindSource});
  const bool bindSourceChowned = bindSourceChownProc.waitForFinished(5000) &&
                                 bindSourceChownProc.exitCode() == 0;
  if (!bindSourceChowned) {
    QSKIP("passwordless chown privilege unavailable in this environment; "
          "see the finding's own fail-closed allowance");
  }
  // Restores ownership back to this test process's own uid before the
  // QTemporaryDir destructor tries to remove it -- see
  // multipleIndependentlyQualified...'s own identical guard for the
  // full sticky-bit-cleanup rationale.
  struct BindSourceChownBackGuard {
    QString path;
    ~BindSourceChownBackGuard() {
      QProcess::execute(QStringLiteral("sudo"),
                        {QStringLiteral("-n"), QStringLiteral("chown"),
                         QString::number(::getuid()) + QLatin1Char(':') +
                             QString::number(::getgid()),
                         path});
    }
  } bindSourceChownBackGuard{bindSource};

  QProcess mountProc;
  mountProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("mount"),
                   QStringLiteral("--bind"), bindSource, fakeHomeParent});
  const bool mounted =
      mountProc.waitForFinished(5000) && mountProc.exitCode() == 0;
  if (!mounted) {
    QSKIP("passwordless bind-mount privilege unavailable in this "
          "environment; see the finding's own fail-closed allowance");
  }
  struct UnmountGuard {
    QString mountPoint;
    ~UnmountGuard() {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPoint});
    }
  } unmountGuard{fakeHomeParent};

  // fakeHome ("<fakeHomeParent>/actual-home") itself is an ORDINARY
  // directory living entirely ON the bind-mounted "mounted-ancestor"
  // mount -- the transition is at the ANCESTOR, never at home's own
  // final component.
  const QString fakeHome = fakeHomeParent + QStringLiteral("/actual-home");
  QVERIFY(QFileInfo::exists(fakeHome));

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));
  AuthoritativeAccountHomeOverrideGuard accountGuard(QDir::cleanPath(fakeHome));
  // Independent cumulative re-review (MEDIUM, "Validate owner/mode for
  // EVERY opened component regardless mount transition"): the mount
  // transition's OWN destination (fakeHomeParent, chowned to root:root
  // above) already genuinely satisfies the real ancestor ownership
  // policy without any override -- but the SCAFFOLDING levels ABOVE
  // it (this test process's own temp-directory hierarchy) are
  // genuinely owned by this unprivileged process, not root, and there
  // is no portable way to make them so. This override isolates the
  // test to what it actually verifies (the real, chowned mount
  // transition), not the unrelated scaffolding ancestors above it.
  HomeComponentOwnershipModePolicyOverrideGuard ownershipGuard(
      /*passes=*/true);

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  // Phase 1: no backing-identity override active -- the REAL,
  // unmodified check must refuse this loopback-backed ancestor mount
  // despite every other check (including the chowned root ownership)
  // passing.
  QVERIFY(!AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));
  QVERIFY(!QFileInfo::exists(configuredUnderFakeHome));

  // Phase 2: an independent backing-identity authority now vouches for
  // this exact volume -- every other check remains genuinely,
  // unmodifiedly exercised, and the transition is now permitted,
  // exactly modelling a real, non-loopback dedicated "/home" partition.
  MountSourceBackingIdentityOverrideGuard backingIdentityGuard(
      /*trusted=*/true);
  QVERIFY(AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));
  QVERIFY(QFileInfo(configuredUnderFakeHome).isDir());
#endif
}

void AssetCacheTests::
    unauthenticatedAncestorMountTransitionModellingADedicatedHomePartitionIsRejected() {
  // Negative control for the test above: the identical ancestor
  // bind-mount shape, but the account database is forced to disagree
  // this fake $HOME is the current account's own home -- the
  // transition must be refused, exactly like every other
  // unauthenticated mount transition regardless of where in home's
  // path it organically falls.
#if !defined(__linux__)
  QSKIP("bind mounts are a Linux-specific concept; not applicable on this "
        "platform");
#else
  const QString fakeHomeGrandparent =
      m_tempDirPath + QStringLiteral("/unauth-ancestor-mount-grandparent");
  QVERIFY(QDir().mkpath(fakeHomeGrandparent));
  const QString fakeHomeParent =
      fakeHomeGrandparent + QStringLiteral("/mounted-ancestor");
  QVERIFY(QDir().mkpath(fakeHomeParent));

  QTemporaryDir bindSourceDir;
  QVERIFY(bindSourceDir.isValid());
  const QString bindSource = bindSourceDir.path();
  QVERIFY(QDir().mkpath(bindSource + QStringLiteral("/actual-home")));
  {
    QFile sentinel(bindSource + QStringLiteral("/actual-home/sentinel.bin"));
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    sentinel.write(QByteArrayLiteral("must-not-be-trusted"));
  }

  QProcess mountProc;
  mountProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("mount"),
                   QStringLiteral("--bind"), bindSource, fakeHomeParent});
  const bool mounted =
      mountProc.waitForFinished(5000) && mountProc.exitCode() == 0;
  if (!mounted) {
    QSKIP("passwordless bind-mount privilege unavailable in this "
          "environment; see the finding's own fail-closed allowance");
  }
  struct UnmountGuard {
    QString mountPoint;
    ~UnmountGuard() {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPoint});
    }
  } unmountGuard{fakeHomeParent};

  const QString fakeHome = fakeHomeParent + QStringLiteral("/actual-home");
  QVERIFY(QFileInfo::exists(fakeHome));

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));
  // Empty override: the account database agrees with NO path at all.
  AuthoritativeAccountHomeOverrideGuard accountGuard(QString());

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  QVERIFY(!AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));
  QVERIFY(!QFileInfo::exists(configuredUnderFakeHome));
#endif
}

void AssetCacheTests::
    arbitrarySameDeviceBindMountOntoAncestorPositionIsRejectedEvenWhenFullyAuthenticatedAndOwnershipQualifies() {
  // Independent cumulative re-review (MEDIUM, repeat finding across
  // several rounds, "home trust... arbitrary same-device bind mount
  // still passes because parent/root/mountpoint/source/options/
  // super-options ignored... Test arbitrary bind on same ext4/btrfs
  // rejected and genuine SteamOS home accepted"): unlike every OTHER
  // "permitted" test in this file (which now deliberately bind-mounts
  // from a genuinely distinct-device, whole-filesystem source -- see
  // createLoopbackExt4BindSourceDirectory()'s own comment), this
  // test's bind source is a perfectly ordinary SIBLING directory of
  // `fakeHomeGrandparent` itself, i.e. on the EXACT SAME underlying
  // device/filesystem. It is chowned to root:root and chmod'd 0755
  // (satisfying the ancestor-position ownership/mode policy exactly
  // like the genuinely-distinct-device positive control does), and
  // $HOME is fully authenticated against the account database -- every
  // OTHER check this project performs is satisfied. Only the NEW
  // same-device rejection this fix adds can refuse this: a bind mount
  // whose destination reports the identical device as its parent can
  // only ever be a same-filesystem redirect of an arbitrary directory,
  // never a genuine, independently-provisioned dedicated partition --
  // exactly the residual gap the reviewer's finding describes.
#if !defined(__linux__)
  QSKIP("bind mounts are a Linux-specific concept; not applicable on this "
        "platform");
#else
  const QString fakeHomeGrandparent =
      m_tempDirPath + QStringLiteral("/same-device-bind-grandparent");
  QVERIFY(QDir().mkpath(fakeHomeGrandparent));
  const QString fakeHomeParent =
      fakeHomeGrandparent + QStringLiteral("/mounted-ancestor");
  QVERIFY(QDir().mkpath(fakeHomeParent));

  // Deliberately a SIBLING of `fakeHomeGrandparent`, both directly
  // under `m_tempDirPath` -- guaranteed to be on the SAME device,
  // never bind-mounted from anywhere else, unlike every positive
  // control test's own loopback-ext4-backed source.
  const QString bindSource =
      m_tempDirPath + QStringLiteral("/same-device-bind-source");
  QVERIFY(QDir().mkpath(bindSource + QStringLiteral("/actual-home")));

  QProcess bindSourceChmodProc;
  bindSourceChmodProc.start(QStringLiteral("chmod"),
                            {QStringLiteral("755"), bindSource});
  QVERIFY(bindSourceChmodProc.waitForFinished(5000));
  QCOMPARE(bindSourceChmodProc.exitCode(), 0);

  QProcess bindSourceChownProc;
  bindSourceChownProc.start(QStringLiteral("sudo"),
                            {QStringLiteral("-n"), QStringLiteral("chown"),
                             QStringLiteral("root:root"), bindSource});
  const bool bindSourceChowned = bindSourceChownProc.waitForFinished(5000) &&
                                 bindSourceChownProc.exitCode() == 0;
  if (!bindSourceChowned) {
    QSKIP("passwordless chown privilege unavailable in this environment; "
          "see the finding's own fail-closed allowance");
  }
  struct BindSourceChownBackGuard {
    QString path;
    ~BindSourceChownBackGuard() {
      QProcess::execute(QStringLiteral("sudo"),
                        {QStringLiteral("-n"), QStringLiteral("chown"),
                         QString::number(::getuid()) + QLatin1Char(':') +
                             QString::number(::getgid()),
                         path});
    }
  } bindSourceChownBackGuard{bindSource};

  QProcess mountProc;
  mountProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("mount"),
                   QStringLiteral("--bind"), bindSource, fakeHomeParent});
  const bool mounted =
      mountProc.waitForFinished(5000) && mountProc.exitCode() == 0;
  if (!mounted) {
    QSKIP("passwordless bind-mount privilege unavailable in this "
          "environment; see the finding's own fail-closed allowance");
  }
  struct UnmountGuard {
    QString mountPoint;
    ~UnmountGuard() {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPoint});
    }
  } unmountGuard{fakeHomeParent};

  const QString fakeHome = fakeHomeParent + QStringLiteral("/actual-home");
  QVERIFY(QFileInfo::exists(fakeHome));

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));
  // Fully authenticated -- exactly like the genuinely-distinct-device
  // positive control -- so the rejection below can only come from the
  // NEW same-device check, never from an unauthenticated $HOME.
  AuthoritativeAccountHomeOverrideGuard accountGuard(QDir::cleanPath(fakeHome));
  HomeComponentOwnershipModePolicyOverrideGuard ownershipGuard(
      /*passes=*/true);

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  QVERIFY2(!AssetCache::resolveTrustedDirectoryNoFollowForTesting(
               configuredUnderFakeHome, /*allowCreateMissingComponents=*/true),
           "a bind mount reporting the SAME device as its parent must be "
           "refused even when fully authenticated and ownership-qualified "
           "-- it can only be a same-device redirect of an arbitrary "
           "directory, never a genuine dedicated partition");
  QVERIFY(!QFileInfo::exists(configuredUnderFakeHome));
#endif
}

void AssetCacheTests::
    authenticatedFreshTopLevelTmpfsHomeMountTransitionIsRejectedDespiteRootBeingSlash() {
  // Independent cumulative re-review (MEDIUM, repeat finding across
  // several rounds, "home trust... even tests arbitrary /dev/shm bind
  // as accepted"): a genuine, freshly-created, TOP-LEVEL tmpfs mount
  // (never a bind-mount of a subdirectory -- `mount -t tmpfs`, so
  // mountinfo's own "root" field genuinely reads "/", isolating this
  // test to the fstype exclusion alone) landing exactly on home's own
  // final path component, fully authenticated against the account
  // database and correctly owned/moded for the final-home position,
  // must still be refused: tmpfs is no longer in
  // trustedLocalMountFilesystemTypes() at all (see that function's own
  // comment for why it was removed) -- this is the CONCRETE,
  // reproducible regression this finding's every prior round pointed
  // at, now proven to fail through the real production decision path,
  // never merely asserted.
#if !defined(__linux__)
  QSKIP("mounts are a Linux-specific concept; not applicable on this "
        "platform");
#else
  const QString fakeHomeParent =
      m_tempDirPath + QStringLiteral("/tmpfs-home-parent");
  QVERIFY(QDir().mkpath(fakeHomeParent));
  const QString fakeHome = fakeHomeParent + QStringLiteral("/actual-home");
  QVERIFY(QDir().mkpath(fakeHome));

  QProcess mountProc;
  mountProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("mount"),
                   QStringLiteral("-t"), QStringLiteral("tmpfs"),
                   QStringLiteral("-o"), QStringLiteral("size=16m,mode=0755"),
                   QStringLiteral("tmpfs"), fakeHome});
  const bool mounted =
      mountProc.waitForFinished(5000) && mountProc.exitCode() == 0;
  if (!mounted) {
    QSKIP("passwordless tmpfs-mount privilege unavailable in this "
          "environment; see the finding's own fail-closed allowance");
  }
  struct UnmountGuard {
    QString mountPoint;
    ~UnmountGuard() {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPoint});
    }
  } unmountGuard{fakeHome};

  // The fresh tmpfs's own root is chowned to this test's own uid so
  // the FINAL-account-home ownership policy (never overridable, see
  // componentPassesOwnershipModePolicy()'s own comment) genuinely
  // passes -- isolating this test to the fstype exclusion alone,
  // exactly like the analogous same-device test isolates itself to
  // the device check alone.
  QProcess chownProc;
  chownProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("chown"),
                   QString::number(::getuid()) + QLatin1Char(':') +
                       QString::number(::getgid()),
                   fakeHome});
  QVERIFY(chownProc.waitForFinished(5000));
  QCOMPARE(chownProc.exitCode(), 0);

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));
  // Fully authenticated -- exactly like a genuine positive control --
  // so the rejection below can only come from the tmpfs fstype
  // exclusion, never from an unauthenticated $HOME.
  AuthoritativeAccountHomeOverrideGuard accountGuard(QDir::cleanPath(fakeHome));
  HomeComponentOwnershipModePolicyOverrideGuard ownershipGuard(
      /*passes=*/true);

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  QVERIFY2(!AssetCache::resolveTrustedDirectoryNoFollowForTesting(
               configuredUnderFakeHome, /*allowCreateMissingComponents=*/true),
           "a genuine, top-level (root==\"/\"), fully-authenticated, "
           "correctly-owned tmpfs mount must STILL be refused -- tmpfs is "
           "no longer a trusted local filesystem type at all");
  QVERIFY(!QFileInfo::exists(configuredUnderFakeHome));
#endif
}

void AssetCacheTests::
    authenticatedBindMountOfASubdirectoryOfATrustedFilesystemIsRejectedDespiteTrustedFstype() {
  // Independent cumulative re-review (MEDIUM, repeat finding, "home
  // trust... still discards mount root... authenticate exact
  // descriptor mount id against position-specific expected... root
  // ..."): a bind mount of an ORDINARY SUBDIRECTORY of a genuinely
  // trusted, distinct-device ext4 loopback filesystem (never that
  // filesystem's own root -- so this can only be explained by the
  // mount-root check, never the fstype/device checks, which this
  // scenario otherwise satisfies exactly like a genuine positive
  // control) landing on home's own final component, fully
  // authenticated and correctly owned/moded, must still be refused:
  // mountinfo's own "root" field for such a mount is never "/",
  // proving it is merely an arbitrary directory bind mount of SOME
  // already-existing filesystem -- regardless of how trustworthy that
  // filesystem's own type or device otherwise is -- never a genuine
  // dedicated whole-partition mount. (This fixture is ALSO loop-backed
  // and would independently fail mountSourceIsTrustedBackingIdentity()
  // -- but the root-field check alone is sufficient and applies
  // FIRST/independently here, so this test's assertion remains valid
  // and specific to that check regardless of backing-identity policy.)
#if !defined(__linux__)
  QSKIP("mounts are a Linux-specific concept; not applicable on this "
        "platform");
#else
  std::unique_ptr<LoopbackExt4Mount> trustedFsKeepAlive;
  const std::optional<QString> trustedFsOpt =
      createLoopbackExt4BindSourceDirectory(trustedFsKeepAlive);
  if (!trustedFsOpt.has_value()) {
    QSKIP("a loopback ext4 filesystem (mkfs.ext4/losetup/passwordless sudo "
          "mount) is unavailable in this environment; cannot model a "
          "genuinely trusted-fstype source without it");
  }
  const QString trustedFsRoot = *trustedFsOpt;
  // The bind SOURCE is a SUBDIRECTORY of the trusted ext4 filesystem,
  // never its own root -- this is the entire point of this test.
  const QString bindSource =
      trustedFsRoot + QStringLiteral("/an-ordinary-subdirectory");
  QVERIFY(QDir().mkpath(bindSource));

  const QString fakeHomeParent =
      m_tempDirPath + QStringLiteral("/subdir-bind-home-parent");
  QVERIFY(QDir().mkpath(fakeHomeParent));
  const QString fakeHome = fakeHomeParent + QStringLiteral("/actual-home");
  QVERIFY(QDir().mkpath(fakeHome));

  QProcess mountProc;
  mountProc.start(QStringLiteral("sudo"),
                  {QStringLiteral("-n"), QStringLiteral("mount"),
                   QStringLiteral("--bind"), bindSource, fakeHome});
  const bool mounted =
      mountProc.waitForFinished(5000) && mountProc.exitCode() == 0;
  if (!mounted) {
    QSKIP("passwordless bind-mount privilege unavailable in this "
          "environment; see the finding's own fail-closed allowance");
  }
  struct UnmountGuard {
    QString mountPoint;
    ~UnmountGuard() {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPoint});
    }
  } unmountGuard{fakeHome};

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));
  AuthoritativeAccountHomeOverrideGuard accountGuard(QDir::cleanPath(fakeHome));
  HomeComponentOwnershipModePolicyOverrideGuard ownershipGuard(
      /*passes=*/true);

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  QVERIFY2(!AssetCache::resolveTrustedDirectoryNoFollowForTesting(
               configuredUnderFakeHome, /*allowCreateMissingComponents=*/true),
           "a bind mount of an ordinary SUBDIRECTORY of an otherwise "
           "trusted, distinct-device filesystem must STILL be refused -- "
           "mountinfo's own root field for it is never \"/\"");
  QVERIFY(!QFileInfo::exists(configuredUnderFakeHome));
#endif
}

void AssetCacheTests::
    unauthenticatedHomeWithDegradedMountIdentificationFailsClosedEvenUnmounted() {
  // An unauthenticated $HOME (account database forced to disagree)
  // whose mount-identification itself is degraded (forced via
  // setMountIdentificationDegradedForTesting(), no privilege required)
  // must fail closed exactly like an outside-home configured path
  // already does when mount identity can't be proven -- even against a
  // perfectly ordinary, unprivileged, entirely UNMOUNTED fake home, so
  // this runs deterministically on every CI runner without needing
  // passwordless sudo.
#if !defined(__linux__)
  QSKIP("mount-identity hardening is a Linux-specific concept; not "
        "applicable on this platform");
#else
  const QString fakeHome =
      m_tempDirPath + QStringLiteral("/degraded-mount-home-parent/actual-home");
  QVERIFY(QDir().mkpath(fakeHome));

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));
  AuthoritativeAccountHomeOverrideGuard accountGuard(QString());
  MountIdentificationDegradationGuard mountGuard(
      /*forceOpenat2Unavailable=*/true, /*forceMountIdUnavailable=*/true);

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  QVERIFY(!AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));
  QVERIFY(!QFileInfo::exists(configuredUnderFakeHome));
#endif
}

void AssetCacheTests::
    mountTransitionPolicyRejectsGroupWritableDestinationEvenWhenFilesystemTypeQualifies() {
#if !defined(Q_OS_UNIX)
  QSKIP("this policy is POSIX-specific; not applicable on this platform");
#else
  const QString dirPath =
      m_tempDirPath + QStringLiteral("/group-writable-mount-destination");
  QVERIFY(QDir().mkpath(dirPath));
  QVERIFY(::chmod(QFile::encodeName(dirPath).constData(),
                  S_IRWXU | S_IRGRP | S_IXGRP | S_IWGRP) == 0);

  // The filesystem-type half of the decision is forced to "qualified"
  // -- proving the refusal below comes ENTIRELY from the ownership/mode
  // check, never masked by an incidental filesystem-type failure.
  MountTransitionPolicyQualificationOverrideGuard fsTypeGuard(
      /*qualified=*/true);

  const std::optional<bool> verdict =
      AssetCache::mountTransitionIsIndependentlyPolicyQualifiedForTesting(
          dirPath);
  QVERIFY(verdict.has_value());
  QVERIFY(!*verdict);
#endif
}

void AssetCacheTests::
    mountTransitionPolicyRejectsWorldWritableDestinationEvenWhenFilesystemTypeQualifies() {
#if !defined(Q_OS_UNIX)
  QSKIP("this policy is POSIX-specific; not applicable on this platform");
#else
  const QString dirPath =
      m_tempDirPath + QStringLiteral("/world-writable-mount-destination");
  QVERIFY(QDir().mkpath(dirPath));
  QVERIFY(::chmod(QFile::encodeName(dirPath).constData(),
                  S_IRWXU | S_IROTH | S_IXOTH | S_IWOTH) == 0);

  MountTransitionPolicyQualificationOverrideGuard fsTypeGuard(
      /*qualified=*/true);

  const std::optional<bool> verdict =
      AssetCache::mountTransitionIsIndependentlyPolicyQualifiedForTesting(
          dirPath);
  QVERIFY(verdict.has_value());
  QVERIFY(!*verdict);
#endif
}

void AssetCacheTests::
    mountTransitionPolicyAcceptsOwnedNonWritableDestinationWhenFilesystemTypeQualifies() {
#if !defined(Q_OS_UNIX)
  QSKIP("this policy is POSIX-specific; not applicable on this platform");
#else
  const QString dirPath =
      m_tempDirPath + QStringLiteral("/ordinary-mount-destination");
  QVERIFY(QDir().mkpath(dirPath));
  // QDir::mkpath() already creates directories with a default mode
  // that is neither group- nor world-writable and owned by this very
  // process's own real uid -- exactly the ordinary, positive-control
  // shape this test needs.
  QVERIFY(::chmod(QFile::encodeName(dirPath).constData(), S_IRWXU) == 0);

  MountTransitionPolicyQualificationOverrideGuard fsTypeGuard(
      /*qualified=*/true);

  const std::optional<bool> verdict =
      AssetCache::mountTransitionIsIndependentlyPolicyQualifiedForTesting(
          dirPath);
  QVERIFY(verdict.has_value());
  QVERIFY(*verdict);
#endif
}

void AssetCacheTests::
    mountTransitionPolicyRejectsDestinationWhenFilesystemTypeOverrideReportsUnqualified() {
#if !defined(Q_OS_UNIX)
  QSKIP("this policy is POSIX-specific; not applicable on this platform");
#else
  const QString dirPath =
      m_tempDirPath +
      QStringLiteral("/perfect-owner-mode-but-untrusted-fstype");
  QVERIFY(QDir().mkpath(dirPath));
  QVERIFY(::chmod(QFile::encodeName(dirPath).constData(), S_IRWXU) == 0);

  // Ownership/mode are PERFECT here -- only the independent
  // filesystem-type evidence is forced to report "not qualified"
  // (modelling a real kernel-recorded network/FUSE-backed filesystem
  // type), and the overall decision must still be refused.
  MountTransitionPolicyQualificationOverrideGuard fsTypeGuard(
      /*qualified=*/false);

  const std::optional<bool> verdict =
      AssetCache::mountTransitionIsIndependentlyPolicyQualifiedForTesting(
          dirPath);
  QVERIFY(verdict.has_value());
  QVERIFY(!*verdict);
#endif
}

void AssetCacheTests::
    mountTransitionPolicyRejectsCurrentUidOwnedDestinationForAncestorPositionEvenWhenFilesystemTypeQualifies() {
#if !defined(Q_OS_UNIX)
  QSKIP("this policy is POSIX-specific; not applicable on this platform");
#else
  const QString dirPath =
      m_tempDirPath + QStringLiteral("/current-uid-owned-ancestor-position");
  QVERIFY(QDir().mkpath(dirPath));
  // QDir::mkpath() already creates directories owned by this very
  // process's own real uid and neither group- nor world-writable --
  // the SAME fixture shape
  // mountTransitionPolicyAcceptsOwnedNonWritableDestinationWhenFilesystemTypeQualifies()
  // above proves passes for a FINAL transition.
  QVERIFY(::chmod(QFile::encodeName(dirPath).constData(), S_IRWXU) == 0);

  MountTransitionPolicyQualificationOverrideGuard fsTypeGuard(
      /*qualified=*/true);

  const std::optional<bool> verdict =
      AssetCache::mountTransitionIsIndependentlyPolicyQualifiedForTesting(
          dirPath, /*isFinalAccountHomeTransition=*/false);
  QVERIFY(verdict.has_value());
  QVERIFY2(!*verdict,
           "a current-uid-owned destination must never qualify as an "
           "ANCESTOR-position mount transition -- only a root-owned one "
           "may, exactly like a real distribution's own dedicated /home "
           "partition");
#endif
}

void AssetCacheTests::
    mountTransitionPolicyAcceptsRootOwnedDestinationForAncestorPositionWhenFilesystemTypeQualifies() {
#if !defined(Q_OS_UNIX)
  QSKIP("this policy is POSIX-specific; not applicable on this platform");
#else
  // "/" itself: present, root-owned, and neither group- nor
  // world-writable (mode 0755 or stricter) on every POSIX system this
  // project targets -- needs no privileged test setup at all to model
  // a genuine root-provisioned dedicated mount destination.
  struct stat rootSt {};
  QVERIFY(::stat("/", &rootSt) == 0);
  if (rootSt.st_uid != 0 || (rootSt.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    QSKIP("this environment's own \"/\" is not root-owned/non-writable; "
          "cannot exercise the ancestor-position positive control without "
          "a genuinely root-owned fixture");
  }

  MountTransitionPolicyQualificationOverrideGuard fsTypeGuard(
      /*qualified=*/true);

  const std::optional<bool> verdict =
      AssetCache::mountTransitionIsIndependentlyPolicyQualifiedForTesting(
          QStringLiteral("/"), /*isFinalAccountHomeTransition=*/false);
  QVERIFY(verdict.has_value());
  QVERIFY2(*verdict,
           "a genuinely root-owned, non-writable destination must qualify "
           "as an ANCESTOR-position mount transition -- this is the exact "
           "legitimate topology (a real distribution's own dedicated /home "
           "partition) the fix must continue to permit");
#endif
}

void AssetCacheTests::
    multipleLoopbackBackedMountTransitionsInTheSameHomeWalkRequireAuthenticatedBackingIdentity() {
  // Cumulative review (independent re-review, MEDIUM, "only one
  // transition allowed"; independent re-review round-6, MEDIUM,
  // "position-sensitive ownership"): bind-mounts BOTH an ancestor of
  // home's final component AND home's own final component onto two
  // SEPARATE real mounts, modelling a genuine SteamOS-style topology
  // with more than one legitimate transition in the same walk. The
  // ANCESTOR-position destination is chowned to root:root (mirroring a
  // real distribution's own root-provisioned dedicated "/home"
  // partition -- see directoryDescriptorPassesAncestorOwnerAndModePolicy()'s
  // comment for why this is now REQUIRED, not merely permitted, for a
  // non-final transition), while the FINAL, account-home-position
  // destination remains ordinary, unprivileged-created and owned by the
  // current uid (exactly as a real user's own home directory must be);
  // the account database is forced to authenticate the final fake
  // $HOME path, and the real /proc/self/mountinfo lookup is exercised
  // unmodified (no filesystem-type override) -- proving genuinely BOTH
  // transitions are granted through the real, unmodified, position-
  // sensitive production decision path end-to-end.
  //
  // Independent review (MEDIUM, repeat finding, "whole-filesystem mount
  // substitution still accepted... reverse test"): BOTH bind sources
  // are loopback ext4 filesystems (necessarily loop-backed), which
  // mountSourceIsTrustedBackingIdentity() now refuses independently at
  // EACH transition, regardless of every other check passing. Phase 1
  // below proves that REAL, unmodified refusal for the walk as a
  // whole; phase 2 proves BOTH transitions still grant once an
  // independent backing-identity authority authenticates both volumes.
#if !defined(__linux__)
  QSKIP("bind mounts are a Linux-specific concept; not applicable on this "
        "platform");
#else
  const QString grandparent =
      m_tempDirPath + QStringLiteral("/multi-transition-grandparent");
  QVERIFY(QDir().mkpath(grandparent));
  const QString mountedAncestor =
      grandparent + QStringLiteral("/mounted-ancestor");
  QVERIFY(QDir().mkpath(mountedAncestor));

  // Independent cumulative re-review (MEDIUM, "arbitrary same-device
  // bind mount still passes"; MEDIUM, "even tests arbitrary /dev/shm
  // bind as accepted"): see createLoopbackExt4BindSourceDirectory()'s
  // own comment -- the ANCESTOR-position bind source (the first of
  // the two transitions this test exercises) must live on a genuinely
  // distinct device from `grandparent`, AND now be a genuine,
  // disk-backed, whole-filesystem mount (root=="/"). The SECOND
  // transition (`homeBindSource` below) needs the identical treatment
  // for the exact same reason -- an ordinary QTemporaryDir() bind
  // source is no longer sufficient at either position, since it is
  // never itself the root of its own filesystem.
  std::unique_ptr<LoopbackExt4Mount> ancestorBindSourceDirKeepAlive;
  const std::optional<QString> ancestorBindSourceOpt =
      createLoopbackExt4BindSourceDirectory(ancestorBindSourceDirKeepAlive);
  if (!ancestorBindSourceOpt.has_value()) {
    QSKIP("a loopback ext4 filesystem (mkfs.ext4/losetup/passwordless sudo "
          "mount) is unavailable in this environment; cannot model a "
          "genuinely distinct-device, whole-filesystem mount transition "
          "without it");
  }
  const QString ancestorBindSource = *ancestorBindSourceOpt;
  QVERIFY(QDir().mkpath(ancestorBindSource + QStringLiteral("/actual-home")));

  // Cumulative review (independent re-review round-6, MEDIUM,
  // "position-sensitive ownership"): the ANCESTOR-position bind
  // source itself (never its "actual-home" subdirectory, which the
  // SECOND, final-position bind mount below shadows entirely) must be
  // root-owned to model a genuine dedicated-partition topology -- see
  // directoryDescriptorPassesAncestorOwnerAndModePolicy()'s own
  // comment. Requires the same passwordless sudo privilege the mount/
  // umount calls below already require; skips (never fails) when
  // unavailable, exactly like every other privileged step in this
  // test.
  // QTemporaryDir itself creates its directory with a restrictive
  // 0700 mode (owner-only); once bind-mounted, the DESTINATION's
  // effective traversal permissions become whatever the SOURCE's own
  // mode is -- so without also relaxing it to a real dedicated
  // partition's ordinary 0755, this test's own (non-root) process
  // would lose all access to the mounted view the instant ownership
  // moves to root. 0755 (root-owned, non-group/world-writable) is
  // exactly the shape directoryDescriptorPassesAncestorOwnerAndModePolicy()
  // requires and a genuine distro-provisioned "/home" mountpoint
  // ordinarily has.
  QProcess ancestorChmodProc;
  ancestorChmodProc.start(QStringLiteral("chmod"),
                          {QStringLiteral("755"), ancestorBindSource});
  QVERIFY(ancestorChmodProc.waitForFinished(5000));
  QCOMPARE(ancestorChmodProc.exitCode(), 0);

  QProcess ancestorChownProc;
  ancestorChownProc.start(QStringLiteral("sudo"),
                          {QStringLiteral("-n"), QStringLiteral("chown"),
                           QStringLiteral("root:root"), ancestorBindSource});
  const bool ancestorChowned = ancestorChownProc.waitForFinished(5000) &&
                               ancestorChownProc.exitCode() == 0;
  if (!ancestorChowned) {
    QSKIP("passwordless chown privilege unavailable in this environment; "
          "see the finding's own fail-closed allowance");
  }
  // Restores ownership back to this test process's own uid before the
  // QTemporaryDir destructor tries to remove it -- a root-owned
  // directory beneath a sticky-bit temp root (common, e.g. /tmp) can
  // only be unlinked by root itself, which would otherwise leave an
  // orphaned root-owned directory behind after this test.
  struct AncestorChownBackGuard {
    QString path;
    ~AncestorChownBackGuard() {
      QProcess::execute(QStringLiteral("sudo"),
                        {QStringLiteral("-n"), QStringLiteral("chown"),
                         QString::number(::getuid()) + QLatin1Char(':') +
                             QString::number(::getgid()),
                         path});
    }
  } ancestorChownBackGuard{ancestorBindSource};

  QProcess ancestorMountProc;
  ancestorMountProc.start(QStringLiteral("sudo"),
                          {QStringLiteral("-n"), QStringLiteral("mount"),
                           QStringLiteral("--bind"), ancestorBindSource,
                           mountedAncestor});
  const bool ancestorMounted = ancestorMountProc.waitForFinished(5000) &&
                               ancestorMountProc.exitCode() == 0;
  if (!ancestorMounted) {
    QSKIP("passwordless bind-mount privilege unavailable in this "
          "environment; see the finding's own fail-closed allowance");
  }
  struct UnmountGuard {
    QString mountPoint;
    ~UnmountGuard() {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPoint});
    }
  } ancestorUnmountGuard{mountedAncestor};

  const QString fakeHome = mountedAncestor + QStringLiteral("/actual-home");
  QVERIFY(QFileInfo::exists(fakeHome));

  // Independent cumulative re-review (MEDIUM, "even tests arbitrary
  // /dev/shm bind as accepted"): the SECOND, final-home-position
  // transition needs the identical genuine whole-filesystem-mount
  // treatment as the ancestor transition above -- see
  // createLoopbackExt4BindSourceDirectory()'s own comment.
  std::unique_ptr<LoopbackExt4Mount> homeBindSourceDirKeepAlive;
  const std::optional<QString> homeBindSourceOpt =
      createLoopbackExt4BindSourceDirectory(homeBindSourceDirKeepAlive);
  if (!homeBindSourceOpt.has_value()) {
    QSKIP("a loopback ext4 filesystem (mkfs.ext4/losetup/passwordless sudo "
          "mount) is unavailable in this environment; cannot model a "
          "genuinely distinct-device, whole-filesystem mount transition "
          "without it");
  }
  const QString homeBindSource = *homeBindSourceOpt;

  QProcess homeMountProc;
  homeMountProc.start(QStringLiteral("sudo"),
                      {QStringLiteral("-n"), QStringLiteral("mount"),
                       QStringLiteral("--bind"), homeBindSource, fakeHome});
  const bool homeMounted =
      homeMountProc.waitForFinished(5000) && homeMountProc.exitCode() == 0;
  if (!homeMounted) {
    QSKIP("passwordless bind-mount privilege unavailable in this "
          "environment; see the finding's own fail-closed allowance");
  }
  struct HomeUnmountGuard {
    QString mountPoint;
    ~HomeUnmountGuard() {
      QProcess::execute(
          QStringLiteral("sudo"),
          {QStringLiteral("-n"), QStringLiteral("umount"), mountPoint});
    }
  } homeUnmountGuard{fakeHome};

  HomeEnvOverrideGuard homeGuard(fakeHome);
  QCOMPARE(QDir::homePath(), QDir::cleanPath(fakeHome));
  AuthoritativeAccountHomeOverrideGuard accountGuard(QDir::cleanPath(fakeHome));
  // Independent cumulative re-review (MEDIUM, "Validate owner/mode for
  // EVERY opened component regardless mount transition"): both real
  // mount transitions this test exercises are already genuinely
  // policy-qualified for real (the ancestor destination chowned to
  // root:root, the final-home destination owned by this process's own
  // uid) -- but the SCAFFOLDING levels above the outermost transition
  // (this test process's own temp-directory hierarchy) are genuinely
  // owned by this unprivileged process, not root, and there is no
  // portable way to make them so. This override isolates the test to
  // what it actually verifies (both real, independently-qualified
  // transitions), not the unrelated scaffolding ancestors above them.
  HomeComponentOwnershipModePolicyOverrideGuard ownershipGuard(
      /*passes=*/true);

  const QString configuredUnderFakeHome =
      fakeHome + QStringLiteral("/assets/v1");
  // Phase 1: no backing-identity override active -- the REAL,
  // unmodified check must refuse this walk despite BOTH mount
  // transitions otherwise being fully authenticated/policy-qualified,
  // since BOTH bind sources are loopback (loop-backed) filesystems.
  QVERIFY(!AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));
  QVERIFY(!QFileInfo::exists(configuredUnderFakeHome));

  // Phase 2: an independent backing-identity authority now vouches for
  // both volumes -- every other check remains genuinely, unmodifiedly
  // exercised, and BOTH transitions are now permitted, exactly
  // modelling a genuine SteamOS-style topology with more than one
  // legitimate, non-loopback transition in the same walk.
  MountSourceBackingIdentityOverrideGuard backingIdentityGuard(
      /*trusted=*/true);
  QVERIFY(AssetCache::resolveTrustedDirectoryNoFollowForTesting(
      configuredUnderFakeHome, /*allowCreateMissingComponents=*/true));
  QVERIFY(QFileInfo(configuredUnderFakeHome).isDir());
#endif
}

void AssetCacheTests::
    configuredDirectoryWithMissingLeafUnderHomeIsNeverAutoCreated() {
  // Round-9+ review: the new home-anchored walker must preserve the
  // pre-existing "never creates any part of a caller-supplied custom
  // cache directory" guarantee exactly -- a configured directory whose
  // leaf does not yet exist must fail closed (disk cache disabled),
  // never be silently created, even though it is a plain, ordinary,
  // non-symlinked path with no attack involved at all.
  const QString parent = m_tempDirPath + QStringLiteral("/plain-parent");
  QVERIFY(QDir().mkpath(parent));
  const QString configuredDirectory =
      parent + QStringLiteral("/not-yet-created-leaf");
  QVERIFY(!QFileInfo::exists(configuredDirectory));

  AssetCache cache(configFor(configuredDirectory));
  QVERIFY(cache.isDiskCacheDisabledForTesting());
  QVERIFY(!QFileInfo::exists(configuredDirectory));
}

void AssetCacheTests::
    invalidateReportsPersistenceFailedWhenManifestUnlinkFails() {
  // Round-6 item 6: invalidate()'s durable-tombstone guarantee is only
  // meaningful if a genuine failure to commit it is actually reported,
  // rather than the caller being told "done" regardless. Force the
  // manifest unlink to fail the same deterministic way
  // failedEvictionDeletionLeavesEntryCountedAsStillOccupyingSpace() does
  // (revoke write permission on the containing directory).
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/undeletable.png")));
  {
    AssetCache cache(configFor(m_tempDirPath));
    cache.store(key, makeEntry(QByteArray(64, 'x')));
  }
  QVERIFY(QFileInfo::exists(
      AssetCache::manifestPathForTesting(m_tempDirPath, key)));

  struct ScopedDirectoryPermissionLock {
    QString path;
    ~ScopedDirectoryPermissionLock() {
      QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner |
                                      QFile::ExeOwner);
    }
  } permissionGuard{m_tempDirPath};
  QVERIFY(QFile::setPermissions(
      m_tempDirPath, QFile::ReadOwner | QFile::ExeOwner)); // r-x, no write

  AssetCache cache(configFor(m_tempDirPath));
  QCOMPARE(cache.invalidate(key),
           AssetCache::InvalidateResult::PersistenceFailed);
}

void AssetCacheTests::
    deleteEntryUnlinksManifestDurablyEvenWhenPrefixEnumerationCannotBeCompleted() {
  // Round-7/8 item 6 (MEDIUM): previously, deleteEntry() only unlinked
  // the manifest when its OWN prefix-enumeration listing happened to
  // include it -- so a directory whose contents cannot currently be
  // LISTED at all (unlinkat/openat/fstatat on an individually-named
  // file needs only search ('x') and write ('w') permission on the
  // containing directory; readdir()'s enumeration additionally needs
  // read ('r') permission -- POSIX directory permissions genuinely
  // separate these) previously reported the manifest as "durably
  // absent" without ever actually attempting to unlink it, letting the
  // OLD entry silently revive once the transient listing failure
  // cleared (a restart, or an expired negative-cache TTL). Revoke read
  // permission (keep write+exec) on the cache directory -- this is
  // deterministically verified below to break enumeration while still
  // allowing a BY-NAME unlink of the manifest to succeed -- and prove
  // invalidate() (which surfaces deleteEntry()'s manifestDurablyAbsent
  // outcome) still reports a genuine, durable removal, with the
  // manifest file actually gone from disk afterward.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/enum-blocked.png")));
  // Constructed and populated BEFORE any permission change below: this
  // instance's retained root directory descriptor (m_rootFd) is opened
  // now, while ordinary permissions still apply. Every later fd-relative
  // operation against that SAME already-open descriptor (fstatat/
  // openat/unlinkat for an individually-named entry) is governed by the
  // directory's CURRENT permission bits at syscall time, never by
  // whatever permission happened to apply when the descriptor was first
  // opened -- so this one instance is deliberately reused below, rather
  // than constructing a fresh AssetCache after permissions are revoked
  // (which would fail to even open the root descriptor at all, since
  // opening a directory for reading genuinely does require read
  // permission, unlike operating on an already-open one).
  AssetCache cache(configFor(m_tempDirPath));
  cache.store(key, makeEntry(QByteArray(64, 'y')));
  const QString manifestPath =
      AssetCache::manifestPathForTesting(m_tempDirPath, key);
  QVERIFY(QFileInfo::exists(manifestPath));

  struct ScopedDirectoryPermissionLock {
    QString path;
    ~ScopedDirectoryPermissionLock() {
      QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner |
                                      QFile::ExeOwner);
    }
  } permissionGuard{m_tempDirPath};
  // -wx: write + execute, but deliberately NO read -- blocks
  // opendir()/readdir() enumeration while still permitting an unlinkat()
  // of an individually-named entry, via the already-open descriptor
  // above, to succeed.
  QVERIFY(QFile::setPermissions(m_tempDirPath,
                                QFile::WriteOwner | QFile::ExeOwner));
  // Sanity-check the fault this test actually depends on: without read
  // permission, QDir's own (path-based) listing of this directory must
  // already come back empty, proving the permission change genuinely
  // breaks enumeration on this platform/filesystem and this test is not
  // vacuously passing for an unrelated reason.
  QVERIFY(QDir(m_tempDirPath).entryList(QDir::NoDotAndDotDot).isEmpty());

  QCOMPARE(cache.invalidate(key),
           AssetCache::InvalidateResult::DurablyInvalidated);

  // Restore permissions now so the assertions below (and the guard's
  // own destructor, which would otherwise be redundant but harmless)
  // can freely read/enumerate the directory again.
  QVERIFY(QFile::setPermissions(
      m_tempDirPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

  // The manifest must be genuinely, physically gone -- not merely
  // reported as gone -- and a brand-new AssetCache instance constructed
  // fresh over the same directory must never see this key again: no
  // revival, whether from an in-process restart or (as here) an
  // entirely new instance.
  QVERIFY(!QFileInfo::exists(manifestPath));
  AssetCache freshCache(configFor(m_tempDirPath));
  QVERIFY(!freshCache.lookupDisk(key).has_value());
}

void AssetCacheTests::
    negativeDiskMaxBytesDisablesDiskCacheInsteadOfDestructivelyEvicting() {
  // Round-9+ review (MEDIUM): before the fix, a negative diskMaxBytes
  // drove reapAndEnforceQuota()'s high/low-water-mark math negative,
  // so a genuinely-stored entry (whose on-disk byte total is always
  // >= 0) was destructively evicted on the very next sweep (including
  // the one this constructor itself runs) -- the entry seeded below via
  // a VALID-config instance would not survive being opened by a
  // negative-config instance pointed at the same directory. The fixed
  // behaviour disables disk persistence entirely for the negative-
  // config instance (fail-closed, matching the existing symlink/root-
  // mismatch failure modes) instead of destructively touching the
  // directory at all -- proving this, the entry seeded through a VALID
  // sibling instance survives being read back afterward.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/negative-disk-limit.png")));
  {
    AssetCache seedCache(configFor(m_tempDirPath));
    seedCache.store(key, makeEntry(QByteArray(64, 'z')));
  }

  AssetCache::Config invalidConfig = configFor(m_tempDirPath);
  invalidConfig.diskMaxBytes = -1;
  AssetCache cache(invalidConfig);
  QVERIFY(cache.isDiskCacheDisabledForTesting());
  // Round-N+ review (MEDIUM, repeat finding, "invalid cache limits
  // publicly constructible"): the object-level defense-in-depth above
  // (isDiskCacheDisabledForTesting()) is preserved, but this instance
  // must ALSO expose the fact that its whole configuration is invalid,
  // via the same typed AssetErrorCode::InvalidConfiguration
  // AssetNetworkFetcher already uses for its own analogous
  // constructor-time validation -- see AssetCache::create()/isValid()/
  // configurationError(). AssetRequestCoordinatorTests exercises the
  // consumer-facing half of this: a coordinator built against exactly
  // this kind of cache must refuse every request immediately rather
  // than ever reaching this destructive disk logic at all.
  QVERIFY(!cache.isValid());
  QVERIFY(cache.configurationError().has_value());
  QCOMPARE(cache.configurationError()->code,
           AssetErrorCode::InvalidConfiguration);
  QVERIFY(AssetCache::validateConfiguration(invalidConfig).has_value());
  const auto factoryResult = AssetCache::create(invalidConfig);
  QVERIFY(!factoryResult);
  QCOMPARE(factoryResult.error().code, AssetErrorCode::InvalidConfiguration);

  // The pre-seeded entry on disk must be completely untouched -- the
  // invalid-config instance never enumerated, evicted, or otherwise
  // wrote to the directory at all.
  AssetCache verifyCache(configFor(m_tempDirPath));
  const auto entry = verifyCache.lookupDisk(key);
  QVERIFY(entry.has_value());
  QCOMPARE(entry->encodedBytes, QByteArray(64, 'z'));
}

void AssetCacheTests::
    negativeMemoryMaxCostBytesDisablesMemoryCacheRatherThanCrashing() {
  // Companion to the disk case above. Cumulative review (PR #18,
  // MEDIUM, "invalid memory config still mutates disk"): a previous
  // version of this exact test asserted the WRONG behaviour --
  // `!cache.isDiskCacheDisabledForTesting()` and a successful
  // `cache.lookupDisk(key)` -- i.e. it actively protected an instance
  // whose own memoryMaxCostBytes was already invalid (and whose
  // isValid()/configurationError() already reported
  // InvalidConfiguration) from ALSO having disk persistence disabled,
  // even though such an instance still freely opened, locked, and
  // reaped a real on-disk cache directory. Config validity must be
  // all-or-nothing for BOTH tiers: an invalid memoryMaxCostBytes alone
  // now disables disk persistence too, exactly like an invalid
  // diskMaxBytes does in
  // negativeDiskMaxBytesDisablesDiskCacheInsteadOfDestructivelyEvicting()
  // above -- proven here the same way that test proves it, via a
  // pre-seeded entry (written by a separate, validly-configured
  // instance) that must survive completely untouched by the
  // invalid-memory-config instance.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/negative-memory-limit.png")));
  {
    AssetCache seedCache(configFor(m_tempDirPath));
    seedCache.store(key, makeEntry(QByteArray(64, 'w')));
  }

  AssetCache::Config invalidConfig = configFor(m_tempDirPath);
  invalidConfig.memoryMaxCostBytes = -1;
  AssetCache cache(invalidConfig);
  // A negative memoryMaxCostBytes must never be forwarded to
  // QCache::setMaxCost() as-is (which would evict every entry
  // immediately, silently defeating the memory cache) -- it is clamped
  // to 0 (memory caching disabled for this instance) so the failure
  // mode is an inert, predictable "no memory caching", never any kind
  // of crash.
  QVERIFY(cache.isDiskCacheDisabledForTesting());
  QVERIFY(!cache.isValid());
  QVERIFY(cache.configurationError().has_value());
  QCOMPARE(cache.configurationError()->code,
           AssetErrorCode::InvalidConfiguration);
  QVERIFY(AssetCache::validateConfiguration(invalidConfig).has_value());
  const auto factoryResult = AssetCache::create(invalidConfig);
  QVERIFY(!factoryResult);
  QCOMPARE(factoryResult.error().code, AssetErrorCode::InvalidConfiguration);

  const QString otherKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/other-negative-memory.png")));
  cache.store(otherKey, makeEntry(QByteArray(64, 'w')));
  // Neither tier retains anything: memory is clamped to 0 cost, and
  // disk persistence is disabled entirely for this instance.
  QVERIFY(!cache.lookupMemory(otherKey).has_value());
  QVERIFY(!cache.lookupDisk(otherKey).has_value());

  // The pre-seeded entry on disk must be completely untouched -- the
  // invalid-config instance never enumerated, evicted, or otherwise
  // wrote to the directory at all.
  AssetCache verifyCache(configFor(m_tempDirPath));
  const auto entry = verifyCache.lookupDisk(key);
  QVERIFY(entry.has_value());
  QCOMPARE(entry->encodedBytes, QByteArray(64, 'w'));
}

void AssetCacheTests::
    bothNegativeDiskAndMemoryLimitsDisableBothTiersWithZeroMutation() {
  // Cumulative review (PR #18, MEDIUM): the two single-invalid-field
  // tests above each independently prove that ANY invalid field alone
  // disables both tiers with zero disk mutation; this test proves the
  // combination -- BOTH fields invalid simultaneously -- behaves
  // identically, never mutating the directory at all, so the fix in
  // the constructor is genuinely an unconditional
  // "any configuration error disables both tiers" rule rather than one
  // that happens to work for each field checked in isolation but could
  // still regress for their conjunction.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/both-negative-limits.png")));
  {
    AssetCache seedCache(configFor(m_tempDirPath));
    seedCache.store(key, makeEntry(QByteArray(64, 'x')));
  }

  AssetCache::Config invalidConfig = configFor(m_tempDirPath);
  invalidConfig.diskMaxBytes = -1;
  invalidConfig.memoryMaxCostBytes = -1;
  AssetCache cache(invalidConfig);
  QVERIFY(cache.isDiskCacheDisabledForTesting());
  QVERIFY(!cache.isValid());
  QVERIFY(cache.configurationError().has_value());
  QCOMPARE(cache.configurationError()->code,
           AssetErrorCode::InvalidConfiguration);
  QVERIFY(AssetCache::validateConfiguration(invalidConfig).has_value());
  const auto factoryResult = AssetCache::create(invalidConfig);
  QVERIFY(!factoryResult);
  QCOMPARE(factoryResult.error().code, AssetErrorCode::InvalidConfiguration);

  const QString otherKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/other-both-negative.png")));
  cache.store(otherKey, makeEntry(QByteArray(64, 'x')));
  QVERIFY(!cache.lookupMemory(otherKey).has_value());
  QVERIFY(!cache.lookupDisk(otherKey).has_value());

  AssetCache verifyCache(configFor(m_tempDirPath));
  const auto entry = verifyCache.lookupDisk(key);
  QVERIFY(entry.has_value());
  QCOMPARE(entry->encodedBytes, QByteArray(64, 'x'));
}

void AssetCacheTests::
    validateConfigurationAndCreateAcceptEveryOrdinaryValidConfig() {
  // Positive control for the two typed-rejection tests below, and for
  // the negative-limit tests above: an entirely ordinary, valid Config
  // -- including the exact default-constructed Config() a real caller
  // would use when it has no reason to override any limit -- must never
  // be rejected.
  const AssetCache::Config validConfig = configFor(m_tempDirPath);
  QVERIFY(!AssetCache::validateConfiguration(validConfig).has_value());
  const auto factoryResult = AssetCache::create(validConfig);
  QVERIFY(factoryResult.has_value());
  QVERIFY(*factoryResult != nullptr);
  QVERIFY((*factoryResult)->isValid());
  QVERIFY(!(*factoryResult)->configurationError().has_value());

  QVERIFY(!AssetCache::validateConfiguration(AssetCache::Config()).has_value());
  const auto defaultFactoryResult = AssetCache::create();
  QVERIFY(defaultFactoryResult.has_value());
  QVERIFY((*defaultFactoryResult)->isValid());
}

void AssetCacheTests::
    validateConfigurationAndCreateRejectNegativeDiskOrMemoryLimitsAsInvalidConfiguration() {
  // Round-N+ review (MEDIUM, repeat finding, "invalid cache limits
  // publicly constructible"): the standalone, pure validator (and the
  // create() factory built on it) must independently agree with the
  // constructor's own defense-in-depth disabling -- see
  // negativeDiskMaxBytesDisablesDiskCacheInsteadOfDestructivelyEvicting()
  // and negativeMemoryMaxCostBytesDisablesMemoryCacheRatherThanCrashing()
  // above, which assert the SAME thing from the already-constructed
  // instance's own side.
  AssetCache::Config negativeDisk = configFor(m_tempDirPath);
  negativeDisk.diskMaxBytes = -1;
  const auto diskError = AssetCache::validateConfiguration(negativeDisk);
  QVERIFY(diskError.has_value());
  QCOMPARE(diskError->code, AssetErrorCode::InvalidConfiguration);
  const auto diskFactoryResult = AssetCache::create(negativeDisk);
  QVERIFY(!diskFactoryResult.has_value());
  QCOMPARE(diskFactoryResult.error().code,
           AssetErrorCode::InvalidConfiguration);

  AssetCache::Config negativeMemory = configFor(m_tempDirPath);
  negativeMemory.memoryMaxCostBytes = -1;
  const auto memoryError = AssetCache::validateConfiguration(negativeMemory);
  QVERIFY(memoryError.has_value());
  QCOMPARE(memoryError->code, AssetErrorCode::InvalidConfiguration);
  const auto memoryFactoryResult = AssetCache::create(negativeMemory);
  QVERIFY(!memoryFactoryResult.has_value());
  QCOMPARE(memoryFactoryResult.error().code,
           AssetErrorCode::InvalidConfiguration);
}

void AssetCacheTests::
    diskUsageBytesReflectsPhysicalAllocationRatherThanLogicalSizeForATinyEntry() {
#if !defined(Q_OS_UNIX)
  QSKIP("st_blocks-based physical accounting is a POSIX/Unix concept; "
        "not applicable on this platform");
#else
  // Round-7 review item 4 ("quota uses logical st_size ... policy
  // claims physical bytes"): a payload of only a few bytes still
  // consumes at least one whole filesystem block on real disk (and so
  // do its sibling manifest/metadata files) -- diskUsageBytes() must
  // reflect that real, physical cost, never the tiny logical byte
  // count, or a cache holding many small entries could badly
  // undercount how much real space it actually occupies, letting
  // quota enforcement never trip at all.
  AssetCache cache(configFor(m_tempDirPath));
  const qint64 emptyBaselineBytes = cache.diskUsageBytes();
  const qint64 rootBaselinePhysicalBytes =
      physicalBytesOnDiskForTesting(m_tempDirPath);

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/tiny-payload.png")));
  const QByteArray tinyPayload(3, 'x'); // far smaller than any real block
  cache.store(key, makeEntry(tinyPayload));

  const qint64 afterStoreBytes = cache.diskUsageBytes();
  const qint64 creditedBytes = afterStoreBytes - emptyBaselineBytes;

  // Three logical bytes of payload can never, by itself, explain the
  // real, physical cost of a payload file PLUS a manifest file PLUS a
  // metadata file, each independently rounded up to at least one whole
  // block.
  QVERIFY2(
      creditedBytes > tinyPayload.size(),
      qPrintable(QStringLiteral("expected physical (block-rounded) accounting "
                                "to exceed the tiny logical payload size of %1 "
                                "bytes, but credited only %2 bytes")
                     .arg(tinyPayload.size())
                     .arg(creditedBytes)));

  // And this must agree EXACTLY with directly stat()-ing the real
  // payload/manifest/metadata files on disk via the same physical
  // (st_blocks*512) convention -- never merely "some inflation", but
  // precisely the same accounting diskUsageBytes() itself now uses.
  // The generation identifier is a fresh, unique value minted per
  // store() call (see mintGenerationIdLocked()'s comment) -- NOT a
  // deterministic function of the payload bytes -- so it must be read
  // back from the manifest this store() just published, never
  // recomputed from a hash.
  const QString manifestPathForKey =
      AssetCache::manifestPathForTesting(m_tempDirPath, key);
  QFile manifestFile(manifestPathForKey);
  QVERIFY(manifestFile.open(QIODevice::ReadOnly));
  const QJsonDocument manifestDoc =
      QJsonDocument::fromJson(manifestFile.readAll());
  manifestFile.close();
  QVERIFY(manifestDoc.isObject());
  const QString generation =
      manifestDoc.object().value(QStringLiteral("generation")).toString();
  QVERIFY(!generation.isEmpty());
  const qint64 manifestPhysicalBytes =
      physicalBytesOnDiskForTesting(manifestPathForKey);
  const qint64 payloadPhysicalBytes = physicalBytesOnDiskForTesting(
      AssetCache::payloadPathForTesting(m_tempDirPath, key, generation));
  const qint64 metadataPhysicalBytes = physicalBytesOnDiskForTesting(
      AssetCache::metadataPathForTesting(m_tempDirPath, key, generation));
  // On some real filesystems (e.g. ext4, whose directories store
  // entries inline in the directory's own data blocks), adding three
  // new directory entries to the cache root can itself grow the ROOT
  // directory's own physical allocation -- diskUsageBytesLocked()
  // legitimately folds that root growth into its total (that's the
  // whole point of this review item: "include root directory"), so the
  // credited delta can exceed the sum of the three sibling files' own
  // physical bytes by exactly however much the root itself grew.
  // Account for that explicitly rather than assuming the root's own
  // size never changes.
  const qint64 rootPhysicalBytesDelta =
      physicalBytesOnDiskForTesting(m_tempDirPath) - rootBaselinePhysicalBytes;
  QVERIFY(rootPhysicalBytesDelta >= 0);
  QCOMPARE(creditedBytes, rootPhysicalBytesDelta + manifestPhysicalBytes +
                              payloadPhysicalBytes + metadataPhysicalBytes);
#endif
}

void AssetCacheTests::
    invalidateAfterRootReplacementReportsPersistenceFailedNotDurable() {
  // Round-9+ review (HIGH): deleteEntry()'s old guard --
  // `if (m_diskCacheDisabled || !verifyRootAnchorLocked()) return {true,
  // true};` -- folded two completely different situations into the
  // same vacuous "durably absent" answer: (a) disk was NEVER available
  // for this instance at all (genuinely vacuous -- nothing could ever
  // be there), and (b) disk WAS available and verified at construction
  // but root verification is failing RIGHT NOW on this exact call
  // (a real manifest can still be sitting on disk, completely
  // untouched, because this call never even attempted to remove it).
  // invalidate() trusts a "durably absent" answer enough to record an
  // authoritative negative-404 tombstone -- so case (b) reported as
  // case (a) would let a still-live cached 200 "revive" later (a fresh
  // process, or a sibling instance, that does not share this exact
  // transient/replacement condition can still open that untouched
  // manifest). This test proves invalidate() now reports the typed
  // PersistenceFailed result instead, and that the entry genuinely
  // does still physically exist afterwards.
  const QString cacheRoot =
      m_tempDirPath + QStringLiteral("/invalidate-root-replaced-root");
  QVERIFY(QDir(m_tempDirPath)
              .mkpath(QStringLiteral("invalidate-root-replaced-root")));

  AssetCache cache(configFor(cacheRoot));
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/invalidate-after-replace.png")));
  cache.store(key, makeEntry(QByteArrayLiteral("still-live-on-disk-bytes")));
  QVERIFY(cache.lookupDisk(key).has_value());
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  // Replace the root exactly as
  // rootReplacedAfterConstructionPermanentlyDisablesDiskIoForBothTargets()
  // does: rename the original (still holding the real manifest for
  // `key`) away, then create a brand-new, empty directory at the exact
  // original path.
  const QString renamedAwayRoot =
      m_tempDirPath +
      QStringLiteral("/invalidate-root-replaced-root-renamed-away");
  QVERIFY(QDir().rename(cacheRoot, renamedAwayRoot));
  QVERIFY(QDir(m_tempDirPath)
              .mkpath(QStringLiteral("invalidate-root-replaced-root")));

  // This is the very FIRST disk-touching call since the replacement --
  // m_diskCacheDisabled is still false going in, so verifyRootAnchorLocked()
  // is what actually fails, and does so for the first time, right here.
  QVERIFY(!cache.isDiskCacheDisabledForTesting());
  const AssetCache::InvalidateResult result = cache.invalidate(key);
  QCOMPARE(result, AssetCache::InvalidateResult::PersistenceFailed);
  // The failed verification permanently disables disk I/O for this
  // instance from here on, exactly like every other disk-touching
  // method -- consistent with the rest of the class's contract.
  QVERIFY(cache.isDiskCacheDisabledForTesting());

  // The manifest this call never actually touched must still be fully
  // intact under the ORIGINAL (renamed-away) directory -- proving
  // nothing was deleted, and that "PersistenceFailed" was the honest
  // answer rather than a false "DurablyInvalidated".
  AssetCache freshCacheOverOriginal(configFor(renamedAwayRoot));
  const auto revived = freshCacheOverOriginal.lookupDisk(key);
  QVERIFY(revived.has_value());
  QCOMPARE(revived->encodedBytes,
           QByteArrayLiteral("still-live-on-disk-bytes"));
}

void AssetCacheTests::
    invalidateWithAlreadyLatchedDiskDisabledReportsPersistenceFailedNotDurable() {
  // Round-N+ review (HIGH, repeat finding): deleteEntry()'s OLD guard
  // for the PRE-latched case -- `if (m_diskCacheDisabled) return {true,
  // true};`, unconditionally, regardless of WHY or WHEN it was latched
  // -- let a manifest that genuinely still exists on the ORIGINAL root
  // object "revive" once a fresh instance (or this very config, fixed)
  // opens it again. This test latches m_diskCacheDisabled via an
  // EARLIER, unrelated disk-touching call (lookupDisk() for a different
  // key, after the same root-replacement the test above exercises) --
  // so the LATER invalidate(key) call below hits the pre-latched branch
  // (m_diskCacheDisabled already true going in) rather than freshly
  // discovering the failure itself, which is the specific gap this
  // round closes.
  const QString cacheRoot =
      m_tempDirPath + QStringLiteral("/invalidate-prelatched-root");
  QVERIFY(
      QDir(m_tempDirPath).mkpath(QStringLiteral("invalidate-prelatched-root")));

  AssetCache cache(configFor(cacheRoot));
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/prelatched.png")));
  cache.store(key, makeEntry(QByteArrayLiteral("still-live-prelatched-bytes")));
  QVERIFY(cache.lookupDisk(key).has_value());
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  const QString renamedAwayRoot =
      m_tempDirPath +
      QStringLiteral("/invalidate-prelatched-root-renamed-away");
  QVERIFY(QDir().rename(cacheRoot, renamedAwayRoot));
  QVERIFY(
      QDir(m_tempDirPath).mkpath(QStringLiteral("invalidate-prelatched-root")));

  // An EARLIER, unrelated disk-touching call latches m_diskCacheDisabled
  // now, for an entirely different key -- this is the call that
  // actually discovers the root replacement.
  const QString otherKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/prelatched-other.png")));
  QVERIFY(!cache.lookupDisk(otherKey).has_value());
  QVERIFY(cache.isDiskCacheDisabledForTesting());

  // THIS is the call under test: m_diskCacheDisabled is ALREADY true
  // going in, so this exercises the pre-latched early-return branch,
  // not a fresh verifyRootAnchorLocked() failure.
  const AssetCache::InvalidateResult result = cache.invalidate(key);
  QCOMPARE(result, AssetCache::InvalidateResult::PersistenceFailed);

  // The manifest this call never actually touched must still be fully
  // intact under the ORIGINAL (renamed-away) directory.
  AssetCache freshCacheOverOriginal(configFor(renamedAwayRoot));
  const auto revived = freshCacheOverOriginal.lookupDisk(key);
  QVERIFY(revived.has_value());
  QCOMPARE(revived->encodedBytes,
           QByteArrayLiteral("still-live-prelatched-bytes"));
}

void AssetCacheTests::
    unreadableNestedSubtreeDisablesPersistenceRatherThanReportingZeroUsage() {
  // Round-N+ review (MEDIUM, repeat finding): sumUsageRelative()'s
  // recursive walk used to silently treat an unopenable/unreadable
  // subdirectory as contributing 0 bytes (and, at the top level,
  // fdopendir() failing on the root itself as 0 for the ENTIRE tree) --
  // letting a filesystem fault make an actually-full cache falsely
  // report as empty, defeating quota enforcement. Plant a real entry
  // (so the cache root's usage is genuinely non-zero to begin with),
  // then a SEPARATE stray subdirectory whose contents cannot be
  // enumerated (permission-based, exactly the same deterministic fault
  // deleteEntryUnlinksManifestDurablyEvenWhenPrefixEnumerationCannotBeCompleted()
  // above already relies on: revoke read, keep write+exec, which blocks
  // opendir()/readdir() while by-name operations on the PARENT
  // continue to work). diskUsageBytes() must now report 0 AND
  // permanently disable disk persistence for this instance -- an
  // indeterminate result, never a number a caller could mistake for
  // "actually empty".
  AssetCache cache(configFor(m_tempDirPath));
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/unreadable-subtree.png")));
  cache.store(key, makeEntry(QByteArray(256, 'q')));
  QVERIFY(cache.lookupDisk(key).has_value());
  QVERIFY(cache.diskUsageBytes() > qint64(0));

  const QString unreadableSubtreeDir =
      m_tempDirPath + QStringLiteral("/unreadable-subtree");
  QVERIFY(QDir(m_tempDirPath).mkpath(QStringLiteral("unreadable-subtree")));
  {
    QFile nested(unreadableSubtreeDir + QStringLiteral("/nested-file.bin"));
    QVERIFY(nested.open(QIODevice::WriteOnly));
    nested.write(QByteArray(128, 'r'));
  }

  struct ScopedDirectoryPermissionLock {
    QString path;
    ~ScopedDirectoryPermissionLock() {
      QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner |
                                      QFile::ExeOwner);
    }
  } permissionGuard{unreadableSubtreeDir};
  // -wx: write + execute, but deliberately NO read -- blocks
  // opendir()/readdir() enumeration of THIS subdirectory's own
  // contents specifically (the cache root itself, and every OTHER
  // entry directly inside it, remains fully listable).
  QVERIFY(QFile::setPermissions(unreadableSubtreeDir,
                                QFile::WriteOwner | QFile::ExeOwner));
  QVERIFY(QDir(unreadableSubtreeDir).entryList(QDir::NoDotAndDotDot).isEmpty());

  QVERIFY(!cache.isDiskCacheDisabledForTesting());
  QCOMPARE(cache.diskUsageBytes(), qint64(0));
  QVERIFY(cache.isDiskCacheDisabledForTesting());
}

namespace {
// Locates tests/helpers/AssetCacheLockHolderMain.cpp's built binary --
// a sibling executable in the same build output directory as THIS test
// binary (see CMakeLists.txt: both `arkham-asset-tests` and
// `arkham-asset-cache-lock-holder` are ordinary qt_add_executable()
// targets configured into the same build tree, never installed
// side-by-side by coincidence).
QString lockHolderHelperPath() {
  QDir dir(QCoreApplication::applicationDirPath());
#if defined(Q_OS_WIN)
  const QString name = QStringLiteral("arkham-asset-cache-lock-holder.exe");
#else
  const QString name = QStringLiteral("arkham-asset-cache-lock-holder");
#endif
  return dir.filePath(name);
}
} // namespace

void AssetCacheTests::
    secondProcessHoldingRootLockForcesThisProcessMemoryOnlyUntilReleased() {
#if !defined(Q_OS_UNIX)
  QSKIP("flock()-based cross-process root locking is a POSIX-specific "
        "mechanism; not applicable on this platform");
#else
  const QString helperPath = lockHolderHelperPath();
  QVERIFY2(QFile::exists(helperPath),
           qPrintable(QStringLiteral("lock-holder helper not found at %1")
                          .arg(helperPath)));

  QProcess holder;
  holder.setProgram(helperPath);
  holder.setArguments({m_tempDirPath});
  holder.start();
  QVERIFY2(holder.waitForStarted(5000), "lock-holder helper failed to start");

  // Deterministically wait for the helper's own explicit readiness
  // line -- never a sleep -- so this test never races the helper's own
  // AssetCache construction (and therefore its flock() acquisition).
  QByteArray readyLine;
  QVERIFY2(QTest::qWaitFor(
               [&]() {
                 holder.waitForReadyRead(50);
                 readyLine += holder.readAllStandardOutput();
                 return readyLine.contains('\n');
               },
               10000),
           "lock-holder helper never produced a readiness line");
  QVERIFY2(readyLine.contains("LOCK-HOLDER-READY"),
           qPrintable(QStringLiteral("lock-holder helper reported: %1")
                          .arg(QString::fromUtf8(readyLine))));

  // The REAL second process now holds this exact root's exclusive
  // cross-process lock. This process's own AssetCache instance over
  // the same root must be denied disk authority entirely.
  {
    AssetCache cache(configFor(m_tempDirPath));
    QVERIFY(cache.isDiskCacheDisabledForTesting());

    const QString key = AssetCache::cacheKeyFor(
        QUrl(QStringLiteral("https://example.com/denied-process.png")));
    cache.store(key, makeEntry(QByteArrayLiteral("memory-only-bytes")));
    // Memory-tier behaviour is unaffected -- only disk is denied.
    QVERIFY(cache.lookupMemory(key).has_value());
    // Zero disk mutation: nothing was ever written to the root the
    // other process still owns.
    QCOMPARE(QDir(m_tempDirPath)
                 .entryList(QDir::NoDotAndDotDot | QDir::AllEntries)
                 .size(),
             0);
  }

  // Release the helper process's lock deterministically (write a line
  // to its stdin, then wait for real process exit -- never a sleep).
  holder.write("release\n");
  QVERIFY2(holder.waitForFinished(5000),
           "lock-holder helper never exited after being signalled");
  QCOMPARE(holder.exitCode(), 0);

  // Now that the OTHER process has genuinely released the lock, THIS
  // process can acquire it normally.
  AssetCache cacheAfterRelease(configFor(m_tempDirPath));
  QVERIFY(!cacheAfterRelease.isDiskCacheDisabledForTesting());
  const QString keyAfter = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/after-release.png")));
  cacheAfterRelease.store(keyAfter,
                          makeEntry(QByteArrayLiteral("disk-now-ok-bytes")));
  QVERIFY(cacheAfterRelease.lookupDisk(keyAfter).has_value());
#endif
}

void AssetCacheTests::
    sameProcessMultipleInstancesOverSameRootAllCooperateWithFullDiskAuthority() {
  // Positive control: two LIVE, simultaneously-existing AssetCache
  // instances in THIS SAME process, both over the exact same root --
  // this must keep working exactly as it always has (several existing
  // tests in this file already rely on overlapping/sequential
  // same-process instances); the process-wide root lock registry must
  // never treat a same-process sibling as a competing owner.
  AssetCache first(configFor(m_tempDirPath));
  QVERIFY(!first.isDiskCacheDisabledForTesting());

  AssetCache second(configFor(m_tempDirPath));
  QVERIFY(!second.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/cooperating-instances.png")));
  first.store(key, makeEntry(QByteArrayLiteral("cooperating-bytes")));
  // Cumulative review (independent re-review, HIGH, "shared root
  // authority incomplete"): the second, sibling instance now genuinely
  // shares ONE memory cache with the first (see RootAuthority's own
  // comment in AssetCache.cpp) -- this is a STRONGER guarantee than the
  // disk-only cooperation this test used to document: the entry is
  // visible to the second sibling's own memory tier directly, with no
  // disk read required at all.
  QVERIFY(second.lookupMemory(key).has_value());
  const auto hitFromSecond = second.lookupDisk(key);
  QVERIFY(hitFromSecond.has_value());
  QCOMPARE(hitFromSecond->encodedBytes, QByteArrayLiteral("cooperating-bytes"));

  // Destroying ONE sibling must not revoke the other's still-live disk
  // authority (the registry's reference count, not an unconditional
  // release, gates when the underlying lock is actually released).
  {
    AssetCache third(configFor(m_tempDirPath));
    QVERIFY(!third.isDiskCacheDisabledForTesting());
  }
  QVERIFY(!first.isDiskCacheDisabledForTesting());
  QVERIFY(!second.isDiskCacheDisabledForTesting());
  const auto hitStillFromSecond = second.lookupDisk(key);
  QVERIFY(hitStillFromSecond.has_value());
}

void AssetCacheTests::execChildProcessNeverInheritsTheRootLockFileDescriptor() {
  // Cumulative review (PR #18, HIGH, "dup() fd lacks CLOEXEC; exec
  // child retains root"): /proc/self/fd enumeration is Linux-specific,
  // so this proof (the only way to POSITIVELY confirm CLOEXEC actually
  // took effect, rather than merely the absence of an indirect
  // symptom) is Linux-only.
#if !defined(__linux__)
  QSKIP("/proc/self/fd enumeration is Linux-specific; not applicable on "
        "this platform");
#else
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());
  const int lockFd = cache.rootLockFileDescriptorForTesting();
  QVERIFY(lockFd >= 0);

  QProcess child;
  child.start(QStringLiteral("/bin/sh"),
              {QStringLiteral("-c"), QStringLiteral("ls -1 /proc/self/fd")});
  QVERIFY2(child.waitForFinished(5000), "child process never exited");
  QCOMPARE(child.exitCode(), 0);
  const QByteArray output = child.readAllStandardOutput();
  const QList<QByteArray> lines = output.split('\n');
  const QByteArray lockFdText = QByteArray::number(lockFd);
  bool inheritedFdFound = false;
  for (const QByteArray &line : lines) {
    if (line.trimmed() == lockFdText) {
      inheritedFdFound = true;
      break;
    }
  }
  QVERIFY2(!inheritedFdFound,
           "the root lock file descriptor leaked into an exec'd child "
           "process -- CLOEXEC did not take effect");
#endif
}

void AssetCacheTests::
    concurrentSameProcessInstancesNeverMintCollidingAccessSequenceValues() {
  AssetCache first(configFor(m_tempDirPath));
  QVERIFY(!first.isDiskCacheDisabledForTesting());
  AssetCache second(configFor(m_tempDirPath));
  QVERIFY(!second.isDiskCacheDisabledForTesting());

  // Alternate stores between the two LIVE, simultaneously-existing
  // instances across many distinct keys -- under the old per-instance
  // counter design, both instances would have recovered the SAME
  // starting access-sequence value at construction and then minted
  // colliding values completely independently of one another.
  QVector<QString> keys;
  constexpr int kKeyCount = 40;
  for (int i = 0; i < kKeyCount; ++i) {
    const QString key = AssetCache::cacheKeyFor(
        QUrl(QStringLiteral("https://example.com/lru-seq-%1.png").arg(i)));
    keys.push_back(key);
    AssetCache &writer = (i % 2 == 0) ? first : second;
    writer.store(key, makeEntry(QByteArrayLiteral("lru-seq-bytes")));
  }

  QSet<quint64> seenSequences;
  for (const QString &key : keys) {
    const auto sequence = first.accessSequenceForTesting(key);
    QVERIFY2(
        sequence.has_value(),
        qPrintable(QStringLiteral("missing access sequence for %1").arg(key)));
    QVERIFY2(!seenSequences.contains(*sequence),
             qPrintable(QStringLiteral("duplicate access-sequence value %1 "
                                       "for key %2 -- two same-process "
                                       "instances minted colliding LRU "
                                       "sequence numbers")
                            .arg(*sequence)
                            .arg(key)));
    seenSequences.insert(*sequence);
  }
  QCOMPARE(seenSequences.size(), kKeyCount);
}

void AssetCacheTests::
    forkedChildProcessNeverJoinsParentsInheritedRootAuthority() {
#if !defined(Q_OS_UNIX)
  QSKIP("fork() is a POSIX-specific mechanism; not applicable on this "
        "platform");
#else
  // The parent constructs (and keeps alive for the duration of this
  // test) a live AssetCache holding this root's real, cross-process
  // flock() -- this also registers registerForkSafetyOnce()'s
  // pthread_atfork() child-handler process-wide, BEFORE this test's own
  // explicit fork() below ever runs.
  AssetCache parentCache(configFor(m_tempDirPath));
  QVERIFY(!parentCache.isDiskCacheDisabledForTesting());
  QVERIFY(parentCache.rootLockRegistryHasLiveEntryForTesting());

  // Deterministic, no-sleep synchronization: a pipe the child uses to
  // report its single-byte verdict, read by the parent via a blocking
  // read that unblocks the instant the child writes (or its end closes
  // on exit, whichever happens first).
  int pipeFds[2] = {-1, -1};
  QVERIFY(::pipe(pipeFds) == 0);

  const pid_t child = ::fork();
  QVERIFY(child >= 0);
  if (child == 0) {
    // CHILD: deliberately does NOT construct any further Qt object
    // (not even a new AssetCache) -- a live Qt/QCoreApplication process
    // (as this test binary is) is not generally safe to fork() without
    // an immediate exec() at all (Qt/the platform runtime may keep
    // internal worker threads/allocator state that a bare fork() can
    // leave inconsistent in the child), independent of anything this
    // fix does or does not do. Rather than let that UNRELATED, general
    // hazard make this test flaky, this test instead directly probes
    // the exact, narrow mechanism the fix actually relies on: the
    // registry itself, via a raw, lock-free, single-threaded-window
    // read (see rootLockRegistryHasLiveEntryForTesting()'s own
    // comment) using only minimal, async-signal-safe-ish work before
    // _exit().
    //
    // A deliberately STRONGER proof -- actually constructing a
    // brand-new AssetCache (with real QMutex/QCache/heap construction)
    // in this exact forked-without-exec child -- was tried and
    // reliably SIGABRTs on this platform: that is the very same
    // general "fork() a live Qt process without exec()" hazard this
    // comment already documents, reproducing independently of whether
    // this fix's own guard runs first. See
    // constructingAssetCacheAfterSimulatedForkFailsDiskAuthorityClosed()
    // below for the deterministic, non-flaky way this file instead
    // proves that exact "construct a brand-new AssetCache after a
    // fork()" contract, without needing a real, hazardous fork() to do
    // it.
    ::close(pipeFds[0]);
    const char verdict =
        parentCache.rootLockRegistryHasLiveEntryForTesting() ? '1' : '0';
    ssize_t written = ::write(pipeFds[1], &verdict, 1);
    (void)written;
    ::close(pipeFds[1]);
    ::_exit(0);
  }

  // PARENT:
  ::close(pipeFds[1]);
  char verdict = '?';
  const ssize_t bytesRead = ::read(pipeFds[0], &verdict, 1);
  ::close(pipeFds[0]);
  int status = 0;
  QVERIFY2(::waitpid(child, &status, 0) == child,
           "waitpid() never reaped the forked child");
  QVERIFY2(WIFEXITED(status),
           qPrintable(QStringLiteral(
                          "forked child did not exit normally (raw status=%1, "
                          "WIFSIGNALED=%2, WTERMSIG=%3)")
                          .arg(status)
                          .arg(WIFSIGNALED(status) ? 1 : 0)
                          .arg(WIFSIGNALED(status) ? WTERMSIG(status) : -1)));

  QVERIFY2(bytesRead == 1, "forked child never reported a verdict");
  // '0' -- the registry, from the CHILD's own post-fork perspective,
  // must be EMPTY for this root: registerForkSafetyOnce()'s
  // pthread_atfork() child-handler ran synchronously during the fork()
  // call itself (before fork() ever returned to this child), permanently
  // marking this process as forked-without-exec -- proving the fix's
  // actual, targeted contract: a forked child never inherits a live
  // belief that it already owns this root's authority.
  QCOMPARE(verdict, '0');

  // The parent's own instance must remain completely unaffected --
  // still fully enabled and still registered, exactly as before the
  // fork.
  QVERIFY(!parentCache.isDiskCacheDisabledForTesting());
  QVERIFY(parentCache.rootLockRegistryHasLiveEntryForTesting());
#endif
}

void AssetCacheTests::
    forkedChildDestroyingInheritedStackAssetCacheTerminatesProcessDeterministically() {
#if !defined(Q_OS_UNIX)
  QSKIP("fork() is a POSIX-specific mechanism; not applicable on this "
        "platform");
#else
  // Independent cumulative re-review (MEDIUM, "fork destruction...
  // real inherited stack object normal-scope destruction under held
  // parent mutex must be addressed, not `_exit`-only test"):
  // forkedChildProcessNeverJoinsParentsInheritedRootAuthority() above
  // deliberately never lets its forked child destroy anything at all
  // (its child calls ::_exit() immediately, which bypasses every
  // destructor entirely) -- so it proves nothing about
  // ~AssetCache()'s own forked-child behaviour. THIS test instead lets
  // a genuinely STACK-scoped (not heap/pointer-held), pre-fork
  // AssetCache instance be destroyed via ORDINARY C++ scope-exit
  // semantics in the child -- exactly as an ordinary function return
  // would -- never an explicit destructor call, and never a premature
  // ::_exit() that would prevent this destructor from ever running at
  // all.
  int pipeFds[2] = {-1, -1};
  QVERIFY(::pipe(pipeFds) == 0);

  pid_t child = -1;
  {
    AssetCache cache(configFor(m_tempDirPath));
    QVERIFY(!cache.isDiskCacheDisabledForTesting());

    child = ::fork();
    QVERIFY(child >= 0);
    if (child == 0) {
      ::close(pipeFds[0]);
      // Deliberately do nothing else here: falling out of this scope
      // block normally, right now, invokes `cache`'s ORDINARY
      // destructor via real C++ stack-unwinding semantics. If
      // ~AssetCache()'s forked-child branch works as designed, this
      // process terminates (via ::_exit()) from WITHIN that destructor
      // call, and every line below this block in the child is never
      // reached at all.
    }
  }
  // If control ever reaches here in the CHILD, the destructor's
  // forked-child branch failed to terminate the process as designed --
  // report that as a distinguishable, deterministic verdict rather than
  // silently falling through to the parent-only code below (which
  // would otherwise construct a SECOND live AssetCache over the
  // identical root from within this same forked-without-exec child --
  // itself an entirely separate hazard this test must not risk
  // triggering).
  if (child == 0) {
    const char verdict = 'X'; // "destructor did not terminate the process"
    ssize_t written = ::write(pipeFds[1], &verdict, 1);
    (void)written;
    ::close(pipeFds[1]);
    ::_exit(99);
  }

  // PARENT: the parent's own `cache` (out of scope above) destructed
  // completely normally -- hasForkedSinceConstruction() is false for
  // it, since its own getpid() never changed across the fork().
  ::close(pipeFds[1]);
  char verdict = '?';
  const ssize_t bytesRead = ::read(pipeFds[0], &verdict, 1);
  ::close(pipeFds[0]);
  int status = 0;
  QVERIFY2(::waitpid(child, &status, 0) == child,
           "waitpid() never reaped the forked child");

  // The decisive assertion: the child must have terminated via the
  // destructor's OWN ::_exit() call while still INSIDE `cache`'s
  // normal-scope destruction -- it must never reach (and report via
  // the pipe) the 'X' fallback verdict above, which would mean the
  // fix failed to intercept this exact "real inherited stack object,
  // ordinary scope-exit" destruction path; and it must never crash via
  // an unrelated signal either, which would mean an unsafe member
  // destructor actually ran and corrupted/deadlocked this process
  // instead of the fix cleanly terminating it first.
  QVERIFY2(bytesRead == 0,
           "the forked child's normal-scope AssetCache destruction did "
           "not terminate the process immediately, as the fix requires "
           "-- it kept running far enough to report a fallback verdict");
  QVERIFY2(
      WIFEXITED(status),
      qPrintable(QStringLiteral("forked child did not exit normally via the "
                                "destructor's own ::_exit() (raw status=%1, "
                                "WIFSIGNALED=%2, WTERMSIG=%3)")
                     .arg(status)
                     .arg(WIFSIGNALED(status) ? 1 : 0)
                     .arg(WIFSIGNALED(status) ? WTERMSIG(status) : -1)));
  QCOMPARE(WEXITSTATUS(status), 70);
#endif
}

void AssetCacheTests::
    constructingAssetCacheAfterSimulatedForkFailsDiskAuthorityClosed() {
#if !defined(Q_OS_UNIX)
  QSKIP("this fork-safety mechanism is POSIX-specific; not applicable on "
        "this platform");
#else
  // RAII-style guaranteed reset: whatever happens below (including a
  // QVERIFY/QCOMPARE failure aborting the test function early), this
  // process-wide override must never leak into an unrelated, later
  // test.
  struct ForcedForkStateGuard {
    ~ForcedForkStateGuard() {
      AssetCache::setForkedSinceLastExecForcedStateForTesting(false);
    }
  } guard;

  QVERIFY(QDir(m_tempDirPath)
              .mkpath(QStringLiteral("simulated-forked-child-root")));
  const QString forkedRoot =
      QDir(m_tempDirPath)
          .filePath(QStringLiteral("simulated-forked-child-root"));

  // Force processHasForkedSinceLastExec() to report exactly the same
  // state a real pthread_atfork() child handler would have already left
  // behind in a genuinely forked-without-exec process -- see that
  // function's own comment in AssetCache.cpp.
  AssetCache::setForkedSinceLastExecForcedStateForTesting(true);

  // A brand-new AssetCache, constructed through the REAL, unmodified
  // production constructor and acquireExclusiveRootOwnershipOrFailClosed()
  // entry point, over a root NOBODY else holds a lock on, must still
  // fail disk authority closed -- proving the review's exact demand
  // ("continuing child must fail disk authority closed on first use")
  // deterministically, without a real (and, verified independently,
  // SIGABRT-hazardous) fork().
  AssetCache forkedLikeCache(configFor(forkedRoot));
  QVERIFY(forkedLikeCache.isDiskCacheDisabledForTesting());

  // Memory-tier behaviour must remain completely unaffected by the
  // fail-closed disk decision -- only disk authority is denied.
  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/simulated-forked-child.png")));
  forkedLikeCache.store(key,
                        makeEntry(QByteArrayLiteral("memory-only-post-fork")));
  QVERIFY(forkedLikeCache.lookupMemory(key).has_value());

  // Zero disk mutation: this root, which nothing else was ever
  // contending for, must remain completely empty -- the fail-closed
  // instance never even attempted to acquire or create anything on
  // disk beneath it.
  QCOMPARE(QDir(forkedRoot)
               .entryList(QDir::NoDotAndDotDot | QDir::AllEntries)
               .size(),
           0);

  // Complementary recovery contract: simulating an exec() (clearing the
  // forced marker, exactly as a real exec() wipes every static back to
  // its initial state) lets a FRESH AssetCache over a FRESH root regain
  // full, ordinary disk authority again -- proving "require exec for
  // fresh authority" is not a one-way, permanently-broken trap.
  AssetCache::setForkedSinceLastExecForcedStateForTesting(false);
  QVERIFY(QDir(m_tempDirPath)
              .mkpath(QStringLiteral("simulated-post-exec-recovery-root")));
  const QString postExecRoot =
      QDir(m_tempDirPath)
          .filePath(QStringLiteral("simulated-post-exec-recovery-root"));
  AssetCache recoveredCache(configFor(postExecRoot));
  QVERIFY(!recoveredCache.isDiskCacheDisabledForTesting());
  const QString recoveredKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/post-exec-recovery.png")));
  recoveredCache.store(recoveredKey,
                       makeEntry(QByteArrayLiteral("disk-recovered-bytes")));
  QVERIFY(recoveredCache.lookupDisk(recoveredKey).has_value());
#endif
}

void AssetCacheTests::
    preForkLiveInstanceRejectsEveryInheritedOperationBeforeTouchingStateForTesting() {
  // RAII-style guaranteed reset -- mirrors
  // constructingAssetCacheAfterSimulatedForkFailsDiskAuthorityClosed()'s
  // own guard above.
  struct ForcedPreForkStateGuard {
    ~ForcedPreForkStateGuard() {
      AssetCache::setPreForkLiveInstanceForcedStateForTesting(false);
    }
  } guard;

  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/pre-fork-live-instance.png")));

  // Fully normal, pre-"fork" operation: an entry genuinely published
  // before the simulated fork, exactly as a real parent process's
  // already-live cache would have before forking.
  const quint64 preForkGeneration = cache.issueKeyGeneration(key);
  cache.store(key, makeEntry(QByteArrayLiteral("pre-fork-bytes")),
              preForkGeneration);
  QVERIFY(cache.lookupMemory(key).has_value());
  QVERIFY(cache.lookupDisk(key).has_value());
  QVERIFY(cache.memoryCostBytes() > 0);
  QVERIFY(cache.diskUsageBytes() > 0);
  QVERIFY(cache.diskEntryCount() > 0);

  // this exact, already-constructed object would observe.
  AssetCache::setPreForkLiveInstanceForcedStateForTesting(true);

  // Fail-before/pass-after (this is the exact bug class the review
  // flagged: "inherited public methods lock inherited QMutex and may
  // deadlock/mutate root"): EVERY public operation below must now
  // fail closed as a safe no-op/miss, never touching m_mutex/m_memory/
  // disk at all.
  QVERIFY(!cache.lookupMemory(key).has_value());
  QVERIFY(!cache.lookupDisk(key).has_value());
  QCOMPARE(cache.issueKeyGeneration(key), AssetCache::kUnconditionalGeneration);
  const AssetCache::KeyGenerationSnapshot snapshot =
      cache.snapshotAndIssueGeneration(key, /*nowMonotonicMs=*/1'000);
  QVERIFY(!snapshot.hit.has_value());
  QVERIFY(!snapshot.hitFromMemory);
  QVERIFY(!snapshot.authoritativeNegative404);
  QCOMPARE(snapshot.issuedGeneration, AssetCache::kUnconditionalGeneration);

  cache.recordNegative404(key, preForkGeneration, /*nowMonotonicMs=*/1'000,
                          /*ttlMs=*/60'000);
  QVERIFY(!cache.hasNegative404ForTesting(key, /*nowMonotonicMs=*/1'000));
  cache.clearNegative404(key); // must not crash/deadlock either.

  const QString otherKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/pre-fork-live-other.png")));
  cache.store(otherKey, makeEntry(QByteArrayLiteral("post-fork-write-bytes")));
  QVERIFY(!cache.lookupMemory(otherKey).has_value());
  QVERIFY(!cache.lookupDisk(otherKey).has_value());

  cache.touchAfterNotModified(key, QStringLiteral("etag"),
                              QStringLiteral("last-modified"),
                              preForkGeneration);
  cache.updateMemoryDecodedImage(key, QImage(), preForkGeneration);
  cache.promoteToMemory(otherKey, makeEntry(QByteArrayLiteral("promoted")),
                        preForkGeneration);
  QVERIFY(!cache.lookupMemory(otherKey).has_value());

  QCOMPARE(cache.invalidate(key),
           AssetCache::InvalidateResult::PersistenceFailed);
  QCOMPARE(cache.invalidateAndRecordNegative404(key, preForkGeneration,
                                                /*nowMonotonicMs=*/1'000,
                                                /*ttlMs=*/60'000),
           AssetCache::InvalidateResult::PersistenceFailed);
  cache.reapAndEnforceQuota(); // must not crash/deadlock either.

  // Independent cumulative re-review (MEDIUM, "Pre-fork live AssetCache
  // objects remain usable in child"): the three read-only usage
  // accessors must ALSO fail closed to their documented safe sentinel
  // (0) rather than locking the inherited m_mutex, exactly like every
  // other public method exercised above.
  QCOMPARE(cache.memoryCostBytes(), 0);
  QCOMPARE(cache.diskUsageBytes(), 0);
  QCOMPARE(cache.diskEntryCount(), 0);

  // The pre-fork entry must remain COMPLETELY untouched by every one of
  // the rejected operations above -- neither evicted, invalidated, nor
  // silently mutated.
  AssetCache::setPreForkLiveInstanceForcedStateForTesting(false);
  const auto stillPresent = cache.lookupMemory(key);
  QVERIFY(stillPresent.has_value());
  QCOMPARE(stillPresent->encodedBytes, QByteArrayLiteral("pre-fork-bytes"));
  QVERIFY(cache.lookupDisk(key).has_value());

  // Recovery: once the simulated fork state clears (mirroring a real
  // exec()), this exact same instance resumes fully normal behavior --
  // this guard is a deliberate, temporary rejection, never a permanent
  // poisoning of the instance.
  const quint64 postClearGeneration = cache.issueKeyGeneration(otherKey);
  cache.store(otherKey, makeEntry(QByteArrayLiteral("resumed-bytes")),
              postClearGeneration);
  QVERIFY(cache.lookupMemory(otherKey).has_value());
  QVERIFY(cache.lookupDisk(otherKey).has_value());
}

void AssetCacheTests::
    realForkedChildAccessorsNeverDeadlockOnMutexHeldByParentAtForkTime() {
#if !defined(Q_OS_UNIX)
  QSKIP("fork() is a POSIX-specific mechanism; not applicable on this "
        "platform");
#else
  AssetCache parentCache(configFor(m_tempDirPath));
  QVERIFY(!parentCache.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/real-fork-mutex-hold.png")));
  parentCache.store(key, makeEntry(QByteArrayLiteral("real-fork-mutex-bytes")));
  QVERIFY(parentCache.lookupMemory(key).has_value());
  QVERIFY(parentCache.memoryCostBytes() > 0);

  // A real background thread genuinely acquires this instance's real
  // m_mutex and holds it -- synchronized deterministically via a
  // spin-wait on an atomic flag (no sleep-based timing race) so the
  // fork() below is GUARANTEED (not merely probabilistically likely)
  // to happen while it is locked.
  std::atomic<bool> holderLocked{false};
  std::atomic<bool> releaseHolder{false};
  std::thread holder([&]() {
    parentCache.lockMutexForTesting();
    holderLocked.store(true, std::memory_order_release);
    while (!releaseHolder.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    parentCache.unlockMutexForTesting();
  });
  while (!holderLocked.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  int pipeFds[2] = {-1, -1};
  QVERIFY(::pipe(pipeFds) == 0);

  const pid_t child = ::fork();
  QVERIFY(child >= 0);
  if (child == 0) {
    // CHILD: fork() only ever duplicates the CALLING thread (this
    // test's own main thread) -- the holder thread above simply does
    // not exist here, so this process's own copy of parentCache's
    // m_mutex is frozen, permanently, in a locked state nothing in
    // this process will ever unlock. Every accessor below MUST fail
    // closed via hasForkedSinceConstruction() strictly BEFORE ever
    // attempting to acquire that mutex -- if any one of them did not,
    // this child would deadlock forever right here, and this test's
    // own bounded poll()/waitpid() below would deterministically catch
    // it (as a reported failure, never an indefinite hang) rather than
    // silently pass. Deliberately does not construct any further Qt
    // object of its own -- see
    // forkedChildProcessNeverJoinsParentsInheritedRootAuthority()'s
    // comment for why that is a separate, independent hazard on this
    // platform, unrelated to the exact mechanism this test targets.
    ::close(pipeFds[0]);
    const qint64 mem = parentCache.memoryCostBytes();
    const qint64 disk = parentCache.diskUsageBytes();
    const int entries = parentCache.diskEntryCount();
    const bool memoryHit = parentCache.lookupMemory(key).has_value();
    const char verdict =
        (mem == 0 && disk == 0 && entries == 0 && !memoryHit) ? '1' : '0';
    ssize_t written = ::write(pipeFds[1], &verdict, 1);
    (void)written;
    ::close(pipeFds[1]);
    ::_exit(0);
  }

  // PARENT: bounded wait (never an indefinite blocking read) -- if a
  // regression reintroduces the deadlock this test targets, this must
  // fail deterministically rather than hang the entire test run
  // forever.
  ::close(pipeFds[1]);
  struct pollfd pfd {};
  pfd.fd = pipeFds[0];
  pfd.events = POLLIN;
  const int pollResult = ::poll(&pfd, 1, /*timeoutMs=*/5000);
  char verdict = '?';
  ssize_t bytesRead = -1;
  if (pollResult > 0 && (pfd.revents & POLLIN)) {
    bytesRead = ::read(pipeFds[0], &verdict, 1);
  }
  ::close(pipeFds[0]);

  if (pollResult <= 0) {
    // The child never reported anything within the timeout -- almost
    // certainly deadlocked on the inherited, permanently-locked mutex.
    // Forcibly reap it so this process does not leak a zombie, but the
    // test itself must still fail loudly rather than silently pass.
    ::kill(child, SIGKILL);
    int reapedStatus = 0;
    ::waitpid(child, &reapedStatus, 0);
    releaseHolder.store(true, std::memory_order_release);
    holder.join();
    QFAIL("forked child never reported a verdict within the timeout -- it "
          "likely deadlocked on the inherited, permanently-locked mutex");
  }

  int status = 0;
  const pid_t waited = ::waitpid(child, &status, 0);

  releaseHolder.store(true, std::memory_order_release);
  holder.join();

  QVERIFY2(waited == child, "waitpid() never reaped the forked child");
  QVERIFY2(WIFEXITED(status),
           qPrintable(QStringLiteral(
                          "forked child did not exit normally (raw status=%1, "
                          "WIFSIGNALED=%2, WTERMSIG=%3)")
                          .arg(status)
                          .arg(WIFSIGNALED(status) ? 1 : 0)
                          .arg(WIFSIGNALED(status) ? WTERMSIG(status) : -1)));
  QVERIFY2(bytesRead == 1, "forked child never reported a verdict");
  QCOMPARE(verdict, '1');

  // The parent's own view of both the mutex and the cache entry remain
  // completely unaffected: the holder thread released the real mutex
  // normally in THIS process, and none of the child's rejected
  // accessor calls ever touched this process's state at all.
  QVERIFY(parentCache.lookupMemory(key).has_value());
  QVERIFY(parentCache.memoryCostBytes() > 0);
#endif
}

void AssetCacheTests::
    siblingInvalidateImmediatelyClearsAnotherSiblingsMemoryView() {
  AssetCache first(configFor(m_tempDirPath));
  QVERIFY(!first.isDiskCacheDisabledForTesting());
  AssetCache second(configFor(m_tempDirPath));
  QVERIFY(!second.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/shared-memory-invalidate.png")));
  first.store(key, makeEntry(QByteArrayLiteral("shared-memory-bytes")));

  // Both siblings genuinely share one memory object -- see
  // RootAuthority's own comment in AssetCache.cpp.
  QVERIFY(first.lookupMemory(key).has_value());
  QVERIFY(second.lookupMemory(key).has_value());

  // The second sibling invalidates the key (e.g. an authoritative 404
  // discovered by a request that second, not first, happens to be
  // servicing).
  QCOMPARE(second.invalidate(key),
           AssetCache::InvalidateResult::DurablyInvalidated);

  // Fail-before/pass-after: prior to this fix, `first` kept its OWN
  // private QCache, so this entry would have remained memory-resident
  // in `first` indefinitely despite `second`'s durable invalidate --
  // this must now be instantly, unconditionally absent from BOTH
  // siblings' view, since it is literally the same shared object.
  QVERIFY(!first.lookupMemory(key).has_value());
  QVERIFY(!second.lookupMemory(key).has_value());
  QVERIFY(!first.lookupDisk(key).has_value());
}

void AssetCacheTests::
    staleIssuedGenerationTokenCannotPublishThroughAnyMutatingEntryPointAfterConcurrentInvalidate() {
  AssetCache first(configFor(m_tempDirPath));
  QVERIFY(!first.isDiskCacheDisabledForTesting());
  AssetCache second(configFor(m_tempDirPath));
  QVERIFY(!second.isDiskCacheDisabledForTesting());

  // --- store(): a token issued before a concurrent invalidate must
  // never be able to publish a brand-new entry afterward. ---
  {
    const QString key = AssetCache::cacheKeyFor(
        QUrl(QStringLiteral("https://example.com/stale-token-store.png")));
    const quint64 staleToken = first.issueKeyGeneration(key);
    QCOMPARE(
        second.invalidate(key),
        AssetCache::InvalidateResult::DurablyInvalidated); // advances the
                                                           // shared watermark
                                                           // past staleToken
    first.store(key, makeEntry(QByteArrayLiteral("must-never-publish")),
                staleToken);
    QVERIFY2(!first.lookupMemory(key).has_value(),
             "store() published using a token issued before a concurrent "
             "invalidate()");
    QVERIFY2(!first.lookupDisk(key).has_value(),
             "store() persisted to disk using a stale generation token");
  }

  // --- touchAfterNotModified(): a stale attempt's confirmed-current
  // revalidation must never clobber a NEWER, already-published entry's
  // metadata. This is the exact production race the review describes:
  // "delayed 200 vs 404/clear" (here: delayed 304-confirm vs.
  // clear-then-republish). ---
  {
    const QString key = AssetCache::cacheKeyFor(
        QUrl(QStringLiteral("https://example.com/stale-token-touch.png")));
    AssetCache::CachedEntry v1 = makeEntry(QByteArrayLiteral("touch-v1"));
    v1.etag = QStringLiteral("\"v1-etag\"");
    first.store(key, v1); // baseline, unconditional (legacy call site)
    QVERIFY(first.lookupDisk(key).has_value());

    // Instance A "begins" a revalidation attempt against v1, capturing
    // its token before anything else changes.
    const quint64 staleToken = first.issueKeyGeneration(key);

    // A durable invalidate, followed by a genuinely NEWER publish
    // (representing a fresh, legitimate fetch racing ahead of A's
    // in-flight revalidation) -- both via second, both unconditional
    // (as any call site not itself carrying the stale token legitimately
    // is).
    QCOMPARE(second.invalidate(key),
             AssetCache::InvalidateResult::DurablyInvalidated);
    AssetCache::CachedEntry v2 = makeEntry(QByteArrayLiteral("touch-v2"));
    v2.etag = QStringLiteral("\"v2-etag\"");
    second.store(key, v2);

    // A's stale revalidation now completes with a 304, carrying its OLD
    // token -- this must be rejected outright, never overwriting v2's
    // metadata with a "confirmed still v1" refresh.
    first.touchAfterNotModified(key, QStringLiteral("\"attacker-etag\""),
                                QString(), staleToken);

    const auto afterStaleTouch = first.lookupDisk(key);
    QVERIFY(afterStaleTouch.has_value());
    QCOMPARE(afterStaleTouch->etag, QStringLiteral("\"v2-etag\""));
    QCOMPARE(afterStaleTouch->encodedBytes, QByteArrayLiteral("touch-v2"));
  }

  // --- promoteToMemory(): a stale token must never even seed a
  // brand-new memory-only entry. ---
  {
    const QString key = AssetCache::cacheKeyFor(
        QUrl(QStringLiteral("https://example.com/stale-token-promote.png")));
    const quint64 staleToken = first.issueKeyGeneration(key);
    QCOMPARE(second.invalidate(key),
             AssetCache::InvalidateResult::DurablyInvalidated);
    first.promoteToMemory(
        key, makeEntry(QByteArrayLiteral("must-never-promote")), staleToken);
    QVERIFY2(!first.lookupMemory(key).has_value(),
             "promoteToMemory() published using a stale generation token");
  }

  // --- updateMemoryDecodedImage(): a stale token must never overwrite a
  // NEWER entry's decoded image. ---
  {
    const QString key = AssetCache::cacheKeyFor(
        QUrl(QStringLiteral("https://example.com/stale-token-decode.png")));
    first.store(key, makeEntry(QByteArrayLiteral("decode-v1")));
    const quint64 staleToken = first.issueKeyGeneration(key);
    QCOMPARE(second.invalidate(key),
             AssetCache::InvalidateResult::DurablyInvalidated);
    AssetCache::CachedEntry v2 = makeEntry(QByteArrayLiteral("decode-v2"));
    QImage legitimateImage(2, 2, QImage::Format_RGB32);
    legitimateImage.fill(Qt::green);
    v2.decodedImage = legitimateImage;
    second.store(key, v2);

    QImage attackerImage(2, 2, QImage::Format_RGB32);
    attackerImage.fill(Qt::red);
    first.updateMemoryDecodedImage(key, attackerImage, staleToken);

    const auto afterStaleUpdate = first.lookupMemory(key);
    QVERIFY(afterStaleUpdate.has_value());
    QVERIFY2(!afterStaleUpdate->decodedImage.isNull(),
             "the legitimate v2 decoded image was wrongly cleared");
    QCOMPARE(afterStaleUpdate->decodedImage.pixelColor(0, 0),
             QColor(Qt::green));
  }
}

void AssetCacheTests::
    unconditionalGenerationDefaultAlwaysPublishesEvenAfterConcurrentInvalidate() {
  AssetCache first(configFor(m_tempDirPath));
  QVERIFY(!first.isDiskCacheDisabledForTesting());
  AssetCache second(configFor(m_tempDirPath));
  QVERIFY(!second.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/unconditional-default.png")));

  // Even after an unrelated sibling invalidate advances the shared
  // watermark for this key, a caller that never opted into the CAS
  // protocol at all (the default kUnconditionalGeneration, exactly what
  // every existing call site in this file and every current
  // AssetRequestCoordinator call site still uses) must be completely
  // unaffected -- proving the new protocol is strictly opt-in, never a
  // silently-mandatory behavior change for pre-existing callers.
  QCOMPARE(second.invalidate(key),
           AssetCache::InvalidateResult::DurablyInvalidated);
  first.store(key, makeEntry(QByteArrayLiteral("unconditional-bytes")));
  QVERIFY(first.lookupMemory(key).has_value());
  QCOMPARE(first.lookupMemory(key)->encodedBytes,
           QByteArrayLiteral("unconditional-bytes"));

  first.promoteToMemory(key,
                        makeEntry(QByteArrayLiteral("unconditional-promoted")));
  QCOMPARE(first.lookupMemory(key)->encodedBytes,
           QByteArrayLiteral("unconditional-promoted"));

  QImage image(2, 2, QImage::Format_RGB32);
  image.fill(Qt::blue);
  first.updateMemoryDecodedImage(key, image);
  QCOMPARE(first.lookupMemory(key)->decodedImage.pixelColor(0, 0),
           QColor(Qt::blue));

  first.touchAfterNotModified(key, QStringLiteral("\"unconditional-etag\""),
                              QString());
  QCOMPARE(first.lookupDisk(key)->etag,
           QStringLiteral("\"unconditional-etag\""));
}

void AssetCacheTests::
    freshlyIssuedTokenAfterInvalidateCanStillPublishNormally() {
  AssetCache first(configFor(m_tempDirPath));
  QVERIFY(!first.isDiskCacheDisabledForTesting());
  AssetCache second(configFor(m_tempDirPath));
  QVERIFY(!second.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(QUrl(
      QStringLiteral("https://example.com/fresh-token-after-invalidate.png")));

  const quint64 staleToken = first.issueKeyGeneration(key);
  QCOMPARE(second.invalidate(key),
           AssetCache::InvalidateResult::DurablyInvalidated);
  // The stale token (issued before the invalidate) must fail...
  first.store(key, makeEntry(QByteArrayLiteral("must-never-publish")),
              staleToken);
  QVERIFY(!first.lookupMemory(key).has_value());

  // ...but a FRESH token, issued strictly AFTER the invalidate, must
  // succeed normally -- proving advanceKeyGenerationPastAllIssuedLocked()
  // only rejects tokens that predate it, never permanently poisons the
  // key against all future legitimate publication.
  const quint64 freshToken = first.issueKeyGeneration(key);
  QVERIFY2(freshToken > staleToken,
           "a token issued after invalidate() must exceed one issued "
           "before it");
  first.store(key, makeEntry(QByteArrayLiteral("legitimate-fresh-bytes")),
              freshToken);
  const auto afterFreshStore = first.lookupMemory(key);
  QVERIFY(afterFreshStore.has_value());
  QCOMPARE(afterFreshStore->encodedBytes,
           QByteArrayLiteral("legitimate-fresh-bytes"));

  // Further mutations using the SAME fresh token (representing later
  // stages of the same still-current attempt, e.g. a subsequent 304
  // revalidation or an on-demand decode publish) must also continue to
  // succeed.
  QImage image(2, 2, QImage::Format_RGB32);
  image.fill(Qt::yellow);
  first.updateMemoryDecodedImage(key, image, freshToken);
  QCOMPARE(first.lookupMemory(key)->decodedImage.pixelColor(0, 0),
           QColor(Qt::yellow));
}

void AssetCacheTests::oldIssuedNegative404CannotClobberNewerAppliedSuccess() {
  // Cumulative review (independent re-review round-5, HIGH, "Old 404 can
  // invalidate newer-issued/finished 200"): simulates two concurrent
  // logical attempts against the exact same cache key -- an OLDER one
  // (issued first, genOld) that is ultimately a 404, and a NEWER one
  // (issued second, genNew) that is ultimately a 200 -- completing in
  // the ADVERSARIAL order (the newer 200 finishes and publishes FIRST,
  // then the older, now-stale 404 arrives afterward). Before this round's
  // fix, the older attempt's negative-404 record was recorded
  // unconditionally by whichever coordinator instance owned it (never
  // gated against the shared applied-generation watermark at all), so it
  // could silently clobber the fresher success. recordNegative404() is
  // now gated by the exact same tryApplyKeyGenerationLocked() CAS check
  // store()/invalidate() already use, so the stale attempt is rejected.
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/old-404-vs-new-200.png")));

  const quint64 genOld = cache.issueKeyGeneration(key);
  const quint64 genNew = cache.issueKeyGeneration(key);
  QVERIFY2(genNew > genOld,
           "a later issuance must strictly exceed an earlier one");

  // The NEWER attempt's success publishes first (it happened to win the
  // network race despite starting second).
  cache.store(key, makeEntry(QByteArrayLiteral("newer-success-bytes")), genNew);
  QVERIFY(cache.lookupMemory(key).has_value());
  QCOMPARE(cache.lookupMemory(key)->encodedBytes,
           QByteArrayLiteral("newer-success-bytes"));

  // The OLDER attempt's definitive 404 arrives strictly afterward and
  // attempts to record a negative-404 tombstone using its own (now
  // stale) issued generation -- this must be silently rejected, never
  // resurrecting an authoritative absence over the fresher success.
  cache.recordNegative404(key, genOld, /*nowMonotonicMs=*/1'000,
                          /*ttlMs=*/60'000);

  QVERIFY2(!cache.hasNegative404ForTesting(key, /*nowMonotonicMs=*/1'000),
           "a stale, older-issued negative-404 attempt must never become "
           "authoritative once a newer generation has already applied a "
           "success for the same key");

  // The newer success must remain fully intact and unaffected -- neither
  // evicted, invalidated, nor shadowed by the rejected negative record.
  const auto stillFresh = cache.lookupMemory(key);
  QVERIFY(stillFresh.has_value());
  QCOMPARE(stillFresh->encodedBytes, QByteArrayLiteral("newer-success-bytes"));

  const AssetCache::KeyGenerationSnapshot snapshot =
      cache.snapshotAndIssueGeneration(key, /*nowMonotonicMs=*/1'000);
  QVERIFY(!snapshot.authoritativeNegative404);
  QVERIFY(snapshot.hit.has_value());
  QCOMPARE(snapshot.hit->encodedBytes,
           QByteArrayLiteral("newer-success-bytes"));
}

void AssetCacheTests::
    invalidateAndRecordNegative404SkipsStaleGenerationLeavingNewerSuccessIntact() {
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(QUrl(QStringLiteral(
      "https://example.com/invalidate-and-record-stale-skip.png")));

  const quint64 genOld = cache.issueKeyGeneration(key);
  const quint64 genNew = cache.issueKeyGeneration(key);
  QVERIFY2(genNew > genOld,
           "a later issuance must strictly exceed an earlier one");

  // The NEWER attempt's success publishes first.
  cache.store(key, makeEntry(QByteArrayLiteral("newer-success-bytes")), genNew);
  QVERIFY(cache.lookupMemory(key).has_value());

  // The OLDER attempt's definitive 404 arrives strictly afterward and
  // calls the SAME combined method the coordinator actually uses --
  // must be rejected wholesale (both the invalidate AND the
  // negative-404 record), never partially applied.
  const AssetCache::InvalidateResult result =
      cache.invalidateAndRecordNegative404(key, genOld,
                                           /*nowMonotonicMs=*/3'000,
                                           /*ttlMs=*/60'000);
  QCOMPARE(result, AssetCache::InvalidateResult::SkippedStaleGeneration);

  // The newer success must remain fully intact -- neither evicted nor
  // invalidated by the rejected stale attempt.
  const auto stillFresh = cache.lookupMemory(key);
  QVERIFY(stillFresh.has_value());
  QCOMPARE(stillFresh->encodedBytes, QByteArrayLiteral("newer-success-bytes"));

  // No negative-404 record was written at all -- the exact bug this
  // method exists to avoid (a plain invalidate() followed by a
  // separately CAS-gated recordNegative404() using the same already-
  // stale token would always self-reject silently, but here we prove
  // the combined method never even attempts the write).
  QVERIFY2(!cache.hasNegative404ForTesting(key, /*nowMonotonicMs=*/3'000),
           "a stale invalidateAndRecordNegative404() attempt must never "
           "record a negative-404, even transiently");
  QCOMPARE(cache.negative404RecordCountForTesting(), 0);

  const AssetCache::KeyGenerationSnapshot snapshot =
      cache.snapshotAndIssueGeneration(key, /*nowMonotonicMs=*/3'000);
  QVERIFY(!snapshot.authoritativeNegative404);
  QVERIFY(snapshot.hit.has_value());
  QCOMPARE(snapshot.hit->encodedBytes,
           QByteArrayLiteral("newer-success-bytes"));
}

void AssetCacheTests::
    negative404BecomesNonAuthoritativeOnceANewerSuccessAppliesForSameKey() {
  // Cumulative review (independent re-review round-5, HIGH, "negative
  // 404 is coordinator-local and can hide sibling-populated cache"): the
  // OPPOSITE completion order from the test above -- the older attempt's
  // 404 is recorded FIRST (while it is still legitimately current), and
  // only afterward does a newer attempt's success apply. The negative
  // record must stop being authoritative the instant the newer
  // generation applies, even though the record itself is never actively
  // cleared by store() (only recordNegative404()'s own CAS gate and
  // hasNegative404's exact-generation-match check are involved) -- proving
  // a sibling's fresher success is never permanently hidden behind an
  // earlier, now-superseded tombstone.
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(QUrl(
      QStringLiteral("https://example.com/negative-then-newer-success.png")));

  const quint64 genOld = cache.issueKeyGeneration(key);
  cache.recordNegative404(key, genOld, /*nowMonotonicMs=*/2'000,
                          /*ttlMs=*/60'000);
  QVERIFY2(cache.hasNegative404ForTesting(key, /*nowMonotonicMs=*/2'000),
           "a negative-404 recorded while still current must be "
           "authoritative");
  QCOMPARE(cache.negative404RecordCountForTesting(), 1);

  const AssetCache::KeyGenerationSnapshot beforeNewerSuccess =
      cache.snapshotAndIssueGeneration(key, /*nowMonotonicMs=*/2'000);
  QVERIFY(beforeNewerSuccess.authoritativeNegative404);
  QVERIFY(!beforeNewerSuccess.hit.has_value());
  const quint64 genNew = beforeNewerSuccess.issuedGeneration;
  QVERIFY2(genNew > genOld,
           "snapshotAndIssueGeneration() must mint a strictly newer token "
           "than the earlier negative-404 attempt's");

  cache.store(key, makeEntry(QByteArrayLiteral("later-sibling-success")),
              genNew);

  QVERIFY2(!cache.hasNegative404ForTesting(key, /*nowMonotonicMs=*/2'000),
           "a negative-404 record must stop being authoritative once a "
           "strictly newer generation has applied a success for the same "
           "key, even though the record itself was never actively "
           "cleared");

  const AssetCache::KeyGenerationSnapshot afterNewerSuccess =
      cache.snapshotAndIssueGeneration(key, /*nowMonotonicMs=*/2'000);
  QVERIFY2(!afterNewerSuccess.authoritativeNegative404,
           "the shared snapshot call must never report a stale negative "
           "record as authoritative once superseded");
  QVERIFY(afterNewerSuccess.hit.has_value());
  QCOMPARE(afterNewerSuccess.hit->encodedBytes,
           QByteArrayLiteral("later-sibling-success"));
}

void AssetCacheTests::
    siblingInstanceSnapshotAndIssueGenerationAtomicallyObservesOtherSiblingsPriorStore() {
  // Cumulative review (independent re-review round-5, HIGH, "cache
  // snapshot lookup then issuance in separate critical sections"): two
  // INDEPENDENT, simultaneously-live AssetCache instances sharing one
  // root (see sameProcessMultipleInstancesOverSameRootAllCooperateWith
  // FullDiskAuthority() above for the identical sharing setup this
  // relies on) -- a sibling's store() completed strictly BEFORE this
  // instance's own snapshotAndIssueGeneration() call must be fully,
  // atomically visible to it: both the hit AND the freshly minted
  // issuance token are produced by the SAME single locked critical
  // section on the shared authority, so there is no window in which a
  // caller could observe stale data paired with a token that appears
  // current.
  AssetCache first(configFor(m_tempDirPath));
  QVERIFY(!first.isDiskCacheDisabledForTesting());
  AssetCache second(configFor(m_tempDirPath));
  QVERIFY(!second.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/sibling-atomic-snapshot.png")));

  const quint64 firstToken = first.issueKeyGeneration(key);
  first.store(key, makeEntry(QByteArrayLiteral("first-sibling-bytes")),
              firstToken);

  // `second` never directly touched this key before -- its own
  // snapshotAndIssueGeneration() call must still see `first`'s already-
  // published entry via the shared memory tier, and mint a token
  // strictly newer than firstToken.
  const AssetCache::KeyGenerationSnapshot snapshot =
      second.snapshotAndIssueGeneration(key, /*nowMonotonicMs=*/3'000);
  QVERIFY(!snapshot.authoritativeNegative404);
  QVERIFY(snapshot.hit.has_value());
  QCOMPARE(snapshot.hit->encodedBytes,
           QByteArrayLiteral("first-sibling-bytes"));
  QVERIFY(snapshot.hitFromMemory);
  QVERIFY2(snapshot.issuedGeneration > firstToken,
           "a sibling's fresh issuance must strictly exceed a token issued "
           "by the OTHER sibling for the same key -- proving the issuance "
           "counter is genuinely shared, not per-instance");

  // The freshly minted token is immediately usable to publish through
  // `second` for the exact same key, proving it is a real, currently-
  // valid CAS baseline against the shared watermark -- not merely an
  // opaque, disconnected number.
  second.store(key, makeEntry(QByteArrayLiteral("second-sibling-bytes")),
               snapshot.issuedGeneration);
  QCOMPARE(first.lookupMemory(key)->encodedBytes,
           QByteArrayLiteral("second-sibling-bytes"));
}

void AssetCacheTests::
    siblingInstanceSnapshotAndIssueGenerationObservesOtherSiblingsNegative404() {
  // Companion to the test above: a negative-404 tombstone recorded by
  // ONE sibling instance must be immediately, fully authoritative from
  // the OTHER sibling instance's OWN snapshotAndIssueGeneration() call --
  // proving the negative-404 record now lives in the shared authority,
  // never a private, per-instance map that a sibling could bypass and
  // silently re-fetch a resource this process already confirmed absent.
  AssetCache first(configFor(m_tempDirPath));
  QVERIFY(!first.isDiskCacheDisabledForTesting());
  AssetCache second(configFor(m_tempDirPath));
  QVERIFY(!second.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(QUrl(
      QStringLiteral("https://example.com/sibling-shared-negative404.png")));

  const quint64 firstToken = first.issueKeyGeneration(key);
  first.recordNegative404(key, firstToken, /*nowMonotonicMs=*/4'000,
                          /*ttlMs=*/60'000);

  const AssetCache::KeyGenerationSnapshot snapshot =
      second.snapshotAndIssueGeneration(key, /*nowMonotonicMs=*/4'000);
  QVERIFY2(snapshot.authoritativeNegative404,
           "a negative-404 record written by one sibling instance must be "
           "authoritative when observed through a DIFFERENT sibling "
           "instance sharing the same root");
  QVERIFY(!snapshot.hit.has_value());
  QVERIFY2(second.hasNegative404ForTesting(key, /*nowMonotonicMs=*/4'000),
           "the public test hook must agree with the snapshot result when "
           "queried from the OTHER sibling instance");
}

void AssetCacheTests::
    outstandingTokenKeepsCasWatermarkAliveAcrossAggressivePruningUntilReleased() {
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());
  // Forces every issued key-generation token to be evicted the instant
  // it is no longer outstanding and idle at all -- see
  // touchAndPruneKeyGenerationMapsLocked()'s comment: a tracked-entry
  // cap of 1 means ANY subsequent issuance for a DIFFERENT key triggers
  // a full eviction sweep (0 itself is a "restore production default"
  // sentinel here, mirroring setMaxTrackedNegative404EntriesForTesting()
  // -- see this setter's own doc comment), and a zero idle threshold
  // makes every non-outstanding key immediately eligible.
  cache.setMaxTrackedKeyGenerationEntriesForTesting(1);
  cache.setKeyGenerationIdleEvictionThresholdMsForTesting(0);

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/outstanding-token-prune.png")));

  // An older token, deliberately never released -- standing in for a
  // request whose own completion handling has not run yet.
  const quint64 olderOutstandingToken = cache.issueKeyGeneration(key);
  // A strictly newer token for the SAME key, applied and then released
  // immediately (as every correctly-behaving caller does once it is
  // genuinely done with it) -- issued==applied is now true for `key`,
  // even though `olderOutstandingToken` is still outstanding.
  const quint64 newerToken = cache.issueKeyGeneration(key);
  QCOMPARE(
      cache.store(key, makeEntry(QByteArrayLiteral("newer-bytes")), newerToken),
      AssetCache::MutationOutcome::Applied);
  cache.releaseKeyGeneration(key, newerToken);

  // Trigger a prune sweep via an entirely UNRELATED key. `key` must
  // survive: its own older token is still genuinely outstanding.
  const QString otherKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/unrelated-prune-trigger.png")));
  cache.issueKeyGeneration(otherKey);

  // The decisive assertion: the older, still-outstanding token must
  // still correctly fail its CAS against the real (newer) applied
  // generation -- never wrongly succeed against a watermark a buggy
  // prune sweep reset to 0 while it was still in flight.
  QCOMPARE(cache.store(key, makeEntry(QByteArrayLiteral("stale-older-bytes")),
                       olderOutstandingToken),
           AssetCache::MutationOutcome::SkippedStaleGeneration);
  QVERIFY2(cache.lookupMemory(key).has_value(),
           "the newer entry must still be present in memory");
  QCOMPARE(cache.lookupMemory(key)->encodedBytes,
           QByteArrayLiteral("newer-bytes"));

  // Once genuinely released, `key` becomes eligible for eviction again
  // -- proving the fix does not simply pin every touched key forever
  // either. A fresh issuance for `key` after this eviction restarts its
  // counter from 0, the clearest possible signal that its prior
  // bookkeeping was actually reclaimed.
  cache.releaseKeyGeneration(key, olderOutstandingToken);
  const QString secondTriggerKey = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/second-unrelated-trigger.png")));
  cache.issueKeyGeneration(secondTriggerKey);

  QCOMPARE(cache.issueKeyGeneration(key), quint64{1});
}

void AssetCacheTests::
    oldTokenStoreCannotApplyWhileANewerTokenForTheSameKeyIsStillOutstanding() {
  // Independent cumulative re-review (HIGH, repeat finding, "tryApply
  // compares highest APPLIED, not latest issued"): this exact
  // scenario -- an older token attempting store() while a strictly
  // newer token for the same key is still outstanding, BEFORE either
  // has applied anything -- passed (wrongly) against the pre-fix code,
  // since it compared `issuedGeneration` only against the applied
  // watermark (still 0 here), never against what else was currently
  // outstanding.
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/old-outstanding-race.png")));

  const quint64 olderToken = cache.issueKeyGeneration(key);
  const quint64 newerToken = cache.issueKeyGeneration(key);
  QVERIFY2(newerToken > olderToken,
           "a later issuance must strictly exceed an earlier one");

  // Neither token has been released or applied anything yet -- both are
  // genuinely, currently outstanding for `key`.
  const AssetCache::MutationOutcome outcome = cache.store(
      key, makeEntry(QByteArrayLiteral("stale-older-bytes")), olderToken);
  QCOMPARE(outcome, AssetCache::MutationOutcome::SkippedStaleGeneration);
  QVERIFY2(!cache.lookupMemory(key).has_value(),
           "the rejected older token's store() must never have published "
           "anything at all -- not even transiently");

  // The newer token, still outstanding, must remain free to publish
  // normally afterward -- the older token's rejected attempt must never
  // have poisoned it.
  QCOMPARE(cache.store(key, makeEntry(QByteArrayLiteral("genuine-newer-bytes")),
                       newerToken),
           AssetCache::MutationOutcome::Applied);
  QVERIFY(cache.lookupMemory(key).has_value());
  QCOMPARE(cache.lookupMemory(key)->encodedBytes,
           QByteArrayLiteral("genuine-newer-bytes"));

  cache.releaseKeyGeneration(key, olderToken);
  cache.releaseKeyGeneration(key, newerToken);
}

void AssetCacheTests::
    oldTokenNegative404CannotBecomeAuthoritativeWhileANewerTokenIsStillOutstanding() {
  // Independent cumulative re-review (HIGH, repeat finding, exact
  // scenario named by the review: "Gen1 404 after Gen2 issuance is
  // accepted and can install tombstone/advance fallback"). Unlike
  // oldIssuedNegative404CannotClobberNewerAppliedSuccess() above (which
  // only proves the tombstone is rejected AFTER the newer generation
  // has already APPLIED a success), this proves the tombstone is
  // rejected even BEFORE the newer token has done anything at all --
  // purely because it is still outstanding. A tombstone that briefly
  // became authoritative here could have driven a real caller's
  // fallback-advance decision (localized -> English -> alternate
  // front) on stale information, even though it is corrected moments
  // later once the newer token's own real outcome is known.
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/old-404-outstanding-race.png")));

  const quint64 olderToken = cache.issueKeyGeneration(key);
  const quint64 newerToken = cache.issueKeyGeneration(key);
  QVERIFY2(newerToken > olderToken,
           "a later issuance must strictly exceed an earlier one");

  cache.recordNegative404(key, olderToken, /*nowMonotonicMs=*/1'000,
                          /*ttlMs=*/60'000);
  QVERIFY2(!cache.hasNegative404ForTesting(key, /*nowMonotonicMs=*/1'000),
           "an older token's negative-404 attempt must never become "
           "authoritative while a strictly newer token for the same key "
           "is still outstanding, even though neither has applied "
           "anything yet");
  QCOMPARE(cache.negative404RecordCountForTesting(), 0);

  const AssetCache::KeyGenerationSnapshot snapshot =
      cache.snapshotAndIssueGeneration(key, /*nowMonotonicMs=*/1'000);
  QVERIFY2(!snapshot.authoritativeNegative404,
           "a third, concurrently-arriving reader must never observe the "
           "rejected tombstone as authoritative either");
  cache.releaseKeyGeneration(key, snapshot.issuedGeneration);
  cache.releaseKeyGeneration(key, olderToken);
  cache.releaseKeyGeneration(key, newerToken);
}

void AssetCacheTests::
    bothOld404FirstAndOldSuccessFirstOrdersLeaveOnlyTheNewerTokenVisibleToAThirdReader() {
  // Independent cumulative re-review (HIGH, repeat finding: "Cover
  // old-404-first, both completion orders, third request..."). Two
  // independent keys, each modelling one of the two adversarial
  // completion orders end-to-end, with a genuine THIRD reader
  // (snapshotAndIssueGeneration()) checked immediately after each --
  // exactly modelling a concurrent request arriving in the exact race
  // window this finding describes.
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  // Order A: the OLDER token's 404 is attempted FIRST (while the newer
  // token is still outstanding, having decided nothing yet), then the
  // newer token's success applies afterward.
  {
    const QString key = AssetCache::cacheKeyFor(
        QUrl(QStringLiteral("https://example.com/order-a-404-then-200.png")));
    const quint64 olderToken = cache.issueKeyGeneration(key);
    const quint64 newerToken = cache.issueKeyGeneration(key);
    QVERIFY(newerToken > olderToken);

    cache.recordNegative404(key, olderToken, /*nowMonotonicMs=*/2'000,
                            /*ttlMs=*/60'000);
    QVERIFY(!cache.hasNegative404ForTesting(key, /*nowMonotonicMs=*/2'000));

    QCOMPARE(cache.store(key, makeEntry(QByteArrayLiteral("order-a-success")),
                         newerToken),
             AssetCache::MutationOutcome::Applied);

    const AssetCache::KeyGenerationSnapshot thirdReader =
        cache.snapshotAndIssueGeneration(key, /*nowMonotonicMs=*/2'000);
    QVERIFY2(!thirdReader.authoritativeNegative404,
             "order A: a third reader must never observe the rejected, "
             "superseded tombstone");
    QVERIFY2(thirdReader.hit.has_value(),
             "order A: a third reader must observe the genuine success");
    QCOMPARE(thirdReader.hit->encodedBytes,
             QByteArrayLiteral("order-a-success"));
    cache.releaseKeyGeneration(key, thirdReader.issuedGeneration);
    cache.releaseKeyGeneration(key, olderToken);
    cache.releaseKeyGeneration(key, newerToken);
  }

  // Order B: the OLDER token's SUCCESS is attempted first (while the
  // newer token is still outstanding), then the newer token's own
  // definitive 404 applies afterward.
  {
    const QString key = AssetCache::cacheKeyFor(
        QUrl(QStringLiteral("https://example.com/order-b-200-then-404.png")));
    const quint64 olderToken = cache.issueKeyGeneration(key);
    const quint64 newerToken = cache.issueKeyGeneration(key);
    QVERIFY(newerToken > olderToken);

    QCOMPARE(cache.store(key,
                         makeEntry(QByteArrayLiteral("order-b-stale-success")),
                         olderToken),
             AssetCache::MutationOutcome::SkippedStaleGeneration);
    QVERIFY2(!cache.lookupMemory(key).has_value(),
             "order B: the older token's stale success must never have "
             "published anything");

    cache.recordNegative404(key, newerToken, /*nowMonotonicMs=*/2'000,
                            /*ttlMs=*/60'000);
    QVERIFY2(
        cache.hasNegative404ForTesting(key, /*nowMonotonicMs=*/2'000),
        "order B: the newer token's genuine 404 must become authoritative");

    const AssetCache::KeyGenerationSnapshot thirdReader =
        cache.snapshotAndIssueGeneration(key, /*nowMonotonicMs=*/2'000);
    QVERIFY2(thirdReader.authoritativeNegative404,
             "order B: a third reader must observe the genuine, current "
             "absence");
    QVERIFY(!thirdReader.hit.has_value());
    cache.releaseKeyGeneration(key, thirdReader.issuedGeneration);
    cache.releaseKeyGeneration(key, olderToken);
    cache.releaseKeyGeneration(key, newerToken);
  }
}

void AssetCacheTests::
    oldTokenNeverBecomesAuthoritativeAgainAfterBlockingNewerTokenIsReleasedWithoutEverApplying() {
  // Independent cumulative re-review (HIGH, repeat finding,
  // "supersession uses highest currently outstanding... Maintain
  // monotonic latestIssued watermark independent of outstanding set...
  // Cancellation never retroactively authorizes older 200/304/404/
  // tombstone/fallback/delivery. Coalescing/resnapshot avoids
  // livelock. Reverse test 5790-5834; cover gen1, gen2 cancel, gen1
  // 404, third request"). This is the exact REVERSE of the removed
  // predecessor test, which incorrectly asserted that releasing a
  // blocking newer (gen2) token without ever applying it lets an
  // older (gen1) token's later retry succeed again -- exactly the bug
  // latestCommittedGenerationLocked() (see its own comment in
  // AssetCache.cpp) exists to close, since it is monotonic for every
  // real, write-intending attempt's own committed token and never
  // shrinks on release, unlike the removed outstanding-set ceiling.
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  const QString key = AssetCache::cacheKeyFor(
      QUrl(QStringLiteral("https://example.com/release-then-retry.png")));

  // gen1: the older token.
  const quint64 gen1 = cache.issueKeyGeneration(key);
  // gen2: strictly superseding, still-outstanding newer token.
  const quint64 gen2 = cache.issueKeyGeneration(key);
  QVERIFY2(gen2 > gen1, "a later issuance must strictly exceed an earlier "
                        "one");

  QCOMPARE(cache.store(key, makeEntry(QByteArrayLiteral("gen1-first-attempt")),
                       gen1),
           AssetCache::MutationOutcome::SkippedStaleGeneration);
  QVERIFY(!cache.lookupMemory(key).has_value());

  // gen2 cancel: gen2's own operation is abandoned entirely -- released
  // without ever calling store()/recordNegative404()/etc. Under the
  // OLD (buggy, outstanding-set) ceiling this would let gen1 become
  // authoritative again; under the fixed, monotonic ceiling it must
  // not.
  cache.releaseKeyGeneration(key, gen2);

  // gen1 404: gen1's own later attempt at a DIFFERENT kind of mutation
  // (a negative-404 tombstone, not just another store()) must also
  // never become authoritative, proving the fix applies uniformly
  // across every mutation kind gated by tryApplyKeyGenerationLocked(),
  // not merely store().
  cache.recordNegative404(key, gen1, /*nowMonotonicMs=*/5'000,
                          /*ttlMs=*/60'000);
  QVERIFY2(!cache.hasNegative404ForTesting(key, /*nowMonotonicMs=*/5'000),
           "gen1's negative-404 must never become authoritative once gen2 "
           "was ever issued, even though gen2 itself was released without "
           "ever applying anything");
  QCOMPARE(cache.negative404RecordCountForTesting(), 0);

  // A plain retry reusing gen1 itself must also still fail -- proves
  // the ceiling genuinely never regresses, independent of which
  // mutation kind is retried.
  QCOMPARE(cache.store(key, makeEntry(QByteArrayLiteral("gen1-retry")), gen1),
           AssetCache::MutationOutcome::SkippedStaleGeneration);
  QVERIFY(!cache.lookupMemory(key).has_value());

  // third request: a genuinely new, independent caller (a fresh
  // snapshotAndIssueGeneration(), modelling a concurrent third
  // request arriving in the exact race window) must observe neither
  // gen1's rejected tombstone nor any stale hit -- and its own,
  // strictly newer token (gen3) is the only one that can still
  // succeed.
  const AssetCache::KeyGenerationSnapshot thirdReader =
      cache.snapshotAndIssueGeneration(key, /*nowMonotonicMs=*/5'000);
  QVERIFY2(!thirdReader.authoritativeNegative404,
           "a third reader must never observe gen1's rejected tombstone as "
           "authoritative");
  QVERIFY(!thirdReader.hit.has_value());
  const quint64 gen3 = thirdReader.issuedGeneration;
  QVERIFY(gen3 > gen2);

  QCOMPARE(
      cache.store(key, makeEntry(QByteArrayLiteral("gen3-succeeds")), gen3),
      AssetCache::MutationOutcome::Applied);
  QVERIFY(cache.lookupMemory(key).has_value());
  QCOMPARE(cache.lookupMemory(key)->encodedBytes,
           QByteArrayLiteral("gen3-succeeds"));

  // Even after gen3's own genuine success, gen1's own retry must
  // STILL never become authoritative again, and must never clobber
  // gen3's already-published, genuinely current content.
  QCOMPARE(
      cache.store(key, makeEntry(QByteArrayLiteral("gen1-too-late")), gen1),
      AssetCache::MutationOutcome::SkippedStaleGeneration);
  QVERIFY(cache.lookupMemory(key).has_value());
  QCOMPARE(cache.lookupMemory(key)->encodedBytes,
           QByteArrayLiteral("gen3-succeeds"));

  cache.releaseKeyGeneration(key, gen1);
  cache.releaseKeyGeneration(key, gen3);
}

void AssetCacheTests::
    releasingTheLastOutstandingTokenForAKeyMakesItPrunableWithNoFurtherActivity() {
  // Independent cumulative re-review (MEDIUM, repeat finding, "release
  // does not prune... remains unbounded if no later activity"). Forces
  // eviction eligibility deterministically (zero idle threshold, see
  // setKeyGenerationIdleEvictionThresholdMsForTesting()'s own comment)
  // and issues tokens for MORE distinct keys than the tracked-entry cap
  // allows, keeping every one of them genuinely outstanding (never
  // released) throughout the whole issuance phase -- this is
  // deliberate: touchAndPruneKeyGenerationMapsLocked() already runs
  // once per issueKeyGenerationLocked() call even against the pre-fix
  // code, so if any token were released before the LAST issuance, that
  // later issuance's own sweep would immediately reclaim it, masking
  // the exact defect this test targets (a sweep that can ONLY ever be
  // triggered by a FUTURE issuance/invalidate for some key, never by a
  // release). Only once every token has been issued does this test
  // release them all -- with NO further issuance for ANY key
  // afterward -- and then proves the tracked map is bounded purely as
  // a consequence of release() itself. Fails against the pre-fix code
  // (which leaves the tracked map stuck at its peak size forever, since
  // nothing else could ever trigger another sweep); passes once
  // release() can also trigger the bounded sweep.
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  cache.setMaxTrackedKeyGenerationEntriesForTesting(4);
  cache.setKeyGenerationIdleEvictionThresholdMsForTesting(0);

  constexpr int kKeyCount = 16;
  QVector<QString> keys;
  QVector<quint64> tokens;
  keys.reserve(kKeyCount);
  tokens.reserve(kKeyCount);
  for (int i = 0; i < kKeyCount; ++i) {
    const QString key = AssetCache::cacheKeyFor(QUrl(
        QStringLiteral("https://example.com/prune-on-release-%1.png").arg(i)));
    keys.append(key);
    // Every earlier key's own token is still genuinely outstanding at
    // the moment each subsequent one is issued -- so none of THEIR
    // own issuance-triggered sweeps can evict anything yet, exactly
    // modelling every token still being genuinely in flight.
    tokens.append(cache.issueKeyGeneration(key));
  }
  QVERIFY2(cache.trackedKeyGenerationEntryCountForTesting() >= kKeyCount,
           "every one of these tokens must still be tracked while "
           "genuinely outstanding, regardless of the cap");

  for (int i = 0; i < kKeyCount; ++i) {
    cache.releaseKeyGeneration(keys[i], tokens[i]);
  }

  // No further issueKeyGeneration()/invalidate() call for ANY key
  // happens after this point -- the releases above are the only
  // events left that could possibly trigger a sweep.
  QVERIFY2(cache.trackedKeyGenerationEntryCountForTesting() <= 4,
           "the tracked key-generation map must remain bounded purely as "
           "a consequence of release() itself being able to trigger the "
           "bounded sweep -- no further issuance for any key ever "
           "occurred after the last of these releases");
}

void AssetCacheTests::
    releaseBurstAgainstProductionIdleThresholdIsStillBoundedByTheUnconditionalHardCap() {
  // Independent cumulative re-review (MEDIUM, repeat finding, "release
  // prunes but 15-minute idle threshold leaves >4096 young entries
  // forever when activity stops... Hard cap must immediately evict
  // eligible non-outstanding entries regardless soft idle... Test
  // default production threshold, burst/release/no later activity
  // bounded"). See this test's own header declaration comment for the
  // full rationale. Deliberately does NOT call
  // setKeyGenerationIdleEvictionThresholdMsForTesting() at all -- the
  // production default (15 real minutes) remains in effect throughout.
  AssetCache cache(configFor(m_tempDirPath));
  QVERIFY(!cache.isDiskCacheDisabledForTesting());

  cache.setMaxTrackedKeyGenerationEntriesForTesting(4);
  cache.setMaxTrackedKeyGenerationEntriesHardCapForTesting(8);

  constexpr int kKeyCount = 32;
  QVector<QString> keys;
  QVector<quint64> tokens;
  keys.reserve(kKeyCount);
  tokens.reserve(kKeyCount);
  for (int i = 0; i < kKeyCount; ++i) {
    const QString key = AssetCache::cacheKeyFor(
        QUrl(QStringLiteral("https://example.com/prune-hard-cap-burst-%1.png")
                 .arg(i)));
    keys.append(key);
    // Every earlier key's own token is still genuinely outstanding at
    // the moment each subsequent one is issued -- so none of THEIR own
    // issuance-triggered sweeps can evict anything yet, exactly
    // modelling every token still being genuinely in flight throughout
    // the whole burst.
    tokens.append(cache.issueKeyGeneration(key));
  }
  QVERIFY2(cache.trackedKeyGenerationEntryCountForTesting() >= kKeyCount,
           "every one of these tokens must still be tracked while "
           "genuinely outstanding, regardless of either cap");

  for (int i = 0; i < kKeyCount; ++i) {
    cache.releaseKeyGeneration(keys[i], tokens[i]);
  }

  // No further issueKeyGeneration()/invalidate() call for ANY key
  // happens after this point, and no real idle time has elapsed at
  // all (the production 15-minute threshold is nowhere close to
  // satisfied) -- the releases above, and specifically the
  // unconditional hard-cap backstop they trigger, are the only
  // mechanism left that could possibly bound this map within this
  // test's own real running time.
  QVERIFY2(cache.trackedKeyGenerationEntryCountForTesting() <= 8,
           "the tracked key-generation map must remain bounded by the "
           "unconditional hard-cap backstop alone, even though every "
           "evicted entry was touched moments ago and the production "
           "15-minute idle-eviction threshold was never overridden or "
           "satisfied");
}

void AssetCacheTests::
    mountTransitionFailsClosedWhenMountIdentificationIsUnavailableEvenWithOrdinaryOwnershipAndModeOnARealLocalMount() {
  // Independent cumulative re-review (MEDIUM, repeat finding, "remove
  // pathname fallback when STATX_MNT_ID unavailable; fail closed").
#if !defined(Q_OS_UNIX)
  QSKIP("this policy is POSIX-specific; not applicable on this platform");
#elif !defined(__linux__)
  QSKIP("STATX_MNT_ID is a Linux-specific concept; not applicable on this "
        "platform");
#else
  const QString dirPath =
      m_tempDirPath + QStringLiteral("/mount-id-unavailable-fail-closed");
  QVERIFY(QDir().mkpath(dirPath));
  // QDir::mkpath() already creates directories owned by this very
  // process's own real uid and neither group- nor world-writable --
  // ownership/mode are PERFECT here; only mount-id evidence is
  // withheld, exactly like
  // mountTransitionPolicyAcceptsOwnedNonWritableDestinationWhenFilesystemTypeQualifies()
  // above (whose otherwise-identical fixture correctly QUALIFIES with
  // mount-id evidence available), proving this refusal comes entirely
  // from the missing mount-id evidence, never from ownership/mode.
  QVERIFY(::chmod(QFile::encodeName(dirPath).constData(), S_IRWXU) == 0);

  // Force this descriptor's own STATX_MNT_ID to be reported
  // unavailable (openat2() itself remains available -- only mount-id
  // resolution is degraded) without needing any real legacy
  // kernel/glibc pairing.
  MountIdentificationDegradationGuard mountIdGuard(
      /*forceOpenat2Unavailable=*/false, /*forceMountIdUnavailable=*/true);

  const std::optional<bool> verdict =
      AssetCache::mountTransitionIsIndependentlyPolicyQualifiedForTesting(
          dirPath, /*isFinalAccountHomeTransition=*/true);
  QVERIFY(verdict.has_value());
  QVERIFY2(!*verdict,
           "a mount transition destination whose own STATX_MNT_ID is "
           "unavailable must fail closed -- never silently degrade to a "
           "spoofable pathname-based mountinfo correlation, even when "
           "this is a perfectly ordinary, correctly-owned real local "
           "directory on this environment's own genuinely trusted "
           "filesystem");
#endif
}
