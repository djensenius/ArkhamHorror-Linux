#pragma once

#include <QObject>

// Network-level tests for AssetNetworkFetcher, driven against a real
// loopback HTTP server (MockHttpServer) rather than a stubbed
// QNetworkReply, per djensenius/ArkhamHorror-Linux#17's requirement for a
// "deterministic local mock server" and "event-loop waits that assert
// actual dispatch/overlap." Covers: no cookies/auth/cache/redirects ever
// sent or followed; incremental byte-cap abort (proven via bytes actually
// transmitted before abort, not just the final error code); Content-Type
// + magic-byte validation; dimension/pixel-budget bombs; unsupported-codec
// handling (environment-adaptive for AVIF); conditional (304) handling,
// including the "304 without a matching conditional request" typed error;
// and cancellation/destruction/stale-callback safety.
class AssetNetworkFetcherTests final : public QObject {
  Q_OBJECT

private slots:
  void successfulFetchNeverSendsCookiesOrAuthHeader();
  void manualRedirectPolicyRejectsEvery3xx_data();
  void manualRedirectPolicyRejectsEvery3xx();
  void notFoundMapsToNotFoundError();
  void serverErrorMapsToUnexpectedStatus();
  void incrementalByteCapAbortsBeforeFullBodyArrives();
  void finalDrainNeverExceedsByteCapForAFastNonIncrementalResponse();
  void contentTypeMismatchIsRejected();
  void magicBytesMismatchIsRejected();
  void dimensionBombIsRejected();
  void pixelBudgetBombIsRejected();
  void malformedImageBodyIsRejected();
  void jpegDecodesRegardlessOfQtPluginKeySpelling();
  void avifCodecSupportIsEnvironmentAdaptive();
  void avifFtypBoxSizeZeroExtendsToEndOfBufferPerIsobmff();
  void avifFtypMinorVersionIsNeverMisreadAsCompatibleBrand();
  void avifFtypTruncatedBoxSizeNeverReadsPastDeclaredBoundary();
  void avifCompatibleBrandMatchAtVeryLastSlotOfLargeBoxIsFound();
  void avifLargeBoxWithNoMatchingBrandAnywhereIsRejected();
  void conditionalRequestAcceptsMatchingNotModified();
  void conditionalRequestWithLastModifiedAcceptsMatchingNotModified();
  void unconditional304IsRejectedAsTypedError();
  void cancelInvokesCallbackExactlyOnceWithCancelled();
  void timeoutFiresExactlyOnceAndCleansUpItsTimer();
  void destructionNeverInvokesStaleCallback();
};
