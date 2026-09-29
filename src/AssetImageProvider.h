#pragma once

#include "AssetImageRequestCoordinator.h"
#include "AssetLocator.h"

#include <QMutex>
#include <QPointer>
#include <QQuickAsyncImageProvider>
#include <QQuickImageResponse>
#include <QSize>
#include <QThread>

#include <atomic>
#include <functional>

namespace Arkham {

class AssetCardImageResponse final : public QQuickImageResponse {
public:
  using CancelCallback = std::function<void()>;

  explicit AssetCardImageResponse(const QSize &requestedSize);
  ~AssetCardImageResponse() override;

  [[nodiscard]] QQuickTextureFactory *textureFactory() const override;
  [[nodiscard]] QString errorString() const override;
  void cancel() override;

  void setCancelCallback(CancelCallback callback);
  void complete(AssetOutcome<QImage> result);

private:
  QSize m_requestedSize;
  mutable QMutex m_mutex;
  QImage m_image;
  QString m_errorString;
  CancelCallback m_cancelCallback;
  bool m_finished{false};
  bool m_cancelled{false};
};

class AssetCardImageProvider final : public QQuickAsyncImageProvider {
public:
  AssetCardImageProvider();
  explicit AssetCardImageProvider(AssetImageRequestCoordinator::Config config);
  ~AssetCardImageProvider() override;

  [[nodiscard]] QQuickImageResponse *
  requestImageResponse(const QString &id, const QSize &requestedSize) override;

private:
  [[nodiscard]] static AssetOutcome<AssetLocator::CardImageKey>
  parseImageId(const QString &id);

  QThread m_workerThread;
  QPointer<AssetImageRequestCoordinator> m_coordinator;
  std::atomic<quint64> m_pendingResponseId{1};
};

} // namespace Arkham
