#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include "AssetCache.h"

namespace {

QByteArray encodePng(const QColor &color, const QSize &size = QSize(8, 8)) {
  QImage image(size, QImage::Format_RGBA8888);
  image.fill(color);

  QByteArray bytes;
  QBuffer buffer(&bytes);
  Q_ASSERT(buffer.open(QIODevice::WriteOnly));
  Q_ASSERT(image.save(&buffer, "PNG"));
  return bytes;
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
  void unwritableRootFallsBackToMemoryOnlyWithDiagnostic();
  void differentUrlsUseDifferentDiskEntries();
  void restoreOverwritesExistingEntry();
};

void AssetCacheTests::memoryHitReturnsDecodedImage() {
  QTemporaryDir root;
  QVERIFY(root.isValid());

  Arkham::AssetCache cache({.rootDirectory = root.path()});
  const QColor red(220, 20, 20, 255);
  const QUrl url = assetUrl(u"memory-hit");
  expectStored(cache.store(url, encodePng(red), QStringLiteral("image/png")));

  const auto hit = cache.lookup(url);
  QCOMPARE(hit.source, Arkham::AssetCache::LookupSource::Memory);
  QVERIFY(firstPixelMatches(hit.image, red));
}

void AssetCacheTests::diskHitAfterRestartReportsDiskSource() {
  QTemporaryDir root;
  QVERIFY(root.isValid());

  const QColor green(20, 180, 20, 255);
  const QUrl url = assetUrl(u"disk-hit");
  {
    Arkham::AssetCache cache({.rootDirectory = root.path()});
    expectStored(
        cache.store(url, encodePng(green), QStringLiteral("image/png")));
  }

  Arkham::AssetCache restarted({.rootDirectory = root.path()});
  const auto hit = restarted.lookup(url);
  QCOMPARE(hit.source, Arkham::AssetCache::LookupSource::Disk);
  QVERIFY(firstPixelMatches(hit.image, green));
}

void AssetCacheTests::diskEvictsLeastRecentlyUsedEntry() {
  const QByteArray png = encodePng(QColor(40, 90, 210, 255));
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
}

void AssetCacheTests::corruptedDiskEntriesAreDeletedAndMiss() {
  const QByteArray png = encodePng(QColor(30, 30, 220, 255));

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

void AssetCacheTests::unwritableRootFallsBackToMemoryOnlyWithDiagnostic() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const QString cacheRoot = QDir(root.path()).filePath(QStringLiteral("cache"));
  QVERIFY(QDir().mkpath(cacheRoot));
  QVERIFY(QFile::setPermissions(cacheRoot, QFileDevice::ReadOwner |
                                               QFileDevice::ExeOwner));

  QTest::ignoreMessage(
      QtWarningMsg,
      QRegularExpression(QStringLiteral(
          "AssetCache disk cache unavailable; using memory-only cache:.*")));
  Arkham::AssetCache cache({.rootDirectory = cacheRoot});
  QCOMPARE(cache.diskStatus(), Arkham::AssetCache::DiskStatus::MemoryOnly);
  QVERIFY(!cache.diagnostic().isEmpty());

  const QColor purple(140, 20, 180, 255);
  const QUrl url = assetUrl(u"unwritable");
  expectStored(
      cache.store(url, encodePng(purple), QStringLiteral("image/png")));
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
  Arkham::AssetCache cache({.rootDirectory = root.path()});
  expectStored(cache.store(first, encodePng(red), QStringLiteral("image/png")));
  expectStored(
      cache.store(second, encodePng(blue), QStringLiteral("image/png")));
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
  Arkham::AssetCache cache(
      {.memoryMaxCostBytes = 0, .rootDirectory = root.path()});
  expectStored(cache.store(url, encodePng(red), QStringLiteral("image/png")));
  expectStored(cache.store(url, encodePng(blue), QStringLiteral("image/png")));

  const auto hit = cache.lookup(url);
  QCOMPARE(hit.source, Arkham::AssetCache::LookupSource::Disk);
  QVERIFY(firstPixelMatches(hit.image, blue));
}

QTEST_APPLESS_MAIN(AssetCacheTests)

#include "AssetCacheTests.moc"
