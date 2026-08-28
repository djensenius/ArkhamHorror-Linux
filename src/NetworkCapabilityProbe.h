#pragma once

#include "ICapabilityProbe.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace Arkham {

// Production capability probe backed by QNetworkAccessManager.
//
// Issues GET /api/v1/capabilities with Accept: application/json.
// No authentication is added — the endpoint is public.
// TLS certificate validation is never bypassed.
//
// The |nam| reference is borrowed; the caller must ensure it outlives this
// probe.  A reference (not pointer) makes null impossible.
// Using |this| as the connection context means outstanding reply lambdas are
// automatically invalidated when the probe is destroyed, preventing dangling
// captures in flight.
class NetworkCapabilityProbe : public ICapabilityProbe {
  Q_OBJECT
public:
  explicit NetworkCapabilityProbe(QNetworkAccessManager &nam,
                                  QObject *parent = nullptr);

  // If |profile| is not valid, emits finished(InvalidProfile) immediately
  // without issuing a network request.
  void probe(const ServerProfile &profile) override;

private:
  void handleReply(QNetworkReply *reply);

  QNetworkAccessManager &m_nam;
};

} // namespace Arkham
