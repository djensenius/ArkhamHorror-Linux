#include "NetworkCapabilityProbe.h"

#include "CompatibilityEvaluator.h"
#include "ContractPin.h"
#include "ServerCapabilities.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

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

NetworkCapabilityProbe::NetworkCapabilityProbe(QNetworkAccessManager &nam,
                                               QObject *parent)
    : ICapabilityProbe(parent), m_nam(nam) {}

void NetworkCapabilityProbe::probe(const ServerProfile &profile) {
  // Guard: reject an invalid profile before issuing any network request.
  if (!profile.isValid()) {
    emit finished(ProbeResult{
        ProbeOutcome::InvalidProfile,
        QStringLiteral("server profile is invalid; cannot issue probe"),
    });
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
  // No Authorization header — the capabilities endpoint is public.

  QNetworkReply *reply = m_nam.get(request);

  // Use |this| as the connection context: the lambda is never called after
  // this probe object is destroyed, preventing use of a dangling |this|.
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    reply->deleteLater();
    handleReply(reply);
  });
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
  const QJsonDocument doc = QJsonDocument::fromJson(body);
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
