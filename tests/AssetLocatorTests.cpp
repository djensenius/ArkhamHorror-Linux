#include "AssetLocatorTests.h"

#include "AssetLocator.h"
#include "StrictLoopbackUrlTable.h"
#include "UrlValidator.h"

#include <QTest>
#include <QUrl>

using namespace Arkham;

namespace {

AssetKey makeKey(const QUrl &base, AssetCategory category,
                 const QString &identifier, AssetSide side = AssetSide::Front,
                 const QString &locale = QString(),
                 AssetFormat format = AssetFormat::Jpeg) {
  AssetKey key;
  key.assetBase = base;
  key.category = category;
  key.identifier = identifier;
  key.side = side;
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

  // AssetKey::assetBase's documented precondition (AssetTypes.h) is that it
  // already holds whatever UrlValidator::validateCustomUrl() returned for
  // the caller's original raw input -- exactly what any real caller (e.g.
  // wiring up an asset-base site setting) must do BEFORE ever constructing
  // an AssetKey. Reproducing that exact call here, against the very same
  // shared table tests/NetworkTests.cpp and tests/AuthClientTests.cpp
  // already drive, is what "reuse the shared policy exactly, never fork a
  // weaker interpretation" means at this boundary.
  //
  // A handful of the table's REJECTED rows are rejected purely because of
  // information present only in the untouched raw string: ambiguous
  // numeric-loopback spellings that QUrl itself silently canonicalises at
  // parse time, Unicode casefold/homoglyph tricks, and control characters
  // in the original text (see UrlValidator.cpp's own comment on
  // isCleartextAuthAllowedForRawInput()). Once turned into a QUrl at all --
  // by ANY code, including this test -- that distinguishing information is
  // gone for good, so those specific rows can only be (and already are)
  // proven at the validateCustomUrl() call itself; they cannot be
  // meaningfully re-proven a second time through AssetLocator's QUrl-typed
  // API, since by construction the QUrl it would receive is
  // indistinguishable from an already-canonical, acceptable one.
  const UrlValidationResult validated = validateCustomUrl(urlString);
  QCOMPARE(bool(validated), expectAccepted);
  if (!expectAccepted) {
    return;
  }

  const AssetKey key =
      makeKey(*validated, AssetCategory::Card, QStringLiteral("valid01"));
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
      basePrefix + QStringLiteral("/cards/valid01.jpg");
  QCOMPARE(result->first().url.toString(QUrl::FullyEncoded),
           expectedCandidateUrl);
}

void AssetLocatorTests::defensiveRevalidationRejectsAlreadyInvalidBase_data() {
  QTest::addColumn<QUrl>("base");

  // These represent an AssetKey::assetBase that violates its own
  // documented precondition (e.g. a caller bug that skipped
  // validateCustomUrl()). Each one still carries, even after a QUrl
  // round-trip, information the shared policy rejects on -- proving
  // AssetLocator's defensive re-validation is real and not merely
  // decorative, without re-deriving the raw-string-only checks that
  // baseUrlPolicyMatchesSharedTable() above documents as out of scope
  // here.
  QTest::newRow("credentials-preserved-through-qurl")
      << QUrl(QStringLiteral("http://user:pass@localhost:9000"));
  QTest::newRow("unsupported-scheme-preserved-through-qurl")
      << QUrl(QStringLiteral("ftp://localhost:9000"));
  QTest::newRow("missing-host") << QUrl(QStringLiteral("file:///etc/passwd"));
  QTest::newRow("completely-invalid-empty-qurl") << QUrl();
}

void AssetLocatorTests::defensiveRevalidationRejectsAlreadyInvalidBase() {
  QFETCH(QUrl, base);

  const AssetKey key =
      makeKey(base, AssetCategory::Card, QStringLiteral("valid01"));
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
  QTest::newRow("uppercase") << QStringLiteral("CARD01") << false;
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

  const AssetKey key = makeKey(AssetLocator::defaultAssetBase(),
                               AssetCategory::Card, identifier);
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

void AssetLocatorTests::categoryPathShape_data() {
  QTest::addColumn<int>("category");
  QTest::addColumn<int>("side");
  QTest::addColumn<QString>("expectedSuffix");

  using C = AssetCategory;
  using S = AssetSide;
  QTest::newRow("card-front")
      << int(C::Card) << int(S::Front) << QStringLiteral("/cards/valid01.jpg");
  QTest::newRow("card-back")
      << int(C::Card) << int(S::Back) << QStringLiteral("/cards/valid01b.jpg");
  QTest::newRow("card-alt-front") << int(C::Card) << int(S::AlternateFront)
                                  << QStringLiteral("/cards/valid01a.jpg");
  QTest::newRow("card-resolved-front")
      << int(C::Card) << int(S::ResolvedFront)
      << QStringLiteral("/cards/valid01-resolved.jpg");
  QTest::newRow("card-mutated-front")
      << int(C::Card) << int(S::MutatedFront)
      << QStringLiteral("/cards/valid01-mutated.jpg");
  QTest::newRow("investigator") << int(C::InvestigatorPortrait) << int(S::Front)
                                << QStringLiteral("/investigators/valid01.jpg");
  QTest::newRow("chaos-token") << int(C::ChaosToken) << int(S::Front)
                               << QStringLiteral("/chaos-tokens/valid01.jpg");
  QTest::newRow("set-icon") << int(C::SetIcon) << int(S::Front)
                            << QStringLiteral("/sets/valid01.jpg");
  QTest::newRow("campaign-box") << int(C::CampaignBox) << int(S::Front)
                                << QStringLiteral("/campaigns/valid01.jpg");
  QTest::newRow("slot-icon") << int(C::SlotIcon) << int(S::Front)
                             << QStringLiteral("/slots/valid01.jpg");
  QTest::newRow("homebrew-card")
      << int(C::HomebrewCard) << int(S::Front)
      << QStringLiteral("/homebrew/cards/valid01.jpg");
  QTest::newRow("homebrew-set") << int(C::HomebrewSet) << int(S::Front)
                                << QStringLiteral("/homebrew/sets/valid01.jpg");
  QTest::newRow("homebrew-box")
      << int(C::HomebrewBox) << int(S::Front)
      << QStringLiteral("/homebrew/boxes/valid01.jpg");
}

void AssetLocatorTests::categoryPathShape() {
  QFETCH(int, category);
  QFETCH(int, side);
  QFETCH(QString, expectedSuffix);

  const AssetKey key =
      makeKey(AssetLocator::defaultAssetBase(), AssetCategory(category),
              QStringLiteral("valid01"), AssetSide(side));
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY2(bool(result),
           qPrintable(result ? QString() : result.error().message));
  QVERIFY(!result->isEmpty());
  QVERIFY2(result->first().url.path().endsWith(expectedSuffix),
           qPrintable(result->first().url.path()));
}

void AssetLocatorTests::nonCardCategoryRejectsNonFrontSide() {
  const AssetKey key = makeKey(AssetLocator::defaultAssetBase(),
                               AssetCategory::InvestigatorPortrait,
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
      makeKey(AssetLocator::defaultAssetBase(), AssetCategory::Card,
              QStringLiteral("01001"), AssetSide::Front, QStringLiteral("it"));
  const AssetOutcome<QVector<AssetCandidate>> knownResult =
      AssetLocator::resolveCandidates(known);
  QVERIFY(bool(knownResult));
  QVERIFY(knownResult->size() >= 2);
  QCOMPARE(knownResult->first().locale, QStringLiteral("it"));
  QVERIFY(knownResult->first().url.path().startsWith(QStringLiteral("/ita/")));

  // "99999" front has no digest entry for any locale: localized candidate
  // must be skipped entirely (never issue a request known to 404).
  const AssetKey unknown =
      makeKey(AssetLocator::defaultAssetBase(), AssetCategory::Card,
              QStringLiteral("99999"), AssetSide::Front, QStringLiteral("it"));
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
      makeKey(AssetLocator::defaultAssetBase(), AssetCategory::Card,
              QStringLiteral("01001"), AssetSide::Front, QStringLiteral("de"));
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY(bool(result));
  for (const AssetCandidate &candidate : *result) {
    QVERIFY(candidate.locale.isEmpty());
  }
}

void AssetLocatorTests::englishLocaleNeverLocalized() {
  const AssetKey key =
      makeKey(AssetLocator::defaultAssetBase(), AssetCategory::Card,
              QStringLiteral("01001"), AssetSide::Front, QStringLiteral("en"));
  const AssetOutcome<QVector<AssetCandidate>> result =
      AssetLocator::resolveCandidates(key);
  QVERIFY(bool(result));
  for (const AssetCandidate &candidate : *result) {
    QVERIFY(candidate.locale.isEmpty());
  }
}

void AssetLocatorTests::alternateFrontFallbackOnlyForFrontCardSide() {
  const AssetKey front =
      makeKey(AssetLocator::defaultAssetBase(), AssetCategory::Card,
              QStringLiteral("valid01"), AssetSide::Front);
  const AssetOutcome<QVector<AssetCandidate>> frontResult =
      AssetLocator::resolveCandidates(front);
  QVERIFY(bool(frontResult));
  QVERIFY(frontResult->last().isAlternateFrontFallback);

  const AssetKey back =
      makeKey(AssetLocator::defaultAssetBase(), AssetCategory::Card,
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
      makeKey(AssetLocator::defaultAssetBase(), AssetCategory::Card,
              QStringLiteral("01001"), AssetSide::Front, QStringLiteral("it"));
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
