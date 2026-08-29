#pragma once

#include "AssetTypes.h"

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Arkham {

// Review round-4 item 1: the ONLY way AssetNetworkFetcher::fetch() can be
// reached at all -- a QUrl by itself, however innocuous-looking, is never
// an acceptable fetch() argument. An AssetFetchUrl cannot be default- or
// copy-constructed from an arbitrary QUrl: its only public constructor
// path is the validating factory validate(), which reuses this project's
// existing shared transport-security policy exactly (see
// isSecureOrLoopbackAuthTransport() in AuthTransportSecurity.h, already
// used for the same purpose elsewhere) rather than forking a weaker,
// asset-only reimplementation of "is this URL safe to request": https is
// permitted to any host; http is permitted ONLY to an exact canonical
// loopback spelling; a URL carrying userinfo, a missing host, a query
// string, or a fragment is rejected outright. A real AssetCandidate::url
// (built by AssetLocator from an already-validated ValidatedAssetSource
// plus a hardened relative path -- see AssetTypes.cpp/AssetLocator.cpp)
// always trivially satisfies this policy; an arbitrary, forged, or
// otherwise unvalidated URL -- passed directly to fetch() by a future or
// buggy call site, bypassing AssetLocator entirely -- can never become an
// AssetFetchUrl in the first place, so it can never reach this class's
// QNetworkAccessManager at all. There is no bypass constructor, public or
// private, other than validate().
class AssetFetchUrl {
public:
  [[nodiscard]] static AssetOutcome<AssetFetchUrl> validate(const QUrl &url);

  [[nodiscard]] const QUrl &url() const { return m_url; }

private:
  explicit AssetFetchUrl(QUrl url) : m_url(std::move(url)) {}

  QUrl m_url;
};

// Dedicated, credential-free HTTP fetcher for asset candidate URLs.
//
// Isolation and transport-security posture (deliberately mirrors
// NetworkAuthenticationClient's documented policy, applied here to image
// fetches instead of authentication requests):
//   - The production constructor owns a dedicated QNetworkAccessManager
//     (destroyed together with this fetcher); no cookie jar is ever
//     attached, and every request explicitly disables cookie load/save
//     and cached-authentication reuse, so no cookie or credential can ever
//     be sent or persisted.
//   - No Authorization header is ever added by this class.
//   - Every request uses QNetworkRequest::ManualRedirectPolicy; every 3xx
//     response is reported as AssetErrorCode::RedirectRejected. No
//     redirect is ever auto-followed, to another origin or the same one.
//   - CacheLoadControlAttribute is AlwaysNetwork and CacheSaveControlAttribute
//     is false: Qt's own built-in HTTP disk cache is never consulted or
//     populated. Caching is handled exclusively by AssetCache, which has
//     its own atomic on-disk format and quota policy.
//   - The response body is capped incrementally as bytes arrive
//     (QNetworkReply::readyRead), not merely checked once at the end:
//     exceeding `limits.maxEncodedBytes` aborts the reply immediately, so
//     a hostile or misconfigured server cannot exhaust memory by
//     streaming an unbounded body before this class ever gets to check its
//     final size.
//
// Content validation (only performed after a full, unaborted 2xx body is
// received):
//   1. The response's declared Content-Type must match the media type for
//      `expectedFormat` (AssetErrorCode::ContentTypeMismatch otherwise).
//   2. The body's magic bytes must independently identify the same format
//      (AssetErrorCode::MagicBytesMismatch otherwise) -- a server lying
//      about Content-Type is caught even if declared correctly by
//      coincidence, and vice versa.
//   3. QImageReader::size() is inspected BEFORE any decode is attempted
//      for JPEG/PNG (AVIF's equivalent dimension check happens inside
//      AssetAvifDecoder.cpp, against parsed-but-not-yet-decoded container
//      metadata -- see decodeAndValidate()'s AVIF branch below); each
//      dimension is capped at `limits.maxDimensionPixels` and
//      width*height (computed in 64-bit, never overflowing) is capped at
//      `limits.maxTotalPixels`, rejecting a dimension/pixel bomb before a
//      single pixel is decoded.
//   4. Only then is the image actually decoded: AVIF goes directly
//      through libavif's own C API (AssetAvifDecoder.h), a required
//      build/runtime dependency, never through Qt's plugin registry.
//      JPEG/PNG still go through QImageReader; if the installed Qt build
//      has no plugin capable of decoding one of those two formats, this
//      is reported as the distinct AssetErrorCode::UnsupportedCodec
//      rather than the generic AssetErrorCode::MalformedImage used for a
//      truncated/corrupt body of an otherwise-supported format.
//
// Every async callback is guarded by a QPointer and an exact per-request
// handle so a reply belonging to an old, cancelled, or superseded request
// can never invoke a callback after this object (or the specific pending
// request) is gone -- see AssetNetworkFetcher::fetch()'s implementation
// comment for the exact mechanism.
class AssetNetworkFetcher final : public QObject {
  Q_OBJECT
public:
  struct Limits {
    qint64 maxEncodedBytes; // 20 MiB
    int maxDimensionPixels;
    qint64 maxTotalPixels; // 32 megapixels

    // A normal (non-default-member-initializer) constructor avoids a Clang
    // diagnostic ("default member initializer needed within definition of
    // enclosing class") that fires when a nested aggregate with in-class
    // default member initializers is used as a default argument value of
    // the *enclosing* class's own constructor.
    Limits()
        : maxEncodedBytes(20LL * 1024 * 1024), maxDimensionPixels(8192),
          maxTotalPixels(32'000'000) {}
  };

  // Review item 7: the sanity ceiling `maxEncodedBytes` (and every
  // derived "+1 over-read" computation in handleReadyRead()/
  // handleFinished()) must stay validated below, so that arithmetic on it
  // can never overflow qint64 and the value always fits comfortably
  // within QByteArray's/qsizetype's real capacity on every platform this
  // project targets (32-bit qsizetype builds included). 1 GiB is already
  // far larger than any plausible single card-art asset.
  static constexpr qint64 kMaxAllowedEncodedBytes = 1LL * 1024 * 1024 * 1024;
  // A configured dimension/pixel cap above this is nonsensical for card
  // art and would itself risk overflow in downstream 32-bit-sized pixel
  // buffers (QImage's own row-stride arithmetic); bounded here rather
  // than trusted from configuration.
  static constexpr int kMaxAllowedDimensionPixels = 65536;
  static constexpr qint64 kMaxAllowedTotalPixels = 4'000'000'000LL;
  // QTimer ultimately stores its interval as a plain `int` millisecond
  // count; bounding the configured timeout here (well under INT_MAX,
  // ~24.8 days) keeps `timer->start(m_timeout)` overflow-free on every
  // platform without ever silently truncating a caller's requested value.
  static constexpr std::chrono::milliseconds kMaxAllowedTimeout{24LL * 60 * 60 *
                                                                1000};

  struct FetchHandle {
    quint64 id{0};
    [[nodiscard]] bool isValid() const noexcept { return id != 0; }
  };

  // Optional conditional-request headers (ETag / Last-Modified) from a
  // previously cached response. When either is non-empty, a 304 response
  // is accepted as success-with-no-body (ConditionalFetchResult::notModified
  // == true). When BOTH are empty (an unconditional request), a 304 is
  // never valid and is reported as
  // AssetErrorCode::ConditionalWithoutCachedBody: this client never
  // silently treats a bodyless response as a successful image.
  struct ConditionalHeaders {
    QString etag;
    QString lastModified;

    [[nodiscard]] bool isEmpty() const {
      return etag.isEmpty() && lastModified.isEmpty();
    }
  };

  struct FetchedAsset {
    QByteArray encodedBytes;
    QString contentType; // normalised, e.g. "image/png"
    QSize dimensions;
    QImage decodedImage;
    QString sha256Hex;
    QString etag;         // may be empty
    QString lastModified; // raw header text, may be empty
    int httpStatus{0};
  };

  struct ConditionalFetchResult {
    bool notModified{false};
    std::optional<FetchedAsset> asset; // present iff !notModified
    // A 304 response MAY carry refreshed validators even though it has
    // no body (RFC 7232 S4.1): the server can extend/rotate an ETag or
    // Last-Modified value at revalidation time without re-sending the
    // representation. Populated only when notModified == true and the
    // corresponding header was actually present; empty otherwise (never
    // populated for a fresh 200, whose validators live on `asset`).
    QString refreshedEtag;
    QString refreshedLastModified;
  };

  using FetchCallback =
      std::function<void(AssetOutcome<ConditionalFetchResult>)>;

  static constexpr std::chrono::seconds kDefaultTimeout{30};

  // Review item 7: preferred construction path for any real (non-test)
  // caller -- validates `limits`/`timeout` BEFORE any QObject/
  // QNetworkAccessManager is ever created, returning a typed
  // AssetErrorCode::InvalidConfiguration instead of throwing. Composition
  // code that wires this fetcher into the running application (outside
  // this PR's scope; see the class comment) must surface this typed
  // error rather than let an exception propagate out of startup.
  [[nodiscard]] static AssetOutcome<std::unique_ptr<AssetNetworkFetcher>>
  create(Limits limits = {},
         std::chrono::milliseconds timeout = kDefaultTimeout,
         QObject *parent = nullptr);
  // TEST-ONLY factory: borrows an externally-owned, isolated
  // QNetworkAccessManager (e.g. one pointed at a loopback test server).
  // See the constructor of the same signature below for why production/
  // composition code must never call this.
  [[nodiscard]] static AssetOutcome<std::unique_ptr<AssetNetworkFetcher>>
  create(QNetworkAccessManager &nam, Limits limits = {},
         std::chrono::milliseconds timeout = kDefaultTimeout,
         QObject *parent = nullptr);

  // Production constructor: owns a dedicated QNetworkAccessManager.
  //
  // Review item 7: an invalid `limits`/`timeout` no longer throws.
  // Instead, this object is still fully constructed but enters a
  // permanently-failed configuration state (see isValid()/
  // configurationError()): every fetch() call on it completes
  // asynchronously with AssetErrorCode::InvalidConfiguration, and no
  // QNetworkAccessManager request is ever issued. Prefer create() over
  // this constructor directly wherever the caller can act on a typed
  // error before ever calling fetch() at all; this constructor exists
  // (fail-closed rather than throwing) so existing call sites that
  // always pass valid, statically-known-good configuration are never
  // forced to handle a factory result they know can never be an error.
  explicit AssetNetworkFetcher(
      Limits limits = {}, std::chrono::milliseconds timeout = kDefaultTimeout,
      QObject *parent = nullptr);
  // TEST-ONLY constructor: borrows an externally-owned, isolated
  // QNetworkAccessManager (e.g. one pointed at a loopback test server).
  // See the production constructor's comment above for the same
  // fail-closed-not-throwing configuration-validation behaviour.
  //
  // Review round-4 item 1: production/composition code must always use
  // the owned-manager constructor (or create()) above, never this one.
  // The borrowed manager is not this class's own object: this
  // constructor still forces QNetworkProxy::NoProxy on it once here (see
  // the .cpp), but fetch() ALSO re-asserts NoProxy immediately before
  // every individual request precisely because a caller retaining its
  // own live reference to the same borrowed manager could otherwise
  // reconfigure its proxy at any later point, silently reintroducing
  // exactly the credential/proxy leak this class exists to prevent. This
  // constructor exists purely so tests can point a fetcher at an
  // in-process loopback MockHttpServer without needing a real, separate
  // QNetworkAccessManager per test case; it must never be reachable from
  // any real application wiring.
  explicit AssetNetworkFetcher(
      QNetworkAccessManager &nam, Limits limits = {},
      std::chrono::milliseconds timeout = kDefaultTimeout,
      QObject *parent = nullptr);
  ~AssetNetworkFetcher() override;

  [[nodiscard]] const Limits &limits() const { return m_limits; }

  // Review item 7: true iff the configuration passed to the constructor
  // was valid. False on a fetcher constructed with invalid
  // limits/timeout: every fetch() call still completes asynchronously
  // (never synchronously), always with AssetErrorCode::InvalidConfiguration,
  // and never touches QNetworkAccessManager.
  [[nodiscard]] bool isValid() const noexcept {
    return !m_configurationError.has_value();
  }
  // Precondition: !isValid(). The exact typed reason construction failed.
  [[nodiscard]] const AssetError &configurationError() const {
    return *m_configurationError;
  }

  // Issues a GET for `fetchUrl.url()`, validating the response against
  // `expectedFormat` as described in the class comment. `conditional` may
  // be empty for a plain unconditional fetch. While this fetcher is
  // alive, `callback` is invoked exactly once, asynchronously (never
  // synchronously from within this call), with either the fetched result
  // or a typed error. If this AssetNetworkFetcher is destroyed while the
  // request is still pending, delivery is suppressed entirely (see the
  // destructor) -- callers must not depend on `callback` firing once
  // destruction is possible.
  //
  // Review round-4 item 1: `fetchUrl` is an AssetFetchUrl, not a bare
  // QUrl -- see that class's comment above. This is the structural
  // enforcement point: there is no overload of fetch() anywhere that
  // accepts an unvalidated QUrl, so an arbitrary or forged URL can never
  // reach this method's QNetworkAccessManager at all, regardless of
  // caller. A defence-in-depth scheme re-check still runs inside fetch()
  // itself (see the .cpp) purely as a belt-and-braces safeguard against
  // a hypothetical future bug in AssetFetchUrl::validate(); it can never
  // actually trigger given AssetFetchUrl's construction invariant today.
  //
  // The returned handle is invalid (FetchHandle::isValid() == false) in
  // that defence-in-depth case: that request is rejected before any
  // network operation begins, so there is nothing for cancel() to
  // intercept, and `callback` will still fire with
  // AssetErrorCode::UnsupportedScheme regardless of whether cancel() is
  // ever called on the returned handle. For every other request, the
  // returned handle is valid and eligible for cancel().
  FetchHandle fetch(const AssetFetchUrl &fetchUrl, AssetFormat expectedFormat,
                    ConditionalHeaders conditional, FetchCallback callback);

  // Cancels a specific in-flight fetch. An invalid, stale, or unknown
  // handle is a safe no-op that never invokes `callback` on cancel()'s
  // behalf (the request's real outcome, if any is still pending
  // delivery, is unaffected). For a handle that IS actually pending,
  // cancellation removes it from the pending set synchronously (so a
  // second cancel() on the same handle is immediately a no-op), but
  // `callback`'s Cancelled delivery itself is queued via the event loop
  // rather than invoked inline from this call. If this AssetNetworkFetcher
  // is destroyed before that queued delivery runs, it is suppressed
  // entirely, exactly like a still-pending fetch() callback would be.
  void cancel(FetchHandle handle);

  // Validates and decodes already-downloaded, already-trusted encoded
  // bytes against `expectedFormat`, applying the exact same magic-byte,
  // codec-support, and dimension/pixel-budget checks (steps 2-4 of the
  // class comment above) that a live network fetch uses -- WITHOUT the
  // Content-Type header check (step 1), since there is no live HTTP
  // response to check it against here. This lets a caller serving a
  // disk-cache hit whose CachedEntry never carried a decoded QImage (only
  // encodedBytes/metadata are ever persisted to disk; see
  // AssetCache::store()/lookupDisk()) decode it on demand through the
  // identical validated codec path a fresh fetch uses, rather than a
  // second, divergent decode implementation.
  [[nodiscard]] AssetOutcome<QImage>
  decodeAndValidate(const QByteArray &encodedBytes,
                    AssetFormat expectedFormat) const;

private:
  struct Pending {
    QNetworkReply *reply{nullptr};
    QTimer *timer{nullptr};
    AssetFormat expectedFormat{AssetFormat::Jpeg};
    bool conditionalRequested{false};
    QByteArray buffer;
    bool overflowed{false};
    FetchCallback callback;
  };

  AssetNetworkFetcher(std::unique_ptr<QNetworkAccessManager> ownedNam,
                      QNetworkAccessManager *borrowedNam, Limits limits,
                      std::chrono::milliseconds timeout, QObject *parent);

  // Review item 7: returns the typed configuration error for
  // `limits`/`timeout` if invalid, or std::nullopt if both are within the
  // bounds documented on kMaxAllowedEncodedBytes/kMaxAllowedDimensionPixels/
  // kMaxAllowedTotalPixels/kMaxAllowedTimeout above. A zero or negative
  // timeout is rejected outright -- it is never silently reinterpreted as
  // "disable the timeout".
  [[nodiscard]] static std::optional<AssetError>
  validateConfiguration(const Limits &limits,
                        std::chrono::milliseconds timeout);

  void completeWithError(quint64 handle, AssetError error);
  void handleReadyRead(quint64 handle);
  void handleFinished(quint64 handle);

  std::unique_ptr<QNetworkAccessManager> m_ownedNam;
  QNetworkAccessManager &m_nam;
  Limits m_limits;
  std::chrono::milliseconds m_timeout;
  quint64 m_nextHandle{1};
  QHash<quint64, Pending> m_pending;
  // Review item 7: set iff the constructor's limits/timeout failed
  // validateConfiguration(); see isValid()/configurationError() above.
  std::optional<AssetError> m_configurationError;
};

} // namespace Arkham
