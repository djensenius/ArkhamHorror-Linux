#pragma once

#include "AssetCache.h"
#include "AssetRequestCoordinator.h"
#include "AssetTypes.h"

#include <QImage>
#include <QObject>
#include <QString>
#include <optional>

namespace Arkham {

// A QML-suitable (but deliberately NOT registered as a QML type here --
// registration, if desired, belongs with whatever composition root wires
// up the rest of the QML type system, which is explicitly out of scope
// for this change) request/state seam for a single logical asset.
//
// Every state-affecting property is a Q_PROPERTY with an explicit NOTIFY
// signal, and every property read remains valid and self-consistent even
// if this object is destroyed mid-request: destruction cancels the
// underlying coordinator request and never emits any further signal.
class AssetImageRequest final : public QObject {
  Q_OBJECT
  Q_PROPERTY(Status status READ status NOTIFY statusChanged)
  Q_PROPERTY(QImage image READ image NOTIFY imageChanged)
  Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
  Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)
  Q_PROPERTY(int errorCode READ errorCode NOTIFY errorChanged)
  Q_PROPERTY(QString accessibleDescription READ accessibleDescription NOTIFY
                 accessibleDescriptionChanged)
public:
  enum class Status { Idle, Loading, Ready, Error };
  Q_ENUM(Status)

  explicit AssetImageRequest(AssetRequestCoordinator &coordinator,
                             QObject *parent = nullptr);
  ~AssetImageRequest() override;

  // Starts (or restarts) loading `key`. Any previously in-flight request
  // owned by this object is cancelled first. Safe to call repeatedly.
  //
  // `callerAccessibleDescription` is the caller-provided accessible
  // description this seam is required to carry (per
  // djensenius/ArkhamHorror-Linux#17) -- e.g. a card's title or an
  // investigator's name -- rather than an internally-invented generic
  // label. It is woven into the Loading/Ready/Error accessibleDescription
  // text at each state transition. Left empty, a generic
  // category-derived label is used instead, so existing callers that do
  // not yet have caller-specific text still get a reasonable default.
  void load(const AssetKey &key,
            const QString &callerAccessibleDescription = QString());

  // Cancels any in-flight request and returns to Idle. A no-op if already
  // Idle/Ready/Error.
  void cancel();

  [[nodiscard]] Status status() const { return m_status; }
  [[nodiscard]] QImage image() const { return m_image; }
  [[nodiscard]] qreal progress() const { return m_progress; }
  [[nodiscard]] QString errorString() const { return m_errorString; }
  [[nodiscard]] int errorCode() const { return m_errorCode; }
  [[nodiscard]] QString accessibleDescription() const {
    return m_accessibleDescription;
  }

signals:
  void statusChanged();
  void imageChanged();
  void progressChanged();
  void errorChanged();
  void accessibleDescriptionChanged();

private:
  void setStatus(Status status);
  void setAccessibleDescription(const QString &description);
  void reset();

  AssetRequestCoordinator &m_coordinator;
  AssetRequestCoordinator::RequestHandle m_handle;
  quint64 m_generation{
      0}; // guards against a stale callback after load()/cancel()

  Status m_status{Status::Idle};
  QImage m_image;
  qreal m_progress{0.0};
  QString m_errorString;
  int m_errorCode{0};
  QString m_accessibleDescription;
};

} // namespace Arkham
