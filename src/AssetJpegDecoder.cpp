#include "AssetJpegDecoder.h"

// libjpeg-turbo's public headers are C++-safe (they wrap their contents in
// `extern "C" { ... }` themselves when __cplusplus is defined), so no
// manual extern "C" wrapper is needed here -- unlike some other C image
// libraries. <cstdio> must be included first: jpeglib.h's public API still
// references `FILE *` in a couple of optional (unused here) entry points
// and jconfig.h's platform detection historically expects <stdio.h> to
// already be visible.
#include <cstdio>

#include <jpeglib.h>

#include <csetjmp>
#include <cstring>

namespace Arkham {

namespace {

// Every piece of per-decode state lives here, as a plain stack local
// (never static, never shared) -- see AssetJpegDecoder.h's header comment
// for why this is what actually fixes the concurrency bug the previous
// ScopedJpegDecodeWarningDetector had. A pointer to this struct is threaded
// through libjpeg's own cinfo.client_data / cinfo.err (a
// JpegErrorManager::pub base, so libjpeg's C callbacks can safely
// reinterpret_cast the jpeg_error_mgr* they receive back to this struct
// via a plain base-pointer cast -- pub is required to be the FIRST member,
// exactly mirroring libjpeg's own documented "subclassing" idiom for
// jpeg_error_mgr).
struct JpegErrorManager {
  jpeg_error_mgr pub;
  jmp_buf setjmpBuffer;
  QString fatalMessage;
};

// error_exit is libjpeg's designated hard-failure hook: it is called
// instead of the library ever returning normally when a condition it
// cannot proceed past occurs (e.g. "not a JPEG file", a structurally
// invalid marker segment, or an internal allocation failure). This
// override never touches any Qt API, any global/static state, or any
// other decode's state -- it reads/writes only through the JpegErrorManager
// this specific cinfo was configured with, then non-locally unwinds back
// to this same call's own setjmp() checkpoint via longjmp(). Per libjpeg's
// own documented contract, error_exit must not return normally, so
// reaching the end of this function without longjmp()ing would itself be
// a bug -- it always does.
void jpegErrorExit(j_common_ptr cinfo) {
  auto *errorManager = reinterpret_cast<JpegErrorManager *>(cinfo->err);
  char buffer[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message)(cinfo, buffer);
  errorManager->fatalMessage = QString::fromLatin1(buffer);
  longjmp(errorManager->setjmpBuffer, 1);
}

// Suppresses libjpeg's default behavior of printing every
// warning/trace message to stderr via fprintf(). The standard
// jpeg_error_mgr::emit_message() implementation (left untouched here --
// see the header comment for why this project intentionally does NOT
// override it) already increments cinfo->err->num_warnings for every
// corrupt-data warning regardless of whether output_message() is invoked,
// so decodeJpegImage() below can read that authoritative, libjpeg-owned
// counter directly after decode instead of needing its own duplicate
// warning-counting hook -- and this override just keeps routine (or
// hostile, repeated-corruption-fuzzing) decode failures from spamming this
// process's stderr, matching the previous implementation's intent of
// observing-without-printing.
void jpegOutputMessageNoop(j_common_ptr) {}

} // namespace

AssetOutcome<QImage> decodeJpegImage(const QByteArray &encodedBytes,
                                     int maxDimensionPixels,
                                     qint64 maxTotalPixels) {
  jpeg_decompress_struct cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));

  JpegErrorManager errorManager;
  cinfo.err = jpeg_std_error(&errorManager.pub);
  errorManager.pub.error_exit = jpegErrorExit;
  errorManager.pub.output_message = jpegOutputMessageNoop;

  // Canonical libjpeg usage (see libjpeg's own documented example.c):
  // the setjmp() checkpoint must be established BEFORE
  // jpeg_create_decompress(), since even that call can invoke error_exit
  // (e.g. on a version-mismatch or allocation failure). Every `goto`-free
  // early return below this point on the fatal-error path funnels through
  // this single `if`.
  if (setjmp(errorManager.setjmpBuffer)) {
    const QString message = errorManager.fatalMessage;
    jpeg_destroy_decompress(&cinfo);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("libjpeg failed to decode the JPEG payload: %1")
            .arg(message)});
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(
      &cinfo, reinterpret_cast<const unsigned char *>(encodedBytes.constData()),
      static_cast<unsigned long>(encodedBytes.size()));

  // jpeg_read_header() only parses the JPEG frame header (SOF) to learn
  // image_width/image_height -- no entropy-coded scan data has been
  // decoded yet, and no full-resolution pixel buffer has been allocated.
  // See the header comment: the dimension/pixel-budget checks below run
  // strictly before jpeg_start_decompress()/jpeg_read_scanlines() ever
  // decode a single MCU.
  jpeg_read_header(&cinfo, TRUE);

  if (cinfo.image_width == 0 || cinfo.image_height == 0) {
    jpeg_destroy_decompress(&cinfo);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("JPEG header declares a non-positive dimension")});
  }
  if (cinfo.image_width > static_cast<unsigned int>(maxDimensionPixels) ||
      cinfo.image_height > static_cast<unsigned int>(maxDimensionPixels)) {
    const unsigned int width = cinfo.image_width;
    const unsigned int height = cinfo.image_height;
    jpeg_destroy_decompress(&cinfo);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::DimensionTooLarge,
        QStringLiteral("JPEG dimension %1x%2 exceeds the configured cap of "
                       "%3 pixels per side")
            .arg(width)
            .arg(height)
            .arg(maxDimensionPixels)});
  }
  // Overflow-safe: both operands are already bounded by maxDimensionPixels
  // above (a configured, small int -- default 8192), so their product
  // fits comfortably in the qint64 accumulator with enormous headroom
  // before any 64-bit overflow could occur, exactly mirroring
  // decodeAvifImage()'s equivalent check.
  const qint64 totalPixels = static_cast<qint64>(cinfo.image_width) *
                             static_cast<qint64>(cinfo.image_height);
  if (totalPixels > maxTotalPixels) {
    jpeg_destroy_decompress(&cinfo);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::PixelBudgetExceeded,
        QStringLiteral("JPEG pixel count %1 exceeds the configured budget "
                       "of %2 pixels")
            .arg(totalPixels)
            .arg(maxTotalPixels)});
  }

  // Force a single, consistent output color space regardless of the
  // source JPEG's own (grayscale/YCbCr/CMYK/YCCK) component layout, so
  // this function always produces a plain 3-byte-per-pixel RGB QImage.
  // An input libjpeg genuinely cannot convert to RGB (e.g. certain Adobe
  // CMYK/YCCK variants) fails via jpeg_start_decompress()'s own
  // error_exit -> longjmp() -> the MalformedImage path above, which is a
  // fail-closed (never silently-wrong-color) outcome, not a security gap.
  cinfo.out_color_space = JCS_RGB;

  jpeg_start_decompress(&cinfo);

  QImage image(static_cast<int>(cinfo.output_width),
               static_cast<int>(cinfo.output_height), QImage::Format_RGB888);
  if (image.isNull()) {
    jpeg_destroy_decompress(&cinfo);
    return AssetOutcome<QImage>(
        AssetError{AssetErrorCode::MalformedImage,
                   QStringLiteral("Failed to allocate a QImage for the "
                                  "decoded JPEG's dimensions")});
  }

  // QImage::Format_RGB888 rows are NOT guaranteed to be tightly packed
  // (width * 3 bytes) -- QImage pads each scanline's stride for
  // alignment. image.scanLine(row) always returns the correct
  // per-row destination regardless of that padding, so decoding directly
  // into it row-by-row (rather than assuming a compact width*height*3
  // buffer) is required for correctness on every platform/Qt build.
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW rowPointer[1] = {image.scanLine(cinfo.output_scanline)};
    jpeg_read_scanlines(&cinfo, rowPointer, 1);
  }

  jpeg_finish_decompress(&cinfo);

  // See jpegOutputMessageNoop()'s comment: num_warnings is incremented by
  // libjpeg's own (untouched) default emit_message() implementation for
  // EVERY corrupt-data warning, regardless of whether output_message() was
  // actually invoked to print it -- reading it here directly from
  // libjpeg's own authoritative per-call error manager (never a
  // process-global) is what replaces the previous
  // ScopedJpegDecodeWarningDetector's Qt-message-handler-based proxy for
  // exactly the same "did libjpeg have to recover from corrupt/incomplete
  // entropy-coded data" question -- this project never blesses a silently
  // recovered partial decode as success.
  const long numWarnings = errorManager.pub.num_warnings;
  jpeg_destroy_decompress(&cinfo);

  if (numWarnings > 0) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("libjpeg recovered from %1 corrupt-data warning(s) "
                       "while decoding; partial/recovered decodes are "
                       "never accepted")
            .arg(numWarnings)});
  }

  return AssetOutcome<QImage>(std::move(image));
}

} // namespace Arkham
