#include "AssetJpegDecoder.h"

#include "AssetTypes.h"

#include <cstdio>

#include <jpeglib.h>

#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace Arkham {

namespace {

struct JpegErrorManager {
  jpeg_error_mgr pub;
  jmp_buf setjmpBuffer;
  char fatalMessage[JMSG_LENGTH_MAX];
};

struct JpegProgressManager {
  jpeg_progress_mgr pub;
  int maxProgressiveScans;
};

struct JpegDecodeState {
  jpeg_decompress_struct cinfo;
  JpegErrorManager errorManager;
  JpegProgressManager progressManager;
  unsigned char *scanlineBuffer;
  bool progressiveScanLimitExceeded;
};

void jpegErrorExit(j_common_ptr cinfo) {
  auto *state = static_cast<JpegDecodeState *>(cinfo->client_data);
  (*cinfo->err->format_message)(cinfo, state->errorManager.fatalMessage);
  longjmp(state->errorManager.setjmpBuffer, 1);
}

void jpegOutputMessageNoop(j_common_ptr) {}

void jpegProgressMonitor(j_common_ptr cinfo) {
  auto *state = static_cast<JpegDecodeState *>(cinfo->client_data);
  const auto *decompress = reinterpret_cast<j_decompress_ptr>(cinfo);
  if (decompress->input_scan_number >
      state->progressManager.maxProgressiveScans) {
    state->progressiveScanLimitExceeded = true;
    longjmp(state->errorManager.setjmpBuffer, 1);
  }
}

AssetDecodeOutcome<qint64>
validateJpegDimensions(unsigned int width, unsigned int height,
                       const AssetDecodeLimits &limits) {
  if (width == 0 || height == 0) {
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("JPEG header declares a non-positive dimension")};
  }
  if (width > static_cast<unsigned int>(limits.maxWidth) ||
      height > static_cast<unsigned int>(limits.maxHeight)) {
    return AssetDecodeError{
        AssetDecodeErrorCode::DimensionTooLarge,
        QStringLiteral(
            "JPEG dimension %1x%2 exceeds the configured cap of %3x%4")
            .arg(width)
            .arg(height)
            .arg(limits.maxWidth)
            .arg(limits.maxHeight),
    };
  }
  const qint64 totalPixels =
      static_cast<qint64>(width) * static_cast<qint64>(height);
  if (totalPixels > limits.maxPixelCount) {
    return AssetDecodeError{
        AssetDecodeErrorCode::PixelBudgetExceeded,
        QStringLiteral(
            "JPEG totals %1 pixels, exceeding the configured cap of %2")
            .arg(totalPixels)
            .arg(limits.maxPixelCount),
    };
  }
  return totalPixels;
}

} // namespace

AssetDecodeOutcome<QImage> decodeJpegImage(const QByteArray &encodedBytes,
                                           const AssetDecodeLimits &limits) {
  if (encodedBytes.size() < 0 ||
      static_cast<quint64>(encodedBytes.size()) >
          static_cast<quint64>((std::numeric_limits<unsigned long>::max)())) {
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("JPEG payload is too large for libjpeg")};
  }

  JpegDecodeState *volatile state =
      static_cast<JpegDecodeState *>(std::malloc(sizeof(JpegDecodeState)));
  if (!state) {
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("failed to allocate JPEG decoder state")};
  }
  std::memset(state, 0, sizeof(*state));

  state->cinfo.err = jpeg_std_error(&state->errorManager.pub);
  state->errorManager.pub.error_exit = jpegErrorExit;
  state->errorManager.pub.output_message = jpegOutputMessageNoop;
  state->progressManager.maxProgressiveScans = limits.maxProgressiveJpegScans;
  state->progressManager.pub.progress_monitor = jpegProgressMonitor;
  state->cinfo.client_data = state;
  state->cinfo.progress = &state->progressManager.pub;

  if (setjmp(state->errorManager.setjmpBuffer)) {
    JpegDecodeState *const failedState = state;
    char messageCopy[JMSG_LENGTH_MAX];
    std::memcpy(messageCopy, failedState->errorManager.fatalMessage,
                sizeof(messageCopy));
    const bool scanLimitExceeded = failedState->progressiveScanLimitExceeded;
    const int maxScans = failedState->progressManager.maxProgressiveScans;
    std::free(failedState->scanlineBuffer);
    jpeg_destroy_decompress(&failedState->cinfo);
    std::free(failedState);
    if (scanLimitExceeded) {
      return AssetDecodeError{
          AssetDecodeErrorCode::ProgressiveScanLimitExceeded,
          QStringLiteral(
              "progressive JPEG exceeded the configured cap of %1 scans")
              .arg(maxScans),
      };
    }
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("libjpeg failed to decode the JPEG payload: %1")
            .arg(QString::fromLatin1(messageCopy)),
    };
  }

  jpeg_create_decompress(&state->cinfo);
  state->cinfo.client_data = state;
  state->cinfo.progress = &state->progressManager.pub;
  jpeg_mem_src(
      &state->cinfo,
      reinterpret_cast<const unsigned char *>(encodedBytes.constData()),
      static_cast<unsigned long>(encodedBytes.size()));

  if (jpeg_read_header(&state->cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&state->cinfo);
    std::free(state);
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("libjpeg did not find a JPEG image header")};
  }

  {
    const auto dimensionValidation = validateJpegDimensions(
        state->cinfo.image_width, state->cinfo.image_height, limits);
    if (!dimensionValidation) {
      const AssetDecodeError error = dimensionValidation.error();
      jpeg_destroy_decompress(&state->cinfo);
      std::free(state);
      return error;
    }
  }

  if (state->cinfo.jpeg_color_space == JCS_CMYK ||
      state->cinfo.jpeg_color_space == JCS_YCCK) {
    jpeg_destroy_decompress(&state->cinfo);
    std::free(state);
    return AssetDecodeError{
        AssetDecodeErrorCode::UnsupportedCodec,
        QStringLiteral("CMYK/YCCK JPEG assets are not supported")};
  }

  state->cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&state->cinfo);

  if (state->cinfo.output_components != 3) {
    jpeg_destroy_decompress(&state->cinfo);
    std::free(state);
    return AssetDecodeError{
        AssetDecodeErrorCode::UnsupportedCodec,
        QStringLiteral("libjpeg could not convert JPEG output to RGB")};
  }

  const int outWidth = static_cast<int>(state->cinfo.output_width);
  const int outHeight = static_cast<int>(state->cinfo.output_height);
  const size_t rowStride = static_cast<size_t>(outWidth) * 3U;
  const size_t bufferSize = rowStride * static_cast<size_t>(outHeight);
  state->scanlineBuffer = static_cast<unsigned char *>(std::malloc(bufferSize));
  if (!state->scanlineBuffer) {
    jpeg_destroy_decompress(&state->cinfo);
    std::free(state);
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("failed to allocate JPEG scanline buffer")};
  }

  while (state->cinfo.output_scanline < state->cinfo.output_height) {
    JSAMPROW rowPointer[1] = {
        state->scanlineBuffer +
        static_cast<size_t>(state->cinfo.output_scanline) * rowStride};
    jpeg_read_scanlines(&state->cinfo, rowPointer, 1);
  }

  jpeg_finish_decompress(&state->cinfo);
  const long warnings = state->errorManager.pub.num_warnings;
  jpeg_destroy_decompress(&state->cinfo);

  unsigned char *const scanlineBuffer = state->scanlineBuffer;
  std::free(state);

  if (warnings > 0) {
    std::free(scanlineBuffer);
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("libjpeg recovered from %1 corrupt-data warning(s)")
            .arg(warnings),
    };
  }

  QImage image(outWidth, outHeight, QImage::Format_RGB888);
  if (image.isNull()) {
    std::free(scanlineBuffer);
    return AssetDecodeError{
        AssetDecodeErrorCode::MalformedImage,
        QStringLiteral("failed to allocate decoded JPEG image")};
  }
  for (int row = 0; row < outHeight; ++row) {
    std::memcpy(image.scanLine(row),
                scanlineBuffer + static_cast<size_t>(row) * rowStride,
                rowStride);
  }
  std::free(scanlineBuffer);

  return image;
}

} // namespace Arkham
