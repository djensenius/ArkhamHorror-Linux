#include "NetworkCapabilityProbe.h"

#include "CompatibilityEvaluator.h"
#include "ContractPin.h"
#include "ServerCapabilities.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtAssert>
#include <optional>
#include <stdexcept>

using namespace Qt::StringLiterals;

namespace Arkham {

namespace {

// Maps CompatibilityOutcome to ProbeOutcome exhaustively.  Returning directly
// (rather than assigning to an uninitialized variable) means the compiler can
// warn if a new CompatibilityOutcome value is added without updating this map.
ProbeOutcome compatOutcomeToProbeOutcome(CompatibilityOutcome outcome) {
  switch (outcome) {
  case CompatibilityOutcome::Compatible:
    return ProbeOutcome::Compatible;
  case CompatibilityOutcome::LegacyFallback:
    return ProbeOutcome::LegacyFallback;
  case CompatibilityOutcome::Incompatible:
    return ProbeOutcome::Incompatible;
  }
  Q_UNREACHABLE_RETURN(ProbeOutcome::Incompatible);
}

} // namespace

NetworkCapabilityProbe::NetworkCapabilityProbe(
    QNetworkAccessManager &nam, std::chrono::milliseconds timeout,
    QObject *parent)
    : ICapabilityProbe(parent), m_nam(nam), m_timeout(timeout) {
  if (timeout < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("capability probe timeout cannot be negative");
  }
}

NetworkCapabilityProbe::~NetworkCapabilityProbe() {
  for (auto it = m_pendingReplies.begin(); it != m_pendingReplies.end(); ++it) {
    if (QTimer *timer = it.value()) {
      timer->stop();
    }
    QNetworkReply *reply = it.key();
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }
  m_pendingReplies.clear();
}

void NetworkCapabilityProbe::probe(const ServerProfile &profile) {
  // Guard: reject an invalid profile before issuing any network request.
  if (!profile.isValid()) {
    const ProbeResult result{
        ProbeOutcome::InvalidProfile,
        QStringLiteral("server profile is invalid; cannot issue probe"),
    };
    QPointer<NetworkCapabilityProbe> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, result]() {
          if (self)
            emit self->finished(result);
        },
        Qt::QueuedConnection);
    return;
  }

  const QUrl capUrl = profile.apiUrl(u"capabilities");
  QNetworkRequest request(capUrl);
  request.setRawHeader("Accept", "application/json");
  // Do not inherit shared-QNAM credentials: disable automatic cookie
  // load/save for this unauthenticated public endpoint.
  request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                       QNetworkRequest::Manual);
  // Set redirect policy explicitly at request level so a manager-level global
  // policy cannot override it.  Follow only same-origin redirects: the
  // capabilities endpoint must not cross origin boundaries.
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::SameOriginRedirectPolicy);

  QNetworkReply *reply = m_nam.get(request);

  // Set up a per-reply deadline timer when a non-zero timeout is configured.
  QTimer *timer = nullptr;
  if (m_timeout.count() > 0) {
    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, reply, timer]() {
      if (!m_pendingReplies.contains(reply))
        return; // reply already handled before timer fired
      // Disconnect our handler first so abort() cannot trigger it.
      QObject::disconnect(reply, nullptr, this, nullptr);
      m_pendingReplies.remove(reply);
      timer->deleteLater();
      reply->abort();
      reply->deleteLater();
      // Emit asynchronously to avoid re-entrancy during abort() completion.
      // QPointer guards against the unlikely case where |this| is destroyed
      // before the queued event is processed.
      QPointer<NetworkCapabilityProbe> self(this);
      QMetaObject::invokeMethod(
          this,
          [self]() {
            if (self)
              emit self->finished(ProbeResult{
                  ProbeOutcome::NetworkError,
                  QStringLiteral("capability probe timed out"),
              });
          },
          Qt::QueuedConnection);
    });
  }
  m_pendingReplies.insert(reply, timer);

  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    // Cancel the per-reply deadline before processing the response.
    if (QTimer *t = m_pendingReplies.value(reply)) {
      t->stop();
      t->deleteLater();
    }
    m_pendingReplies.remove(reply);
    reply->deleteLater();
    handleReply(reply);
  });

  if (timer) {
    timer->start(m_timeout);
  }
}

void NetworkCapabilityProbe::handleReply(QNetworkReply *reply) {
  // Use the HTTP status attribute as the primary discriminator.  If it is
  // absent the reply did not carry a valid HTTP response (transport failure
  // or pre-response error).
  const QVariant statusAttr =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);

  if (!statusAttr.isValid()) {
    emit finished(ProbeResult{
        ProbeOutcome::NetworkError,
        reply->errorString(),
    });
    return;
  }

  const int status = statusAttr.toInt();

  // 404 → server predates the capabilities endpoint → LegacyFallback.
  if (status == 404) {
    const ServerCapabilities fallback = ServerCapabilities::legacyFallback();
    const CompatibilityResult comp =
        CompatibilityEvaluator::evaluate(fallback, currentPin());
    emit finished(ProbeResult{
        ProbeOutcome::LegacyFallback,
        comp.diagnostic,
        comp,
        status,
    });
    return;
  }

  // Any non-2xx (except the 404 already handled) → HttpError.
  if (status < 200 || status >= 300) {
    emit finished(ProbeResult{
        ProbeOutcome::HttpError,
        QStringLiteral("unexpected HTTP status %1").arg(status),
        std::nullopt,
        status,
    });
    return;
  }

  // 2xx with a transport-level error means the body may be incomplete.
  // Surface as NetworkError rather than attempting to decode a partial body.
  if (reply->error() != QNetworkReply::NoError) {
    emit finished(ProbeResult{
        ProbeOutcome::NetworkError,
        reply->errorString(),
        std::nullopt,
        status,
    });
    return;
  }

  // 2xx, no error — decode the JSON body.
  const QByteArray body = reply->readAll();
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
  if (parseError.error != QJsonParseError::NoError) {
    emit finished(ProbeResult{
        ProbeOutcome::MalformedJson,
        QStringLiteral("invalid JSON at offset %1: %2")
            .arg(parseError.offset)
            .arg(parseError.errorString()),
        std::nullopt,
        status,
    });
    return;
  }
  if (!doc.isObject()) {
    emit finished(ProbeResult{
        ProbeOutcome::MalformedJson,
        QStringLiteral("response body is not a JSON object"),
        std::nullopt,
        status,
    });
    return;
  }

  const auto capsResult = ServerCapabilities::fromJson(doc.object());
  if (!capsResult.has_value()) {
    emit finished(ProbeResult{
        ProbeOutcome::MalformedJson,
        capsResult.error(),
        std::nullopt,
        status,
    });
    return;
  }

  // Evaluate compatibility and map the outcome via the exhaustive helper.
  const CompatibilityResult comp =
      CompatibilityEvaluator::evaluate(*capsResult, currentPin());
  emit finished(ProbeResult{compatOutcomeToProbeOutcome(comp.outcome),
                            comp.diagnostic, comp, status});
}

} // namespace Arkham
