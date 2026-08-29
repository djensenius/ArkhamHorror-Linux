#include "AssetLocatorTests.h"

#include "AssetLocator.h"
#include "StrictLoopbackUrlTable.h"
#include "UrlValidator.h"

#include <QTest>
#include <QUrl>
#include <optional>

using namespace Arkham;

namespace {

QString kDefaultBase() {
  return QStringLiteral("https://assets.arkhamhorror.app");
}

// Builds an AssetKey via ValidatedAssetSource::fromRaw(rawBase) -- the
// ONLY way to obtain a genuinely valid assetBase -- so every test in this
// file exercises the exact same structural guarantee real callers get.
// `format` defaults to the real, caller-non-configurable canonical format
// for `category` (AssetLocator::canonicalFormatFor()) so most tests never
// need to think about format at all; pass an explicit override only when
// deliberately testing AssetErrorCode::FormatMismatchForCategory.
AssetKey makeKey(const QString &rawBase, AssetCategory category,
                 const QString &identifier, AssetSide side = AssetSide::Front,
                 const QString &locale = QString(),
                 std::optional<AssetFormat> format = std::nullopt,
                 const QString &homebrewNamespace = QString(),
                 const QString &mutationId = QString()) {
  const AssetOutcome<ValidatedAssetSource> base =
      ValidatedAssetSource::fromRaw(rawBase);
  // Copilot review: Q_ASSERT compiles out in release builds, which would
  // silently turn a fixture base-URL failure here into a confusing
  // downstream test failure instead of a clear, immediate diagnosis.
  // qFatal() is enforced in every build configuration (matches the
  // existing convention in tests/AssetImageRequestTests.cpp and
  // tests/AssetRequestCoordinatorTests.cpp).
  if (!base) {
    qFatal("makeKey() fixture base URL failed validation: %s",
           qPrintable(base.error().message));
  }
  AssetKey key;
  key.assetBase = *base;
  key.category = category;
  key.identifier = identifier;
  key.side = side;
  key.locale = locale;
  key.homebrewNamespace = homebrewNamespace;
  key.mutationId = mutationId;
  key.format = format.value_or(AssetLocator::canonicalFormatFor(category));
  return key;
}

// Builds an AssetSide::Back AssetKey. Unlike makeKey() above, `format`
// has no single per-category default (see CardBackKind's doc comment in
// AssetTypes.h -- generic/custom backs are NOT always the category's
// canonicalFormatFor()), so it is always explicit here.
AssetKey makeBackKey(const QString &rawBase, AssetCategory category,
                     CardBackKind backKind, AssetFormat format,
                     const QString &identifier = QString(),
                     const QString &otherSideIdentifier = QString(),
                     const QString &customBackFilename = QString(),
                     const QString &homebrewNamespace = QString(),
                     const QString &locale = QString()) {
  const AssetOutcome<ValidatedAssetSource> base =
      ValidatedAssetSource::fromRaw(rawBase);
  if (!base) {
    qFatal("makeBackKey() fixture base URL failed validation: %s",
           qPrintable(base.error().message));
  }
  AssetKey key;
  key.assetBase = *base;
  key.category = category;
  key.side = AssetSide::Back;
  key.backKind = backKind;
  key.identifier = identifier;
  key.otherSideIdentifier = otherSideIdentifier;
  key.customBackFilename = customBackFilename;
  key.homebrewNamespace = homebrewNamespace;
  key.locale = locale;
  key.format = format;
  return key;
}

} // namespace

void AssetLocatorTests::baseUrlPolicyMatchesSharedTable_data() {
  QTest::addColumn<QString>("urlString");
  QTest::addColumn<bool>("expectAccepted");
  QTest::addColumn<QString>("expectedAuthenticateUrl");

  for (const auto &row : Arkham::Test::strictLoopbackUrlRows()) {
    QTest::newRow(row.name)
        << row.urlString << row.expectAccepted << row.expectedAuthenticateUrl;
  }
}

void AssetLocatorTests::baseUrlPolicyMatchesSharedTable() {
  QFETCH(QString, urlString);
  QFETCH(bool, expectAccepted);
  QFETCH(QString, expectedAuthenticateUrl);

  // Reproducing validateCustomUrl() against the SAME shared table
  // tests/NetworkTests.cpp and tests/AuthClientTests.cpp already drive is
  // what "reuse the shared policy exactly, never fork a weaker
  // interpretation" means at this boundary. ValidatedAssetSource::fromRaw()
  // is the production entry point that runs this exact check, so this
  // test drives it via that entry point rather than calling
  // validateCustomUrl() directly, proving the full AssetKey construction
  // path (not just the underlying validator) reuses the policy.
  const AssetOutcome<ValidatedAssetSource> base =
      ValidatedAssetSource::fromRaw(urlString);
  QCOMPARE(bool(base), expectAccepted);
  if (!expectAccepted) {
    return;
  }

  AssetKey key;
  key.assetBase = *base;
  key.category = AssetCategory::Card;
  key.identifier = QStringLiteral("valid01");
  key.format = AssetFormat::Avif;
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);

  QVERIFY2(bool(result),
           qPrintable(result ? QString() : result.error().message));
  QVERIFY(!result->isEmpty());

  // Every accepted row's expectedAuthenticateUrl is
  // "<scheme>://<host>[:port][/base]/api/v1/authenticate"; strip the
  // shared "/api/v1/authenticate" suffix to recover exactly the
  // normalised scheme+host+port+base-path prefix AssetLocator must reuse
  // unchanged when building its own candidate path.
  QString basePrefix = expectedAuthenticateUrl;
  QVERIFY(basePrefix.endsWith(QStringLiteral("/api/v1/authenticate")));
  basePrefix.chop(QStringLiteral("/api/v1/authenticate").size());

  const QString expectedCandidateUrl =
      basePrefix + QStringLiteral("/img/arkham/cards/valid01.avif");
  QCOMPARE(result->first().url.toString(QUrl::FullyEncoded),
           expectedCandidateUrl);
}

void AssetLocatorTests::defaultConstructedAssetBaseRejected() {
  // A default-constructed AssetKey's assetBase is a default-constructed
  // ValidatedAssetSource: isValid() == false, structurally (there is no
  // QUrl round-trip left to defensively re-validate; see AssetTypes.h and
  // AssetLocator.cpp's header comments). This is now the ONLY way to
  // reach AssetErrorCode::InvalidAssetBase -- there is no longer a
  // "hostile QUrl bypassing fromRaw()" scenario to test, because
  // AssetKey::assetBase can no longer be assigned any QUrl at all.
  AssetKey key;
  key.category = AssetCategory::Card;
  key.identifier = QStringLiteral("valid01");
  key.format = AssetFormat::Avif;
  // key.assetBase left default-constructed.

  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY(!result);
  QCOMPARE(result.error().code, AssetErrorCode::InvalidAssetBase);
}

// Review item 3: asset-specific raw-string path hardening layered ON TOP
// of the shared UrlValidator policy (never a fork/weakening of it -- see
// ValidatedAssetSource::fromRaw() in AssetTypes.cpp). Every row here
// exercises ValidatedAssetSource::fromRaw() directly (not through a full
// AssetKey/resolveCandidates() round trip) since that is the exact
// function under test.
void AssetLocatorTests::
    assetBasePathRejectsDotSegmentsAndEncodedVariants_data() {
  QTest::addColumn<QString>("rawBase");
  QTest::addColumn<bool>("expectValid");

  QTest::newRow("valid-no-path")
      << QStringLiteral("https://assets.example.com") << true;
  QTest::newRow("valid-reverse-proxy-prefix")
      << QStringLiteral("https://example.com/cdn/assets") << true;
  QTest::newRow("literal-dot-segment")
      << QStringLiteral("https://example.com/a/../b") << false;
  QTest::newRow("literal-dot-only-segment")
      << QStringLiteral("https://example.com/a/./b") << false;
  QTest::newRow("trailing-dotdot-segment")
      << QStringLiteral("https://example.com/a/..") << false;
  QTest::newRow("percent-encoded-dotdot-lowercase")
      << QStringLiteral("https://example.com/a/%2e%2e/b") << false;
  QTest::newRow("percent-encoded-dotdot-uppercase")
      << QStringLiteral("https://example.com/a/%2E%2E/b") << false;
  QTest::newRow("percent-encoded-dotdot-mixed-case")
      << QStringLiteral("https://example.com/a/%2e%2E/b") << false;
  QTest::newRow("double-encoded-dotdot")
      << QStringLiteral("https://example.com/a/%252e%252e/b") << false;
  QTest::newRow("percent-encoded-separator-smuggles-dotdot-segment")
      << QStringLiteral("https://example.com/a%2f..%2fb") << false;
  QTest::newRow("literal-backslash")
      << QStringLiteral("https://example.com/a\\b") << false;
  QTest::newRow("percent-encoded-backslash")
      << QStringLiteral("https://example.com/a%5cb") << false;
  QTest::newRow("percent-encoded-control-character")
      << QStringLiteral("https://example.com/a%00b") << false;
  QTest::newRow("reverse-proxy-prefix-then-escape")
      << QStringLiteral("https://example.com/cdn/../etc") << false;
}

void AssetLocatorTests::assetBasePathRejectsDotSegmentsAndEncodedVariants() {
  QFETCH(QString, rawBase);
  QFETCH(bool, expectValid);

  const AssetOutcome<ValidatedAssetSource> result =
      ValidatedAssetSource::fromRaw(rawBase);
  if (expectValid) {
    QVERIFY2(bool(result),
             qPrintable(result ? QString() : result.error().message));
  } else {
    QVERIFY(!result);
    QCOMPARE(result.error().code, AssetErrorCode::InvalidAssetBase);
  }
}

// Review item 3: "https://host:443" and "https://host" (and the http:80
// equivalent) must resolve to the EXACT SAME normalizedUrl() -- included
// verbatim in every cache-namespace derivation -- so two spellings of the
// same server never split into two disjoint cache namespaces. A
// non-default port must never be silently dropped.
void AssetLocatorTests::assetBaseCanonicalizesAwayExplicitDefaultPort_data() {
  QTest::addColumn<QString>("rawBaseA");
  QTest::addColumn<QString>("rawBaseB");
  QTest::addColumn<bool>("expectEqual");

  QTest::newRow("https-explicit-443-matches-implicit")
      << QStringLiteral("https://example.com:443")
      << QStringLiteral("https://example.com") << true;
  QTest::newRow("http-explicit-80-matches-implicit-loopback")
      << QStringLiteral("http://localhost:80")
      << QStringLiteral("http://localhost") << true;
  QTest::newRow("https-non-default-port-preserved-distinct")
      << QStringLiteral("https://example.com:8443")
      << QStringLiteral("https://example.com") << false;
  QTest::newRow("https-explicit-443-with-path-matches")
      << QStringLiteral("https://example.com:443/cdn")
      << QStringLiteral("https://example.com/cdn") << true;
}

void AssetLocatorTests::assetBaseCanonicalizesAwayExplicitDefaultPort() {
  QFETCH(QString, rawBaseA);
  QFETCH(QString, rawBaseB);
  QFETCH(bool, expectEqual);

  const AssetOutcome<ValidatedAssetSource> a =
      ValidatedAssetSource::fromRaw(rawBaseA);
  const AssetOutcome<ValidatedAssetSource> b =
      ValidatedAssetSource::fromRaw(rawBaseB);
  QVERIFY2(bool(a), qPrintable(a ? QString() : a.error().message));
  QVERIFY2(bool(b), qPrintable(b ? QString() : b.error().message));

  if (expectEqual) {
    QCOMPARE(a->normalizedUrl(), b->normalizedUrl());
    QVERIFY(*a == *b);
  } else {
    QVERIFY(a->normalizedUrl() != b->normalizedUrl());
    QVERIFY(!(*a == *b));
  }
}

void AssetLocatorTests::hostileIdentifiersRejected_data() {
  QTest::addColumn<QString>("identifier");
  QTest::addColumn<bool>("expectValid");

  QTest::newRow("valid") << QStringLiteral("valid-id_01") << true;
  QTest::newRow("empty") << QString() << false;
  QTest::newRow("path-traversal") << QStringLiteral("../etc/passwd") << false;
  QTest::newRow("nested-slash") << QStringLiteral("a/b") << false;
  QTest::newRow("dot") << QStringLiteral(".") << false;
  QTest::newRow("dotdot") << QStringLiteral("..") << false;
  // Real pinned digest sources contain uppercase segments in official card
  // identifiers (e.g. contracts/asset-locale-digest-sources/ita.json's
  // "cards/04242B.avif"); the grammar accepts ASCII letters of either
  // case, case preserved (never normalized), while still rejecting every
  // other hostile construct below.
  QTest::newRow("uppercase") << QStringLiteral("CARD01") << true;
  QTest::newRow("mixed-case-suffix") << QStringLiteral("04242B") << true;
  QTest::newRow("space") << QStringLiteral("a b") << false;
  QTest::newRow("userinfo-at") << QStringLiteral("a@b") << false;
  QTest::newRow("colon") << QStringLiteral("a:b") << false;
  QTest::newRow("query") << QStringLiteral("a?b=1") << false;
  QTest::newRow("fragment") << QStringLiteral("a#b") << false;
  QTest::newRow("percent-encoded") << QStringLiteral("a%2e%2e") << false;
  QTest::newRow("leading-dash") << QStringLiteral("-abc") << false;
  QTest::newRow("trailing-dash") << QStringLiteral("abc-") << false;
  QTest::newRow("non-ascii") << QStringLiteral("caf\u00e9") << false;
  QTest::newRow("too-long-official")
      << QString(QStringLiteral("a")).repeated(33) << false;
}

void AssetLocatorTests::hostileIdentifiersRejected() {
  QFETCH(QString, identifier);
  QFETCH(bool, expectValid);

  const AssetKey key = makeKey(kDefaultBase(), AssetCategory::Card, identifier);
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);

  if (expectValid) {
    QVERIFY2(bool(result),
             qPrintable(result ? QString() : result.error().message));
  } else {
    QVERIFY(!result);
    QCOMPARE(result.error().code, AssetErrorCode::InvalidIdentifier);
  }
}

// Golden canonical paths, ported directly from the real web client's own
// path-building helpers (see AssetLocator.cpp's header comment for exact
// source citations) -- no live CDN dependency: every expected string
// below is a literal copied from that source inspection. Every row uses
// locale="" so the English/default candidate is always result->first(),
// letting a single assertion per row cover every category/side/homebrew/
// mutation shape without needing per-row candidate-index bookkeeping.
void AssetLocatorTests::goldenCanonicalPaths_data() {
  QTest::addColumn<int>("category");
  QTest::addColumn<int>("side");
  QTest::addColumn<QString>("identifier");
  QTest::addColumn<QString>("homebrewNamespace");
  QTest::addColumn<QString>("mutationId");
  QTest::addColumn<QString>("expectedUrl");

  using C = AssetCategory;
  using S = AssetSide;
  const QString base = kDefaultBase();

  // The user-cited exact canonical path.
  QTest::newRow("card-front-cited-exact")
      << int(C::Card) << int(S::Front) << QStringLiteral("01001") << QString()
      << QString() << base + QStringLiteral("/img/arkham/cards/01001.avif");

  QTest::newRow("card-front-leading-c-stripped")
      << int(C::Card) << int(S::Front) << QStringLiteral("c01001") << QString()
      << QString() << base + QStringLiteral("/img/arkham/cards/01001.avif");

  QTest::newRow("card-alternate-front-direct")
      << int(C::Card) << int(S::AlternateFront) << QStringLiteral("01001")
      << QString() << QString()
      << base + QStringLiteral("/img/arkham/cards/01001a.avif");

  QTest::newRow("card-resolved-front-generic-rule")
      << int(C::Card) << int(S::ResolvedFront) << QStringLiteral("01002a")
      << QString() << QString()
      << base + QStringLiteral("/img/arkham/cards/01002b.avif");

  QTest::newRow("card-resolved-front-pinned-override")
      << int(C::Card) << int(S::ResolvedFront) << QStringLiteral("03276a")
      << QString() << QString()
      << base + QStringLiteral("/img/arkham/cards/03276ab.avif");

  QTest::newRow("card-mutated-front")
      << int(C::Card) << int(S::MutatedFront) << QStringLiteral("01003")
      << QString() << QStringLiteral("mut42")
      << base + QStringLiteral("/img/arkham/cards/01003_mut42.avif");

  QTest::newRow("investigator-portrait-root-and-strip")
      << int(C::InvestigatorPortrait) << int(S::Front)
      << QStringLiteral("c01001") << QString() << QString()
      << base + QStringLiteral("/img/arkham/portraits/01001.jpg");

  QTest::newRow("chaos-token-ct-prefix")
      << int(C::ChaosToken) << int(S::Front) << QStringLiteral("skull")
      << QString() << QString()
      << base + QStringLiteral("/img/arkham/chaos-tokens/ct-skull.png");

  QTest::newRow("set-icon-root-and-strip")
      << int(C::SetIcon) << int(S::Front) << QStringLiteral("c01001")
      << QString() << QString()
      << base + QStringLiteral("/img/arkham/sets/01001.png");

  QTest::newRow("campaign-box-root")
      << int(C::CampaignBox) << int(S::Front) << QStringLiteral("core")
      << QString() << QString()
      << base + QStringLiteral("/img/arkham/boxes/core.jpg");

  QTest::newRow("slot-icon-root")
      << int(C::SlotIcon) << int(S::Front) << QStringLiteral("ally")
      << QString() << QString()
      << base + QStringLiteral("/img/arkham/slots/ally.png");

  QTest::newRow("homebrew-card-namespace-and-strip")
      << int(C::HomebrewCard) << int(S::Front) << QStringLiteral("c1")
      << QStringLiteral("mycampaign") << QString()
      << base + QStringLiteral("/img/arkham/homebrew/mycampaign/cards/1.avif");

  // Review item 2: HomebrewSet must use INDEPENDENTLY validated namespace
  // (homebrewNamespace) and set-id (identifier) fields -- never the same
  // string reused for both like HomebrewBox's single per-campaign cover
  // art below.
  QTest::newRow("homebrew-set-independent-namespace-and-set-id")
      << int(C::HomebrewSet) << int(S::Front)
      << QStringLiteral("electric-nightmare") << QStringLiteral("dark-matter")
      << QString()
      << base + QStringLiteral("/img/arkham/homebrew/dark-matter/sets/"
                               "electric-nightmare.png");

  // The one real special case (GameRow.vue's campaignIcon: a raw
  // ":{homebrewId}" ID with no second colon) happens to reuse the SAME
  // string for both namespace and set-id -- but that is the CALLER
  // passing the same value twice, not a structural same-field reuse in
  // the type, so this row exercises it via two independently-validated
  // fields that just happen to be equal.
  QTest::newRow("homebrew-set-campaign-icon-special-case")
      << int(C::HomebrewSet) << int(S::Front) << QStringLiteral("mycampaign")
      << QStringLiteral("mycampaign") << QString()
      << base + QStringLiteral(
                    "/img/arkham/homebrew/mycampaign/sets/mycampaign.png");

  QTest::newRow("homebrew-box-identifier-reused-twice")
      << int(C::HomebrewBox) << int(S::Front) << QStringLiteral("mybox")
      << QString() << QString()
      << base + QStringLiteral("/img/arkham/homebrew/mybox/boxes/mybox.jpg");
}

void AssetLocatorTests::goldenCanonicalPaths() {
  QFETCH(int, category);
  QFETCH(int, side);
  QFETCH(QString, identifier);
  QFETCH(QString, homebrewNamespace);
  QFETCH(QString, mutationId);
  QFETCH(QString, expectedUrl);

  const AssetKey key = makeKey(kDefaultBase(), AssetCategory(category),
                               identifier, AssetSide(side), QString(),
                               std::nullopt, homebrewNamespace, mutationId);
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY2(bool(result),
           qPrintable(result ? QString() : result.error().message));
  QVERIFY(!result->isEmpty());
  QCOMPARE(result->first().url.toString(QUrl::FullyEncoded), expectedUrl);
}

void AssetLocatorTests::formatMismatchForCategoryRejected_data() {
  QTest::addColumn<int>("category");
  QTest::addColumn<int>("wrongFormat");

  using C = AssetCategory;
  using F = AssetFormat;
  QTest::newRow("card-wants-avif-not-jpeg") << int(C::Card) << int(F::Jpeg);
  QTest::newRow("card-wants-avif-not-png") << int(C::Card) << int(F::Png);
  QTest::newRow("investigator-wants-jpeg-not-avif")
      << int(C::InvestigatorPortrait) << int(F::Avif);
  QTest::newRow("set-icon-wants-png-not-jpeg")
      << int(C::SetIcon) << int(F::Jpeg);
  QTest::newRow("campaign-box-wants-jpeg-not-png")
      << int(C::CampaignBox) << int(F::Png);
}

void AssetLocatorTests::formatMismatchForCategoryRejected() {
  QFETCH(int, category);
  QFETCH(int, wrongFormat);

  const AssetKey key = makeKey(kDefaultBase(), AssetCategory(category),
                               QStringLiteral("valid01"), AssetSide::Front,
                               QString(), AssetFormat(wrongFormat));
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY(!result);
  QCOMPARE(result.error().code, AssetErrorCode::FormatMismatchForCategory);
}

void AssetLocatorTests::homebrewNamespaceValidated_data() {
  QTest::addColumn<int>("category");
  QTest::addColumn<QString>("homebrewNamespace");
  QTest::addColumn<bool>("expectValid");

  using C = AssetCategory;
  QTest::newRow("homebrew-card-missing-namespace")
      << int(C::HomebrewCard) << QString() << false;
  QTest::newRow("homebrew-card-hostile-namespace")
      << int(C::HomebrewCard) << QStringLiteral("../etc") << false;
  QTest::newRow("homebrew-card-uppercase-namespace")
      << int(C::HomebrewCard) << QStringLiteral("MyCampaign") << false;
  QTest::newRow("homebrew-card-valid-namespace")
      << int(C::HomebrewCard) << QStringLiteral("my-campaign_1") << true;
  QTest::newRow("non-homebrew-category-with-namespace-rejected")
      << int(C::Card) << QStringLiteral("should-not-be-here") << false;
  // Review item 2: HomebrewSet now independently requires/validates its
  // own homebrewNamespace exactly like HomebrewCard (previously it had no
  // namespace field at all and silently reused `identifier` for both the
  // directory and file name).
  QTest::newRow("homebrew-set-missing-namespace")
      << int(C::HomebrewSet) << QString() << false;
  QTest::newRow("homebrew-set-hostile-namespace")
      << int(C::HomebrewSet) << QStringLiteral("../etc") << false;
  QTest::newRow("homebrew-set-colon-namespace")
      << int(C::HomebrewSet) << QStringLiteral("a:b") << false;
  QTest::newRow("homebrew-set-uppercase-namespace")
      << int(C::HomebrewSet) << QStringLiteral("MyCampaign") << false;
  QTest::newRow("homebrew-set-valid-namespace")
      << int(C::HomebrewSet) << QStringLiteral("my-campaign_1") << true;
}

void AssetLocatorTests::homebrewNamespaceValidated() {
  QFETCH(int, category);
  QFETCH(QString, homebrewNamespace);
  QFETCH(bool, expectValid);

  const AssetKey key =
      makeKey(kDefaultBase(), AssetCategory(category), QStringLiteral("c1"),
              AssetSide::Front, QString(), std::nullopt, homebrewNamespace);
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  if (expectValid) {
    QVERIFY2(bool(result),
             qPrintable(result ? QString() : result.error().message));
  } else {
    QVERIFY(!result);
    QCOMPARE(result.error().code, AssetErrorCode::InvalidHomebrewNamespace);
  }
}

// Review item 2: HomebrewSet's own set-id (identifier) is independently
// validated with the SAME strict grammar every other category's
// identifier uses, so it also rejects traversal/colon/separator hostile
// input -- not merely accepted verbatim because the namespace field is
// (now correctly) present.
void AssetLocatorTests::homebrewSetIdentifierRejected_data() {
  QTest::addColumn<QString>("identifier");
  QTest::addColumn<bool>("expectValid");

  QTest::newRow("valid") << QStringLiteral("electric-nightmare") << true;
  QTest::newRow("empty") << QString() << false;
  QTest::newRow("path-traversal") << QStringLiteral("../etc/passwd") << false;
  QTest::newRow("nested-slash") << QStringLiteral("a/b") << false;
  QTest::newRow("colon") << QStringLiteral("a:b") << false;
  QTest::newRow("percent-encoded") << QStringLiteral("a%2e%2e") << false;
}

void AssetLocatorTests::homebrewSetIdentifierRejected() {
  QFETCH(QString, identifier);
  QFETCH(bool, expectValid);

  const AssetKey key = makeKey(kDefaultBase(), AssetCategory::HomebrewSet,
                               identifier, AssetSide::Front, QString(),
                               std::nullopt, QStringLiteral("dark-matter"));
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  if (expectValid) {
    QVERIFY2(bool(result),
             qPrintable(result ? QString() : result.error().message));
  } else {
    QVERIFY(!result);
    QCOMPARE(result.error().code, AssetErrorCode::InvalidIdentifier);
  }
}

void AssetLocatorTests::mutationIdValidated_data() {
  QTest::addColumn<QString>("mutationId");
  QTest::addColumn<int>("side");
  QTest::addColumn<bool>("expectValid");

  QTest::newRow("mutated-front-missing-mutation-id")
      << QString() << int(AssetSide::MutatedFront) << false;
  QTest::newRow("mutated-front-hostile-mutation-id")
      << QStringLiteral("../etc") << int(AssetSide::MutatedFront) << false;
  QTest::newRow("mutated-front-valid-mutation-id")
      << QStringLiteral("mut-42_a") << int(AssetSide::MutatedFront) << true;
  // Real pinned digest sources contain uppercase mutationId segments (e.g.
  // contracts/asset-locale-digest-sources/fr.json's
  // "cards/01514_Mutated19.avif"); the identifier grammar must accept ASCII
  // uppercase letters, case preserved, not just lowercase.
  QTest::newRow("mutated-front-uppercase-mutation-id")
      << QStringLiteral("Mutated19") << int(AssetSide::MutatedFront) << true;
  QTest::newRow("front-side-with-mutation-id-rejected")
      << QStringLiteral("mut42") << int(AssetSide::Front) << false;
}

void AssetLocatorTests::mutationIdValidated() {
  QFETCH(QString, mutationId);
  QFETCH(int, side);
  QFETCH(bool, expectValid);

  const AssetKey key =
      makeKey(kDefaultBase(), AssetCategory::Card, QStringLiteral("01001"),
              AssetSide(side), QString(), std::nullopt, QString(), mutationId);
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  if (expectValid) {
    QVERIFY2(bool(result),
             qPrintable(result ? QString() : result.error().message));
  } else {
    QVERIFY(!result);
    QCOMPARE(result.error().code, AssetErrorCode::InvalidMutationId);
  }
}

void AssetLocatorTests::resolvedFrontOverridesAndGenericRule_data() {
  QTest::addColumn<QString>("identifier");
  QTest::addColumn<QString>("expectedArtCode");

  // The four pinned per-instance overrides (03276/03279's resolved sides
  // do not follow the generic rule) -- see resolvedSideOverrides() in
  // AssetLocator.cpp.
  QTest::newRow("override-03276a")
      << QStringLiteral("03276a") << QStringLiteral("03276ab");
  QTest::newRow("override-03276b")
      << QStringLiteral("03276b") << QStringLiteral("03276bb");
  QTest::newRow("override-03279a")
      << QStringLiteral("03279a") << QStringLiteral("03279ab");
  QTest::newRow("override-03279b")
      << QStringLiteral("03279b") << QStringLiteral("03279bb");

  // Generic rule: strip one trailing [aceg] if present, then append "b".
  QTest::newRow("generic-trailing-a")
      << QStringLiteral("01002a") << QStringLiteral("01002b");
  QTest::newRow("generic-trailing-c")
      << QStringLiteral("01002c") << QStringLiteral("01002b");
  QTest::newRow("generic-trailing-e")
      << QStringLiteral("01002e") << QStringLiteral("01002b");
  QTest::newRow("generic-trailing-g")
      << QStringLiteral("01002g") << QStringLiteral("01002b");
  QTest::newRow("generic-no-trailing-transform-char")
      << QStringLiteral("01001") << QStringLiteral("01001b");
}

void AssetLocatorTests::resolvedFrontOverridesAndGenericRule() {
  QFETCH(QString, identifier);
  QFETCH(QString, expectedArtCode);

  const AssetKey key = makeKey(kDefaultBase(), AssetCategory::Card, identifier,
                               AssetSide::ResolvedFront);
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY2(bool(result),
           qPrintable(result ? QString() : result.error().message));
  QVERIFY(!result->isEmpty());
  const QString expectedUrl = kDefaultBase() +
                              QStringLiteral("/img/arkham/cards/") +
                              expectedArtCode + QStringLiteral(".avif");
  QCOMPARE(result->first().url.toString(QUrl::FullyEncoded), expectedUrl);
}

void AssetLocatorTests::alternateFrontSkippedWhenIdentifierNotDigitEnding() {
  // A DIRECT AlternateFront request for an identifier not ending in a
  // digit has no valid art code at all: this is a hard error
  // (InvalidSideForIdentifier), not silently resolved to something else.
  const AssetKey direct =
      makeKey(kDefaultBase(), AssetCategory::Card, QStringLiteral("01001a"),
              AssetSide::AlternateFront);
  const AssetOutcome<QVector<AssetCandidate>> directResult =
      AssetLocator::resolveCandidates(direct);
  QVERIFY(!directResult);
  QCOMPARE(directResult.error().code, AssetErrorCode::InvalidSideForIdentifier);

  // The AUTO-DERIVED alternate-front fallback (appended after a plain
  // Front request) is different: it is silently OMITTED, not an error,
  // exactly mirroring altFrontImage() returning null in the real client.
  const AssetKey front = makeKey(kDefaultBase(), AssetCategory::Card,
                                 QStringLiteral("01001a"), AssetSide::Front);
  const AssetOutcome<QVector<AssetCandidate>> frontResult =
      AssetLocator::resolveCandidates(front);
  QVERIFY2(bool(frontResult),
           qPrintable(frontResult ? QString() : frontResult.error().message));
  for (const AssetCandidate &candidate : *frontResult) {
    QVERIFY(!candidate.isAlternateFrontFallback);
  }
}

void AssetLocatorTests::nonCardCategoryRejectsNonFrontSide() {
  const AssetKey key =
      makeKey(kDefaultBase(), AssetCategory::InvestigatorPortrait,
              QStringLiteral("valid01"), AssetSide::Back);
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY(!result);
  QCOMPARE(result.error().code, AssetErrorCode::InvalidSideForCategory);
}

void AssetLocatorTests::localizedCandidateOnlyWhenDigestConfirms() {
  // "01001" front has an "it"->"ita" localized entry in
  // contracts/asset-locale-digest.json.
  const AssetKey known =
      makeKey(kDefaultBase(), AssetCategory::Card, QStringLiteral("01001"),
              AssetSide::Front, QStringLiteral("it"));
  const AssetOutcome<QVector<AssetCandidate>> knownResult =
      AssetLocator::resolveCandidates(known);
  QVERIFY(bool(knownResult));
  QVERIFY(knownResult->size() >= 2);
  QCOMPARE(knownResult->first().locale, QStringLiteral("it"));
  QVERIFY2(knownResult->first().url.path().startsWith(
               QStringLiteral("/img/arkham/ita/")),
           qPrintable(knownResult->first().url.path()));
  QCOMPARE(knownResult->first().url.toString(QUrl::FullyEncoded),
           kDefaultBase() + QStringLiteral("/img/arkham/ita/cards/01001.avif"));

  // "99999" front has no digest entry for any locale: localized candidate
  // must be skipped entirely (never issue a request known to 404).
  const AssetKey unknown =
      makeKey(kDefaultBase(), AssetCategory::Card, QStringLiteral("99999"),
              AssetSide::Front, QStringLiteral("it"));
  const AssetOutcome<QVector<AssetCandidate>> unknownResult =
      AssetLocator::resolveCandidates(unknown);
  QVERIFY(bool(unknownResult));
  for (const AssetCandidate &candidate : *unknownResult) {
    QVERIFY(candidate.locale.isEmpty());
  }
}

void AssetLocatorTests::unmappedLocaleSkipsLocalizedCandidate() {
  // "de" is not in the pinned locale map at all.
  const AssetKey key =
      makeKey(kDefaultBase(), AssetCategory::Card, QStringLiteral("01001"),
              AssetSide::Front, QStringLiteral("de"));
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY(bool(result));
  for (const AssetCandidate &candidate : *result) {
    QVERIFY(candidate.locale.isEmpty());
  }
}

void AssetLocatorTests::englishLocaleNeverLocalized() {
  const AssetKey key =
      makeKey(kDefaultBase(), AssetCategory::Card, QStringLiteral("01001"),
              AssetSide::Front, QStringLiteral("en"));
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY(bool(result));
  for (const AssetCandidate &candidate : *result) {
    QVERIFY(candidate.locale.isEmpty());
  }
}

void AssetLocatorTests::alternateFrontFallbackOnlyForFrontCardSide() {
  const AssetKey front = makeKey(kDefaultBase(), AssetCategory::Card,
                                 QStringLiteral("valid01"), AssetSide::Front);
  const AssetOutcome<QVector<AssetCandidate>> frontResult =
      AssetLocator::resolveCandidates(front);
  QVERIFY(bool(frontResult));
  QVERIFY(frontResult->last().isAlternateFrontFallback);

  const AssetKey back =
      makeBackKey(kDefaultBase(), AssetCategory::Card,
                  CardBackKind::SameCodeStripTrailingAThenAppendB,
                  AssetFormat::Avif, QStringLiteral("valid01"));
  const AssetOutcome<QVector<AssetCandidate>> backResult =
      AssetLocator::resolveCandidates(back);
  QVERIFY(bool(backResult));
  for (const AssetCandidate &candidate : *backResult) {
    QVERIFY(!candidate.isAlternateFrontFallback);
  }
}

void AssetLocatorTests::candidatesAreDedupedAndBounded() {
  const AssetKey key =
      makeKey(kDefaultBase(), AssetCategory::Card, QStringLiteral("01001"),
              AssetSide::Front, QStringLiteral("it"));
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY(bool(result));
  QVERIFY(result->size() <= 3);

  QSet<QString> seen;
  for (const AssetCandidate &candidate : *result) {
    const QString urlString = candidate.url.toString(QUrl::FullyEncoded);
    QVERIFY2(!seen.contains(urlString), qPrintable(urlString));
    seen.insert(urlString);
  }
}

// Review item 1: every CardBackKind branch, golden-tested against the
// exact real web client transform it ports (see CardBackKind's doc
// comment in AssetTypes.h for the source citation per branch) -- in
// particular, the "01121ab" branches below are the REAL, correct
// behavior for EnemyType/StoryType-shaped backs, while "01121b" is the
// REAL, correct behavior for Act/Agenda/Scenario/Investigator-shaped
// backs. Both are exercised so a future regression can never collapse
// them back into a single blind "append b" rule.
void AssetLocatorTests::cardBackKindGoldenPaths_data() {
  QTest::addColumn<int>("backKind");
  QTest::addColumn<QString>("identifier");
  QTest::addColumn<QString>("otherSideIdentifier");
  QTest::addColumn<QString>("customBackFilename");
  QTest::addColumn<QString>("homebrewNamespace");
  QTest::addColumn<int>("format");
  QTest::addColumn<QString>("expectedUrl");

  using K = CardBackKind;
  using F = AssetFormat;
  const QString base = kDefaultBase();

  QTest::newRow("same-code-append-b-trailing-a-preserved")
      << int(K::SameCodeAppendB) << QStringLiteral("01121a") << QString()
      << QString() << QString() << int(F::Avif)
      << base + QStringLiteral("/img/arkham/cards/01121ab.avif");

  QTest::newRow("same-code-strip-trailing-a-then-append-b")
      << int(K::SameCodeStripTrailingAThenAppendB) << QStringLiteral("01121a")
      << QString() << QString() << QString() << int(F::Avif)
      << base + QStringLiteral("/img/arkham/cards/01121b.avif");

  QTest::newRow("same-code-strip-trailing-a-no-trailing-a-present")
      << int(K::SameCodeStripTrailingAThenAppendB) << QStringLiteral("01001")
      << QString() << QString() << QString() << int(F::Avif)
      << base + QStringLiteral("/img/arkham/cards/01001b.avif");

  QTest::newRow("explicit-other-side-independent-code")
      << int(K::ExplicitOtherSide) << QString() << QStringLiteral("c01002")
      << QString() << QString() << int(F::Avif)
      << base + QStringLiteral("/img/arkham/cards/01002.avif");

  QTest::newRow("same-as-front-location-type")
      << int(K::SameAsFront) << QStringLiteral("01003") << QString()
      << QString() << QString() << int(F::Avif)
      << base + QStringLiteral("/img/arkham/cards/01003.avif");

  QTest::newRow("generic-encounter-back-fixed-jpeg-path")
      << int(K::GenericEncounterBack) << QString() << QString() << QString()
      << QString() << int(F::Jpeg)
      << base + QStringLiteral("/img/arkham/backs/back_encounter.jpg");

  QTest::newRow("generic-player-back-fixed-jpeg-path")
      << int(K::GenericPlayerBack) << QString() << QString() << QString()
      << QString() << int(F::Jpeg)
      << base + QStringLiteral("/img/arkham/backs/back_player.jpg");

  QTest::newRow("custom-back-verbatim-filename-jpg")
      << int(K::CustomBack) << QString() << QString()
      << QStringLiteral("my-homebrew-back.jpg") << QString() << int(F::Jpeg)
      << base + QStringLiteral("/img/arkham/backs/my-homebrew-back.jpg");

  QTest::newRow("custom-back-verbatim-filename-png")
      << int(K::CustomBack) << QString() << QString()
      << QStringLiteral("my-homebrew-back.png") << QString() << int(F::Png)
      << base + QStringLiteral("/img/arkham/backs/my-homebrew-back.png");

  QTest::newRow("same-code-append-b-homebrew-namespace")
      << int(K::SameCodeAppendB) << QStringLiteral("c1") << QString()
      << QString() << QStringLiteral("mycampaign") << int(F::Avif)
      << base + QStringLiteral("/img/arkham/homebrew/mycampaign/cards/1b.avif");
}

void AssetLocatorTests::cardBackKindGoldenPaths() {
  QFETCH(int, backKind);
  QFETCH(QString, identifier);
  QFETCH(QString, otherSideIdentifier);
  QFETCH(QString, customBackFilename);
  QFETCH(QString, homebrewNamespace);
  QFETCH(int, format);
  QFETCH(QString, expectedUrl);

  const AssetCategory category = homebrewNamespace.isEmpty()
                                     ? AssetCategory::Card
                                     : AssetCategory::HomebrewCard;
  const AssetKey key = makeBackKey(
      kDefaultBase(), category, CardBackKind(backKind), AssetFormat(format),
      identifier, otherSideIdentifier, customBackFilename, homebrewNamespace);
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY2(bool(result),
           qPrintable(result ? QString() : result.error().message));
  QVERIFY(!result->isEmpty());
  QCOMPARE(result->first().url.toString(QUrl::FullyEncoded), expectedUrl);
  QCOMPARE(int(result->first().format), format);
}

// Review item 1: each CardBackKind requires exactly one of identifier/
// otherSideIdentifier/customBackFilename (or none, for the two generic
// fixed backs) -- supplying the WRONG field for a given kind, or leaving
// the required one empty, must be rejected rather than silently ignored.
void AssetLocatorTests::cardBackFieldApplicabilityRejected_data() {
  QTest::addColumn<int>("backKind");
  QTest::addColumn<QString>("identifier");
  QTest::addColumn<QString>("otherSideIdentifier");
  QTest::addColumn<QString>("customBackFilename");
  QTest::addColumn<int>("expectedError");

  using K = CardBackKind;
  using E = AssetErrorCode;

  QTest::newRow("same-code-append-b-missing-identifier")
      << int(K::SameCodeAppendB) << QString() << QString() << QString()
      << int(E::InvalidIdentifier);
  QTest::newRow("same-code-append-b-stray-other-side")
      << int(K::SameCodeAppendB) << QStringLiteral("01001")
      << QStringLiteral("01002") << QString()
      << int(E::InvalidOtherSideIdentifier);
  QTest::newRow("explicit-other-side-missing")
      << int(K::ExplicitOtherSide) << QString() << QString() << QString()
      << int(E::InvalidOtherSideIdentifier);
  QTest::newRow("explicit-other-side-stray-identifier")
      << int(K::ExplicitOtherSide) << QStringLiteral("01001")
      << QStringLiteral("01002") << QString() << int(E::InvalidIdentifier);
  QTest::newRow("custom-back-missing-filename")
      << int(K::CustomBack) << QString() << QString() << QString()
      << int(E::InvalidCustomBackFilename);
  QTest::newRow("custom-back-malformed-no-extension")
      << int(K::CustomBack) << QString() << QString()
      << QStringLiteral("noextension") << int(E::InvalidCustomBackFilename);
  QTest::newRow("custom-back-traversal-filename")
      << int(K::CustomBack) << QString() << QString()
      << QStringLiteral("../etc/passwd.jpg")
      << int(E::InvalidCustomBackFilename);
  QTest::newRow("custom-back-unsupported-extension")
      << int(K::CustomBack) << QString() << QString()
      << QStringLiteral("back.gif") << int(E::InvalidCustomBackFilename);
  QTest::newRow("generic-encounter-back-stray-identifier")
      << int(K::GenericEncounterBack) << QStringLiteral("01001") << QString()
      << QString() << int(E::InvalidIdentifier);
  QTest::newRow("same-as-front-stray-custom-back")
      << int(K::SameAsFront) << QStringLiteral("01001") << QString()
      << QStringLiteral("x.jpg") << int(E::InvalidCustomBackFilename);
}

void AssetLocatorTests::cardBackFieldApplicabilityRejected() {
  QFETCH(int, backKind);
  QFETCH(QString, identifier);
  QFETCH(QString, otherSideIdentifier);
  QFETCH(QString, customBackFilename);
  QFETCH(int, expectedError);

  // format is irrelevant here (field-applicability errors are returned
  // before format is ever checked); pass a plausible value per kind so
  // the earlier applicability check under test is what actually fires.
  const AssetFormat format =
      (CardBackKind(backKind) == CardBackKind::GenericEncounterBack ||
       CardBackKind(backKind) == CardBackKind::GenericPlayerBack)
          ? AssetFormat::Jpeg
          : AssetFormat::Avif;
  const AssetKey key =
      makeBackKey(kDefaultBase(), AssetCategory::Card, CardBackKind(backKind),
                  format, identifier, otherSideIdentifier, customBackFilename);
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY(!result);
  QCOMPARE(int(result.error().code), expectedError);
}

void AssetLocatorTests::cardBackRequiresBackKind() {
  const AssetKey key = makeKey(kDefaultBase(), AssetCategory::Card,
                               QStringLiteral("01001"), AssetSide::Back);
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY(!result);
  QCOMPARE(result.error().code, AssetErrorCode::InvalidBackKind);
}

void AssetLocatorTests::nonBackSideRejectsBackFields() {
  AssetKey key = makeKey(kDefaultBase(), AssetCategory::Card,
                         QStringLiteral("01001"), AssetSide::Front);
  key.backKind = CardBackKind::SameCodeAppendB;
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY(!result);
  QCOMPARE(result.error().code, AssetErrorCode::InvalidBackKind);
}

// The reviewer's specific concern: a blind "code + b" locator would turn
// "01121a" into the NONEXISTENT "01121ab" even for Act/Agenda/Scenario/
// Investigator-shaped cards, which actually resolve to "01121b". This
// asserts SameCodeStripTrailingAThenAppendB never produces "01121ab".
void AssetLocatorTests::cardBackNonexistentTrailingAbRejected() {
  const AssetKey key =
      makeBackKey(kDefaultBase(), AssetCategory::Card,
                  CardBackKind::SameCodeStripTrailingAThenAppendB,
                  AssetFormat::Avif, QStringLiteral("01121a"));
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY2(bool(result),
           qPrintable(result ? QString() : result.error().message));
  const QString url = result->first().url.toString(QUrl::FullyEncoded);
  QVERIFY2(!url.endsWith(QStringLiteral("01121ab.avif")), qPrintable(url));
  QVERIFY(url.endsWith(QStringLiteral("01121b.avif")));
}
