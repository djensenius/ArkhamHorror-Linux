#include "AssetImageProvider.h"

#include <QMutexLocker>
#include <QQuickTextureFactory>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <utility>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

AssetError invalidProviderId(const QString &message) {
  return AssetError{AssetErrorCode::InvalidAssetKey, message};
}

QString normalizedMutationSuffix(QString mutation) {
  if (mutation.isEmpty()) {
    return {};
  }
  if (!mutation.startsWith(u'_')) {
    mutation.prepend(u'_');
  }
  return mutation;
}

AssetOutcome<QString> splitMutationSuffix(QString *cardCode) {
  const qsizetype marker = cardCode->indexOf("_Mutated"_L1);
  if (marker < 0) {
    return QString();
  }
  const QString suffix = cardCode->mid(marker);
  if (!AssetLocator::isMutationSuffix(suffix)) {
    return invalidProviderId(
        QStringLiteral("mutation suffix must match _Mutated<N>"));
  }
  cardCode->truncate(marker);
  return suffix;
}

AssetOutcome<AssetLocator::CardFace> parseFace(const QString &text) {
  if (text.isEmpty() || text == "front"_L1) {
    return AssetLocator::CardFace::Front;
  }
  if (text == "back"_L1) {
    return AssetLocator::CardFace::Back;
  }
  return invalidProviderId(
      QStringLiteral("card image face must be front or back"));
}

QString imageErrorString(const AssetError &error) {
  if (error.message.isEmpty()) {
    return QStringLiteral("card image request failed");
  }
  if (error.url.isEmpty()) {
    return error.message;
  }
  return QStringLiteral("%1: %2").arg(error.message, error.url.toString());
}

QImage scaledForRequest(const QImage &image, const QSize &requestedSize) {
  if (image.isNull() || !requestedSize.isValid() ||
      (requestedSize.width() <= 0 && requestedSize.height() <= 0)) {
    return image;
  }
  if (requestedSize.width() > 0 && requestedSize.height() <= 0) {
    return image.scaledToWidth(requestedSize.width(), Qt::SmoothTransformation);
  }
  if (requestedSize.height() > 0 && requestedSize.width() <= 0) {
    return image.scaledToHeight(requestedSize.height(),
                                Qt::SmoothTransformation);
  }
  return image.scaled(requestedSize, Qt::KeepAspectRatio,
                      Qt::SmoothTransformation);
}

} // namespace

AssetCardImageResponse::AssetCardImageResponse(const QSize &requestedSize)
    : m_requestedSize(requestedSize),
      m_state(std::make_shared<AssetCardImageResponseState>()),
      m_completionTimer(new QTimer(this)) {
  m_completionTimer->setInterval(5);
  connect(m_completionTimer, &QTimer::timeout, this,
          [this]() { drainCompletion(); });
  m_completionTimer->start();
}

AssetCardImageResponse::~AssetCardImageResponse() { cancel(); }

QQuickTextureFactory *AssetCardImageResponse::textureFactory() const {
  QMutexLocker locker(&m_mutex);
  if (m_image.isNull()) {
    return nullptr;
  }
  return QQuickTextureFactory::textureFactoryForImage(m_image);
}

QString AssetCardImageResponse::errorString() const {
  QMutexLocker locker(&m_mutex);
  return m_errorString;
}

void AssetCardImageResponse::cancel() {
  CancelCallback callback;
  {
    std::lock_guard lock(m_state->mutex);
    if (!m_state->alive || m_state->finished || m_state->cancelled) {
      return;
    }
    m_state->alive = false;
    m_state->cancelled = true;
    callback = std::move(m_state->cancelCallback);
    m_state->cancelCallback = {};
  }
  if (callback) {
    callback();
  }
}

void AssetCardImageResponse::setCancelCallback(CancelCallback callback) {
  bool callImmediately = false;
  {
    std::lock_guard lock(m_state->mutex);
    if (m_state->cancelled && !m_state->finished) {
      callImmediately = true;
    } else {
      m_state->cancelCallback = std::move(callback);
    }
  }
  if (callImmediately && callback) {
    callback();
  }
}

void AssetCardImageResponse::complete(AssetOutcome<QImage> result) {
  completeState(m_state, std::move(result));
  drainCompletion();
}

void AssetCardImageResponse::completeState(
    std::shared_ptr<AssetCardImageResponseState> state,
    AssetOutcome<QImage> result) {
  if (!state) {
    return;
  }
  std::lock_guard lock(state->mutex);
  if (!state->alive || state->cancelled || state->finished ||
      state->result.has_value()) {
    return;
  }
  state->result.emplace(std::move(result));
}

void AssetCardImageResponse::drainCompletion() {
  std::optional<AssetOutcome<QImage>> result;
  {
    std::lock_guard lock(m_state->mutex);
    if (!m_state->alive || m_state->cancelled || m_state->finished ||
        !m_state->result.has_value()) {
      return;
    }
    result.emplace(std::move(*m_state->result));
    m_state->result.reset();
    m_state->finished = true;
    m_state->alive = false;
    m_state->cancelCallback = {};
  }

  {
    QMutexLocker locker(&m_mutex);
    if (*result) {
      m_image = scaledForRequest(**result, m_requestedSize);
    } else {
      m_errorString = imageErrorString(result->error());
    }
  }
  if (m_completionTimer) {
    m_completionTimer->stop();
  }
  emit finished();
}

AssetCardImageProvider::AssetCardImageProvider()
    : AssetCardImageProvider(AssetImageRequestCoordinator::Config{}) {}

AssetCardImageProvider::AssetCardImageProvider(
    AssetImageRequestCoordinator::Config config)
    : m_coordinatorHandle(std::make_shared<CoordinatorHandle>()) {
  auto *coordinator = new AssetImageRequestCoordinator(std::move(config));
  coordinator->moveToThread(&m_workerThread);
  QObject::connect(&m_workerThread, &QThread::finished, coordinator,
                   &QObject::deleteLater);
  {
    std::lock_guard lock(m_coordinatorHandle->mutex);
    m_coordinatorHandle->coordinator = coordinator;
  }
  m_workerThread.setObjectName(QStringLiteral("arkham-card-image-provider"));
  m_workerThread.start();
}

AssetCardImageProvider::~AssetCardImageProvider() {
  AssetImageRequestCoordinator *coordinator = nullptr;
  {
    std::lock_guard lock(m_coordinatorHandle->mutex);
    coordinator = m_coordinatorHandle->coordinator;
    m_coordinatorHandle->alive = false;
    m_coordinatorHandle->coordinator = nullptr;
  }
  if (coordinator) {
    QMetaObject::invokeMethod(coordinator,
                              &AssetImageRequestCoordinator::cancelAll,
                              Qt::BlockingQueuedConnection);
  }
  m_workerThread.quit();
  m_workerThread.wait();
}

QQuickImageResponse *
AssetCardImageProvider::requestImageResponse(const QString &id,
                                             const QSize &requestedSize) {
  auto *response = new AssetCardImageResponse(requestedSize);
  const auto key = parseImageId(id);
  if (!key) {
    response->complete(key.error());
    return response;
  }

  const std::shared_ptr<AssetCardImageResponseState> responseState =
      response->state();
  const std::shared_ptr<CoordinatorHandle> coordinatorHandle =
      m_coordinatorHandle;

  response->setCancelCallback([responseState, coordinatorHandle]() {
    std::optional<quint64> requestId;
    {
      std::lock_guard responseLock(responseState->mutex);
      requestId = responseState->requestId;
    }
    if (!requestId.has_value()) {
      return;
    }

    std::lock_guard coordinatorLock(coordinatorHandle->mutex);
    if (!coordinatorHandle->alive || !coordinatorHandle->coordinator) {
      return;
    }
    QMetaObject::invokeMethod(
        coordinatorHandle->coordinator,
        [coordinatorHandle, requestId = *requestId]() {
          AssetImageRequestCoordinator *coordinator = nullptr;
          {
            std::lock_guard coordinatorLock(coordinatorHandle->mutex);
            if (!coordinatorHandle->alive || !coordinatorHandle->coordinator) {
              return;
            }
            coordinator = coordinatorHandle->coordinator;
          }
          coordinator->cancel(requestId);
        },
        Qt::QueuedConnection);
  });

  {
    std::lock_guard coordinatorLock(coordinatorHandle->mutex);
    if (!coordinatorHandle->alive || !coordinatorHandle->coordinator) {
      response->complete(
          AssetError{AssetErrorCode::NetworkError,
                     QStringLiteral("card image provider is not available")});
      return response;
    }
    QMetaObject::invokeMethod(
        coordinatorHandle->coordinator,
        [coordinatorHandle, responseState, key = *key]() mutable {
          AssetImageRequestCoordinator *coordinator = nullptr;
          {
            std::lock_guard coordinatorLock(coordinatorHandle->mutex);
            if (!coordinatorHandle->alive || !coordinatorHandle->coordinator) {
              return;
            }
            coordinator = coordinatorHandle->coordinator;
          }

          const quint64 requestId = coordinator->requestCardImage(
              key, [responseState](AssetOutcome<QImage> result) mutable {
                AssetCardImageResponse::completeState(responseState,
                                                      std::move(result));
              });

          bool cancelImmediately = false;
          {
            std::lock_guard responseLock(responseState->mutex);
            if (!responseState->alive || responseState->cancelled) {
              cancelImmediately = true;
            } else {
              responseState->requestId = requestId;
            }
          }
          if (cancelImmediately) {
            coordinator->cancel(requestId);
          }
        },
        Qt::QueuedConnection);
  }

  return response;
}

AssetOutcome<AssetLocator::CardImageKey>
AssetCardImageProvider::parseImageId(const QString &id) {
  const QUrl url(QStringLiteral("image://arkham-card/") + id, QUrl::StrictMode);
  if (!url.isValid()) {
    return invalidProviderId(
        QStringLiteral("invalid card image provider id: %1")
            .arg(url.errorString()));
  }

  QString path = url.path(QUrl::FullyDecoded);
  while (path.startsWith(u'/')) {
    path.remove(0, 1);
  }
  const QStringList segments = path.split(u'/', Qt::SkipEmptyParts);

  AssetLocator::CardFace face = AssetLocator::CardFace::Front;
  QString cardCode;
  if (segments.size() == 1) {
    cardCode = segments.first();
  } else if (segments.size() == 2) {
    const auto parsedFace = parseFace(segments.first());
    if (!parsedFace) {
      return parsedFace.error();
    }
    face = *parsedFace;
    cardCode = segments.at(1);
  } else {
    return invalidProviderId(QStringLiteral(
        "card image provider id must be <code> or <face>/<code>"));
  }

  const auto suffixFromCode = splitMutationSuffix(&cardCode);
  if (!suffixFromCode) {
    return suffixFromCode.error();
  }
  QString mutationSuffix = *suffixFromCode;

  const QUrlQuery query(url);
  if (query.hasQueryItem(QStringLiteral("face"))) {
    const auto parsedFace =
        parseFace(query.queryItemValue(QStringLiteral("face")));
    if (!parsedFace) {
      return parsedFace.error();
    }
    face = *parsedFace;
  }
  if (query.hasQueryItem(QStringLiteral("mutation"))) {
    if (!mutationSuffix.isEmpty()) {
      return invalidProviderId(
          QStringLiteral("mutation suffix must be provided only once"));
    }
    mutationSuffix = normalizedMutationSuffix(
        query.queryItemValue(QStringLiteral("mutation")));
    if (!AssetLocator::isMutationSuffix(mutationSuffix)) {
      return invalidProviderId(
          QStringLiteral("mutation suffix must match _Mutated<N>"));
    }
  }

  return AssetLocator::CardImageKey{cardCode, face, mutationSuffix};
}

} // namespace Arkham
