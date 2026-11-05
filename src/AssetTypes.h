#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QUrl>
#include <QtAssert>

#include <optional>
#include <utility>

namespace Arkham {

// Shared asset pipeline failures. There is intentionally no zero-valued
// success state: success is represented by AssetOutcome<T> carrying a value,
// and every failure carries one of these explicit non-zero codes. Unknown is a
// defensive default for uninitialised AssetError values, never a success.
enum class AssetErrorCode : int {
  Unknown = 1,
  InvalidAssetBaseUrl,
  InvalidAssetKey,
  InvalidFetchUrl,
  UnsupportedScheme,
  InsecureTransport,
  NetworkError,
  Timeout,
  TooManyRedirects,
  MissingRedirectLocation,
  RedirectRejected,
  ResponseTooLarge,
  NotFound,
  HttpError,
  NotModifiedWithoutCache,
  UnsupportedContentType,
  DecodeError,
};

struct AssetError {
  AssetErrorCode code{AssetErrorCode::Unknown};
  QString message;
  int httpStatus{0};
  QUrl url;
};

template <typename T> class AssetOutcome {
public:
  AssetOutcome(T value) : m_value(std::move(value)) {}          // NOLINT
  AssetOutcome(AssetError error) : m_error(std::move(error)) {} // NOLINT

  [[nodiscard]] bool has_value() const noexcept { return m_value.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] const T &operator*() const { return *m_value; }
  [[nodiscard]] T &operator*() { return *m_value; }
  [[nodiscard]] const T *operator->() const { return &*m_value; }
  [[nodiscard]] T *operator->() { return &*m_value; }
  [[nodiscard]] const AssetError &error() const {
    Q_ASSERT_X(!has_value(), "AssetOutcome::error",
               "error() is valid only on a failed asset outcome");
    return m_error;
  }

private:
  std::optional<T> m_value;
  AssetError m_error;
};

// Encoded still-image formats accepted by the native asset decoder. The
// format is selected only by sniffing the payload bytes; file extensions and
// caller labels are deliberately not part of this leaf component.
enum class AssetImageFormat {
  Png,
  Jpeg,
  Avif,
};

// Decode failures are explicit error states. There is intentionally no
// zero-valued "no error" enumerator: a successful decode is represented by
// AssetDecodeOutcome<T> carrying a value, never by an error code.
enum class AssetDecodeErrorCode : int {
  InvalidLimits = 1,
  EncodedBytesTooLarge,
  UnrecognizedFormat,
  DimensionTooLarge,
  PixelBudgetExceeded,
  ProgressiveScanLimitExceeded,
  UnsupportedCodec,
  MalformedImage,
};

struct AssetDecodeError {
  AssetDecodeErrorCode code{AssetDecodeErrorCode::MalformedImage};
  QString message;
};

template <typename T> class AssetDecodeOutcome {
public:
  AssetDecodeOutcome(T value) : m_value(std::move(value)) {} // NOLINT
  AssetDecodeOutcome(AssetDecodeError error)
      : m_error(std::move(error)) {} // NOLINT

  [[nodiscard]] bool has_value() const noexcept { return m_value.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] const T &operator*() const { return *m_value; }
  [[nodiscard]] T &operator*() { return *m_value; }
  [[nodiscard]] const T *operator->() const { return &*m_value; }
  [[nodiscard]] T *operator->() { return &*m_value; }
  [[nodiscard]] const AssetDecodeError &error() const {
    Q_ASSERT_X(!has_value(), "AssetDecodeOutcome::error",
               "error() is valid only on a failed decode outcome");
    return m_error;
  }

private:
  std::optional<T> m_value;
  AssetDecodeError m_error;
};

struct AssetDecodeLimits {
  qsizetype maxEncodedBytes = 16 * 1024 * 1024;
  int maxWidth = 8192;
  int maxHeight = 8192;
  qint64 maxPixelCount = 32LL * 1024LL * 1024LL;
  int maxProgressiveJpegScans = 100;
};

[[nodiscard]] AssetDecodeOutcome<AssetImageFormat>
detectAssetImageFormat(const QByteArray &encodedBytes);

// Normalizes a requested decode bound. An invalid or empty size (including a
// request that names only one dimension) means "decode at full size" and is
// returned as QSize().
[[nodiscard]] QSize normalizedAssetDecodeBound(const QSize &bound);

// The size decodeAssetImage() produces for a source of sourceSize under bound:
// the largest size that fits inside the bound while keeping the aspect ratio,
// never larger than the source (the decoder never upscales). A full-size
// bound (see normalizedAssetDecodeBound) returns sourceSize unchanged.
[[nodiscard]] QSize fittedAssetDecodeSize(const QSize &sourceSize,
                                          const QSize &bound);

// Decodes encodedBytes after enforcing limits against the declared full image
// dimensions. When bound is a non-empty size, the result fits inside bound,
// keeps the aspect ratio, and is never upscaled. PNG and AVIF are downscaled
// to exactly fittedAssetDecodeSize(fullSize, bound). JPEG first uses libjpeg's
// DCT scaling (1/2, 1/4, or 1/8) to the smallest output that still covers that
// target, then fits that output to bound. libjpeg rounds scaled sides up, so
// JPEG output can differ from fittedAssetDecodeSize(fullSize, bound) by about
// a pixel in one dimension (599x839 at 150x210 gives 150x210, not 149x210),
// but it never exceeds bound. When the target is more than half the source in
// either dimension, no DCT reduction covers it and JPEG still decodes at full
// resolution before the final downscale.
[[nodiscard]] AssetDecodeOutcome<QImage>
decodeAssetImage(const QByteArray &encodedBytes,
                 const AssetDecodeLimits &limits = AssetDecodeLimits{},
                 const QSize &bound = QSize());

} // namespace Arkham
