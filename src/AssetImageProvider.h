#pragma once

#include "AssetImageRequestCoordinator.h"
#include "AssetLocator.h"

#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QQuickImageResponse>
#include <QSize>
#include <QThread>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

class QTimer;

namespace Arkham {

struct AssetCardImageResponseState {
  std::mutex mutex;
  bool alive{true};
  bool cancelled{false};
  bool finished{false};
  std::optional<AssetOutcome<QImage>> result;
  std::function<void()> cancelCallback;
  std::optional<quint64> requestId;
};

class AssetCardImageResponse final : public QQuickImageResponse {
public:
  using CancelCallback = std::function<void()>;

  explicit AssetCardImageResponse(const QSize &requestedSize);
  ~AssetCardImageResponse() override;

  [[nodiscard]] QQuickTextureFactory *textureFactory() const override;
  [[nodiscard]] QString errorString() const override;
  void cancel() override;

  [[nodiscard]] std::shared_ptr<AssetCardImageResponseState> state() const {
    return m_state;
  }
  void setCancelCallback(CancelCallback callback);
  void complete(AssetOutcome<QImage> result);
  static void completeState(std::shared_ptr<AssetCardImageResponseState> state,
                            AssetOutcome<QImage> result);

private:
  void drainCompletion();

  QSize m_requestedSize;
  mutable QMutex m_mutex;
  QImage m_image;
  QString m_errorString;
  std::shared_ptr<AssetCardImageResponseState> m_state;
  QTimer *m_completionTimer{nullptr};
};

class AssetCardImageProvider final : public QQuickAsyncImageProvider {
public:
  AssetCardImageProvider();
  explicit AssetCardImageProvider(AssetImageRequestCoordinator::Config config);
  ~AssetCardImageProvider() override;

  [[nodiscard]] QQuickImageResponse *
  requestImageResponse(const QString &id, const QSize &requestedSize) override;

private:
  struct CoordinatorHandle {
    std::mutex mutex;
    bool alive{true};
    AssetImageRequestCoordinator *coordinator{nullptr};
  };

  [[nodiscard]] static AssetOutcome<AssetLocator::CardImageKey>
  parseImageId(const QString &id);

  QThread m_workerThread;
  std::shared_ptr<CoordinatorHandle> m_coordinatorHandle;
};

} // namespace Arkham
