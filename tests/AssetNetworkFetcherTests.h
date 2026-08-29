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
// cancellation/destruction/stale-callback safety; and (review item 10) a
// strict JPEG codestream completeness check -- a genuine End-Of-Image
// marker must actually be present in the bytes, since Qt's bundled
// decoder silently synthesises a missing one for a body truncated
// mid-transfer, while a complete body containing byte-stuffed 0xFF
// bytes in its entropy-coded scan data must still decode normally.
class AssetNetworkFetcherTests final : public QObject {
  Q_OBJECT

private slots:
  void successfulFetchNeverSendsCookiesOrAuthHeader();
  void nonHttpSchemeIsRejectedAsUnsupportedSchemeWithoutTouchingNetwork();
  void manualRedirectPolicyRejectsEvery3xx_data();
  void manualRedirectPolicyRejectsEvery3xx();
  void notFoundMapsToNotFoundError();
  void serverErrorMapsToUnexpectedStatus_data();
  void serverErrorMapsToUnexpectedStatus();
  void incrementalByteCapAbortsBeforeFullBodyArrives();
  void finalDrainNeverExceedsByteCapForAFastNonIncrementalResponse();
  void contentTypeMismatchIsRejected();
  void magicBytesMismatchIsRejected();
  void dimensionBombIsRejected();
  void pixelBudgetBombIsRejected();
  void malformedImageBodyIsRejected();
  void jpegDecodesRegardlessOfQtPluginKeySpelling();
  void truncatedJpegMissingEoiIsRejectedDespiteQtDecodingIt();
  void truncatedJpegMissingEoiAtVaryingCutPointsAllRejected_data();
  void truncatedJpegMissingEoiAtVaryingCutPointsAllRejected();
  void jpegWithStuffedFFBytesInScanDataStillDecodesWhenComplete();
  void jpegTrailingDataAfterGenuineEoiIsRejected();
  void avifRealFixtureAlwaysDecodesViaLibavif();
  void avifImageSequenceIsRejectedAsMalformedImage();
  void avifMalformedContainerIsReportedAsMalformedImage();
  void avifDimensionBombIsRejectedBeforeAnyPixelDecodeOrAllocation();
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
  void applicationProxyWithCredentialsIsNeverUsedOrLeaked();
  void
  invalidLimitsOrTimeoutIsRejectedAsInvalidConfigurationWithoutThrowing_data();
  void invalidLimitsOrTimeoutIsRejectedAsInvalidConfigurationWithoutThrowing();
  void validConfigurationFactorySucceeds();
};
