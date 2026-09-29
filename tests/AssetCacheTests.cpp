#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtTest>

#include "AssetCache.h"

namespace {

QByteArray encodePng(const QColor &color, const QSize &size = QSize(8, 8)) {
  QImage image(size, QImage::Format_RGBA8888);
  image.fill(color);

  QByteArray bytes;
  QBuffer buffer(&bytes);
  const bool opened = buffer.open(QIODevice::WriteOnly);
  if (!opened) {
    return {};
  }
  const bool saved = image.save(&buffer, "PNG");
  if (!saved) {
    return {};
  }
  return bytes;
}

QByteArray encodePatternPng(const QSize &size = QSize(64, 64)) {
  QImage image(size, QImage::Format_RGBA8888);
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      image.setPixelColor(x, y,
                          QColor((x * 37 + y * 11) % 256,
                                 (x * 17 + y * 43) % 256,
                                 (x * 67 + y * 29) % 256, 255));
    }
  }

  QByteArray bytes;
  QBuffer buffer(&bytes);
  const bool opened = buffer.open(QIODevice::WriteOnly);
  if (!opened) {
    return {};
  }
  const bool saved = image.save(&buffer, "PNG");
  if (!saved) {
    return {};
  }
  return bytes;
}

bool directoryAcceptsSaveFile(const QString &directory) {
  const QString probePath = QDir(directory).filePath(
      QStringLiteral(".arkham-cache-permission-probe"));
  bool writable = false;
  {
    QSaveFile probe(probePath);
    if (probe.open(QIODevice::WriteOnly)) {
      writable = probe.write("ok", 2) == 2 && probe.commit();
    }
  }
  QFile::remove(probePath);
  return writable;
}

QUrl assetUrl(QStringView suffix) {
  return QUrl(
      QStringLiteral("https://assets.example.test/img/%1.avif").arg(suffix));
}

bool firstPixelMatches(const QImage &image, const QColor &color) {
  return !image.isNull() && image.pixelColor(0, 0).rgba() == color.rgba();
}

void expectStored(const Arkham::AssetDecodeOutcome<QImage> &stored) {
  QVERIFY2(stored.has_value(),
           stored.has_value() ? "" : qPrintable(stored.error().message));
}

QString cachePath(const QTemporaryDir &dir, const QUrl &url) {
  return QDir(dir.path())
      .filePath(Arkham::AssetCache::diskCacheFileNameForUrl(url));
}

} // namespace

class AssetCacheTests final : public QObject {
  Q_OBJECT

private slots:
  void memoryHitReturnsDecodedImage();
  void diskHitAfterRestartReportsDiskSource();
  void diskEvictsLeastRecentlyUsedEntry();
  void memoryCapEvictsDecodedImages();
  void corruptedDiskEntriesAreDeletedAndMiss();
  void failedOverwriteDropsStaleDiskEntry();
  void memoryHitsRefreshDiskEvictionOrder();
  void oversizedEntryDoesNotEvictExistingDiskCache();
  void startupRemovesUnindexedRegularFiles();
  void unwritableRootFallsBackToMemoryOnlyWithDiagnostic();
  void differentUrlsUseDifferentDiskEntries();
  void restoreOverwritesExistingEntry();
};

void AssetCacheTests::memoryHitReturnsDecodedImage() {
  QTemporaryDir root;
  QVERIFY(root.isValid());

  Arkham::AssetCache cache({.rootDirectory = root.path()});
  const QColor red(220, 20, 20, 255);
  const QByteArray png = encodePng(red);
  QVERIFY(!png.isEmpty());
  const QUrl url = assetUrl(u"memory-hit");
  expectStored(cache.store(url, png, QStringLiteral("image/png")));

  const auto hit = cache.lookup(url);
  QCOMPARE(hit.source, Arkham::AssetCache::LookupSource::Memory);
  QVERIFY(firstPixelMatches(hit.image, red));
}

void AssetCacheTests::diskHitAfterRestartReportsDiskSource() {
  QTemporaryDir root;
  QVERIFY(root.isValid());

  const QColor green(20, 180, 20, 255);
  const QByteArray png = encodePng(green);
  QVERIFY(!png.isEmpty());
  const QUrl url = assetUrl(u"disk-hit");
  {
    Arkham::AssetCache cache({.rootDirectory = root.path()});
    expectStored(cache.store(url, png, QStringLiteral("image/png")));
  }

  Arkham::AssetCache restarted({.rootDirectory = root.path()});
  const auto hit = restarted.lookup(url);
  QCOMPARE(hit.source, Arkham::AssetCache::LookupSource::Disk);
  QVERIFY(firstPixelMatches(hit.image, green));
  QCOMPARE(restarted.lookup(url).source,
           Arkham::AssetCache::LookupSource::Memory);
}

void AssetCacheTests::diskEvictsLeastRecentlyUsedEntry() {
  const QByteArray png = encodePng(QColor(40, 90, 210, 255));
  QVERIFY(!png.isEmpty());
  qint64 singleEntryBytes = 0;
  {
    QTemporaryDir measuringRoot;
    QVERIFY(measuringRoot.isValid());
    Arkham::AssetCache measuringCache({.memoryMaxCostBytes = 0,
                                       .diskMaxBytes = 1024LL * 1024LL,
                                       .rootDirectory = measuringRoot.path()});
    expectStored(measuringCache.store(assetUrl(u"measure"), png,
                                      QStringLiteral("image/png")));
    singleEntryBytes = measuringCache.indexedDiskBytes();
    QVERIFY(singleEntryBytes > 0);
  }

  QTemporaryDir root;
  QVERIFY(root.isValid());
  Arkham::AssetCache cache({.memoryMaxCostBytes = 0,
                            .diskMaxBytes = (singleEntryBytes * 2) + 8,
                            .rootDirectory = root.path()});

  const QUrl first = assetUrl(u"lru-first");
  const QUrl second = assetUrl(u"lru-second");
  const QUrl third = assetUrl(u"lru-third");
  expectStored(cache.store(first, png, QStringLiteral("image/png")));
  expectStored(cache.store(second, png, QStringLiteral("image/png")));
  QCOMPARE(cache.lookup(first).source, Arkham::AssetCache::LookupSource::Disk);
  expectStored(cache.store(third, png, QStringLiteral("image/png")));

  QCOMPARE(cache.lookup(first).source, Arkham::AssetCache::LookupSource::Disk);
  QCOMPARE(cache.lookup(second).source, Arkham::AssetCache::LookupSource::Miss);
  QCOMPARE(cache.lookup(third).source, Arkham::AssetCache::LookupSource::Disk);
}

void AssetCacheTests::memoryCapEvictsDecodedImages() {
  QTemporaryDir root;
  QVERIFY(root.isValid());

  const QByteArray png = encodePng(QColor(160, 80, 20, 255), QSize(16, 16));
  QVERIFY(!png.isEmpty());
  const auto decoded = Arkham::decodeAssetImage(png);
  expectStored(decoded);
  const qint64 oneImageCost = decoded->sizeInBytes();

  Arkham::AssetCache cache(
      {.memoryMaxCostBytes = oneImageCost, .rootDirectory = root.path()});
  const QUrl first = assetUrl(u"memory-first");
  const QUrl second = assetUrl(u"memory-second");
  expectStored(cache.store(first, png, QStringLiteral("image/png")));
  expectStored(cache.store(second, png, QStringLiteral("image/png")));

  QCOMPARE(cache.lookup(second).source,
           Arkham::AssetCache::LookupSource::Memory);
  QCOMPARE(cache.lookup(first).source, Arkham::AssetCache::LookupSource::Disk);
  QCOMPARE(cache.lookup(first).source,
           Arkham::AssetCache::LookupSource::Memory);
  QCOMPARE(cache.lookup(second).source, Arkham::AssetCache::LookupSource::Disk);
}

void AssetCacheTests::corruptedDiskEntriesAreDeletedAndMiss() {
  const QByteArray png = encodePng(QColor(30, 30, 220, 255));
  QVERIFY(!png.isEmpty());

  auto checkCorruption = [&](auto corruptFile) {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QUrl url = assetUrl(u"corrupt");
    const QString path = cachePath(root, url);
    Arkham::AssetCache cache({.rootDirectory = root.path()});
    expectStored(cache.store(url, png, QStringLiteral("image/png")));
    cache.clearMemory();
    QVERIFY(QFileInfo::exists(path));

    corruptFile(path);
    QCOMPARE(cache.lookup(url).source, Arkham::AssetCache::LookupSource::Miss);
    QVERIFY(!QFileInfo::exists(path));
  };

  checkCorruption([](const QString &path) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.seek(file.size() - 1));
    const QByteArray original = file.read(1);
    QVERIFY(original.size() == 1);
    QVERIFY(file.seek(file.size() - 1));
    const char flipped = static_cast<char>(original.front() ^ 0x01);
    QCOMPARE(file.write(&flipped, 1), 1);
  });
  checkCorruption([](const QString &path) {
    QFile file(path);
    QVERIFY(file.resize(8));
  });
  checkCorruption([](const QString &path) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write("bad header\n\nnot an image") > 0);
  });
}

void AssetCacheTests::failedOverwriteDropsStaleDiskEntry() {
  QTemporaryDir root;
  QVERIFY(root.isValid());

  const QUrl url = assetUrl(u"failed-overwrite");
  const QByteArray redPng = encodePng(QColor(220, 0, 0, 255));
  const QByteArray bluePng = encodePng(QColor(0, 0, 220, 255));
  QVERIFY(!redPng.isEmpty());
  QVERIFY(!bluePng.isEmpty());
  {
    Arkham::AssetCache cache({.memoryMaxCostBytes = 0,
                              .diskMaxBytes = 1024LL * 1024LL,
                              .rootDirectory = root.path()});
    expectStored(cache.store(url, redPng, QStringLiteral("image/png")));
    QCOMPARE(cache.lookup(url).source, Arkham::AssetCache::LookupSource::Disk);

    QVERIFY(QFile::setPermissions(root.path(), QFileDevice::ReadOwner |
                                                   QFileDevice::ExeOwner));
    if (directoryAcceptsSaveFile(root.path())) {
      QVERIFY(QFile::setPermissions(root.path(), QFileDevice::ReadOwner |
                                                     QFileDevice::WriteOwner |
                                                     QFileDevice::ExeOwner));
      QSKIP("chmod did not make the temporary cache root unwritable");
    }

    expectStored(cache.store(url, bluePng, QStringLiteral("image/png")));
    QVERIFY(!cache.diagnostic().isEmpty());
    QVERIFY(QFile::setPermissions(root.path(), QFileDevice::ReadOwner |
                                                   QFileDevice::WriteOwner |
                                                   QFileDevice::ExeOwner));
    cache.clearMemory();
    QCOMPARE(cache.lookup(url).source, Arkham::AssetCache::LookupSource::Miss);
  }

  Arkham::AssetCache restarted({.memoryMaxCostBytes = 0,
                                .diskMaxBytes = 1024LL * 1024LL,
                                .rootDirectory = root.path()});
  QCOMPARE(restarted.lookup(url).source,
           Arkham::AssetCache::LookupSource::Miss);
}

void AssetCacheTests::memoryHitsRefreshDiskEvictionOrder() {
  const QByteArray png = encodePng(QColor(40, 90, 210, 255));
  QVERIFY(!png.isEmpty());
  qint64 singleEntryBytes = 0;
  {
    QTemporaryDir measuringRoot;
    QVERIFY(measuringRoot.isValid());
    Arkham::AssetCache measuringCache({.memoryMaxCostBytes = 0,
                                       .diskMaxBytes = 1024LL * 1024LL,
                                       .rootDirectory = measuringRoot.path()});
    expectStored(measuringCache.store(assetUrl(u"memory-lru-measure"), png,
                                      QStringLiteral("image/png")));
    singleEntryBytes = measuringCache.indexedDiskBytes();
    QVERIFY(singleEntryBytes > 0);
  }

  QTemporaryDir root;
  QVERIFY(root.isValid());
  Arkham::AssetCache cache({.diskMaxBytes = (singleEntryBytes * 2) + 8,
                            .rootDirectory = root.path()});

  const QUrl first = assetUrl(u"memory-lru-first");
  const QUrl second = assetUrl(u"memory-lru-second");
  const QUrl third = assetUrl(u"memory-lru-third");
  expectStored(cache.store(first, png, QStringLiteral("image/png")));
  expectStored(cache.store(second, png, QStringLiteral("image/png")));
  QCOMPARE(cache.lookup(first).source,
           Arkham::AssetCache::LookupSource::Memory);
  expectStored(cache.store(third, png, QStringLiteral("image/png")));

  cache.clearMemory();
  QCOMPARE(cache.lookup(first).source, Arkham::AssetCache::LookupSource::Disk);
  QCOMPARE(cache.lookup(second).source, Arkham::AssetCache::LookupSource::Miss);
  QCOMPARE(cache.lookup(third).source, Arkham::AssetCache::LookupSource::Disk);
}

void AssetCacheTests::oversizedEntryDoesNotEvictExistingDiskCache() {
  QTemporaryDir measuringRoot;
  QVERIFY(measuringRoot.isValid());
  const QByteArray smallPng = encodePng(QColor(12, 34, 56, 255));
  const QByteArray largePng = encodePatternPng();
  QVERIFY(!smallPng.isEmpty());
  QVERIFY(!largePng.isEmpty());
  QVERIFY(largePng.size() > smallPng.size());

  qint64 smallEntryBytes = 0;
  {
    Arkham::AssetCache measuringCache({.memoryMaxCostBytes = 0,
                                       .diskMaxBytes = 1024LL * 1024LL,
                                       .rootDirectory = measuringRoot.path()});
    expectStored(measuringCache.store(assetUrl(u"oversize-measure"), smallPng,
                                      QStringLiteral("image/png")));
    smallEntryBytes = measuringCache.indexedDiskBytes();
    QVERIFY(smallEntryBytes > 0);
  }

  QTemporaryDir root;
  QVERIFY(root.isValid());
  Arkham::AssetCache cache({.memoryMaxCostBytes = 0,
                            .diskMaxBytes = smallEntryBytes + 8,
                            .rootDirectory = root.path()});
  const QUrl existing = assetUrl(u"oversize-existing");
  const QUrl oversized = assetUrl(u"oversize-new");
  expectStored(cache.store(existing, smallPng, QStringLiteral("image/png")));
  QCOMPARE(cache.lookup(existing).source,
           Arkham::AssetCache::LookupSource::Disk);

  expectStored(cache.store(oversized, largePng, QStringLiteral("image/png")));
  QVERIFY(!cache.diagnostic().isEmpty());
  cache.clearMemory();
  QCOMPARE(cache.lookup(existing).source,
           Arkham::AssetCache::LookupSource::Disk);
  QCOMPARE(cache.lookup(oversized).source,
           Arkham::AssetCache::LookupSource::Miss);
}

void AssetCacheTests::startupRemovesUnindexedRegularFiles() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const QString strayPath =
      QDir(root.path()).filePath(QStringLiteral("qsavefile-temp"));
  const QString zeroHashPath =
      QDir(root.path()).filePath(QString(64, QLatin1Char('a')));
  const QString invalidHashPath =
      QDir(root.path())
          .filePath(QString(63, QLatin1Char('b')) + QStringLiteral("g"));

  {
    QFile stray(strayPath);
    QVERIFY(stray.open(QIODevice::WriteOnly));
    QCOMPARE(stray.write("leftover", 8), 8);
  }
  {
    QFile zeroHash(zeroHashPath);
    QVERIFY(zeroHash.open(QIODevice::WriteOnly));
  }
  {
    QFile invalidHash(invalidHashPath);
    QVERIFY(invalidHash.open(QIODevice::WriteOnly));
    QCOMPARE(invalidHash.write("invalid", 7), 7);
  }

  Arkham::AssetCache cache({.rootDirectory = root.path()});
  QCOMPARE(cache.diskStatus(), Arkham::AssetCache::DiskStatus::Enabled);
  QVERIFY(!QFileInfo::exists(strayPath));
  QVERIFY(!QFileInfo::exists(zeroHashPath));
  QVERIFY(!QFileInfo::exists(invalidHashPath));
}

void AssetCacheTests::unwritableRootFallsBackToMemoryOnlyWithDiagnostic() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const QString cacheRoot = QDir(root.path()).filePath(QStringLiteral("cache"));
  QVERIFY(QDir().mkpath(cacheRoot));
  QVERIFY(QFile::setPermissions(cacheRoot, QFileDevice::ReadOwner |
                                               QFileDevice::ExeOwner));
  if (directoryAcceptsSaveFile(cacheRoot)) {
    QVERIFY(QFile::setPermissions(cacheRoot, QFileDevice::ReadOwner |
                                                 QFileDevice::WriteOwner |
                                                 QFileDevice::ExeOwner));
    QSKIP("chmod did not make the temporary cache root unwritable");
  }

  QTest::ignoreMessage(
      QtWarningMsg,
      QRegularExpression(QStringLiteral(
          "AssetCache disk cache unavailable; using memory-only cache:.*")));
  Arkham::AssetCache cache({.rootDirectory = cacheRoot});
  QCOMPARE(cache.diskStatus(), Arkham::AssetCache::DiskStatus::MemoryOnly);
  QVERIFY(!cache.diagnostic().isEmpty());

  const QColor purple(140, 20, 180, 255);
  const QByteArray png = encodePng(purple);
  QVERIFY(!png.isEmpty());
  const QUrl url = assetUrl(u"unwritable");
  expectStored(cache.store(url, png, QStringLiteral("image/png")));
  const auto hit = cache.lookup(url);
  QCOMPARE(hit.source, Arkham::AssetCache::LookupSource::Memory);
  QVERIFY(firstPixelMatches(hit.image, purple));

  QVERIFY(QFile::setPermissions(cacheRoot, QFileDevice::ReadOwner |
                                               QFileDevice::WriteOwner |
                                               QFileDevice::ExeOwner));
}

void AssetCacheTests::differentUrlsUseDifferentDiskEntries() {
  QTemporaryDir root;
  QVERIFY(root.isValid());

  const QUrl first = assetUrl(u"different-one");
  const QUrl second = assetUrl(u"different-two");
  const QString firstName = Arkham::AssetCache::diskCacheFileNameForUrl(first);
  const QString secondName =
      Arkham::AssetCache::diskCacheFileNameForUrl(second);
  QVERIFY(!firstName.isEmpty());
  QVERIFY(!secondName.isEmpty());
  QVERIFY(firstName != secondName);

  const QColor red(210, 10, 10, 255);
  const QColor blue(10, 10, 210, 255);
  const QByteArray redPng = encodePng(red);
  const QByteArray bluePng = encodePng(blue);
  QVERIFY(!redPng.isEmpty());
  QVERIFY(!bluePng.isEmpty());
  Arkham::AssetCache cache({.rootDirectory = root.path()});
  expectStored(cache.store(first, redPng, QStringLiteral("image/png")));
  expectStored(cache.store(second, bluePng, QStringLiteral("image/png")));
  cache.clearMemory();

  const auto firstHit = cache.lookup(first);
  const auto secondHit = cache.lookup(second);
  QCOMPARE(firstHit.source, Arkham::AssetCache::LookupSource::Disk);
  QCOMPARE(secondHit.source, Arkham::AssetCache::LookupSource::Disk);
  QVERIFY(firstPixelMatches(firstHit.image, red));
  QVERIFY(firstPixelMatches(secondHit.image, blue));
}

void AssetCacheTests::restoreOverwritesExistingEntry() {
  QTemporaryDir root;
  QVERIFY(root.isValid());

  const QUrl url = assetUrl(u"overwrite");
  const QColor red(220, 0, 0, 255);
  const QColor blue(0, 0, 220, 255);
  const QByteArray redPng = encodePng(red);
  const QByteArray bluePng = encodePng(blue);
  QVERIFY(!redPng.isEmpty());
  QVERIFY(!bluePng.isEmpty());
  Arkham::AssetCache cache(
      {.memoryMaxCostBytes = 0, .rootDirectory = root.path()});
  expectStored(cache.store(url, redPng, QStringLiteral("image/png")));
  expectStored(cache.store(url, bluePng, QStringLiteral("image/png")));

  const auto hit = cache.lookup(url);
  QCOMPARE(hit.source, Arkham::AssetCache::LookupSource::Disk);
  QVERIFY(firstPixelMatches(hit.image, blue));
}

QTEST_APPLESS_MAIN(AssetCacheTests)

#include "AssetCacheTests.moc"
