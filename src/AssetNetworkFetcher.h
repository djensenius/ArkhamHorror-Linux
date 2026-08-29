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
//   3. QImageReader::size() is inspected BEFORE any decode is attempted;
//      each dimension is capped at `limits.maxDimensionPixels` and
//      width*height (computed in 64-bit, never overflowing) is capped at
//      `limits.maxTotalPixels`, rejecting a dimension/pixel bomb before a
//      single pixel is decoded.
//   4. Only then is the image actually decoded. If the installed Qt build
//      has no plugin capable of decoding `expectedFormat` at all, this is
//      reported as the distinct AssetErrorCode::UnsupportedCodec rather
//      than the generic AssetErrorCode::MalformedImage used for a
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

  // Production constructor: owns a dedicated QNetworkAccessManager.
  explicit AssetNetworkFetcher(
      Limits limits = {}, std::chrono::milliseconds timeout = kDefaultTimeout,
      QObject *parent = nullptr);
  // Test constructor: borrows an externally-owned, isolated
  // QNetworkAccessManager (e.g. one pointed at a loopback test server).
  explicit AssetNetworkFetcher(
      QNetworkAccessManager &nam, Limits limits = {},
      std::chrono::milliseconds timeout = kDefaultTimeout,
      QObject *parent = nullptr);
  ~AssetNetworkFetcher() override;

  [[nodiscard]] const Limits &limits() const { return m_limits; }

  // Issues a GET for `url`, validating the response against `expectedFormat`
  // as described in the class comment. `conditional` may be empty for a
  // plain unconditional fetch. `callback` is always invoked exactly once,
  // asynchronously (never synchronously from within this call), with
  // either the fetched result or a typed error.
  //
  // The returned handle is invalid (FetchHandle::isValid() == false) when
  // `url`'s scheme is anything other than http/https: that request is
  // rejected before any network operation begins, so there is nothing
  // for cancel() to intercept, and `callback` will still fire with
  // AssetErrorCode::UnsupportedScheme regardless of whether cancel() is
  // ever called on the returned handle. For every other request, the
  // returned handle is valid and eligible for cancel().
  FetchHandle fetch(const QUrl &url, AssetFormat expectedFormat,
                    ConditionalHeaders conditional, FetchCallback callback);

  // Cancels a specific in-flight fetch. An invalid, stale, or unknown
  // handle is a safe no-op that never invokes `callback` on cancel()'s
  // behalf (the request's real outcome, if any is still pending
  // delivery, is unaffected). For a handle that IS actually pending,
  // cancellation is synchronous and guaranteed: the callback is invoked
  // exactly once with AssetErrorCode::Cancelled, and it is never invoked
  // again afterwards for this handle.
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

  void completeWithError(quint64 handle, AssetError error);
  void handleReadyRead(quint64 handle);
  void handleFinished(quint64 handle);

  std::unique_ptr<QNetworkAccessManager> m_ownedNam;
  QNetworkAccessManager &m_nam;
  Limits m_limits;
  std::chrono::milliseconds m_timeout;
  quint64 m_nextHandle{1};
  QHash<quint64, Pending> m_pending;
};

} // namespace Arkham
