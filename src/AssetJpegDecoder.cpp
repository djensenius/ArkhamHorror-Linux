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
#include <cstdlib>
#include <cstring>

namespace Arkham {

namespace {

// Cumulative-review finding (PR #18, exact head 4a47ea34): the previous
// version of this file kept `jpeg_decompress_struct cinfo` and
// `JpegErrorManager errorManager` (which itself held a QString) as plain
// automatic (stack) locals declared BEFORE the setjmp() checkpoint below,
// and both are mutated (via pointers to them threaded through libjpeg)
// AFTER that checkpoint -- exactly the shape [csetjmp.syn] describes as
// producing indeterminate values on the longjmp() recovery path: "values
// of objects of automatic storage duration that are local to the function
// containing the invocation of the corresponding setjmp macro that do not
// have volatile-qualified type ... and have been changed between the
// setjmp invocation and longjmp call are indeterminate". The previous
// code's own comment argued that taking cinfo/errorManager's address (to
// pass to libjpeg) was sufficient to force the compiler to keep them
// memory-resident and thereby sidestep this in practice -- true for every
// compiler actually observed, but the standard's text grants no such
// "address taken" exception, so this remained formally UB and a
// plausible target for future/aggressive optimizers (or a stricter
// sanitizer) to exploit.
//
// This is fixed at the root by moving every piece of per-decode mutable
// state (cinfo, the error manager, and the scanline buffer pointer) into
// ONE heap-allocated (not automatic-storage) block, reached for the rest
// of this function exclusively through a single `volatile`-qualified
// local pointer that is assigned its final value exactly once, before
// the setjmp() checkpoint is ever reached, and never reassigned
// afterward. [csetjmp.syn]'s indeterminate-value rule applies only to
// automatic-storage-duration OBJECTS local to the setjmp-containing
// function -- it says nothing about, and does not reach through, a
// pointer to dynamically-allocated (heap) memory: the heap block itself
// has no "automatic storage duration" for the rule to apply to, and the
// one local pointer used to reach it is additionally volatile-qualified
// (matching this project's own pre-existing idiom for exactly this
// hazard, previously applied only to the scanline buffer) so its own
// value is unambiguously well-defined too. Because this struct is a
// plain aggregate of C types (a C struct, a C array, and a raw pointer --
// no QString, no other type with a non-trivial constructor/destructor),
// it is trivially constructible: a single std::malloc() + std::memset()
// fully initializes it with no placement-new and nothing for a longjmp
// to skip past. A JpegErrorManager pointer is threaded through libjpeg's
// own cinfo.client_data -- a field libjpeg defines and documents
// specifically for carrying arbitrary per-call caller state, safe to
// read back with a plain static_cast regardless of this struct's own
// layout, deliberately never relying on a "pub is the first member"
// base-pointer reinterpret_cast trick instead.
struct JpegErrorManager {
  jpeg_error_mgr pub;
  jmp_buf setjmpBuffer;
  // A fixed-size C buffer, not QString: format_message() writes directly
  // into it from inside error_exit(), which runs on the very edge of the
  // longjmp() boundary -- keeping this a trivial POD field (rather than a
  // type with allocation/copy machinery of its own) means there is
  // nothing here that could itself throw, allocate, or otherwise
  // misbehave while still "inside" the region a longjmp() might unwind
  // out of. It is converted to a QString only after decodeJpegImage() has
  // fully left the setjmp-protected region below.
  char fatalMessage[JMSG_LENGTH_MAX];
};

// All per-decode mutable state, heap-allocated as a single block and
// reached via one unchanged, volatile-qualified pointer -- see the
// JpegErrorManager comment above for the full rationale.
struct JpegDecodeState {
  jpeg_decompress_struct cinfo;
  JpegErrorManager errorManager;
  unsigned char *scanlineBuffer;
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
  auto *errorManager = static_cast<JpegErrorManager *>(cinfo->client_data);
  (*cinfo->err->format_message)(cinfo, errorManager->fatalMessage);
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
  // Heap-allocate ALL mutable libjpeg/error state up front, entirely
  // before the setjmp() checkpoint below is ever reached, and access it
  // for the rest of this function only through this single local
  // pointer. It is `volatile`-qualified (matching this project's existing
  // idiom, previously applied to the scanline buffer alone) and assigned
  // its final, only value here -- never reassigned again anywhere else in
  // this function -- so its own value is well-defined after any
  // longjmp(), and the heap memory it points to is entirely outside
  // [csetjmp.syn]'s indeterminate-value rule regardless, since that rule
  // only ever applies to automatic-storage-duration objects. See the
  // JpegDecodeState/JpegErrorManager comments above for the full
  // rationale for this restructuring.
  JpegDecodeState *volatile state =
      static_cast<JpegDecodeState *>(std::malloc(sizeof(JpegDecodeState)));
  if (!state) {
    return AssetOutcome<QImage>(
        AssetError{AssetErrorCode::MalformedImage,
                   QStringLiteral("Failed to allocate JPEG decode state")});
  }
  // JpegDecodeState is a trivial aggregate of C types (jpeg_decompress_struct,
  // a jmp_buf array, a fixed char buffer, and a raw pointer) -- a plain
  // memset is a complete, valid initialization; no placement-new and no
  // constructor for a longjmp to ever skip past.
  std::memset(state, 0, sizeof(*state));

  state->cinfo.err = jpeg_std_error(&state->errorManager.pub);
  state->errorManager.pub.error_exit = jpegErrorExit;
  state->errorManager.pub.output_message = jpegOutputMessageNoop;
  state->cinfo.client_data = &state->errorManager;

  // Canonical libjpeg usage (see libjpeg's own documented example.c):
  // the setjmp() checkpoint must be established BEFORE
  // jpeg_create_decompress(), since even that call can invoke error_exit
  // (e.g. on a version-mismatch or allocation failure). Every `goto`-free
  // early return below this point on the fatal-error path funnels through
  // this single `if`.
  if (setjmp(state->errorManager.setjmpBuffer)) {
    // Re-derived from the single volatile pointer captured above -- see
    // its declaration comment for why this is well-defined. Everything
    // read/freed here lives in the heap block it points to, never in
    // this function's own automatic storage.
    JpegDecodeState *const failedState = state;
    char messageCopy[JMSG_LENGTH_MAX];
    std::memcpy(messageCopy, failedState->errorManager.fatalMessage,
                sizeof(messageCopy));
    std::free(failedState->scanlineBuffer);
    jpeg_destroy_decompress(&failedState->cinfo);
    std::free(failedState);
    // QString construction happens only here, strictly after this
    // function has permanently left the setjmp-protected region above --
    // there is no remaining code path that could possibly longjmp() back
    // into this function again.
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("libjpeg failed to decode the JPEG payload: %1")
            .arg(QString::fromLatin1(messageCopy))});
  }

  jpeg_create_decompress(&state->cinfo);
  jpeg_mem_src(
      &state->cinfo,
      reinterpret_cast<const unsigned char *>(encodedBytes.constData()),
      static_cast<unsigned long>(encodedBytes.size()));

  // jpeg_read_header() only parses the JPEG frame header (SOF) to learn
  // image_width/image_height -- no entropy-coded scan data has been
  // decoded yet, and no full-resolution pixel buffer has been allocated.
  // See the header comment: the dimension/pixel-budget checks below run
  // strictly before jpeg_start_decompress()/jpeg_read_scanlines() ever
  // decode a single MCU.
  jpeg_read_header(&state->cinfo, TRUE);

  if (state->cinfo.image_width == 0 || state->cinfo.image_height == 0) {
    jpeg_destroy_decompress(&state->cinfo);
    std::free(state);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("JPEG header declares a non-positive dimension")});
  }
  if (state->cinfo.image_width >
          static_cast<unsigned int>(maxDimensionPixels) ||
      state->cinfo.image_height >
          static_cast<unsigned int>(maxDimensionPixels)) {
    const unsigned int width = state->cinfo.image_width;
    const unsigned int height = state->cinfo.image_height;
    jpeg_destroy_decompress(&state->cinfo);
    std::free(state);
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
  const qint64 totalPixels = static_cast<qint64>(state->cinfo.image_width) *
                             static_cast<qint64>(state->cinfo.image_height);
  if (totalPixels > maxTotalPixels) {
    jpeg_destroy_decompress(&state->cinfo);
    std::free(state);
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
  state->cinfo.out_color_space = JCS_RGB;

  jpeg_start_decompress(&state->cinfo);

  const int outWidth = static_cast<int>(state->cinfo.output_width);
  const int outHeight = static_cast<int>(state->cinfo.output_height);
  // A tightly-packed (no padding) row stride: this raw buffer is never
  // handed to Qt directly, so it has none of QImage's own per-platform
  // scanline-alignment requirements -- those are applied once, below,
  // when copying into the real QImage after every libjpeg call that
  // could possibly longjmp() has already returned successfully.
  const size_t rowStride = static_cast<size_t>(outWidth) * 3;
  state->scanlineBuffer = static_cast<unsigned char *>(
      std::malloc(rowStride * static_cast<size_t>(outHeight)));
  if (!state->scanlineBuffer) {
    jpeg_destroy_decompress(&state->cinfo);
    std::free(state);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral(
            "Failed to allocate a scanline buffer for the decoded JPEG's "
            "dimensions")});
  }

  while (state->cinfo.output_scanline < state->cinfo.output_height) {
    JSAMPROW rowPointer[1] = {
        state->scanlineBuffer +
        static_cast<size_t>(state->cinfo.output_scanline) * rowStride};
    jpeg_read_scanlines(&state->cinfo, rowPointer, 1);
  }

  jpeg_finish_decompress(&state->cinfo);

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
  const long numWarnings = state->errorManager.pub.num_warnings;
  jpeg_destroy_decompress(&state->cinfo);

  // Every libjpeg call that could possibly reach error_exit() -> longjmp()
  // has now returned normally: no further code path in this function can
  // ever reach the setjmp() checkpoint above, so it is safe from here on
  // to free the heap state block (nothing further needs it) and to
  // construct a C++ object with a non-trivial destructor (QImage).
  unsigned char *const scanlineBuffer = state->scanlineBuffer;
  std::free(state);

  if (numWarnings > 0) {
    std::free(scanlineBuffer);
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("libjpeg recovered from %1 corrupt-data warning(s) "
                       "while decoding; partial/recovered decodes are "
                       "never accepted")
            .arg(numWarnings)});
  }

  QImage image(outWidth, outHeight, QImage::Format_RGB888);
  if (image.isNull()) {
    std::free(scanlineBuffer);
    return AssetOutcome<QImage>(
        AssetError{AssetErrorCode::MalformedImage,
                   QStringLiteral("Failed to allocate a QImage for the "
                                  "decoded JPEG's dimensions")});
  }
  // QImage::Format_RGB888 rows are NOT guaranteed to be tightly packed
  // (width * 3 bytes) -- QImage pads each scanline's stride for
  // alignment, so each row is copied individually via scanLine() rather
  // than assuming a single compact width*height*3 memcpy is valid.
  for (int row = 0; row < outHeight; ++row) {
    std::memcpy(image.scanLine(row),
                scanlineBuffer + static_cast<size_t>(row) * rowStride,
                rowStride);
  }
  std::free(scanlineBuffer);

  return AssetOutcome<QImage>(std::move(image));
}

} // namespace Arkham
