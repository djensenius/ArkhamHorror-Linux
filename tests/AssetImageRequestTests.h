#pragma once

#include <QObject>
#include <QTemporaryDir>
#include <memory>

// Tests for the QML-suitable AssetImageRequest state seam: Q_PROPERTY
// NOTIFY-driven status/image/progress/error/accessibleDescription
// transitions for both a successful load and a failed one, safe
// cancellation mid-flight, and safe destruction mid-flight (no crash, no
// stale signal).
class AssetImageRequestTests final : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void successfulLoadTransitionsIdleLoadingReady();
  void failedLoadTransitionsIdleLoadingError();
  void cancelMidFlightReturnsToIdleWithoutFurtherSignals();
  void destructionMidFlightNeverEmitsAfterDestruction();

private:
  QString m_tempDirPath;
  std::unique_ptr<QTemporaryDir> m_tempDir;
};
