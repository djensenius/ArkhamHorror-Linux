#pragma once

#include "ProbeResult.h"
#include "ServerProfile.h"

#include <QObject>

namespace Arkham {

// Injectable abstract base for asynchronous server capability probes.
//
// Concrete implementations:
//   NetworkCapabilityProbe — production, backed by QNetworkAccessManager.
//
// Typical usage:
//   connect(probe, &ICapabilityProbe::finished, context, callback);
//   probe->probe(profile);
//   // finished() is emitted asynchronously after probe() returns.
class ICapabilityProbe : public QObject {
  Q_OBJECT
public:
  explicit ICapabilityProbe(QObject *parent = nullptr) : QObject(parent) {}
  ~ICapabilityProbe() override = default;

  // Issue an asynchronous capability probe against the given server profile.
  // Emits finished() exactly once per call while the probe is alive.
  // Destroying the probe cancels any queued or in-flight completion; no
  // finished() signal is delivered after the probe object is destroyed.
  virtual void probe(const ServerProfile &profile) = 0;

signals:
  // Emitted exactly once per probe() call while the probe is alive, with the
  // full typed outcome.  Not emitted if the probe is destroyed before the
  // result is ready.
  void finished(Arkham::ProbeResult result);
};

} // namespace Arkham
