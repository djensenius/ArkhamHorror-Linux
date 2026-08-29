#include "AssetImageRequest.h"

#include <QCoreApplication>
#include <QPointer>

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
  // Bump the generation before cancelling. This alone does NOT protect a
  // queued coordinator callback that captures `this` -- that callback is
  // guarded independently by the QPointer in load() below, since
  // cancel()'s own queued delivery of the Cancelled result can run after
  // this destructor has already finished.
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
  // Clear any previously-loaded image immediately: otherwise a caller
  // that reuses one AssetImageRequest across two different keys (e.g. a
  // QML Image binding that switches source identifiers) would keep
  // showing the OLD image throughout the new Loading phase.
  if (!m_image.isNull()) {
    m_image = QImage();
    emit imageChanged();
  }
  setAccessibleDescription(
      QStringLiteral("Loading %1 image \"%2\"...")
          .arg(categoryLabel(key.category), key.identifier));

  // The coordinator retains this callback (via std::function, possibly
  // queued past this object's own lifetime -- e.g. a cancel() during
  // destruction still queues one final Cancelled delivery for exactly
  // this consumer). A raw `this` capture would be a use-after-free once
  // that queued delivery runs after this object is destroyed, so every
  // member access below is guarded by a QPointer liveness check first.
  QPointer<AssetImageRequest> self(this);
  m_handle = m_coordinator.request(
      key, [self, generation](AssetOutcome<AssetCache::CachedEntry> result) {
        if (!self) {
          return; // this AssetImageRequest was destroyed
        }
        // A stale callback from a superseded load()/cancel() is rejected
        // by generation number: only the most recent load() may mutate
        // state.
        if (generation != self->m_generation) {
          return;
        }
        self->m_handle = {};
        if (!result) {
          const AssetError &error = result.error();
          self->m_status = Status::Error;
          self->m_errorString = error.message;
          self->m_errorCode = static_cast<int>(error.code);
          self->m_image = QImage();
          self->setAccessibleDescription(
              QStringLiteral("Failed to load image: %1").arg(error.message));
          emit self->statusChanged();
          emit self->errorChanged();
          emit self->imageChanged();
          self->m_progress = 1.0;
          emit self->progressChanged();
          return;
        }

        self->m_image = result->decodedImage;
        self->m_status = Status::Ready;
        self->setAccessibleDescription(QStringLiteral("Image loaded"));
        emit self->statusChanged();
        emit self->imageChanged();
        self->m_progress = 1.0;
        emit self->progressChanged();
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
