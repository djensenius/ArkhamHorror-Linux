#pragma once

#include <QObject>
#include <QTemporaryDir>
#include <memory>

// Tests for AssetCache: memory/disk hit ordering, SHA-256 cache-key
// namespace isolation, atomic payload+metadata publication and
// crash/corruption repair (orphan payload, orphan metadata, mismatched
// pair, stray temp file), metadata-driven LRU quota/watermark eviction,
// touch-after-304 lastAccess refresh, and restart persistence (a fresh
// AssetCache instance pointed at the same directory sees prior entries).
class AssetCacheTests final : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void storeThenLookupMemoryAndDiskRoundTrips();
  void cacheKeyIsNamespacedByFullResolvedUrl();
  void orphanPayloadWithoutMetadataIsRepaired();
  void orphanMetadataWithoutPayloadIsRepaired();
  void mismatchedPayloadMetadataPairIsRepaired();
  void strayFileNotMatchingKeyShapeIsRemoved();
  void quotaEvictsOldestAccessFirstDownToLowWaterMark();
  void touchAfterNotModifiedRefreshesLastAccessAndHeaders();
  void restartingWithSameDirectorySeesPriorEntries();

private:
  QString m_tempDirPath;
  std::unique_ptr<QTemporaryDir> m_tempDir;
};
