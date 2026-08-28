// Tests for: URL validation, ServerProfile factories, QSettingsProfileStore,
// and NetworkCapabilityProbe (via a stub QNetworkAccessManager).
//
// Uses QTEST_GUILESS_MAIN because queued probe delivery needs a
// QCoreApplication event loop, but these tests do not need a GUI application.

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QQueue>
#include <QSettings>
#include <QTemporaryFile>
#include <QtGlobal>
#include <QtTest>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <utility>

#include "AuthTransportSecurity.h"
#include "ICapabilityProbe.h"
#include "IProfileStore.h"
#include "NetworkCapabilityProbe.h"
#include "ProbeResult.h"
#include "QSettingsProfileStore.h"
#include "ServerProfile.h"
#include "UrlValidator.h"
#include "ValueOrError.h"

using namespace Arkham;

// ─── Stub QNetworkReply ────────────────────────────────────────────────────
//
// Schedules finished() on the next event-loop iteration so that probe tests
// exercise the same async delivery path as production code.

class StubNetworkReply final : public QNetworkReply {
  Q_OBJECT
public:
  // Pass statusCode == 0 to simulate a transport failure (no HTTP attribute).
  // Pass hanging == true for a reply that never finishes until abort() is
  // called; used to test probe timeout behaviour.
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
      return 0; // clean EOF; -1 would signal an I/O error
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

// ─── Stub QNetworkAccessManager ───────────────────────────────────────────

class StubNetworkAccessManager final : public QNetworkAccessManager {
  Q_OBJECT
public:
  explicit StubNetworkAccessManager(QObject *parent = nullptr)
      : QNetworkAccessManager(parent) {}

  void enqueue(int statusCode, QByteArray body,
               QNetworkReply::NetworkError err = QNetworkReply::NoError) {
    m_queue.enqueue({statusCode, std::move(body), err, false});
  }

  // Enqueue a reply that never emits finished() until abort() is called.
  // Used to test probe timeout behaviour.
  void enqueueHanging() {
    m_queue.enqueue({0, {}, QNetworkReply::NoError, true});
  }

  QUrl lastUrl() const { return m_lastUrl; }
  QString lastAcceptHeader() const { return m_lastAcceptHeader; }
  QPointer<QNetworkReply> lastReply() const { return m_lastReply; }
  // Returns the full last request so callers can inspect any attribute/header.
  const QNetworkRequest &lastRequest() const { return m_lastRequest; }

protected:
  QNetworkReply *createRequest(Operation, const QNetworkRequest &req,
                               QIODevice *) override {
    m_lastUrl = req.url();
    m_lastAcceptHeader = QString::fromLatin1(req.rawHeader("Accept"));
    m_lastRequest = req;
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
  QUrl m_lastUrl;
  QString m_lastAcceptHeader;
  QNetworkRequest m_lastRequest;
  QPointer<QNetworkReply> m_lastReply;
};

// ─── Helpers ──────────────────────────────────────────────────────────────

// Loads a vendored contract fixture.  relPath must start with '/'.
// Returns a failure with a diagnostic on any I/O or path error.
static ValueOrError<QByteArray> loadContractFixture(const QString &relPath) {
  if (!relPath.startsWith(QLatin1Char('/'))) {
    return failure(
        QStringLiteral("contract path must start with '/': %1").arg(relPath));
  }
  const QString path = QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + relPath;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    return failure(
        QStringLiteral("cannot open %1: %2").arg(path, f.errorString()));
  }
  return f.readAll();
}

// Runs an async probe and returns ValueOrError<ProbeResult>.
//
// Disconnect before returning so the lambda cannot access the captured stack
// variable after its lifetime ends.
static ValueOrError<ProbeResult> runProbe(NetworkCapabilityProbe &probe,
                                          const ServerProfile &profile) {
  std::optional<ProbeResult> captured;
  QMetaObject::Connection conn;
  conn = QObject::connect(
      &probe, &ICapabilityProbe::finished,
      [&captured](ProbeResult result) { captured = std::move(result); });
  probe.probe(profile);
  const QDeadlineTimer deadline(2000);
  while (!captured.has_value() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  }
  QObject::disconnect(conn); // safe no-op if already fired
  if (!captured.has_value()) {
    return failure(
        QStringLiteral("probe did not emit finished within 2000 ms"));
  }
  return std::move(*captured);
}

// ─── Test class ───────────────────────────────────────────────────────────

class NetworkTests final : public QObject {
  Q_OBJECT

private slots:
  // ── URL validation ────────────────────────────────────────────────────────
  void urlAcceptsHttps();
  void urlAcceptsHttp();
  void urlAcceptsNonDefaultPort();
  void urlAcceptsPathPrefix();
  void urlStripsTrailingSlash();
  void urlTrimsSurroundingWhitespace();
  void urlRejectsControlCharactersAtEdges();
  void urlRejectsEmpty();
  void urlRejectsNonHttpScheme();
  void urlRejectsMissingHost();
  void urlRejectsCredentials();
  void urlRejectsFragment();
  void urlRejectsQuery();
  void urlRejectsDuplicateApiPathExact();
  void urlRejectsDuplicateApiPathPrefix();
  void urlRejectsDuplicateApiPathInfix();    // /proxy/api/v1
  void urlRejectsDuplicateApiPathInfixSub(); // /proxy/api/v1/foo
  void urlAcceptsApiV10();                   // /api/v10 is valid
  void urlRejectsInsecureTransport();
  void urlStrictLoopbackPolicy_data();
  void urlStrictLoopbackPolicy();
  void rawIPv6BracketTrailingGarbageRejected();
  void rawPortValidationRejectsMalformedAndOutOfRangePorts();

  // ── ServerProfile factories and path construction ─────────────────────────
  void hostedDefaultProperties();
  void hostedDefaultHasStableId();
  void hostedDefaultApiUrl();
  void customProfileRejectsInvalidUrl();
  void customProfileRejectsBlankDisplayName();
  void customProfileRejectsWhitespaceOnlyDisplayName();
  void customProfileTrimsDisplayName();
  void customProfileHasUniqueIds();
  void customProfileWithNonDefaultPort();
  void customProfileWithPathPrefix();
  void customProfileApiUrlWithPrefix();
  void customProfileWebsocketUrlWithPrefix();
  void customWithIdRejectsReservedId();
  void customWithIdCanonicalizesBracedUuid();

  // ── QSettingsProfileStore persistence ─────────────────────────────────────
  void storeEmptyOnFirstRun();
  void storeRoundTripHosted();
  void storeRoundTripCustom();
  void storeRoundTripCustomPreservesId();
  void storeRoundTripMixed();
  void storeSelectionByIdRoundTrip();
  void storeSelectionNoSelectionOnFirstRun();
  void storeSelectionSurvivesReorder();
  void storeCorruptKind();
  void storeCorruptCustomUrl();
  void storeCorruptProfileId();
  void storeCorruptSelectedProfileId();
  void storeEmptySelectedProfileIdIsNoSelection();
  void storeSaveRejectsProfileWithoutId();
  void storeSaveRejectsDuplicateIds();
  void storeSaveRemovesDeletedProfileKeys();
  void storeSaveChecksStatusAfterSync();
  void storeLoadBalancesBeginEndArray();
  void storeLoadRejectsDuplicateIds();
  void storeLoadRejectsWrongHostedId();
  void storeSaveSelectedInvalidUuid();
  void storeSelectionCanonicalizes();
  void storeSaveFailsWithPreExistingError();
  void storeLoadSelectedIdFailsOnError();

  // ── NetworkCapabilityProbe outcomes ───────────────────────────────────────
  void probeUrlIsCapabilitiesEndpoint();
  void probeSendsAcceptHeader();
  void probeCompatibleViaFixture();
  void probe404LegacyFallback();
  void probeMalformedJsonSyntax();
  void probeMalformedJsonNotObject();
  void probeMalformedJsonBadSchema();
  void probeNonSuccessStatus();
  void probeIncompatible();
  void probeTransportFailure();
  void probeRejectsInvalidProfile();
  void probe2xxWithReplyError();
  void probeNoAuthHeader();
  void probeNoCookies();
  void probeDestructionDeletesInFlightReply();
  void probeTimesOut();
  void probeZeroTimeoutDisablesDeadline();
  void probeRejectsNegativeTimeout();
  void probeRedirectPolicySet();
  void probeRedirectPolicyIgnoresManagerGlobal();

  // ── Fixture helper ────────────────────────────────────────────────────────
  void fixtureHelperRequiresLeadingSlash();
  void fixtureHelperFailsOnMissingFile();
};

// ─── URL validation ───────────────────────────────────────────────────────

void NetworkTests::urlAcceptsHttps() {
  const auto r = validateCustomUrl(QStringLiteral("https://example.com"));
  QVERIFY(r.has_value());
  QCOMPARE(r->scheme(), QStringLiteral("https"));
  QCOMPARE(r->host(), QStringLiteral("example.com"));
}

void NetworkTests::urlAcceptsHttp() {
  const auto r = validateCustomUrl(QStringLiteral("http://localhost"));
  QVERIFY(r.has_value());
  QCOMPARE(r->scheme(), QStringLiteral("http"));
}

void NetworkTests::urlAcceptsNonDefaultPort() {
  const auto r = validateCustomUrl(QStringLiteral("http://localhost:3000"));
  QVERIFY(r.has_value());
  QCOMPARE(r->port(), 3000);
}

void NetworkTests::urlAcceptsPathPrefix() {
  const auto r = validateCustomUrl(
      QStringLiteral("https://192.168.1.100:8080/selfhosted"));
  QVERIFY(r.has_value());
  QCOMPARE(r->path(), QStringLiteral("/selfhosted"));
  QCOMPARE(r->port(), 8080);
}

void NetworkTests::urlStripsTrailingSlash() {
  const auto r =
      validateCustomUrl(QStringLiteral("https://example.com/prefix/"));
  QVERIFY(r.has_value());
  QCOMPARE(r->path(), QStringLiteral("/prefix"));
}

void NetworkTests::urlTrimsSurroundingWhitespace() {
  // This exercises plain ASCII space (U+0020), which QString::trimmed()
  // strips. trimmed() also silently strips other QChar::isSpace() code
  // points that are NOT control characters -- e.g. Unicode space
  // separators such as NBSP (U+00A0) or EM SPACE (U+2003) -- and that is
  // fine: those are ordinary whitespace, not a policy concern here (see
  // the containsControlCharacter() comment in UrlValidator.cpp). What is
  // rejected outright, before trimming even runs, is any Unicode "control"
  // character (category Cc: tab, newline, CR, and similar), anywhere in
  // the original input -- including purely leading or trailing ones (see
  // UrlErrorCode::ControlCharacterPresent and
  // urlStrictLoopbackPolicy_data()'s "-trailing-tab-"/"-trailing-newline"
  // rows), precisely so a trailing control character cannot be silently
  // laundered into a clean-looking, accepted URL.
  const auto r =
      validateCustomUrl(QStringLiteral("  https://example.com/prefix  "));
  QVERIFY(r.has_value());
  QCOMPARE(*r, QUrl(QStringLiteral("https://example.com/prefix")));
}

// Complements urlTrimsSurroundingWhitespace(): proves a control character
// at either edge of the ORIGINAL input is rejected outright, for an
// ordinary https URL (not merely the http-loopback-specific rows in
// urlStrictLoopbackPolicy_data()) -- i.e. this is not a loopback-only
// policy but applies to validateCustomUrl()'s input handling universally,
// before QString::trimmed() (which would otherwise silently strip a
// leading/trailing tab or newline, same as plain space) ever runs.
void NetworkTests::urlRejectsControlCharactersAtEdges() {
  const auto leading =
      validateCustomUrl(QStringLiteral("\thttps://example.com"));
  QVERIFY(!leading.has_value());
  QCOMPARE(leading.error().code, UrlErrorCode::ControlCharacterPresent);

  const auto trailing =
      validateCustomUrl(QStringLiteral("https://example.com\n"));
  QVERIFY(!trailing.has_value());
  QCOMPARE(trailing.error().code, UrlErrorCode::ControlCharacterPresent);

  const auto embedded =
      validateCustomUrl(QStringLiteral("https://exa\rmple.com"));
  QVERIFY(!embedded.has_value());
  QCOMPARE(embedded.error().code, UrlErrorCode::ControlCharacterPresent);
}

void NetworkTests::urlRejectsEmpty() {
  const auto r = validateCustomUrl(QString{});
  QVERIFY(!r.has_value());
  QCOMPARE(r.error().code, UrlErrorCode::InvalidUrl);
}

void NetworkTests::urlRejectsNonHttpScheme() {
  const auto r = validateCustomUrl(QStringLiteral("ftp://example.com"));
  QVERIFY(!r.has_value());
  QCOMPARE(r.error().code, UrlErrorCode::UnsupportedScheme);

  const auto r2 = validateCustomUrl(QStringLiteral("file:///etc/passwd"));
  QVERIFY(!r2.has_value());
  QCOMPARE(r2.error().code, UrlErrorCode::UnsupportedScheme);
}

void NetworkTests::urlRejectsMissingHost() {
  const auto r = validateCustomUrl(QStringLiteral("https:///missing-host"));
  QVERIFY(!r.has_value());
  QCOMPARE(r.error().code, UrlErrorCode::MissingHost);
}

void NetworkTests::urlRejectsCredentials() {
  const auto r =
      validateCustomUrl(QStringLiteral("https://user:pass@example.com"));
  QVERIFY(!r.has_value());
  QCOMPARE(r.error().code, UrlErrorCode::CredentialsPresent);
}

void NetworkTests::urlRejectsFragment() {
  const auto r =
      validateCustomUrl(QStringLiteral("https://example.com#section"));
  QVERIFY(!r.has_value());
  QCOMPARE(r.error().code, UrlErrorCode::FragmentPresent);
}

void NetworkTests::urlRejectsQuery() {
  const auto r =
      validateCustomUrl(QStringLiteral("https://example.com?foo=bar"));
  QVERIFY(!r.has_value());
  QCOMPARE(r.error().code, UrlErrorCode::QueryPresent);
}

void NetworkTests::urlRejectsDuplicateApiPathExact() {
  const auto r =
      validateCustomUrl(QStringLiteral("https://example.com/api/v1"));
  QVERIFY(!r.has_value());
  QCOMPARE(r.error().code, UrlErrorCode::DuplicateApiPath);
}

void NetworkTests::urlRejectsDuplicateApiPathPrefix() {
  const auto r =
      validateCustomUrl(QStringLiteral("https://example.com/api/v1/whoami"));
  QVERIFY(!r.has_value());
  QCOMPARE(r.error().code, UrlErrorCode::DuplicateApiPath);
}

void NetworkTests::urlRejectsDuplicateApiPathInfix() {
  // /proxy/api/v1 — /api/v1 segment appears in the middle and at end
  const auto r =
      validateCustomUrl(QStringLiteral("https://example.com/proxy/api/v1"));
  QVERIFY(!r.has_value());
  QCOMPARE(r.error().code, UrlErrorCode::DuplicateApiPath);
}

void NetworkTests::urlRejectsDuplicateApiPathInfixSub() {
  // /proxy/api/v1/capabilities — infix with trailing sub-path
  const auto r = validateCustomUrl(
      QStringLiteral("https://example.com/proxy/api/v1/capabilities"));
  QVERIFY(!r.has_value());
  QCOMPARE(r.error().code, UrlErrorCode::DuplicateApiPath);
}

void NetworkTests::urlAcceptsApiV10() {
  // /api/v10 must not be confused with /api/v1 — "v10" is a different segment
  const auto r =
      validateCustomUrl(QStringLiteral("https://example.com/api/v10"));
  QVERIFY2(r.has_value(), qPrintable(r.error().message));
  QCOMPARE(r->path(), QStringLiteral("/api/v10"));
}

void NetworkTests::urlRejectsInsecureTransport() {
  const auto r = validateCustomUrl(QStringLiteral("http://example.com"));
  QVERIFY(!r.has_value());
  QCOMPARE(r.error().code, UrlErrorCode::InsecureTransport);
}

void NetworkTests::urlStrictLoopbackPolicy_data() {
  QTest::addColumn<QString>("urlString");
  QTest::addColumn<bool>("expectAccepted");
  // Meaningful only when expectAccepted is false: the exact UrlErrorCode
  // (stored as int; UrlErrorCode is not a registered QMetaType) every
  // rejected row must produce. Asserting the precise code per row (rather
  // than accepting either InsecureTransport or CredentialsPresent for
  // every row) ensures a regression that misclassifies one row under the
  // wrong error code is still caught.
  QTest::addColumn<int>("expectedErrorCode");

  const int invalidUrl = static_cast<int>(UrlErrorCode::InvalidUrl);
  const int controlCharacterPresent =
      static_cast<int>(UrlErrorCode::ControlCharacterPresent);
  const int insecureTransport =
      static_cast<int>(UrlErrorCode::InsecureTransport);
  const int credentialsPresent =
      static_cast<int>(UrlErrorCode::CredentialsPresent);

  // ── Accepted: exact canonical loopback spellings over http ─────────────
  QTest::newRow("http-localhost")
      << QStringLiteral("http://localhost") << true << 0;
  QTest::newRow("http-localhost-port")
      << QStringLiteral("http://localhost:9000") << true << 0;
  QTest::newRow("http-localhost-path")
      << QStringLiteral("http://localhost/selfhosted") << true << 0;
  QTest::newRow("http-127.0.0.1")
      << QStringLiteral("http://127.0.0.1") << true << 0;
  QTest::newRow("http-127.0.0.1-port-path")
      << QStringLiteral("http://127.0.0.1:9000/selfhosted") << true << 0;
  QTest::newRow("http-bracketed-::1")
      << QStringLiteral("http://[::1]") << true << 0;
  QTest::newRow("http-bracketed-::1-port")
      << QStringLiteral("http://[::1]:9000") << true << 0;
  QTest::newRow("http-localhost-port-min")
      << QStringLiteral("http://localhost:1") << true << 0;
  QTest::newRow("http-localhost-port-max")
      << QStringLiteral("http://localhost:65535") << true << 0;

  // ── Accepted: https for any host, including odd literals and base paths ─
  QTest::newRow("https-any-host")
      << QStringLiteral("https://example.com") << true << 0;
  QTest::newRow("https-lan-ip")
      << QStringLiteral("https://192.168.1.100:8080/selfhosted") << true << 0;
  QTest::newRow("https-127.1-unrestricted")
      << QStringLiteral("https://127.1") << true << 0;
  QTest::newRow("https-localhost-lookalike")
      << QStringLiteral("https://localhost.evil.example") << true << 0;
  QTest::newRow("https-base-path")
      << QStringLiteral("https://example.com/arkham") << true << 0;

  // ── Rejected: ambiguous/non-canonical numeric loopback spellings ────────
  QTest::newRow("http-127.1")
      << QStringLiteral("http://127.1") << false << insecureTransport;
  QTest::newRow("http-single-integer")
      << QStringLiteral("http://2130706433") << false << insecureTransport;
  QTest::newRow("http-octal-looking")
      << QStringLiteral("http://0177.0.0.1") << false << insecureTransport;
  QTest::newRow("http-hex-looking")
      << QStringLiteral("http://0x7f.0.0.1") << false << insecureTransport;
  QTest::newRow("http-octal-single-integer")
      << QStringLiteral("http://017700000001") << false << insecureTransport;
  QTest::newRow("http-leading-zero-full")
      << QStringLiteral("http://127.000.000.001") << false << insecureTransport;
  QTest::newRow("http-leading-zero-last-octet")
      << QStringLiteral("http://127.0.0.01") << false << insecureTransport;
  QTest::newRow("http-shortened-form")
      << QStringLiteral("http://127.0.1") << false << insecureTransport;

  // ── Rejected: IPv4-mapped / expanded / alternate IPv6 spellings ─────────
  QTest::newRow("http-ipv4-mapped-ipv6")
      << QStringLiteral("http://[::ffff:127.0.0.1]") << false
      << insecureTransport;
  QTest::newRow("http-ipv6-expanded")
      << QStringLiteral("http://[0:0:0:0:0:0:0:1]") << false
      << insecureTransport;
  QTest::newRow("http-ipv6-alternate-shortened")
      << QStringLiteral("http://[::0:1]") << false << insecureTransport;
  QTest::newRow("http-ipv6-zone-id")
      << QStringLiteral("http://[::1%25eth0]") << false << insecureTransport;

  // ── Rejected: trailing-dot / subdomain / lookalike localhost forms ──────
  QTest::newRow("http-localhost-trailing-dot")
      << QStringLiteral("http://localhost.") << false << insecureTransport;
  QTest::newRow("http-localhost-subdomain")
      << QStringLiteral("http://localhost.evil.example") << false
      << insecureTransport;
  QTest::newRow("http-127-lookalike-subdomain")
      << QStringLiteral("http://127.0.0.1.evil.example") << false
      << insecureTransport;
  QTest::newRow("http-notlocalhost")
      << QStringLiteral("http://notlocalhost") << false << insecureTransport;

  // ── Rejected: userinfo, malformed, LAN/public addresses over http ───────
  // Userinfo is rejected unconditionally (CredentialsPresent), even for an
  // otherwise-canonical loopback host, and is checked before the
  // http/loopback policy itself (see UrlValidator::validateCustomUrl()'s
  // ordering), so this row must produce CredentialsPresent, not
  // InsecureTransport.
  QTest::newRow("http-userinfo-on-loopback")
      << QStringLiteral("http://user:pass@localhost:9000") << false
      << credentialsPresent;
  QTest::newRow("http-lan-ip")
      << QStringLiteral("http://192.168.1.100") << false << insecureTransport;
  QTest::newRow("http-public-host")
      << QStringLiteral("http://example.com") << false << insecureTransport;

  // ── Rejected: malformed/absent/out-of-range ports on an otherwise-exact
  //    loopback host or bracketed IPv6 literal. QUrl's own StrictMode
  //    parsing already rejects most malformed port syntax (non-digit
  //    garbage, percent-escapes, double colons, negative, >65535) as an
  //    unparseable URL entirely (UrlErrorCode::InvalidUrl) before this
  //    function's raw-authority port check is ever reached; the remaining
  //    two forms QUrl treats as syntactically valid -- an empty port
  //    ("host:") and port 0 -- are only caught by this function's own
  //    stricter, in-range (1..65535), non-empty port rule
  //    (UrlErrorCode::InsecureTransport, since the raw-text loopback policy
  //    check is what actually rejects them). Both groups are asserted here
  //    through the real public path so a regression in either QUrl's
  //    parsing assumptions or this function's own port rule is caught.
  QTest::newRow("http-localhost-empty-port")
      << QStringLiteral("http://localhost:") << false << insecureTransport;
  QTest::newRow("http-localhost-zero-port")
      << QStringLiteral("http://localhost:0") << false << insecureTransport;
  QTest::newRow("http-bracketed-::1-empty-port")
      << QStringLiteral("http://[::1]:") << false << insecureTransport;
  QTest::newRow("http-localhost-non-numeric-port")
      << QStringLiteral("http://localhost:evil") << false << invalidUrl;
  QTest::newRow("http-localhost-percent-escaped-port")
      << QStringLiteral("http://localhost:%39") << false << invalidUrl;
  QTest::newRow("http-localhost-double-colon-port")
      << QStringLiteral("http://localhost::9000") << false << invalidUrl;
  QTest::newRow("http-localhost-multiple-port-segments")
      << QStringLiteral("http://localhost:9000:9000") << false << invalidUrl;
  QTest::newRow("http-bracketed-::1-non-numeric-port")
      << QStringLiteral("http://[::1]:evil") << false << invalidUrl;
  QTest::newRow("http-localhost-port-overflow")
      << QStringLiteral("http://localhost:99999") << false << invalidUrl;
  QTest::newRow("http-localhost-port-negative")
      << QStringLiteral("http://localhost:-1") << false << invalidUrl;

  // ── Rejected: control characters in the ORIGINAL input, which must not
  //    be laundered away by trimmed() before this policy ever sees them.
  QTest::newRow("http-localhost-trailing-tab-after-port")
      << QStringLiteral("http://localhost:9000\t") << false
      << controlCharacterPresent;
  QTest::newRow("http-localhost-tab-before-colon")
      << QStringLiteral("http://localhost\t:9000") << false
      << controlCharacterPresent;
  QTest::newRow("http-localhost-trailing-newline")
      << QStringLiteral("http://localhost\n") << false
      << controlCharacterPresent;
}

void NetworkTests::urlStrictLoopbackPolicy() {
  QFETCH(QString, urlString);
  QFETCH(bool, expectAccepted);
  QFETCH(int, expectedErrorCode);

  const auto r = validateCustomUrl(urlString);
  QCOMPARE(r.has_value(), expectAccepted);
  if (!expectAccepted) {
    QCOMPARE(static_cast<int>(r.error().code), expectedErrorCode);
  }
}

// Direct, production-function-level regression for a bracketed-IPv6-literal
// parsing bug: extractRawHostFromAuthority() (the internal helper
// isCleartextAuthAllowedForRawInput() uses to pull the raw host substring
// out of a "[...]"-bracketed authority) used to return everything between
// "[" and the first "]" with no check on what followed the closing
// bracket, so a malformed raw authority like "[::1]evil" was silently
// truncated into the exact string "::1" -- indistinguishable from a
// genuinely safe, canonical "::1" loopback host.
//
// In the one production call site (UrlValidator::validateCustomUrl()),
// QUrl's own StrictMode parsing already rejects "http://[::1]evil" as a
// syntactically invalid URL before isCleartextAuthAllowedForRawInput() is
// ever reached (confirmed: QUrl requires the bracket's "]" to be followed
// immediately by either ":port" or the end of the authority). That
// QUrl-level gate is an implementation detail of one caller, though, and
// isCleartextAuthAllowedForRawInput() is itself a public function in
// AuthTransportSecurity.h with its own documented "exactly '::1', optionally
// followed by :port" contract -- so this test exercises it directly,
// independent of QUrl's incidental gatekeeping, proving the fix holds on
// its own merits and guarding against any future caller that invokes it
// against text QUrl has not already validated.
void NetworkTests::rawIPv6BracketTrailingGarbageRejected() {
  // Malformed: trailing text immediately after "]" that is not ":port".
  QVERIFY(!isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://[::1]evil")));
  QVERIFY(!isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://[::1]evil:9000")));
  QVERIFY(!isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://[::1]:9000evil")));
  // Also malformed: no closing bracket at all.
  QVERIFY(!isCleartextAuthAllowedForRawInput(QStringLiteral("http"),
                                             QStringLiteral("http://[::1")));

  // Still correctly accepted: the legitimate forms this must not regress.
  QVERIFY(isCleartextAuthAllowedForRawInput(QStringLiteral("http"),
                                            QStringLiteral("http://[::1]")));
  QVERIFY(isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://[::1]:9000")));
}

// Direct, production-function-level regression proving
// isCleartextAuthAllowedForRawInput()'s own raw-authority port parsing
// rejects malformed, empty, or out-of-range ports on its own merits -- not
// merely because QUrl happens to already reject the same text as an
// unparseable URL in the one real caller (UrlValidator::validateCustomUrl).
// Exercising the function directly here means a future caller that invokes
// it against text QUrl has not already validated (or a change to QUrl's
// own parsing leniency) cannot silently reintroduce these bypasses.
void NetworkTests::rawPortValidationRejectsMalformedAndOutOfRangePorts() {
  // Empty port after a bare host's ":" or a bracketed literal's "]:".
  QVERIFY(!isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://localhost:")));
  QVERIFY(!isCleartextAuthAllowedForRawInput(QStringLiteral("http"),
                                             QStringLiteral("http://[::1]:")));

  // Non-numeric / percent-escaped / control-bearing port suffixes.
  QVERIFY(!isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://localhost:evil")));
  QVERIFY(!isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://localhost:%39")));
  QVERIFY(!isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://localhost:9000\t")));
  QVERIFY(!isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://[::1]:evil")));

  // Malformed multiple separators.
  QVERIFY(!isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://localhost::9000")));
  QVERIFY(!isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://localhost:9000:9000")));

  // Zero, negative, and overflow ports (out of the required 1..65535
  // range; zero and (via non-digit "-") negative are only reachable
  // through this function's own range/digit check since QUrl already
  // treats them as syntactically fine except where a literal "-" makes
  // the whole URL unparseable).
  QVERIFY(!isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://localhost:0")));
  QVERIFY(!isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://localhost:99999")));

  // Still correctly accepted: valid in-range ports at both boundaries.
  QVERIFY(isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://localhost:1")));
  QVERIFY(isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://localhost:65535")));
  QVERIFY(isCleartextAuthAllowedForRawInput(
      QStringLiteral("http"), QStringLiteral("http://localhost")));
}

// ─── ServerProfile factories and path construction ────────────────────────

void NetworkTests::hostedDefaultProperties() {
  const ServerProfile p = ServerProfile::hostedDefault();
  QCOMPARE(p.kind(), ServerProfileKind::HostedDefault);
  QVERIFY(!p.displayName().isEmpty());
  QCOMPARE(p.baseUrl(), QUrl(QStringLiteral("https://arkhamhorror.app")));
  QVERIFY(p.isValid());
}

void NetworkTests::hostedDefaultHasStableId() {
  // The hosted default must always produce the same, non-empty ID.
  const QString id1 = ServerProfile::hostedDefault().profileId();
  const QString id2 = ServerProfile::hostedDefault().profileId();
  QVERIFY(!id1.isEmpty());
  QCOMPARE(id1, id2);
}

void NetworkTests::hostedDefaultApiUrl() {
  const ServerProfile p = ServerProfile::hostedDefault();
  QCOMPARE(
      p.apiUrl(u"capabilities"),
      QUrl(QStringLiteral("https://arkhamhorror.app/api/v1/capabilities")));
}

void NetworkTests::customProfileRejectsInvalidUrl() {
  const auto r =
      ServerProfile::custom(QStringLiteral("bad"), QStringLiteral("not-a-url"));
  QVERIFY(!r.has_value());
  QVERIFY(!r.error().isEmpty());
}

void NetworkTests::customProfileRejectsBlankDisplayName() {
  const auto r = ServerProfile::custom(QStringLiteral(""),
                                       QStringLiteral("https://example.com"));
  QVERIFY(!r.has_value());
  QVERIFY(r.error().contains(QStringLiteral("display name")));
}

void NetworkTests::customProfileRejectsWhitespaceOnlyDisplayName() {
  const auto r = ServerProfile::custom(QStringLiteral("   "),
                                       QStringLiteral("https://example.com"));
  QVERIFY(!r.has_value());
  QVERIFY(r.error().contains(QStringLiteral("display name")));
}

void NetworkTests::customProfileTrimsDisplayName() {
  const auto r = ServerProfile::custom(QStringLiteral("  My Server  "),
                                       QStringLiteral("https://example.com"));
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->displayName(), QStringLiteral("My Server"));
}

void NetworkTests::customProfileHasUniqueIds() {
  const auto p1 = ServerProfile::custom(
      QStringLiteral("A"), QStringLiteral("https://a.example.com"));
  const auto p2 = ServerProfile::custom(
      QStringLiteral("B"), QStringLiteral("https://b.example.com"));
  QVERIFY(p1.has_value());
  QVERIFY(p2.has_value());
  QVERIFY(!p1->profileId().isEmpty());
  QVERIFY(!p2->profileId().isEmpty());
  QVERIFY(p1->profileId() != p2->profileId());
  QVERIFY(p1->profileId() != ServerProfile::hostedDefault().profileId());
}

void NetworkTests::customProfileWithNonDefaultPort() {
  const auto r = ServerProfile::custom(QStringLiteral("Local"),
                                       QStringLiteral("http://localhost:3000"));
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->kind(), ServerProfileKind::Custom);
  QCOMPARE(r->displayName(), QStringLiteral("Local"));
  QCOMPARE(r->baseUrl().port(), 3000);
}

void NetworkTests::customProfileWithPathPrefix() {
  const auto r = ServerProfile::custom(
      QStringLiteral("Self"),
      QStringLiteral("https://myserver.example.com/arkham"));
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->baseUrl().path(), QStringLiteral("/arkham"));
}

void NetworkTests::customProfileApiUrlWithPrefix() {
  const auto r =
      ServerProfile::custom(QStringLiteral("Self"),
                            QStringLiteral("https://192.168.1.5:8080/prefix"));
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  const QUrl api = r->apiUrl(u"capabilities");
  QCOMPARE(api, QUrl(QStringLiteral(
                    "https://192.168.1.5:8080/prefix/api/v1/capabilities")));
}

void NetworkTests::customProfileWebsocketUrlWithPrefix() {
  const auto r = ServerProfile::custom(
      QStringLiteral("Self"), QStringLiteral("https://ws.example.com/arkham"));
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  // Pass only the API-relative path; the stored /arkham prefix is applied once
  // by websocketUrl → apiUrl, not repeated by the caller.
  const QUrl ws = r->websocketUrl(u"games/g1");
  QCOMPARE(ws,
           QUrl(QStringLiteral("wss://ws.example.com/arkham/api/v1/games/g1")));
}

void NetworkTests::customWithIdRejectsReservedId() {
  // The deterministic hosted-default UUID must not be assignable to custom
  // profiles — it is reserved for the single canonical hosted entry.
  const QString hostedId = ServerProfile::hostedDefault().profileId();
  const auto r = ServerProfile::customWithId(
      hostedId, QStringLiteral("X"), QStringLiteral("https://x.example.com"));
  QVERIFY(!r.has_value());
  QVERIFY(r.error().contains(QStringLiteral("reserved")));
}

void NetworkTests::customWithIdCanonicalizesBracedUuid() {
  // customWithId must accept a UUID with braces and store it without.
  const QString braced =
      QStringLiteral("{12345678-1234-1234-1234-123456789abc}");
  const QString noBraces =
      QStringLiteral("12345678-1234-1234-1234-123456789abc");
  const auto r = ServerProfile::customWithId(
      braced, QStringLiteral("P"), QStringLiteral("https://p.example.com"));
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->profileId(), noBraces);
}

// ─── QSettingsProfileStore ────────────────────────────────────────────────

void NetworkTests::storeEmptyOnFirstRun() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  const QString path = tmp.fileName();
  tmp.close();
  QFile::remove(path); // ensure no data

  QSettingsProfileStore store(path);
  const auto profiles = store.loadProfiles();
  QVERIFY2(profiles.has_value(), qPrintable(profiles.error()));
  QVERIFY(profiles->isEmpty());

  const auto id = store.loadSelectedProfileId();
  QVERIFY2(id.has_value(), qPrintable(id.error()));
  QVERIFY(id->isEmpty()); // no selection
}

void NetworkTests::storeRoundTripHosted() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  QSettingsProfileStore store(tmp.fileName());
  QVERIFY(store.saveProfiles({ServerProfile::hostedDefault()}).has_value());

  const auto after = store.loadProfiles();
  QVERIFY2(after.has_value(), qPrintable(after.error()));
  QCOMPARE(after->size(), 1);
  QCOMPARE(after->at(0).kind(), ServerProfileKind::HostedDefault);
  QCOMPARE(after->at(0).baseUrl(),
           QUrl(QStringLiteral("https://arkhamhorror.app")));
  QCOMPARE(after->at(0).profileId(),
           ServerProfile::hostedDefault().profileId());
}

void NetworkTests::storeRoundTripCustom() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  const auto p = ServerProfile::custom(QStringLiteral("My Server"),
                                       QStringLiteral("http://localhost:3000"));
  QVERIFY(p.has_value());

  QSettingsProfileStore store(tmp.fileName());
  QVERIFY(store.saveProfiles({*p}).has_value());

  const auto after = store.loadProfiles();
  QVERIFY2(after.has_value(), qPrintable(after.error()));
  QCOMPARE(after->size(), 1);
  QCOMPARE(after->at(0).kind(), ServerProfileKind::Custom);
  QCOMPARE(after->at(0).displayName(), QStringLiteral("My Server"));
  QCOMPARE(after->at(0).baseUrl().port(), 3000);
}

void NetworkTests::storeRoundTripCustomPreservesId() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  const auto p = ServerProfile::custom(
      QStringLiteral("Persist"), QStringLiteral("https://srv.example.com"));
  QVERIFY(p.has_value());
  const QString originalId = p->profileId();

  QSettingsProfileStore store(tmp.fileName());
  QVERIFY(store.saveProfiles({*p}).has_value());

  const auto after = store.loadProfiles();
  QVERIFY2(after.has_value(), qPrintable(after.error()));
  QCOMPARE(after->size(), 1);
  QCOMPARE(after->at(0).profileId(), originalId);
}

void NetworkTests::storeRoundTripMixed() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  const auto custom = ServerProfile::custom(
      QStringLiteral("Local"), QStringLiteral("http://localhost:4000"));
  QVERIFY(custom.has_value());

  QSettingsProfileStore store(tmp.fileName());
  const QList<ServerProfile> before = {ServerProfile::hostedDefault(), *custom};
  QVERIFY(store.saveProfiles(before).has_value());
  QVERIFY(store.saveSelectedProfileId(custom->profileId()).has_value());

  const auto after = store.loadProfiles();
  QVERIFY2(after.has_value(), qPrintable(after.error()));
  QCOMPARE(after->size(), 2);
  QCOMPARE(after->at(0).kind(), ServerProfileKind::HostedDefault);
  QCOMPARE(after->at(1).kind(), ServerProfileKind::Custom);

  const auto selId = store.loadSelectedProfileId();
  QVERIFY2(selId.has_value(), qPrintable(selId.error()));
  QCOMPARE(*selId, custom->profileId());
}

void NetworkTests::storeSelectionByIdRoundTrip() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  const auto p = ServerProfile::custom(QStringLiteral("S"),
                                       QStringLiteral("https://s.example.com"));
  QVERIFY(p.has_value());

  QSettingsProfileStore store(tmp.fileName());
  QVERIFY(store.saveSelectedProfileId(p->profileId()).has_value());

  const auto loaded = store.loadSelectedProfileId();
  QVERIFY2(loaded.has_value(), qPrintable(loaded.error()));
  QCOMPARE(*loaded, p->profileId());
}

void NetworkTests::storeSelectionNoSelectionOnFirstRun() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();
  QFile::remove(tmp.fileName());

  QSettingsProfileStore store(tmp.fileName());
  const auto id = store.loadSelectedProfileId();
  QVERIFY2(id.has_value(), qPrintable(id.error()));
  QVERIFY(id->isEmpty()); // no selection
}

void NetworkTests::storeSelectionSurvivesReorder() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  const auto pA = ServerProfile::custom(
      QStringLiteral("A"), QStringLiteral("https://a.example.com"));
  const auto pB = ServerProfile::custom(
      QStringLiteral("B"), QStringLiteral("https://b.example.com"));
  const auto pC = ServerProfile::custom(
      QStringLiteral("C"), QStringLiteral("https://c.example.com"));
  QVERIFY(pA.has_value() && pB.has_value() && pC.has_value());

  QSettingsProfileStore store(tmp.fileName());

  // Save [A, B, C], select B
  QVERIFY(store.saveProfiles({*pA, *pB, *pC}).has_value());
  QVERIFY(store.saveSelectedProfileId(pB->profileId()).has_value());

  // Reload and verify B is still selected
  const auto selId1 = store.loadSelectedProfileId();
  QVERIFY2(selId1.has_value(), qPrintable(selId1.error()));
  QCOMPARE(*selId1, pB->profileId());

  // Reorder to [C, A, B] and reload — ID-based selection is unaffected
  QVERIFY(store.saveProfiles({*pC, *pA, *pB}).has_value());

  const auto selId2 = store.loadSelectedProfileId();
  QVERIFY2(selId2.has_value(), qPrintable(selId2.error()));
  QCOMPARE(*selId2, pB->profileId());
}

void NetworkTests::storeCorruptKind() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  {
    QSettings s(tmp.fileName(), QSettings::IniFormat);
    s.beginWriteArray(QStringLiteral("Profiles"), 1);
    s.setArrayIndex(0);
    s.setValue(QStringLiteral("id"),
               QStringLiteral("12345678-1234-1234-1234-123456789abc"));
    s.setValue(QStringLiteral("kind"), QStringLiteral("unknown-kind"));
    s.setValue(QStringLiteral("displayName"), QStringLiteral("x"));
    s.setValue(QStringLiteral("baseUrl"),
               QStringLiteral("https://x.example.com"));
    s.endArray();
    s.sync();
  }

  QSettingsProfileStore store(tmp.fileName());
  const auto profiles = store.loadProfiles();
  QVERIFY(!profiles.has_value());
  QVERIFY(profiles.error().contains(QStringLiteral("unknown-kind")));
}

void NetworkTests::storeCorruptCustomUrl() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  {
    QSettings s(tmp.fileName(), QSettings::IniFormat);
    s.beginWriteArray(QStringLiteral("Profiles"), 1);
    s.setArrayIndex(0);
    s.setValue(QStringLiteral("id"),
               QStringLiteral("12345678-1234-1234-1234-123456789abc"));
    s.setValue(QStringLiteral("kind"), QStringLiteral("custom"));
    s.setValue(QStringLiteral("displayName"), QStringLiteral("Bad"));
    s.setValue(QStringLiteral("baseUrl"),
               QStringLiteral("https://x.example.com/api/v1"));
    s.endArray();
    s.sync();
  }

  QSettingsProfileStore store(tmp.fileName());
  const auto profiles = store.loadProfiles();
  QVERIFY(!profiles.has_value());
  QVERIFY(!profiles.error().isEmpty());
}

void NetworkTests::storeCorruptProfileId() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  {
    QSettings s(tmp.fileName(), QSettings::IniFormat);
    s.beginWriteArray(QStringLiteral("Profiles"), 1);
    s.setArrayIndex(0);
    s.setValue(QStringLiteral("id"), QStringLiteral("not-a-uuid"));
    s.setValue(QStringLiteral("kind"), QStringLiteral("custom"));
    s.setValue(QStringLiteral("displayName"), QStringLiteral("P"));
    s.setValue(QStringLiteral("baseUrl"),
               QStringLiteral("https://ok.example.com"));
    s.endArray();
    s.sync();
  }

  QSettingsProfileStore store(tmp.fileName());
  const auto profiles = store.loadProfiles();
  QVERIFY(!profiles.has_value());
  QVERIFY(!profiles.error().isEmpty());
}

void NetworkTests::storeCorruptSelectedProfileId() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  {
    QSettings s(tmp.fileName(), QSettings::IniFormat);
    s.setValue(QStringLiteral("Selection/profileId"),
               QStringLiteral("not-a-valid-uuid"));
    s.sync();
  }

  QSettingsProfileStore store(tmp.fileName());
  const auto id = store.loadSelectedProfileId();
  QVERIFY(!id.has_value());
  QVERIFY(id.error().contains(QStringLiteral("not-a-valid-uuid")));
}

void NetworkTests::storeEmptySelectedProfileIdIsNoSelection() {
  // An explicitly written empty string is treated as "no selection", not as
  // corrupt data (matches saveSelectedProfileId("") which removes the key).
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();
  QFile::remove(tmp.fileName());

  QSettingsProfileStore store(tmp.fileName());
  QVERIFY(store.saveSelectedProfileId(QString{}).has_value());

  const auto id = store.loadSelectedProfileId();
  QVERIFY2(id.has_value(), qPrintable(id.error()));
  QVERIFY(id->isEmpty());
}

void NetworkTests::storeSaveRejectsProfileWithoutId() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  // A default-constructed profile has no ID; this is the only public
  // constructor ServerProfile exposes besides the validated factories
  // (hostedDefault()/custom()/customWithId()), all of which always set a
  // stable ID.
  const ServerProfile blankProfile;
  QVERIFY(blankProfile.profileId().isEmpty());

  QSettingsProfileStore store(tmp.fileName());
  const auto result = store.saveProfiles({blankProfile});
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("ID")));
}

void NetworkTests::storeSaveRejectsDuplicateIds() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  // Two hostedDefault() instances share the same deterministic ID.
  QSettingsProfileStore store(tmp.fileName());
  const auto result = store.saveProfiles(
      {ServerProfile::hostedDefault(), ServerProfile::hostedDefault()});
  QVERIFY(!result.has_value());
  QVERIFY(result.error().contains(QStringLiteral("duplicate")));
}

void NetworkTests::storeSaveRemovesDeletedProfileKeys() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  const auto first = ServerProfile::custom(
      QStringLiteral("First"), QStringLiteral("https://first.example.com"));
  const auto second = ServerProfile::custom(
      QStringLiteral("Second"), QStringLiteral("https://second.example.com"));
  const auto third = ServerProfile::custom(
      QStringLiteral("Third"), QStringLiteral("https://third.example.com"));
  QVERIFY(first.has_value() && second.has_value() && third.has_value());

  QSettingsProfileStore store(tmp.fileName());
  QVERIFY(store.saveProfiles({*first, *second, *third}).has_value());
  QVERIFY(store.saveProfiles({*first}).has_value());

  {
    QSettings raw(tmp.fileName(), QSettings::IniFormat);
    const QStringList keys = raw.allKeys();
    QVERIFY(!keys.contains(QStringLiteral("Profiles/2/id")));
    QVERIFY(!keys.contains(QStringLiteral("Profiles/3/id")));
  }

  QVERIFY(store.saveProfiles({}).has_value());
  QSettings raw(tmp.fileName(), QSettings::IniFormat);
  for (const QString &key : raw.allKeys()) {
    QVERIFY2(key == QStringLiteral("Profiles/size"),
             qPrintable(QStringLiteral("stale profile key: %1").arg(key)));
  }
}

void NetworkTests::storeSaveChecksStatusAfterSync() {
  // Use an existing regular file as the "parent directory" of the QSettings
  // path.  On POSIX, a regular file cannot contain child entries (ENOTDIR),
  // so QSettings::sync() will report AccessError regardless of whether
  // Qt's QSaveFile tries to call mkpath first.  This avoids both permission
  // hacks and the mkpath-creates-missing-dirs behaviour on some platforms.
  QTemporaryFile barrier;
  QVERIFY(barrier.open());
  const QString barrierPath = barrier.fileName();
  barrier.close(); // keep file on disk; destructor will remove it

  const QString badPath = barrierPath + QStringLiteral("/profiles.ini");
  QSettingsProfileStore store(badPath);
  const auto result = store.saveProfiles({ServerProfile::hostedDefault()});
  QVERIFY(!result.has_value());
}

void NetworkTests::storeLoadBalancesBeginEndArray() {
  // Ensure that a corrupt profile early in a multi-profile list does not
  // leave beginReadArray/endArray unbalanced (which would corrupt subsequent
  // reads from the same QSettings instance).
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  const auto good = ServerProfile::custom(
      QStringLiteral("Good"), QStringLiteral("https://ok.example.com"));
  QVERIFY(good.has_value());

  {
    QSettings s(tmp.fileName(), QSettings::IniFormat);
    // Profile 0: corrupt kind
    s.beginWriteArray(QStringLiteral("Profiles"), 2);
    s.setArrayIndex(0);
    s.setValue(QStringLiteral("id"),
               QStringLiteral("12345678-1234-1234-1234-000000000001"));
    s.setValue(QStringLiteral("kind"), QStringLiteral("bad-kind"));
    s.setValue(QStringLiteral("displayName"), QStringLiteral("x"));
    s.setValue(QStringLiteral("baseUrl"),
               QStringLiteral("https://x.example.com"));
    // Profile 1: valid
    s.setArrayIndex(1);
    s.setValue(QStringLiteral("id"), good->profileId());
    s.setValue(QStringLiteral("kind"), QStringLiteral("custom"));
    s.setValue(QStringLiteral("displayName"), good->displayName());
    s.setValue(QStringLiteral("baseUrl"), good->baseUrl().toString());
    s.endArray();
    s.sync();
  }

  QSettingsProfileStore store(tmp.fileName());

  // Load must fail on the corrupt entry — and endArray must still be called.
  const auto profiles = store.loadProfiles();
  QVERIFY(!profiles.has_value());

  // A second load on the same store must also work (not crash or hang),
  // proving endArray was balanced on the first call.
  const auto profiles2 = store.loadProfiles();
  QVERIFY(!profiles2.has_value()); // still corrupt
}

void NetworkTests::storeLoadRejectsDuplicateIds() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  const QString hostedId = ServerProfile::hostedDefault().profileId();
  {
    // Write two "hosted" entries — both will carry the canonical hosted ID,
    // which is a duplicate.
    QSettings s(tmp.fileName(), QSettings::IniFormat);
    s.beginWriteArray(QStringLiteral("Profiles"), 2);
    s.setArrayIndex(0);
    s.setValue(QStringLiteral("id"), hostedId);
    s.setValue(QStringLiteral("kind"), QStringLiteral("hosted"));
    s.setValue(QStringLiteral("displayName"), QStringLiteral("Arkham Horror"));
    s.setValue(QStringLiteral("baseUrl"),
               QStringLiteral("https://arkhamhorror.app"));
    s.setArrayIndex(1);
    s.setValue(QStringLiteral("id"), hostedId);
    s.setValue(QStringLiteral("kind"), QStringLiteral("hosted"));
    s.setValue(QStringLiteral("displayName"), QStringLiteral("Arkham Horror"));
    s.setValue(QStringLiteral("baseUrl"),
               QStringLiteral("https://arkhamhorror.app"));
    s.endArray();
    s.sync();
  }

  QSettingsProfileStore store(tmp.fileName());
  const auto profiles = store.loadProfiles();
  QVERIFY(!profiles.has_value());
  QVERIFY(profiles.error().contains(QStringLiteral("duplicate")));
}

void NetworkTests::storeLoadRejectsWrongHostedId() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  {
    // Write a "hosted" entry with a non-canonical ID.
    QSettings s(tmp.fileName(), QSettings::IniFormat);
    s.beginWriteArray(QStringLiteral("Profiles"), 1);
    s.setArrayIndex(0);
    s.setValue(QStringLiteral("id"),
               QStringLiteral("12345678-1234-1234-1234-123456789abc"));
    s.setValue(QStringLiteral("kind"), QStringLiteral("hosted"));
    s.setValue(QStringLiteral("displayName"), QStringLiteral("Arkham Horror"));
    s.setValue(QStringLiteral("baseUrl"),
               QStringLiteral("https://arkhamhorror.app"));
    s.endArray();
    s.sync();
  }

  QSettingsProfileStore store(tmp.fileName());
  const auto profiles = store.loadProfiles();
  QVERIFY(!profiles.has_value());
  const QString error = profiles.error();
  QVERIFY(error.contains(QStringLiteral("wrong ID")));
  QVERIFY(
      error.contains(QStringLiteral("12345678-1234-1234-1234-123456789abc")));
  QVERIFY(error.contains(ServerProfile::hostedDefault().profileId()));
  QVERIFY(!error.contains(QStringLiteral("%3")));
}

void NetworkTests::storeSaveSelectedInvalidUuid() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  QSettingsProfileStore store(tmp.fileName());
  const auto result = store.saveSelectedProfileId(QStringLiteral("not-a-uuid"));
  QVERIFY(!result.has_value());
  QVERIFY(!result.error().isEmpty());
}

void NetworkTests::storeSelectionCanonicalizes() {
  QTemporaryFile tmp;
  QVERIFY(tmp.open());
  tmp.close();

  // Pass a UUID with braces; expect it to be stored and returned without.
  const QString braced =
      QStringLiteral("{12345678-1234-1234-1234-123456789abc}");
  const QString noBraces =
      QStringLiteral("12345678-1234-1234-1234-123456789abc");

  QSettingsProfileStore store(tmp.fileName());
  QVERIFY(store.saveSelectedProfileId(braced).has_value());

  const auto loaded = store.loadSelectedProfileId();
  QVERIFY2(loaded.has_value(), qPrintable(loaded.error()));
  QCOMPARE(*loaded, noBraces);
}

void NetworkTests::storeSaveFailsWithPreExistingError() {
  // Induce a sticky AccessError by pointing the store at a path whose parent
  // is a regular file (POSIX ENOTDIR).  The first sync() fails and sets a
  // sticky AccessError on the QSettings object.  The second saveProfiles()
  // call must detect the pre-existing sticky error before touching storage.
  QTemporaryFile barrier;
  QVERIFY(barrier.open());
  const QString barrierPath = barrier.fileName();
  barrier.close();

  const QString storePath = barrierPath + QStringLiteral("/profiles.ini");
  QSettingsProfileStore store(storePath);

  // First save: sync() fails on the invalid path → sticky AccessError.
  const auto firstSave = store.saveProfiles({ServerProfile::hostedDefault()});
  QVERIFY(!firstSave.has_value()); // sync error

  // Second save: must detect the pre-existing sticky error and refuse to
  // touch storage.  The "pre-existing" keyword distinguishes this failure
  // from a new sync failure.
  const auto secondSave = store.saveProfiles({ServerProfile::hostedDefault()});
  QVERIFY(!secondSave.has_value());
  QVERIFY(secondSave.error().contains(QStringLiteral("pre-existing")));

  // The settings file must not have been created on either attempt.
  QVERIFY(!QFile::exists(storePath));
}

void NetworkTests::storeLoadSelectedIdFailsOnError() {
  // After a sync failure leaves a sticky AccessError on a store,
  // loadSelectedProfileId must return failure rather than silently
  // returning "no selection".
  QTemporaryFile barrier;
  QVERIFY(barrier.open());
  const QString barrierPath = barrier.fileName();
  barrier.close();

  const QString storePath = barrierPath + QStringLiteral("/sel.ini");
  QSettingsProfileStore store(storePath);

  // Induce sticky AccessError via a save whose sync() cannot succeed.
  const auto p = ServerProfile::custom(QStringLiteral("X"),
                                       QStringLiteral("https://x.example.com"));
  QVERIFY(p.has_value());
  const auto saveResult = store.saveSelectedProfileId(p->profileId());
  QVERIFY(!saveResult.has_value()); // sync failed — AccessError is now sticky

  // loadSelectedProfileId must propagate the pre-existing error, not
  // return an empty "no selection" result.
  const auto id = store.loadSelectedProfileId();
  QVERIFY(!id.has_value());
  QVERIFY(!id.error().isEmpty());
}

// ─── NetworkCapabilityProbe ───────────────────────────────────────────────

void NetworkTests::probeUrlIsCapabilitiesEndpoint() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({
    "schemaRevision": "0.1.11",
    "status": "baseline-incomplete",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.0",
    "capabilities": []
  })"));

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(nam.lastUrl(), QUrl(QStringLiteral(
                              "https://arkhamhorror.app/api/v1/capabilities")));
}

void NetworkTests::probeSendsAcceptHeader() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({
    "schemaRevision": "0.1.11",
    "status": "baseline-incomplete",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.0",
    "capabilities": []
  })"));

  NetworkCapabilityProbe probe(nam);
  runProbe(probe, ServerProfile::hostedDefault());
  QCOMPARE(nam.lastAcceptHeader(), QStringLiteral("application/json"));
}

void NetworkTests::probeCompatibleViaFixture() {
  const auto fixtureResult =
      loadContractFixture(QStringLiteral("/fixtures/capabilities.json"));
  if (!fixtureResult.has_value())
    QFAIL(qPrintable(fixtureResult.error()));

  StubNetworkAccessManager nam;
  nam.enqueue(200, *fixtureResult);

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->outcome, ProbeOutcome::Compatible);
  QVERIFY(r->compatibility.has_value());
  QVERIFY(r->compatibility->isCompatible());
  QCOMPARE(r->httpStatus, 200);
}

void NetworkTests::probe404LegacyFallback() {
  StubNetworkAccessManager nam;
  nam.enqueue(404, {}, QNetworkReply::ContentNotFoundError);

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->outcome, ProbeOutcome::LegacyFallback);
  QCOMPARE(r->httpStatus, 404);
  QVERIFY(r->compatibility.has_value());
  QVERIFY(r->compatibility->outcome == CompatibilityOutcome::LegacyFallback);
  QVERIFY(r->compatibility->isUsable());
  QVERIFY(!r->compatibility->isCompatible());
}

void NetworkTests::probeMalformedJsonNotObject() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral("[1, 2, 3]"));

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->outcome, ProbeOutcome::MalformedJson);
  QCOMPARE(r->httpStatus, 200);
}

void NetworkTests::probeMalformedJsonSyntax() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"schemaRevision":)"));

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->outcome, ProbeOutcome::MalformedJson);
  QVERIFY(r->diagnostic.startsWith(QStringLiteral("invalid JSON at offset ")));
  QVERIFY(r->diagnostic !=
          QStringLiteral("response body is not a JSON object"));
  QCOMPARE(r->httpStatus, 200);
}

void NetworkTests::probeMalformedJsonBadSchema() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({"foo": "bar"})"));

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->outcome, ProbeOutcome::MalformedJson);
  QVERIFY(!r->diagnostic.isEmpty());
}

void NetworkTests::probeNonSuccessStatus() {
  StubNetworkAccessManager nam;
  nam.enqueue(503, {}, QNetworkReply::ServiceUnavailableError);

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->outcome, ProbeOutcome::HttpError);
  QCOMPARE(r->httpStatus, 503);
  QVERIFY(r->diagnostic.contains(QStringLiteral("503")));
}

void NetworkTests::probeIncompatible() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({
    "schemaRevision": "0.1.11",
    "status": "baseline-incomplete",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.12",
    "capabilities": []
  })"));

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->outcome, ProbeOutcome::Incompatible);
  QVERIFY(r->compatibility.has_value());
  QVERIFY(r->compatibility->code == IncompatibilityCode::ClientTooOld);
}

void NetworkTests::probeTransportFailure() {
  StubNetworkAccessManager nam;
  nam.enqueue(0, {}, QNetworkReply::NetworkSessionFailedError);

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->outcome, ProbeOutcome::NetworkError);
  QCOMPARE(r->httpStatus, 0);
  QVERIFY(!r->diagnostic.isEmpty());
}

void NetworkTests::probeRejectsInvalidProfile() {
  // A default-constructed profile (no scheme, no host) is always invalid;
  // this is the only public constructor ServerProfile exposes besides the
  // validated factories.
  const ServerProfile bad;
  QVERIFY(!bad.isValid());

  StubNetworkAccessManager nam; // no request should be issued
  NetworkCapabilityProbe probe(nam);
  std::optional<ProbeResult> result;
  QObject::connect(&probe, &ICapabilityProbe::finished,
                   [&result](ProbeResult value) { result = std::move(value); });

  probe.probe(bad);
  // Even preflight failures complete asynchronously.
  QVERIFY(!result.has_value());
  QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 2000);
  QCOMPARE(result->outcome, ProbeOutcome::InvalidProfile);
  QVERIFY(!result->diagnostic.isEmpty());
}

void NetworkTests::probe2xxWithReplyError() {
  // Simulate a 200 response where QNetworkReply::error() is also set
  // (e.g. connection reset mid-transfer).  The probe must surface NetworkError,
  // not attempt to decode a potentially partial body.
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral("partial"),
              QNetworkReply::RemoteHostClosedError);

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  QCOMPARE(r->outcome, ProbeOutcome::NetworkError);
  QCOMPARE(r->httpStatus, 200);
}

void NetworkTests::probeNoAuthHeader() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({
    "schemaRevision": "0.1.11",
    "status": "baseline-incomplete",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.0",
    "capabilities": []
  })"));

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));
  // The capabilities endpoint is unauthenticated; no Authorization header must
  // be sent even when a shared QNAM has credentials configured.
  QVERIFY(nam.lastRequest().rawHeader("Authorization").isEmpty());
  QCOMPARE(nam.lastRequest()
               .attribute(QNetworkRequest::AuthenticationReuseAttribute)
               .toInt(),
           static_cast<int>(QNetworkRequest::Manual));
}

void NetworkTests::probeNoCookies() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({
    "schemaRevision": "0.1.11",
    "status": "baseline-incomplete",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.0",
    "capabilities": []
  })"));

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));

  // Both cookie attributes must be set to Manual so a shared QNAM cookie jar
  // does not leak session cookies into or out of this public endpoint.
  QCOMPARE(nam.lastRequest()
               .attribute(QNetworkRequest::CookieLoadControlAttribute)
               .toInt(),
           static_cast<int>(QNetworkRequest::Manual));
  QCOMPARE(nam.lastRequest()
               .attribute(QNetworkRequest::CookieSaveControlAttribute)
               .toInt(),
           static_cast<int>(QNetworkRequest::Manual));
}

void NetworkTests::probeDestructionDeletesInFlightReply() {
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral("{}"));

  QPointer<QNetworkReply> reply;
  {
    NetworkCapabilityProbe probe(nam);
    probe.probe(ServerProfile::hostedDefault());
    reply = nam.lastReply();
    QVERIFY(!reply.isNull());
  }

  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QVERIFY(reply.isNull());
}

void NetworkTests::probeTimesOut() {
  // A hanging reply that never finishes: the probe must time out and emit
  // exactly one NetworkError; no second emission must follow.
  StubNetworkAccessManager nam;
  nam.enqueueHanging();

  int emitCount = 0;
  ProbeResult lastResult;
  {
    NetworkCapabilityProbe probe(nam, std::chrono::milliseconds(50));
    QObject::connect(&probe, &ICapabilityProbe::finished,
                     [&emitCount, &lastResult](ProbeResult r) {
                       ++emitCount;
                       lastResult = std::move(r);
                     });
    probe.probe(ServerProfile::hostedDefault());

    QTRY_COMPARE_WITH_TIMEOUT(emitCount, 1, 2000);
    QCOMPARE(lastResult.outcome, ProbeOutcome::NetworkError);
    QVERIFY(lastResult.diagnostic.contains(QStringLiteral("timed out")));

    // Extra settling time: verify no second emission.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
    QCOMPARE(emitCount, 1);
  }
}

void NetworkTests::probeZeroTimeoutDisablesDeadline() {
  StubNetworkAccessManager nam;
  nam.enqueueHanging();

  int emitCount = 0;
  {
    NetworkCapabilityProbe probe(nam, std::chrono::milliseconds::zero());
    QObject::connect(&probe, &ICapabilityProbe::finished,
                     [&emitCount](const ProbeResult &) { ++emitCount; });
    probe.probe(ServerProfile::hostedDefault());

    QTest::qWait(100);
    QCOMPARE(emitCount, 0);
  }

  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  QCOMPARE(emitCount, 0);
}

void NetworkTests::probeRejectsNegativeTimeout() {
  StubNetworkAccessManager nam;

  bool rejected = false;
  try {
    NetworkCapabilityProbe probe(nam, std::chrono::milliseconds{-1});
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  QVERIFY(rejected);
}

void NetworkTests::probeRedirectPolicySet() {
  // The probe must set SameOriginRedirectPolicy at request level so the
  // capabilities endpoint never follows cross-origin redirects.
  StubNetworkAccessManager nam;
  nam.enqueue(200, QByteArrayLiteral(R"({
    "schemaRevision": "0.1.11",
    "status": "baseline-incomplete",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.0",
    "capabilities": []
  })"));

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));

  QCOMPARE(nam.lastRequest()
               .attribute(QNetworkRequest::RedirectPolicyAttribute)
               .toInt(),
           static_cast<int>(QNetworkRequest::SameOriginRedirectPolicy));
}

void NetworkTests::probeRedirectPolicyIgnoresManagerGlobal() {
  // Even when the QNAM has a conflicting global redirect policy, the probe
  // must still set SameOriginRedirectPolicy at the request level.
  StubNetworkAccessManager nam;
  nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
  nam.enqueue(200, QByteArrayLiteral(R"({
    "schemaRevision": "0.1.11",
    "status": "baseline-incomplete",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.0",
    "capabilities": []
  })"));

  NetworkCapabilityProbe probe(nam);
  const auto r = runProbe(probe, ServerProfile::hostedDefault());
  QVERIFY2(r.has_value(), qPrintable(r.error()));

  QCOMPARE(nam.lastRequest()
               .attribute(QNetworkRequest::RedirectPolicyAttribute)
               .toInt(),
           static_cast<int>(QNetworkRequest::SameOriginRedirectPolicy));
}

// ─── Fixture helper ───────────────────────────────────────────────────────

void NetworkTests::fixtureHelperRequiresLeadingSlash() {
  const auto r =
      loadContractFixture(QStringLiteral("fixtures/capabilities.json"));
  QVERIFY(!r.has_value());
  QVERIFY(r.error().contains(QStringLiteral("'/'")));
}

void NetworkTests::fixtureHelperFailsOnMissingFile() {
  const auto r =
      loadContractFixture(QStringLiteral("/fixtures/does-not-exist.json"));
  QVERIFY(!r.has_value());
  QVERIFY(!r.error().isEmpty());
}

QTEST_GUILESS_MAIN(NetworkTests)

#include "NetworkTests.moc"
