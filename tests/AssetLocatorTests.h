#pragma once

#include <QObject>

// Tests for the pure, side-effect-free AssetLocator::resolveCandidates():
// base-URL policy reuse (driven by the SAME shared strictLoopbackUrlRows()
// table tests/NetworkTests.cpp and tests/AuthClientTests.cpp already drive
// -- see StrictLoopbackUrlTable.h -- so this test can never silently
// diverge into a weaker, asset-only interpretation of the policy), hostile
// identifier rejection, per-category path shape, the localized -> English
// -> alternate-front fallback chain (digest-gated), deduplication/bound,
// and non-Card side rejection.
class AssetLocatorTests final : public QObject {
  Q_OBJECT

private slots:
  void baseUrlPolicyMatchesSharedTable_data();
  void baseUrlPolicyMatchesSharedTable();

  void defensiveRevalidationRejectsAlreadyInvalidBase_data();
  void defensiveRevalidationRejectsAlreadyInvalidBase();

  void hostileIdentifiersRejected_data();
  void hostileIdentifiersRejected();

  void categoryPathShape_data();
  void categoryPathShape();

  void nonCardCategoryRejectsNonFrontSide();

  void localizedCandidateOnlyWhenDigestConfirms();
  void unmappedLocaleSkipsLocalizedCandidate();
  void englishLocaleNeverLocalized();

  void alternateFrontFallbackOnlyForFrontCardSide();
  void candidatesAreDedupedAndBounded();
};
