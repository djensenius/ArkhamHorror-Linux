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

  QTest::newRow("card-back")
      << int(C::Card) << int(S::Back) << QStringLiteral("01001") << QString()
      << QString() << base + QStringLiteral("/img/arkham/cards/01001b.avif");

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

  QTest::newRow("homebrew-set-identifier-reused-twice")
      << int(C::HomebrewSet) << int(S::Front) << QStringLiteral("myset")
      << QString() << QString()
      << base + QStringLiteral("/img/arkham/homebrew/myset/sets/myset.png");

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

  const AssetKey back = makeKey(kDefaultBase(), AssetCategory::Card,
                                QStringLiteral("valid01"), AssetSide::Back);
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
