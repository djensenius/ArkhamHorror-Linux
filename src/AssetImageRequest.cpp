#include "AssetImageRequest.h"

#include <QCoreApplication>

namespace Arkham {

namespace {

// A short, human-readable category label for the accessibility
// description only -- not used for any path/URL/cache-key construction,
// where AssetCategory is always handled as a typed enumerator.
QString categoryLabel(AssetCategory category) {
  switch (category) {
  case AssetCategory::Card:
    return QStringLiteral("card");
  case AssetCategory::InvestigatorPortrait:
    return QStringLiteral("investigator portrait");
  case AssetCategory::ChaosToken:
    return QStringLiteral("chaos token");
  case AssetCategory::SetIcon:
    return QStringLiteral("set icon");
  case AssetCategory::CampaignBox:
    return QStringLiteral("campaign box");
  case AssetCategory::SlotIcon:
    return QStringLiteral("slot icon");
  case AssetCategory::HomebrewCard:
    return QStringLiteral("homebrew card");
  case AssetCategory::HomebrewSet:
    return QStringLiteral("homebrew set");
  case AssetCategory::HomebrewBox:
    return QStringLiteral("homebrew box");
  }
  return QStringLiteral("asset");
}

} // namespace

AssetImageRequest::AssetImageRequest(AssetRequestCoordinator &coordinator,
                                     QObject *parent)
    : QObject(parent), m_coordinator(coordinator) {}

AssetImageRequest::~AssetImageRequest() {
  // Bump the generation before cancelling so that even if the coordinator
  // were to (incorrectly) invoke a callback synchronously during
  // destruction, it could never touch this half-destroyed object -- the
  // callback captures `this` only via a QPointer-guarded lambda owned by
  // the coordinator, and cancel() below detaches this consumer from the
  // shared operation without this object being consulted again.
  ++m_generation;
  if (m_handle.isValid()) {
    m_coordinator.cancel(m_handle);
    m_handle = {};
  }
}

void AssetImageRequest::load(const AssetKey &key) {
  cancel();
  ++m_generation;
  const quint64 generation = m_generation;

  setStatus(Status::Loading);
  m_progress = 0.0;
  emit progressChanged();
  m_errorString.clear();
  m_errorCode = 0;
  setAccessibleDescription(
      QStringLiteral("Loading %1 image \"%2\"...")
          .arg(categoryLabel(key.category), key.identifier));

  m_handle = m_coordinator.request(
      key, [this, generation](AssetOutcome<AssetCache::CachedEntry> result) {
        // A stale callback from a superseded load()/cancel() (or this
        // object having since been destroyed, in which case this lambda
        // itself would never run because the coordinator only reaches
        // consumers still registered under a live handle) is rejected by
        // generation number: only the most recent load() may mutate
        // state.
        if (generation != m_generation) {
          return;
        }
        m_handle = {};
        if (!result) {
          const AssetError &error = result.error();
          m_status = Status::Error;
          m_errorString = error.message;
          m_errorCode = static_cast<int>(error.code);
          m_image = QImage();
          setAccessibleDescription(
              QStringLiteral("Failed to load image: %1").arg(error.message));
          emit statusChanged();
          emit errorChanged();
          emit imageChanged();
          m_progress = 1.0;
          emit progressChanged();
          return;
        }

        m_image = result->decodedImage;
        m_status = Status::Ready;
        setAccessibleDescription(QStringLiteral("Image loaded"));
        emit statusChanged();
        emit imageChanged();
        m_progress = 1.0;
        emit progressChanged();
      });
}

void AssetImageRequest::cancel() {
  ++m_generation;
  if (m_handle.isValid()) {
    m_coordinator.cancel(m_handle);
    m_handle = {};
  }
  if (m_status == Status::Loading) {
    setStatus(Status::Idle);
    m_progress = 0.0;
    emit progressChanged();
    setAccessibleDescription(QString());
  }
}

void AssetImageRequest::setStatus(Status status) {
  if (m_status == status) {
    return;
  }
  m_status = status;
  emit statusChanged();
}

void AssetImageRequest::setAccessibleDescription(const QString &description) {
  if (m_accessibleDescription == description) {
    return;
  }
  m_accessibleDescription = description;
  emit accessibleDescriptionChanged();
}

} // namespace Arkham
