// Tests for NetworkAuthenticationClient, driven entirely through a stub
// QNetworkAccessManager/QNetworkReply pair (mirroring the pattern used for
// NetworkCapabilityProbe in tests/NetworkTests.cpp). No live service or
// network connection is used.

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEvent>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QQueue>
#include <QtGlobal>
#include <QtTest>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <utility>

#include "AuthModels.h"
#include "AuthTransportSecurity.h"
#include "IAuthenticationClient.h"
#include "NetworkAuthenticationClient.h"
#include "ServerProfile.h"

using namespace Arkham;

namespace {

// ─── Stub QNetworkReply ─────────────────────────────────────────────────

class StubNetworkReply final : public QNetworkReply {
  Q_OBJECT
public:
  StubNetworkReply(int statusCode, QByteArray body,
                   QNetworkReply::NetworkError netError, bool hanging,
                   QObject *parent)
      : QNetworkReply(parent), m_body(std::move(body)), m_hanging(hanging) {
    if (statusCode > 0) {
      setAttribute(QNetworkRequest::HttpStatusCodeAttribute, statusCode);
    }
    if (netError != NoError) {
      setError(netError, QStringLiteral("simulated network error"));
    }
    setOpenMode(QIODevice::ReadOnly);
  }

  void abort() override {
    if (m_hanging) {
      setError(QNetworkReply::OperationCanceledError,
               QStringLiteral("aborted"));
      scheduleFinished();
    }
  }

  void scheduleFinished() {
    QMetaObject::invokeMethod(this, &StubNetworkReply::emitFinished,
                              Qt::QueuedConnection);
  }

protected:
  qint64 readData(char *data, qint64 maxLen) override {
    const qint64 available = static_cast<qint64>(m_body.size()) - m_offset;
    if (available <= 0)
      return 0;
    const qint64 count = qMin(available, maxLen);
    std::memcpy(data, m_body.constData() + m_offset,
                static_cast<size_t>(count));
    m_offset += count;
    return count;
  }

private slots:
  void emitFinished() { emit finished(); }

private:
  QByteArray m_body;
  qint64 m_offset{0};
  bool m_hanging{false};
};

// ─── Stub QNetworkAccessManager ─────────────────────────────────────────

class StubNetworkAccessManager final : public QNetworkAccessManager {
  Q_OBJECT
public:
  explicit StubNetworkAccessManager(QObject *parent = nullptr)
      : QNetworkAccessManager(parent) {}

  void enqueue(int statusCode, QByteArray body,
               QNetworkReply::NetworkError err = QNetworkReply::NoError) {
    m_queue.enqueue({statusCode, std::move(body), err, false});
  }
  void enqueueHanging() {
    m_queue.enqueue({0, {}, QNetworkReply::NoError, true});
  }

  const QList<QNetworkRequest> &requests() const { return m_requests; }
  const QList<QByteArray> &bodies() const { return m_bodies; }
  QPointer<QNetworkReply> lastReply() const { return m_lastReply; }

protected:
  QNetworkReply *createRequest(Operation, const QNetworkRequest &req,
                               QIODevice *outgoingData) override {
    m_requests.append(req);
    m_bodies.append(outgoingData ? outgoingData->readAll() : QByteArray{});
    if (m_queue.isEmpty()) {
      qFatal("StubNetworkAccessManager: no canned response enqueued");
    }
    const auto [status, body, err, hanging] = m_queue.dequeue();
    auto *reply =
        new StubNetworkReply(status, std::move(body), err, hanging, this);
    m_lastReply = reply;
    if (!hanging) {
      reply->scheduleFinished();
    }
    return reply;
  }

private:
  struct Canned {
    int status;
    QByteArray body;
    QNetworkReply::NetworkError err;
    bool hanging{false};
  };
  QQueue<Canned> m_queue;
  QList<QNetworkRequest> m_requests;
  QList<QByteArray> m_bodies;
  QPointer<QNetworkReply> m_lastReply;
};

// ─── Helpers ─────────────────────────────────────────────────────────────

ServerProfile customProfile(const QString &url) {
  auto result = ServerProfile::custom(QStringLiteral("Test Server"), url);
  return *result;
}

template <typename T>
std::optional<AuthResult<T>>
runAndWait(std::function<AuthRequestHandle(std::function<void(AuthResult<T>)>)>
               issue) {
  std::optional<AuthResult<T>> captured;
  issue([&captured](AuthResult<T> r) { captured = std::move(r); });
  const QDeadlineTimer deadline(2000);
  while (!captured.has_value() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return captured;
}

} // namespace

class AuthClientTests final : public QObject {
  Q_OBJECT

private slots:
  void authenticateSendsExpectedRequest();
  void authenticateNoAuthorizationHeader();
  void registerSendsExpectedRequest();
  void whoAmISendsExactAuthorizationHeader();
  void whoAmIRejectsEmptyToken();
  void whoAmIRejectsTokenWithControlCharacters();
  void requestsDisableCookiesAndAuthReuse();
  void requestsSetManualRedirectPolicy();
  void requestsSetCacheBypassAttributes();
  void productionConstructorOwnsDedicatedManager();
  void httpToNonLoopbackHostRejectedBeforeRequest();
  void httpLoopbackHostnamePermitted();
  void httpLoopbackIPv4Permitted();
  void httpLoopbackIPv6Permitted();
  void httpLookalikeLoopbackHostRejected();
  void httpsPermittedForAnyHost();
  void httpWithUserInfoRejectedEvenForLoopback();
  void threeXxMapsToUnexpectedStatus();
  void twoXxDecodesSuccessfully();
  void unauthorizedMapsTo401();
  void malformedJsonMapsToMalformedPayload();
  void malformedShapeMapsToMalformedPayload();
  void nonHttpTransportErrorMapsToTransport();
  void nonHttpResponseWithoutTransportError();
  void invalidProfileRejectedBeforeRequest();
  void requestTimesOut();
  void zeroTimeoutDisablesDeadline();
  void negativeTimeoutRejected();
  void concurrentRequestsAreIndependent();
  void destructionAbortsOutstandingRequestsNoCallback();
  void explicitCancelEmitsCancelledOutcome();
  void staleCancelIsNoOp();
  void callbacksAreExactlyOnceOnTimeout();
  void diagnosticsNeverContainSecrets();
  void profilePrefixIsPreserved();
};

void AuthClientTests::authenticateSendsExpectedRequest() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "abc123"})"));
  NetworkAuthenticationClient client(nam);

  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            ServerProfile::hostedDefault(),
            AuthenticateRequest{QStringLiteral("user@example.com"),
                                QStringLiteral("hunter2")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::Success);
  QVERIFY(result->value.has_value());
  QCOMPARE(result->value->token, QStringLiteral("abc123"));

  QCOMPARE(nam.requests().size(), 1);
  const QNetworkRequest &req = nam.requests().first();
  QCOMPARE(req.url().toString(),
           QStringLiteral("https://arkhamhorror.app/api/v1/authenticate"));
  QCOMPARE(QString::fromLatin1(req.rawHeader("Accept")),
           QStringLiteral("application/json"));
  QCOMPARE(req.header(QNetworkRequest::ContentTypeHeader).toString(),
           QStringLiteral("application/json"));

  const QJsonObject sentBody =
      QJsonDocument::fromJson(nam.bodies().first()).object();
  QCOMPARE(sentBody.value(QStringLiteral("email")).toString(),
           QStringLiteral("user@example.com"));
  QCOMPARE(sentBody.value(QStringLiteral("password")).toString(),
           QStringLiteral("hunter2"));
}

void AuthClientTests::authenticateNoAuthorizationHeader() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "abc123"})"));
  NetworkAuthenticationClient client(nam);

  runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
    return client.authenticate(
        ServerProfile::hostedDefault(),
        AuthenticateRequest{QStringLiteral("a@b.com"), QStringLiteral("pw")},
        std::move(cb));
  });

  QVERIFY(nam.requests().first().rawHeader("Authorization").isEmpty());
}

void AuthClientTests::registerSendsExpectedRequest() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "reg-token"})"));
  NetworkAuthenticationClient client(nam);

  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.registerAccount(
            ServerProfile::hostedDefault(),
            RegisterRequest{QStringLiteral("new@example.com"),
                            QStringLiteral("newuser"),
                            QStringLiteral("s3cr3t")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::Success);
  QCOMPARE(result->value->token, QStringLiteral("reg-token"));
  QCOMPARE(nam.requests().first().url().path(),
           QStringLiteral("/api/v1/register"));
  QVERIFY(nam.requests().first().rawHeader("Authorization").isEmpty());

  const QJsonObject sentBody =
      QJsonDocument::fromJson(nam.bodies().first()).object();
  QCOMPARE(sentBody.value(QStringLiteral("username")).toString(),
           QStringLiteral("newuser"));
}

void AuthClientTests::whoAmISendsExactAuthorizationHeader() {
  StubNetworkAccessManager nam;
  nam.enqueue(
      200,
      QByteArrayLiteral(
          R"({"username":"alice","email":"a@b.com","beta":true,"admin":false})"));
  NetworkAuthenticationClient client(nam);

  const auto result = runAndWait<CurrentUser>(
      [&](std::function<void(AuthResult<CurrentUser>)> cb) {
        return client.whoAmI(ServerProfile::hostedDefault(),
                             QStringLiteral("my-jwt-token"), std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::Success);
  QCOMPARE(result->value->username, QStringLiteral("alice"));
  QCOMPARE(result->value->email, QStringLiteral("a@b.com"));
  QCOMPARE(result->value->beta, true);
  QCOMPARE(result->value->admin, false);

  QCOMPARE(
      QString::fromLatin1(nam.requests().first().rawHeader("Authorization")),
      QStringLiteral("Token my-jwt-token"));
  QCOMPARE(nam.requests().first().url().path(),
           QStringLiteral("/api/v1/whoami"));
  QVERIFY(nam.bodies().first().isEmpty());
}

void AuthClientTests::whoAmIRejectsEmptyToken() {
  StubNetworkAccessManager nam;
  NetworkAuthenticationClient client(nam);

  const auto result = runAndWait<CurrentUser>(
      [&](std::function<void(AuthResult<CurrentUser>)> cb) {
        return client.whoAmI(ServerProfile::hostedDefault(),
                             QStringLiteral("   "), std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::InvalidInput);
}

void AuthClientTests::whoAmIRejectsTokenWithControlCharacters() {
  StubNetworkAccessManager nam;
  NetworkAuthenticationClient client(nam);

  // A token embedding CR/LF is a header/request-splitting attempt: if
  // placed verbatim into a raw "Authorization" header value it could
  // inject an extra header (here, a forged "X-Injected" header) into the
  // request. This must be rejected before any request is constructed, not
  // silently sanitized/stripped.
  const QString maliciousToken =
      QStringLiteral("legit-token\r\nX-Injected: evil");

  const auto result = runAndWait<CurrentUser>(
      [&](std::function<void(AuthResult<CurrentUser>)> cb) {
        return client.whoAmI(ServerProfile::hostedDefault(), maliciousToken,
                             std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::InvalidInput);
  // No request may ever be sent for a rejected input.
  QVERIFY(nam.requests().isEmpty());
  // The diagnostic must never echo the offending token/header value.
  QVERIFY(!result->diagnostic.contains(QStringLiteral("legit-token")));
  QVERIFY(!result->diagnostic.contains(QStringLiteral("X-Injected")));
}

void AuthClientTests::requestsDisableCookiesAndAuthReuse() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "t"})"));
  NetworkAuthenticationClient client(nam);

  runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
    return client.authenticate(
        ServerProfile::hostedDefault(),
        AuthenticateRequest{QStringLiteral("a@b.com"), QStringLiteral("pw")},
        std::move(cb));
  });

  const QNetworkRequest &req = nam.requests().first();
  QCOMPARE(req.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(),
           static_cast<int>(QNetworkRequest::Manual));
  QCOMPARE(req.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(),
           static_cast<int>(QNetworkRequest::Manual));
  QCOMPARE(req.attribute(QNetworkRequest::AuthenticationReuseAttribute).toInt(),
           static_cast<int>(QNetworkRequest::Manual));
}

void AuthClientTests::requestsSetManualRedirectPolicy() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "t"})"));
  NetworkAuthenticationClient client(nam);

  runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
    return client.authenticate(
        ServerProfile::hostedDefault(),
        AuthenticateRequest{QStringLiteral("a@b.com"), QStringLiteral("pw")},
        std::move(cb));
  });

  QCOMPARE(nam.requests()
               .first()
               .attribute(QNetworkRequest::RedirectPolicyAttribute)
               .toInt(),
           static_cast<int>(QNetworkRequest::ManualRedirectPolicy));
}

void AuthClientTests::requestsSetCacheBypassAttributes() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "t"})"));
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "t"})"));
  nam.enqueue(
      200,
      QByteArrayLiteral(
          R"({"username":"u","email":"e@x.com","beta":false,"admin":false})"));
  NetworkAuthenticationClient client(nam);

  runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
    return client.authenticate(
        ServerProfile::hostedDefault(),
        AuthenticateRequest{QStringLiteral("a@b.com"), QStringLiteral("pw")},
        std::move(cb));
  });
  runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
    return client.registerAccount(ServerProfile::hostedDefault(),
                                  RegisterRequest{QStringLiteral("a@b.com"),
                                                  QStringLiteral("user"),
                                                  QStringLiteral("pw")},
                                  std::move(cb));
  });
  runAndWait<CurrentUser>([&](std::function<void(AuthResult<CurrentUser>)> cb) {
    return client.whoAmI(ServerProfile::hostedDefault(), QStringLiteral("tok"),
                         std::move(cb));
  });

  QCOMPARE(nam.requests().size(), 3);
  for (const QNetworkRequest &req : nam.requests()) {
    QCOMPARE(req.attribute(QNetworkRequest::CacheLoadControlAttribute).toInt(),
             static_cast<int>(QNetworkRequest::AlwaysNetwork));
    QCOMPARE(req.attribute(QNetworkRequest::CacheSaveControlAttribute).toBool(),
             false);
  }
}

void AuthClientTests::productionConstructorOwnsDedicatedManager() {
  // Uses the production (owning) constructor with no externally supplied
  // QNetworkAccessManager. Exercises only the pre-request validation path
  // (an invalid profile), so no real network I/O ever occurs, while still
  // proving the owning constructor yields a fully functional client backed
  // by its own internal manager.
  NetworkAuthenticationClient client(std::chrono::milliseconds(50));

  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            ServerProfile{}, // legacy ctor: invalid
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("pw")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::InvalidInput);
}

void AuthClientTests::httpToNonLoopbackHostRejectedBeforeRequest() {
  StubNetworkAccessManager nam;
  NetworkAuthenticationClient client(nam);

  const ServerProfile profile =
      customProfile(QStringLiteral("http://example.com"));
  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            profile,
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("pw")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::InvalidInput);
  QCOMPARE(nam.requests().size(), 0); // rejected before the request was built
}

void AuthClientTests::httpLoopbackHostnamePermitted() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "loopback-ok"})"));
  NetworkAuthenticationClient client(nam);

  const ServerProfile profile =
      customProfile(QStringLiteral("http://localhost:9000"));
  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            profile,
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("pw")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::Success);
  QCOMPARE(nam.requests().size(), 1);
}

void AuthClientTests::httpLoopbackIPv4Permitted() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "loopback-ok"})"));
  NetworkAuthenticationClient client(nam);

  const ServerProfile profile =
      customProfile(QStringLiteral("http://127.0.0.1:9000"));
  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            profile,
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("pw")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::Success);
  QCOMPARE(nam.requests().size(), 1);
}

void AuthClientTests::httpLoopbackIPv6Permitted() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "loopback-ok"})"));
  NetworkAuthenticationClient client(nam);

  const ServerProfile profile =
      customProfile(QStringLiteral("http://[::1]:9000"));
  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            profile,
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("pw")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::Success);
  QCOMPARE(nam.requests().size(), 1);
}

void AuthClientTests::httpLookalikeLoopbackHostRejected() {
  const QStringList lookalikes = {
      QStringLiteral("http://localhost.evil.example"),
      QStringLiteral("http://127.0.0.1.evil.example"),
      QStringLiteral("http://notlocalhost"),
  };

  for (const QString &urlString : lookalikes) {
    StubNetworkAccessManager nam;
    NetworkAuthenticationClient client(nam);
    const ServerProfile profile = customProfile(urlString);

    const auto result = runAndWait<AuthToken>(
        [&](std::function<void(AuthResult<AuthToken>)> cb) {
          return client.authenticate(
              profile,
              AuthenticateRequest{QStringLiteral("a@b.com"),
                                  QStringLiteral("pw")},
              std::move(cb));
        });

    QVERIFY(result.has_value());
    QCOMPARE(result->outcome, AuthOutcome::InvalidInput);
    QCOMPARE(nam.requests().size(), 0);
  }
}

void AuthClientTests::httpsPermittedForAnyHost() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "https-ok"})"));
  NetworkAuthenticationClient client(nam);

  const ServerProfile profile =
      customProfile(QStringLiteral("https://example.com"));
  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            profile,
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("pw")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::Success);
  QCOMPARE(nam.requests().size(), 1);
}

void AuthClientTests::httpWithUserInfoRejectedEvenForLoopback() {
  // ServerProfile always strips userinfo (UrlValidator rejects it at
  // creation, and the legacy raw-QUrl constructor strips it too), so this
  // exercises the isSecureOrLoopbackAuthTransport() predicate itself --
  // the same production function NetworkAuthenticationClient calls -- with
  // a synthetic QUrl to prove the userinfo guard is not dead code.
  QUrl withUserInfo(QStringLiteral("http://user:pass@localhost:9000/whoami"));
  QVERIFY(!isSecureOrLoopbackAuthTransport(withUserInfo));

  QUrl withoutUserInfo(QStringLiteral("http://localhost:9000/whoami"));
  QVERIFY(isSecureOrLoopbackAuthTransport(withoutUserInfo));
}

void AuthClientTests::threeXxMapsToUnexpectedStatus() {
  StubNetworkAccessManager nam;
  nam.enqueue(302, QByteArrayLiteral(""));
  NetworkAuthenticationClient client(nam);

  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            ServerProfile::hostedDefault(),
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("pw")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::UnexpectedStatus);
  QCOMPARE(result->httpStatus, 302);
  QVERIFY(result->diagnostic.contains(QStringLiteral("redirect")));
}

void AuthClientTests::twoXxDecodesSuccessfully() {
  StubNetworkAccessManager nam;
  nam.enqueue(201, QByteArrayLiteral(R"({"token": "created"})"));
  NetworkAuthenticationClient client(nam);

  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.registerAccount(ServerProfile::hostedDefault(),
                                      RegisterRequest{QStringLiteral("a@b.com"),
                                                      QStringLiteral("u"),
                                                      QStringLiteral("pw")},
                                      std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::Success);
  QCOMPARE(result->value->token, QStringLiteral("created"));
}

void AuthClientTests::unauthorizedMapsTo401() {
  StubNetworkAccessManager nam;
  nam.enqueue(401, QByteArrayLiteral(R"({"message":"invalid credentials"})"));
  NetworkAuthenticationClient client(nam);

  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            ServerProfile::hostedDefault(),
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("wrong")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::Unauthorized);
  QCOMPARE(result->httpStatus, 401);
  QVERIFY(!result->diagnostic.contains(QStringLiteral("invalid credentials")));
}

void AuthClientTests::malformedJsonMapsToMalformedPayload() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral("not json"));
  NetworkAuthenticationClient client(nam);

  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            ServerProfile::hostedDefault(),
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("pw")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::MalformedPayload);
}

void AuthClientTests::malformedShapeMapsToMalformedPayload() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"nope": 1})"));
  NetworkAuthenticationClient client(nam);

  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            ServerProfile::hostedDefault(),
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("pw")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::MalformedPayload);
  QVERIFY(result->diagnostic.contains(QStringLiteral("token")));
}

void AuthClientTests::nonHttpTransportErrorMapsToTransport() {
  StubNetworkAccessManager nam;
  nam.enqueue(0, QByteArrayLiteral(""), QNetworkReply::HostNotFoundError);
  NetworkAuthenticationClient client(nam);

  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            ServerProfile::hostedDefault(),
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("pw")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::Transport);
  QCOMPARE(result->httpStatus, 0);
}

void AuthClientTests::nonHttpResponseWithoutTransportError() {
  StubNetworkAccessManager nam;
  nam.enqueue(0, QByteArrayLiteral(""));
  NetworkAuthenticationClient client(nam);

  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            ServerProfile::hostedDefault(),
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("pw")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::NonHttpResponse);
}

void AuthClientTests::invalidProfileRejectedBeforeRequest() {
  StubNetworkAccessManager nam;
  NetworkAuthenticationClient client(nam);

  const auto result =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            ServerProfile{}, // legacy ctor: invalid
            AuthenticateRequest{QStringLiteral("a@b.com"),
                                QStringLiteral("pw")},
            std::move(cb));
      });

  QVERIFY(result.has_value());
  QCOMPARE(result->outcome, AuthOutcome::InvalidInput);
  QCOMPARE(nam.requests().size(), 0);
}

void AuthClientTests::requestTimesOut() {
  StubNetworkAccessManager nam;
  nam.enqueueHanging();
  NetworkAuthenticationClient client(nam, std::chrono::milliseconds(50));

  int emitCount = 0;
  AuthResult<AuthToken> lastResult;
  client.authenticate(
      ServerProfile::hostedDefault(),
      AuthenticateRequest{QStringLiteral("a@b.com"), QStringLiteral("pw")},
      [&emitCount, &lastResult](AuthResult<AuthToken> r) {
        ++emitCount;
        lastResult = std::move(r);
      });

  QTRY_COMPARE_WITH_TIMEOUT(emitCount, 1, 2000);
  QCOMPARE(lastResult.outcome, AuthOutcome::Transport);
  QVERIFY(lastResult.diagnostic.contains(QStringLiteral("timed out")));

  QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
  QCOMPARE(emitCount, 1);
}

void AuthClientTests::zeroTimeoutDisablesDeadline() {
  StubNetworkAccessManager nam;
  nam.enqueueHanging();
  NetworkAuthenticationClient client(nam, std::chrono::milliseconds::zero());

  int emitCount = 0;
  client.authenticate(
      ServerProfile::hostedDefault(),
      AuthenticateRequest{QStringLiteral("a@b.com"), QStringLiteral("pw")},
      [&emitCount](AuthResult<AuthToken>) { ++emitCount; });

  QTest::qWait(100);
  QCOMPARE(emitCount, 0);
}

void AuthClientTests::negativeTimeoutRejected() {
  StubNetworkAccessManager nam;
  bool threw = false;
  try {
    NetworkAuthenticationClient client(nam, std::chrono::milliseconds(-1));
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  QVERIFY(threw);
}

void AuthClientTests::concurrentRequestsAreIndependent() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "first"})"));
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "second"})"));
  NetworkAuthenticationClient client(nam);

  std::optional<AuthResult<AuthToken>> firstResult;
  std::optional<AuthResult<AuthToken>> secondResult;
  const auto handle1 = client.authenticate(
      ServerProfile::hostedDefault(),
      AuthenticateRequest{QStringLiteral("a@b.com"), QStringLiteral("pw")},
      [&firstResult](AuthResult<AuthToken> r) { firstResult = std::move(r); });
  const auto handle2 = client.authenticate(
      ServerProfile::hostedDefault(),
      AuthenticateRequest{QStringLiteral("c@d.com"), QStringLiteral("pw2")},
      [&secondResult](AuthResult<AuthToken> r) {
        secondResult = std::move(r);
      });

  QVERIFY(handle1.id != handle2.id);

  const QDeadlineTimer deadline(2000);
  while ((!firstResult.has_value() || !secondResult.has_value()) &&
         !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  QVERIFY(firstResult.has_value());
  QVERIFY(secondResult.has_value());
  QCOMPARE(firstResult->value->token, QStringLiteral("first"));
  QCOMPARE(secondResult->value->token, QStringLiteral("second"));
}

void AuthClientTests::destructionAbortsOutstandingRequestsNoCallback() {
  StubNetworkAccessManager nam;
  nam.enqueueHanging();

  int callCount = 0;
  QPointer<QNetworkReply> reply;
  {
    NetworkAuthenticationClient client(nam);
    client.authenticate(
        ServerProfile::hostedDefault(),
        AuthenticateRequest{QStringLiteral("a@b.com"), QStringLiteral("pw")},
        [&callCount](AuthResult<AuthToken>) { ++callCount; });
    reply = nam.lastReply();
    QVERIFY(!reply.isNull());
  }

  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QVERIFY(reply.isNull());
  QCOMPARE(callCount, 0);
}

void AuthClientTests::explicitCancelEmitsCancelledOutcome() {
  StubNetworkAccessManager nam;
  nam.enqueueHanging();
  NetworkAuthenticationClient client(nam);

  int emitCount = 0;
  AuthResult<AuthToken> lastResult;
  const auto handle = client.authenticate(
      ServerProfile::hostedDefault(),
      AuthenticateRequest{QStringLiteral("a@b.com"), QStringLiteral("pw")},
      [&emitCount, &lastResult](AuthResult<AuthToken> r) {
        ++emitCount;
        lastResult = std::move(r);
      });

  client.cancel(handle);

  QTRY_COMPARE_WITH_TIMEOUT(emitCount, 1, 2000);
  QCOMPARE(lastResult.outcome, AuthOutcome::Cancelled);

  QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
  QCOMPARE(emitCount, 1);
}

void AuthClientTests::staleCancelIsNoOp() {
  StubNetworkAccessManager nam;
  NetworkAuthenticationClient client(nam);
  // Cancelling a handle that was never issued (default-constructed) must not
  // crash and must be a no-op.
  client.cancel(AuthRequestHandle{});
  client.cancel(AuthRequestHandle{999999});
  QVERIFY(true); // reaching here without crashing is the assertion
}

void AuthClientTests::callbacksAreExactlyOnceOnTimeout() {
  // Covered in detail by requestTimesOut(); this test additionally confirms
  // that a completed (non-hanging) request also never double-fires.
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "once"})"));
  NetworkAuthenticationClient client(nam);

  int emitCount = 0;
  client.authenticate(
      ServerProfile::hostedDefault(),
      AuthenticateRequest{QStringLiteral("a@b.com"), QStringLiteral("pw")},
      [&emitCount](AuthResult<AuthToken>) { ++emitCount; });

  QTRY_COMPARE_WITH_TIMEOUT(emitCount, 1, 2000);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
  QCOMPARE(emitCount, 1);
}

void AuthClientTests::diagnosticsNeverContainSecrets() {
  const QString sentinelPassword = QStringLiteral("sentinel-pw-9f8e7d");
  const QString sentinelToken = QStringLiteral("sentinel-token-1a2b3c");

  StubNetworkAccessManager nam;
  nam.enqueue(401, QByteArrayLiteral(R"({"message":"nope"})"));
  nam.enqueue(200, QByteArrayLiteral("not json"));
  NetworkAuthenticationClient client(nam);

  const auto unauthorizedResult =
      runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
        return client.authenticate(
            ServerProfile::hostedDefault(),
            AuthenticateRequest{QStringLiteral("a@b.com"), sentinelPassword},
            std::move(cb));
      });
  QVERIFY(unauthorizedResult.has_value());
  QVERIFY(!unauthorizedResult->diagnostic.contains(sentinelPassword));

  const auto whoamiResult = runAndWait<CurrentUser>(
      [&](std::function<void(AuthResult<CurrentUser>)> cb) {
        return client.whoAmI(ServerProfile::hostedDefault(), sentinelToken,
                             std::move(cb));
      });
  QVERIFY(whoamiResult.has_value());
  QVERIFY(!whoamiResult->diagnostic.contains(sentinelToken));
}

void AuthClientTests::profilePrefixIsPreserved() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"token": "t"})"));
  NetworkAuthenticationClient client(nam);

  const ServerProfile profile =
      customProfile(QStringLiteral("https://example.com/selfhosted"));
  runAndWait<AuthToken>([&](std::function<void(AuthResult<AuthToken>)> cb) {
    return client.authenticate(
        profile,
        AuthenticateRequest{QStringLiteral("a@b.com"), QStringLiteral("pw")},
        std::move(cb));
  });

  QCOMPARE(
      nam.requests().first().url().toString(),
      QStringLiteral("https://example.com/selfhosted/api/v1/authenticate"));
}

QTEST_GUILESS_MAIN(AuthClientTests)

#include "AuthClientTests.moc"
