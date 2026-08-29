#pragma once

#include "AssetTypes.h"

#include <QByteArray>
#include <QImage>

// Cumulative-review finding (PR #18, exact head 14cf8de6): the previous
// JPEG decode path went through QImageReader (Qt's qjpeg plugin), with a
// separate ScopedJpegDecodeWarningDetector installing a PROCESS-GLOBAL
// Qt message handler (qInstallMessageHandler()) for the duration of each
// decode to observe whether libjpeg needed to log a corrupt-data warning
// under the qjpeg plugin's "qt.gui.imageio.jpeg" logging category. That
// mechanism has exactly one, shared, static set of state
// (s_previousHandler/s_active/s_sawJpegPluginMessage) for the ENTIRE
// process: two overlapping ScopedJpegDecodeWarningDetector lifetimes
// (concurrent decodes on different threads, or any future reentrant call
// path) corrupt each other's state and can install the handler as its own
// "previous" handler, so restoring it on destruction reinstalls itself --
// an infinite-recursion self-call the next time ANY message is logged,
// which overflows the stack. There is also no requirement that Qt's
// logging category infrastructure is even reentrant/thread-safe in the
// first place for concurrent installs.
//
// This module fixes that at the root by never touching Qt's plugin
// registry or its global logging state for JPEG at all: it decodes JPEG
// directly against libjpeg's own public C API, exactly mirroring
// AssetAvifDecoder.h/.cpp's existing pattern for AVIF. Every piece of
// per-decode state (the custom jpeg_error_mgr, its setjmp buffer, and its
// warning/error flags) lives in a plain local, stack-allocated struct
// passed via cinfo.client_data -- nothing is ever static or shared across
// calls, so concurrent/reentrant decodes on different jpeg_decompress_struct
// instances (even on different threads) are completely independent of one
// another by construction, with no lock and no shared global required.
namespace Arkham {

// Decodes a complete, in-memory JPEG payload (`encodedBytes`, already
// magic-byte-sniffed, Content-Type-checked, and marker-structure/EOI-
// checked by the caller -- see AssetNetworkFetcher::decodeAndValidate())
// into a QImage.
//
// Dimension/pixel-budget limits are enforced immediately after
// jpeg_read_header() (which only parses the JPEG frame header -- SOF -- to
// learn image_width/image_height; no entropy-coded scan data has been
// decoded and no full-resolution pixel buffer has been allocated yet) and
// strictly BEFORE jpeg_start_decompress()/jpeg_read_scanlines() ever
// decode a single MCU, mirroring decodeAvifImage()'s equivalent ordering.
//
// If libjpeg's decoder needed to recover from corrupt or incomplete
// entropy-coded data (recorded via the standard IJG `num_warnings`
// counter on this call's own, non-shared jpeg_error_mgr -- exactly the
// same condition the previous Qt-category-based detector aimed to
// observe, but read directly from the authoritative source instead of
// proxied through Qt's global logging), this is reported as
// AssetErrorCode::MalformedImage regardless of whether libjpeg otherwise
// completed the decode: this project never blesses silently-recovered
// partial image data as a successful result.
[[nodiscard]] AssetOutcome<QImage>
decodeJpegImage(const QByteArray &encodedBytes, int maxDimensionPixels,
                qint64 maxTotalPixels);

} // namespace Arkham
