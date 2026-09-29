#include "AssetImageProvider.h"

#include <QMutexLocker>
#include <QQuickTextureFactory>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include <utility>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

AssetError invalidProviderId(const QString &message) {
  return AssetError{AssetErrorCode::InvalidAssetKey, message};
}

bool isMutationSuffix(const QString &suffix) {
  constexpr QStringView kPrefix = u"_Mutated";
  if (!suffix.startsWith(kPrefix) || suffix.size() == kPrefix.size()) {
    return false;
  }
  for (qsizetype index = kPrefix.size(); index < suffix.size(); ++index) {
    const QChar c = suffix.at(index);
    if (c < u'0' || c > u'9') {
      return false;
    }
  }
  return true;
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
  if (!isMutationSuffix(suffix)) {
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
  if (image.isNull() || !requestedSize.isValid() || requestedSize.isEmpty()) {
    return image;
  }
  return image.scaled(requestedSize, Qt::KeepAspectRatio,
                      Qt::SmoothTransformation);
}

} // namespace

AssetCardImageResponse::AssetCardImageResponse(const QSize &requestedSize)
    : m_requestedSize(requestedSize) {}

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
    QMutexLocker locker(&m_mutex);
    if (m_cancelled || m_finished) {
      return;
    }
    m_cancelled = true;
    callback = std::move(m_cancelCallback);
  }
  if (callback) {
    callback();
  }
}

void AssetCardImageResponse::setCancelCallback(CancelCallback callback) {
  bool callImmediately = false;
  {
    QMutexLocker locker(&m_mutex);
    if (m_cancelled && !m_finished) {
      callImmediately = true;
    } else {
      m_cancelCallback = std::move(callback);
    }
  }
  if (callImmediately && callback) {
    callback();
  }
}

void AssetCardImageResponse::complete(AssetOutcome<QImage> result) {
  {
    QMutexLocker locker(&m_mutex);
    if (m_cancelled || m_finished) {
      return;
    }
    m_finished = true;
    m_cancelCallback = {};
    if (result) {
      m_image = scaledForRequest(*result, m_requestedSize);
    } else {
      m_errorString = imageErrorString(result.error());
    }
  }
  emit finished();
}

AssetCardImageProvider::AssetCardImageProvider()
    : AssetCardImageProvider(AssetImageRequestCoordinator::Config{}) {}

AssetCardImageProvider::AssetCardImageProvider(
    AssetImageRequestCoordinator::Config config) {
  auto *coordinator = new AssetImageRequestCoordinator(std::move(config));
  coordinator->moveToThread(&m_workerThread);
  QObject::connect(&m_workerThread, &QThread::finished, coordinator,
                   &QObject::deleteLater);
  m_coordinator = coordinator;
  m_workerThread.setObjectName(QStringLiteral("arkham-card-image-provider"));
  m_workerThread.start();
}

AssetCardImageProvider::~AssetCardImageProvider() {
  if (m_coordinator) {
    QMetaObject::invokeMethod(m_coordinator,
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

  const quint64 responseId = m_pendingResponseId.fetch_add(1);
  QPointer<AssetImageRequestCoordinator> coordinator = m_coordinator;
  QPointer<AssetCardImageResponse> guardedResponse(response);
  if (!coordinator) {
    response->complete(
        AssetError{AssetErrorCode::NetworkError,
                   QStringLiteral("card image provider is not available")});
    return response;
  }

  response->setCancelCallback([coordinator, responseId]() {
    if (!coordinator) {
      return;
    }
    QMetaObject::invokeMethod(
        coordinator,
        [coordinator, responseId]() {
          if (coordinator) {
            coordinator->cancel(responseId);
          }
        },
        Qt::QueuedConnection);
  });

  QMetaObject::invokeMethod(
      coordinator,
      [coordinator, guardedResponse, key = *key, responseId]() mutable {
        if (!coordinator || !guardedResponse) {
          return;
        }
        coordinator->requestCardImage(
            responseId, key,
            [guardedResponse](AssetOutcome<QImage> result) mutable {
              if (!guardedResponse) {
                return;
              }
              QMetaObject::invokeMethod(
                  guardedResponse,
                  [guardedResponse, result = std::move(result)]() mutable {
                    if (guardedResponse) {
                      guardedResponse->complete(std::move(result));
                    }
                  },
                  Qt::QueuedConnection);
            });
      },
      Qt::QueuedConnection);

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
    if (!isMutationSuffix(mutationSuffix)) {
      return invalidProviderId(
          QStringLiteral("mutation suffix must match _Mutated<N>"));
    }
  }

  return AssetLocator::CardImageKey{cardCode, face, mutationSuffix};
}

} // namespace Arkham
