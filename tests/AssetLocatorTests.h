#pragma once

#include <QObject>

// Tests for the pure, side-effect-free AssetLocator::resolveCandidates():
// base-URL policy reuse (driven by the SAME shared strictLoopbackUrlRows()
// table tests/NetworkTests.cpp and tests/AuthClientTests.cpp already drive
// -- see StrictLoopbackUrlTable.h -- so this test can never silently
// diverge into a weaker, asset-only interpretation of the policy),
// ValidatedAssetSource's structural (not merely defensive) base-URL
// enforcement, hostile identifier rejection, real-world golden-path
// canonical routes ported from the actual web client
// (halogenandtoast/ArkhamHorror), category-fixed-format enforcement,
// homebrew namespace/mutation-id validation, the localized -> English ->
// alternate-front fallback chain (digest-gated), deduplication/bound, and
// non-Card side rejection.
class AssetLocatorTests final : public QObject {
  Q_OBJECT

private slots:
  void baseUrlPolicyMatchesSharedTable_data();
  void baseUrlPolicyMatchesSharedTable();

  void defaultConstructedAssetBaseRejected();

  void assetBasePathRejectsDotSegmentsAndEncodedVariants_data();
  void assetBasePathRejectsDotSegmentsAndEncodedVariants();

  void assetBaseCanonicalizesAwayExplicitDefaultPort_data();
  void assetBaseCanonicalizesAwayExplicitDefaultPort();

  void hostileIdentifiersRejected_data();
  void hostileIdentifiersRejected();

  void goldenCanonicalPaths_data();
  void goldenCanonicalPaths();

  void formatMismatchForCategoryRejected_data();
  void formatMismatchForCategoryRejected();

  void homebrewNamespaceValidated_data();
  void homebrewNamespaceValidated();

  void homebrewSetIdentifierRejected_data();
  void homebrewSetIdentifierRejected();

  void mutationIdValidated_data();
  void mutationIdValidated();

  void resolvedFrontOverridesAndGenericRule_data();
  void resolvedFrontOverridesAndGenericRule();

  void alternateFrontSkippedWhenIdentifierNotDigitEnding();

  void nonCardCategoryRejectsNonFrontSide();

  void localizedCandidateOnlyWhenDigestConfirms();
  void unmappedLocaleSkipsLocalizedCandidate();
  void englishLocaleNeverLocalized();

  void alternateFrontFallbackOnlyForFrontCardSide();
  void candidatesAreDedupedAndBounded();

  void cardBackKindGoldenPaths_data();
  void cardBackKindGoldenPaths();

  void cardBackFieldApplicabilityRejected_data();
  void cardBackFieldApplicabilityRejected();

  void cardBackRequiresBackKind();
  void nonBackSideRejectsBackFields();
  void cardBackNonexistentTrailingAbRejected();

  // Round-6 item 9: homebrew ChaosToken route parity, tightened
  // HomebrewCard card-code grammar (vs. the real client's
  // `/^:(.+):(\d+[a-z]*)$/`), and the "identifier `c` strips to empty"
  // degenerate-path bug.
  void homebrewChaosTokenGoldenPath_data();
  void homebrewChaosTokenGoldenPath();

  void chaosTokenInvalidHomebrewNamespaceRejected_data();
  void chaosTokenInvalidHomebrewNamespaceRejected();

  void nonOptionalCategoryStillRejectsHomebrewNamespace();

  void homebrewCardIdentifierGrammarEnforced_data();
  void homebrewCardIdentifierGrammarEnforced();

  void identifierStrippingToEmptyCardCodeRejected_data();
  void identifierStrippingToEmptyCardCodeRejected();
};
