#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

#include <optional>
#include <utility>

namespace Arkham {

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
  [[nodiscard]] const AssetDecodeError &error() const { return m_error; }

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

[[nodiscard]] AssetDecodeOutcome<QImage>
decodeAssetImage(const QByteArray &encodedBytes,
                 const AssetDecodeLimits &limits = AssetDecodeLimits{});

} // namespace Arkham
