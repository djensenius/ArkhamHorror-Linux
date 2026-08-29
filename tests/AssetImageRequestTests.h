#pragma once

#include <QObject>
#include <QTemporaryDir>
#include <memory>

// Tests for the QML-suitable AssetImageRequest state seam: Q_PROPERTY
// NOTIFY-driven status/image/progress/error/accessibleDescription
// transitions for both a successful load and a failed one, safe
// cancellation mid-flight, safe destruction mid-flight (no crash, no
// stale signal) for both a genuine in-flight network fetch AND an
// immediate (synchronous pre-network-error) completion -- the latter is
// the exact shape of a real use-after-free bug this class once had, since
// AssetRequestCoordinator::request() used to return an invalid handle for
// any immediately-queued completion, silently defeating this object's
// destructor's `if (m_handle.isValid())` cancel guard -- and that a fresh
// load() clears any previously-loaded image immediately rather than
// showing stale content throughout the new Loading phase.
class AssetImageRequestTests final : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void successfulLoadTransitionsIdleLoadingReady();
  void callerProvidedAccessibleDescriptionIsCarriedThroughAllStates();
  void failedLoadTransitionsIdleLoadingError();
  void cancelMidFlightReturnsToIdleWithoutFurtherSignals();
  void destructionMidFlightNeverEmitsAfterDestruction();
  void destructionImmediatelyAfterImmediateCompletionNeverCrashes();
  void reloadingWithNewKeyClearsPreviousImageDuringLoading();
  void reloadingAfterErrorEmitsErrorChangedWhenClearingStaleError();

private:
  QString m_tempDirPath;
  std::unique_ptr<QTemporaryDir> m_tempDir;
};
