#pragma once

#include "ICapabilityProbe.h"
#include "ServerProfile.h"

#include <QHash>
#include <QObject>
#include <chrono>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

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
//
// The optional |timeout| caps the wall-clock time to wait for each server
// response.  Defaults to 30 s in production; inject a shorter value in tests.
// Pass std::chrono::milliseconds(0) to disable per-reply deadlines.
//
// Outstanding replies are aborted and scheduled for deletion when the probe
// is destroyed; per-reply timers are stopped before replies are aborted, so
// no response is delivered after destruction.
class NetworkCapabilityProbe : public ICapabilityProbe {
  Q_OBJECT
public:
  static constexpr std::chrono::seconds kDefaultTimeout{30};

  explicit NetworkCapabilityProbe(
      QNetworkAccessManager &nam,
      std::chrono::milliseconds timeout = kDefaultTimeout,
      QObject *parent = nullptr);
  ~NetworkCapabilityProbe() override;

  // Starts an asynchronous probe against |profile|.
  // If |profile| is not valid, emits finished(InvalidProfile) asynchronously
  // (via a queued event) after probe() returns, without issuing a network
  // request.
  void probe(const ServerProfile &profile) override;

private:
  void handleReply(QNetworkReply *reply);

  QNetworkAccessManager &m_nam;
  std::chrono::milliseconds m_timeout;
  // Maps each in-flight reply to its per-reply deadline timer.  Timer pointer
  // is nullptr when timeout was disabled (m_timeout == 0).
  QHash<QNetworkReply *, QTimer *> m_pendingReplies;
};

} // namespace Arkham
