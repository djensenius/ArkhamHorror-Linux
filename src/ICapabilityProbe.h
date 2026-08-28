#pragma once

#include "ProbeResult.h"
#include "ServerProfile.h"

#include <QObject>

namespace Arkham {

// Injectable abstract base for asynchronous server capability probes.
//
// Concrete implementations:
//   NetworkCapabilityProbe — production, backed by QNetworkAccessManager.
//   (In tests) a local stub class that emits finished() synchronously.
//
// Typical usage:
//   connect(probe, &ICapabilityProbe::finished, context, callback);
//   probe->probe(profile);
//   // finished() emitted once per probe() call, asynchronously or (in stubs)
//   // synchronously.
class ICapabilityProbe : public QObject {
  Q_OBJECT
public:
  explicit ICapabilityProbe(QObject *parent = nullptr) : QObject(parent) {}
  ~ICapabilityProbe() override = default;

  // Issue an asynchronous capability probe against the given server profile.
  // Emits finished() exactly once when the probe completes or fails.
  virtual void probe(const ServerProfile &profile) = 0;

signals:
  // Emitted exactly once per probe() call with the full typed outcome.
  void finished(Arkham::ProbeResult result);
};

} // namespace Arkham
