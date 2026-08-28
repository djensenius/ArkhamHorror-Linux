#pragma once

#include "ICapabilityProbe.h"

#include <QSet>

class QNetworkAccessManager;
class QNetworkReply;

namespace Arkham {

// Production capability probe backed by QNetworkAccessManager.
//
// Issues GET <currentPin().expectedApiBasePath>/capabilities with
// Accept: application/json.
// No authentication is added — the endpoint is public.
// TLS certificate validation is never bypassed.
//
// The |nam| reference is borrowed; the caller must ensure it outlives this
// probe.  A reference (not pointer) makes null impossible.
// Outstanding replies are aborted and scheduled for deletion when the probe
// is destroyed; |this| remains the connection context so response handlers
// cannot outlive the probe.
class NetworkCapabilityProbe : public ICapabilityProbe {
  Q_OBJECT
public:
  explicit NetworkCapabilityProbe(QNetworkAccessManager &nam,
                                  QObject *parent = nullptr);
  ~NetworkCapabilityProbe() override;

  // If |profile| is not valid, emits finished(InvalidProfile) immediately
  // without issuing a network request.
  void probe(const ServerProfile &profile) override;

private:
  void handleReply(QNetworkReply *reply);

  QNetworkAccessManager &m_nam;
  QSet<QNetworkReply *> m_replies;
};

} // namespace Arkham
