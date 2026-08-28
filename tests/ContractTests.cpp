#include <QFile>
#include <QJsonDocument>
#include <QtTest>

#include "CompatibilityEvaluator.h"
#include "ContractPin.h"
#include "ContractRevision.h"
#include "ServerCapabilities.h"

using namespace Arkham;

class ContractTests final : public QObject {
  Q_OBJECT

private slots:
  // ContractRevision ─────────────────────────────────────────────────────────
  void numericVersionOrdering();
  void versionEquality();
  void versionToString();
  void malformedVersionsRejected();
  void negativeVersionComponentsRejected();
  void extraVersionComponentsRejected();
  void strictAsciiOnlyParsing();

  // ServerCapabilities ───────────────────────────────────────────────────────
  void parsesVendoredFixture();
  void contractPinMatchesVendoredJson();
  void unknownStatusAllowed();
  void unknownCapabilityAllowed();
  void missingRequiredFieldsRejected();
  void wrongJsonTypesRejected();
  void duplicateCapabilitiesPreserved();
  void legacyFallbackIsConservative();

  // CompatibilityEvaluator ───────────────────────────────────────────────────
  void exactRevisionCompatible();
  void serverNewerRevisionCompatible();
  void clientTooOldServerRequires0112();
  void clientAcceptedByLenientServer();
  void serverTooOld();
  void serverFloorIndependentFromSupportedRevision();
  void apiBaseMismatch();
  void unknownCapabilityDoesNotPreventCompatibility();
  void legacyFallbackYieldsLegacyOutcome();
};

// ─── Helpers
// ──────────────────────────────────────────────────────────────────

static QJsonObject parseJson(QLatin1StringView text) {
  return QJsonDocument::fromJson(QByteArray(text.data(), text.size())).object();
}

// Returns the raw bytes of a vendored contract file or an error string.
// relPath must start with '/'.  QFAIL must be called at the test-method level,
// not inside a lambda, so callers check .has_value() and QFAIL themselves.
static ValueOrError<QByteArray> openContractFile(const QString &relPath) {
  const QString path = QStringLiteral(ARKHAM_TEST_CONTRACTS_DIR) + relPath;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return failure(
        QStringLiteral("cannot open %1: %2").arg(path, f.errorString()));
  return f.readAll();
}

static ContractPin testPin() {
  return ContractPin{
      .backendCommit =
          QStringLiteral("2bf2935cde121498435744a06fcf63502a80ae43"),
      .sourceRef = QStringLiteral("djensenius/ArkhamHorror#23"),
      .supportedSchemaRevision = {0, 1, 11},
      .minimumServerSchemaRevision = {0, 1, 11},
      .sourceNativeClientMinimumRevision = {0, 1, 0},
      .expectedApiBasePath = QStringLiteral("/api/v1"),
  };
}

static ServerCapabilities goodServer() {
  return ServerCapabilities{
      .schemaRevision = {0, 1, 11},
      .status = QStringLiteral("baseline-incomplete"),
      .apiBasePath = QStringLiteral("/api/v1"),
      .nativeClientMinimumRevision = {0, 1, 0},
      .capabilities = {QStringLiteral("events.shared-state-versioning")},
  };
}

// ─── ContractRevision
// ─────────────────────────────────────────────────────────

void ContractTests::numericVersionOrdering() {
  // Core requirement: numeric ordering, NOT lexical.
  // Lexically "0.1.9" > "0.1.11" (because '9' > '1'), but numerically
  // 0.1.9 < 0.1.11.
  const auto v9 = ContractRevision::parse(u"0.1.9");
  const auto v11 = ContractRevision::parse(u"0.1.11");
  QVERIFY(v9.has_value());
  QVERIFY(v11.has_value());
  QVERIFY(*v9 < *v11);
  QVERIFY(!(*v11 < *v9));

  const auto v100 = ContractRevision::parse(u"1.0.0");
  const auto v099 = ContractRevision::parse(u"0.99.99");
  QVERIFY(v100.has_value());
  QVERIFY(v099.has_value());
  QVERIFY(*v099 < *v100);
}

void ContractTests::versionEquality() {
  const auto a = ContractRevision::parse(u"1.2.3");
  const auto b = ContractRevision::parse(u"1.2.3");
  QVERIFY(a.has_value());
  QVERIFY(b.has_value());
  QVERIFY(*a == *b);
  QVERIFY(!(*a != *b));

  QVERIFY((ContractRevision{0, 1, 0} == ContractRevision{0, 1, 0}));
  QVERIFY((ContractRevision{0, 1, 0} < ContractRevision{0, 1, 1}));
  QVERIFY((ContractRevision{0, 2, 0} > ContractRevision{0, 1, 99}));
}

void ContractTests::versionToString() {
  const auto parsed = ContractRevision::parse(u"0.1.11");
  QVERIFY(parsed.has_value());
  QCOMPARE(parsed->toString(), QStringLiteral("0.1.11"));
  QCOMPARE((ContractRevision{1, 0, 0}.toString()), QStringLiteral("1.0.0"));
  QCOMPARE((ContractRevision{0, 1, 0}.toString()), QStringLiteral("0.1.0"));
}

void ContractTests::malformedVersionsRejected() {
  const QStringList bad = {
      QStringLiteral(""),     QStringLiteral("1.2"),   QStringLiteral("1.2.x"),
      QStringLiteral("abc"),  QStringLiteral("1.x.3"), QStringLiteral("x.2.3"),
      QStringLiteral(".1.2"), QStringLiteral("1.2."),
  };
  for (const QString &s : bad) {
    const auto result = ContractRevision::parse(s);
    QVERIFY2(!result.has_value(),
             qPrintable(QStringLiteral("expected failure for \"%1\"").arg(s)));
  }
}

void ContractTests::negativeVersionComponentsRejected() {
  QVERIFY(!ContractRevision::parse(u"-1.2.3").has_value());
  QVERIFY(!ContractRevision::parse(u"1.-2.3").has_value());
  QVERIFY(!ContractRevision::parse(u"1.2.-3").has_value());
}

void ContractTests::extraVersionComponentsRejected() {
  QVERIFY(!ContractRevision::parse(u"1.2.3.4").has_value());
  QVERIFY(!ContractRevision::parse(u"0.1.0.0").has_value());
}

void ContractTests::strictAsciiOnlyParsing() {
  // The schema pattern is [0-9]+.[0-9]+.[0-9]+.  toInt() accepts leading '+',
  // leading/trailing whitespace, and some Unicode digits — all must be
  // rejected.
  const QStringList bad = {
      QStringLiteral("+1.2.3"),  // leading '+' in major
      QStringLiteral("1.+2.3"),  // leading '+' in minor
      QStringLiteral("1.2.+3"),  // leading '+' in patch
      QStringLiteral(" 1.2.3"),  // leading space in major component
      QStringLiteral("1. 2.3"),  // leading space in minor component
      QStringLiteral("1.2.3 "),  // trailing space in patch component
      QStringLiteral("1.2. 3"),  // space before patch digits
      QStringLiteral("1.2.3\n"), // trailing newline in patch
  };
  for (const QString &s : bad) {
    const auto result = ContractRevision::parse(s);
    QVERIFY2(!result.has_value(),
             qPrintable(QStringLiteral("expected failure for \"%1\"").arg(s)));
  }

  // Leading zeros are accepted (schema says [0-9]+, not [1-9][0-9]*).
  QVERIFY(ContractRevision::parse(u"0.01.0").has_value());
}

// ─── ServerCapabilities
// ───────────────────────────────────────────────────────

void ContractTests::parsesVendoredFixture() {
  // Load from the vendored fixture, not from an inline duplicate string.
  // This fixture is git-show 2bf2935:contracts/fixtures/capabilities.json,
  // pinned to the PR#23 backend commit.
  const auto fileResult =
      openContractFile(QStringLiteral("/fixtures/capabilities.json"));
  if (!fileResult.has_value())
    QFAIL(qPrintable(fileResult.error()));
  const QJsonDocument doc = QJsonDocument::fromJson(*fileResult);
  QVERIFY(doc.isObject());

  const auto result = ServerCapabilities::fromJson(doc.object());
  if (!result.has_value())
    QFAIL(qPrintable(result.error()));

  QCOMPARE(result->schemaRevision, (ContractRevision{0, 1, 11}));
  QCOMPARE(result->status, QStringLiteral("baseline-incomplete"));
  QCOMPARE(result->apiBasePath, QStringLiteral("/api/v1"));
  QCOMPARE(result->nativeClientMinimumRevision, (ContractRevision{0, 1, 0}));
  QCOMPARE(result->capabilities.size(), 4);
  QVERIFY(result->hasCapability(u"events.shared-state-versioning"));
  QVERIFY(result->hasCapability(u"games.step-probe"));
  QVERIFY(result->hasCapability(u"websockets.authorization-header"));
  QVERIFY(result->hasCapability(u"websockets.spectator-read-only"));
  QVERIFY(!result->hasCapability(u"nonexistent.capability"));
  QVERIFY(!result->isLegacyFallback);
}

void ContractTests::contractPinMatchesVendoredJson() {
  // Proves that the C++ currentPin() and contracts/contract-pin.json cannot
  // silently drift: both must encode identical revision metadata.
  const auto fileResult =
      openContractFile(QStringLiteral("/contract-pin.json"));
  if (!fileResult.has_value())
    QFAIL(qPrintable(fileResult.error()));
  const QJsonDocument doc = QJsonDocument::fromJson(*fileResult);
  QVERIFY(doc.isObject());
  const QJsonObject obj = doc.object();

  const ContractPin &pin = currentPin();

  QCOMPARE(obj.value(QStringLiteral("backendCommit")).toString(),
           pin.backendCommit);
  QCOMPARE(obj.value(QStringLiteral("sourceRef")).toString(), pin.sourceRef);
  QCOMPARE(obj.value(QStringLiteral("apiBasePath")).toString(),
           pin.expectedApiBasePath);

  auto jsonSchema = ContractRevision::parse(
      obj.value(QStringLiteral("schemaRevision")).toString());
  if (!jsonSchema.has_value())
    QFAIL(qPrintable(jsonSchema.error()));
  QCOMPARE(*jsonSchema, pin.supportedSchemaRevision);

  auto jsonMinimumServer = ContractRevision::parse(
      obj.value(QStringLiteral("minimumServerSchemaRevision")).toString());
  if (!jsonMinimumServer.has_value())
    QFAIL(qPrintable(jsonMinimumServer.error()));
  QCOMPARE(*jsonMinimumServer, pin.minimumServerSchemaRevision);

  auto jsonNative = ContractRevision::parse(
      obj.value(QStringLiteral("nativeClientMinimumRevision")).toString());
  if (!jsonNative.has_value())
    QFAIL(qPrintable(jsonNative.error()));
  QCOMPARE(*jsonNative, pin.sourceNativeClientMinimumRevision);
}

void ContractTests::unknownStatusAllowed() {
  // Forward-compatible: any status string is accepted without error.
  // The published value "baseline-incomplete" is also an open string.
  const QLatin1StringView json{R"({
    "schemaRevision": "0.1.11",
    "status": "some-future-status-value",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.0",
    "capabilities": []
  })"};
  const auto result = ServerCapabilities::fromJson(parseJson(json));
  if (!result.has_value())
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->status, QStringLiteral("some-future-status-value"));
}

void ContractTests::unknownCapabilityAllowed() {
  // Forward-compatible: unknown capability identifiers are preserved.
  const QLatin1StringView json{R"({
    "schemaRevision": "0.1.11",
    "status": "baseline-incomplete",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.0",
    "capabilities": ["future.unknown-cap", "events.shared-state-versioning"]
  })"};
  const auto result = ServerCapabilities::fromJson(parseJson(json));
  if (!result.has_value())
    QFAIL(qPrintable(result.error()));
  QVERIFY(result->hasCapability(u"future.unknown-cap"));
  QVERIFY(result->hasCapability(u"events.shared-state-versioning"));
}

void ContractTests::missingRequiredFieldsRejected() {
  const QStringList missing = {
      // schemaRevision absent
      QStringLiteral(
          R"({"status":"baseline-incomplete","apiBasePath":"/api/v1",)"
          R"("nativeClientMinimumRevision":"0.1.0","capabilities":[]})"),
      // status absent
      QStringLiteral(
          R"({"schemaRevision":"0.1.0","apiBasePath":"/api/v1",)"
          R"("nativeClientMinimumRevision":"0.1.0","capabilities":[]})"),
      // apiBasePath absent
      QStringLiteral(
          R"({"schemaRevision":"0.1.0","status":"baseline-incomplete",)"
          R"("nativeClientMinimumRevision":"0.1.0","capabilities":[]})"),
      // nativeClientMinimumRevision absent
      QStringLiteral(
          R"({"schemaRevision":"0.1.0","status":"baseline-incomplete",)"
          R"("apiBasePath":"/api/v1","capabilities":[]})"),
      // capabilities absent
      QStringLiteral(
          R"({"schemaRevision":"0.1.0","status":"baseline-incomplete",)"
          R"("apiBasePath":"/api/v1","nativeClientMinimumRevision":"0.1.0"})"),
  };
  for (const QString &json : missing) {
    const auto result = ServerCapabilities::fromJson(
        QJsonDocument::fromJson(json.toUtf8()).object());
    QVERIFY2(!result.has_value(), qPrintable(json));
  }
}

void ContractTests::wrongJsonTypesRejected() {
  // A number where a string is expected must yield an actionable error.
  const QLatin1StringView numberAsRevision{R"({
    "schemaRevision": 1,
    "status": "baseline-incomplete",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.0",
    "capabilities": []
  })"};
  const auto r1 = ServerCapabilities::fromJson(parseJson(numberAsRevision));
  QVERIFY(!r1.has_value());
  QVERIFY(r1.error().contains(QStringLiteral("schemaRevision")));

  const QLatin1StringView stringAsCaps{R"({
    "schemaRevision": "0.1.0",
    "status": "baseline-incomplete",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.0",
    "capabilities": "not-an-array"
  })"};
  const auto r2 = ServerCapabilities::fromJson(parseJson(stringAsCaps));
  QVERIFY(!r2.has_value());
  QVERIFY(r2.error().contains(QStringLiteral("capabilities")));

  const QLatin1StringView numberInCapsArray{R"({
    "schemaRevision": "0.1.0",
    "status": "baseline-incomplete",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.0",
    "capabilities": [42]
  })"};
  const auto r3 = ServerCapabilities::fromJson(parseJson(numberInCapsArray));
  QVERIFY(!r3.has_value());
  QVERIFY(r3.error().contains(QStringLiteral("capabilities[0]")));
}

void ContractTests::duplicateCapabilitiesPreserved() {
  // Duplicates are not a parse error (forward-compatible); hasCapability works.
  const QLatin1StringView json{R"({
    "schemaRevision": "0.1.11",
    "status": "baseline-incomplete",
    "apiBasePath": "/api/v1",
    "nativeClientMinimumRevision": "0.1.0",
    "capabilities": ["games.step-probe", "games.step-probe"]
  })"};
  const auto result = ServerCapabilities::fromJson(parseJson(json));
  if (!result.has_value())
    QFAIL(qPrintable(result.error()));
  QCOMPARE(result->capabilities.size(), 2);
  QVERIFY(result->hasCapability(u"games.step-probe"));
}

void ContractTests::legacyFallbackIsConservative() {
  const ServerCapabilities fallback = ServerCapabilities::legacyFallback();
  QVERIFY(fallback.isLegacyFallback);
  QVERIFY(fallback.capabilities.isEmpty());
  QVERIFY(!fallback.hasCapability(u"events.shared-state-versioning"));
  QVERIFY(!fallback.hasCapability(u"games.step-probe"));
  QCOMPARE(fallback.apiBasePath, QStringLiteral("/api/v1"));
  QVERIFY((fallback.schemaRevision <= ContractRevision{0, 1, 0}));
}

// ─── CompatibilityEvaluator
// ───────────────────────────────────────────────────

void ContractTests::exactRevisionCompatible() {
  // Client support 0.1.11 >= server minimum 0.1.0 → Compatible.
  const auto result = CompatibilityEvaluator::evaluate(goodServer(), testPin());
  QVERIFY2(result.isCompatible(), qPrintable(result.diagnostic));
  QVERIFY(result.code == IncompatibilityCode::None);
}

void ContractTests::serverNewerRevisionCompatible() {
  // Server schema 0.1.12 >= client minimum 0.1.11 → still Compatible.
  ServerCapabilities server = goodServer();
  server.schemaRevision = {0, 1, 12};
  const auto result = CompatibilityEvaluator::evaluate(server, testPin());
  QVERIFY2(result.isCompatible(), qPrintable(result.diagnostic));
}

void ContractTests::clientTooOldServerRequires0112() {
  // Server minimum 0.1.12, client schema (pin) 0.1.11 → ClientTooOld.
  // This is the canonical numeric-ordering rejection:
  //   pin.supportedSchemaRevision {0,1,11} <
  //   server.nativeClientMinimumRevision
  //   {0,1,12}.
  ServerCapabilities server = goodServer();
  server.nativeClientMinimumRevision = {0, 1, 12};
  const auto result = CompatibilityEvaluator::evaluate(server, testPin());
  QVERIFY(!result.isCompatible());
  QVERIFY(result.outcome == CompatibilityOutcome::Incompatible);
  QVERIFY(result.code == IncompatibilityCode::ClientTooOld);
  QVERIFY(result.diagnostic.contains(QStringLiteral("client")));
}

void ContractTests::clientAcceptedByLenientServer() {
  // Server minimum 0.1.0, client schema (pin) 0.1.11 → Compatible.
  //   pin.supportedSchemaRevision {0,1,11} >=
  //   server.nativeClientMinimumRevision
  //   {0,1,0}.
  ServerCapabilities server = goodServer();
  server.nativeClientMinimumRevision = {0, 1, 0};
  const auto result = CompatibilityEvaluator::evaluate(server, testPin());
  QVERIFY2(result.isCompatible(), qPrintable(result.diagnostic));
}

void ContractTests::serverTooOld() {
  // Server schema 0.1.9 < minimum modern server 0.1.11 → ServerTooOld.
  // Numeric ordering: 9 < 11, so 0.1.9 < 0.1.11 (not a lexical comparison).
  ServerCapabilities server = goodServer();
  server.schemaRevision = {0, 1, 9};
  const auto result = CompatibilityEvaluator::evaluate(server, testPin());
  QVERIFY(!result.isCompatible());
  QVERIFY(result.outcome == CompatibilityOutcome::Incompatible);
  QVERIFY(result.code == IncompatibilityCode::ServerTooOld);
  QVERIFY(result.diagnostic.contains(QStringLiteral("server")));
}

void ContractTests::serverFloorIndependentFromSupportedRevision() {
  ContractPin pin = testPin();
  pin.minimumServerSchemaRevision = {0, 1, 9};

  ServerCapabilities server = goodServer();
  server.schemaRevision = {0, 1, 10};

  const auto result = CompatibilityEvaluator::evaluate(server, pin);
  QVERIFY2(result.isCompatible(), qPrintable(result.diagnostic));
}

void ContractTests::apiBaseMismatch() {
  ServerCapabilities server = goodServer();
  server.apiBasePath = QStringLiteral("/api/v2");
  const auto result = CompatibilityEvaluator::evaluate(server, testPin());
  QVERIFY(!result.isCompatible());
  QVERIFY(result.outcome == CompatibilityOutcome::Incompatible);
  QVERIFY(result.code == IncompatibilityCode::ApiBaseMismatch);
  QVERIFY(result.diagnostic.contains(QStringLiteral("apiBasePath")));
}

void ContractTests::unknownCapabilityDoesNotPreventCompatibility() {
  // Unknown capability from a newer server is forward-compatible.
  ServerCapabilities server = goodServer();
  server.capabilities.append(QStringLiteral("future.experimental-feature"));
  const auto result = CompatibilityEvaluator::evaluate(server, testPin());
  QVERIFY2(result.isCompatible(), qPrintable(result.diagnostic));
}

void ContractTests::legacyFallbackYieldsLegacyOutcome() {
  // A 404 on the capabilities endpoint → legacyFallback() → LegacyFallback
  // outcome, not Incompatible.  The caller can then decide whether to offer
  // degraded/conservative mode rather than hard-rejecting the session.
  const ServerCapabilities fallback = ServerCapabilities::legacyFallback();
  const auto result = CompatibilityEvaluator::evaluate(fallback, testPin());

  QVERIFY(result.outcome == CompatibilityOutcome::LegacyFallback);
  QVERIFY(!result.isCompatible());
  QVERIFY(result.isUsable()); // usable in degraded mode
  QVERIFY(result.code == IncompatibilityCode::None);
  QVERIFY(!result.diagnostic.isEmpty());
}

QTEST_APPLESS_MAIN(ContractTests)

#include "ContractTests.moc"
