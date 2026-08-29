#include "AssetNetworkFetcher.h"

#include "AssetAvifDecoder.h"
#include "AuthTransportSecurity.h"

#include <QAuthenticator>
#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtAssert>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

using namespace Qt::StringLiterals;

namespace Arkham {

AssetOutcome<AssetFetchUrl> AssetFetchUrl::validate(const QUrl &url) {
  // Checked first, and reported with the specific UnsupportedScheme code
  // (rather than folding it into the generic InvalidCandidateUrl below),
  // purely to preserve a precise, distinguishable error for this common
  // case -- exactly as AssetNetworkFetcher::fetch() itself used to report
  // it before this validation moved here.
  const QString scheme = url.scheme();
  if (scheme != "http"_L1 && scheme != "https"_L1) {
    return AssetError{
        AssetErrorCode::UnsupportedScheme,
        QStringLiteral("only http and https URLs may be fetched")};
  }
  // Reuses AuthTransportSecurity's already shared, already-tested
  // transport-safety predicate exactly (see isSecureOrLoopbackAuthTransport()'s
  // doc comment in AuthTransportSecurity.h) rather than forking a
  // second, asset-specific reimplementation of the same policy: https is
  // permitted to any host; http is permitted ONLY to an exact canonical
  // loopback spelling; any userinfo component or missing host is
  // rejected regardless of scheme.
  if (!isSecureOrLoopbackAuthTransport(url)) {
    return AssetError{
        AssetErrorCode::InvalidCandidateUrl,
        QStringLiteral("URL is not a valid https (any host) or http "
                       "(canonical-loopback-only) fetch target, or carries "
                       "credentials")};
  }
  // Neither a query string nor a fragment is ever legitimately part of a
  // resolved asset candidate URL -- matching
  // UrlValidator::validateCustomUrl()'s policy for the same reason (see
  // UrlValidator.cpp).
  if (url.hasQuery()) {
    return AssetError{
        AssetErrorCode::InvalidCandidateUrl,
        QStringLiteral("fetch URL must not contain a query string (?...)")};
  }
  if (!url.fragment().isEmpty()) {
    return AssetError{
        AssetErrorCode::InvalidCandidateUrl,
        QStringLiteral("fetch URL must not contain a fragment (#...)")};
  }
  return AssetFetchUrl(url);
}

namespace {

// Returns the AssetFormat whose magic bytes match `bytes`, or std::nullopt
// if none of the three supported formats' signatures match. This is an
// independent check from the declared Content-Type: both must agree with
// the caller's expected format for a fetch to succeed.
std::optional<AssetFormat> sniffMagicBytes(const QByteArray &bytes) {
  static const QByteArray kPngSignature =
      QByteArrayLiteral("\x89PNG\r\n\x1a\n");
  if (bytes.startsWith(kPngSignature)) {
    return AssetFormat::Png;
  }
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
      static_cast<unsigned char>(bytes[1]) == 0xD8 &&
      static_cast<unsigned char>(bytes[2]) == 0xFF) {
    return AssetFormat::Jpeg;
  }
  // AVIF is ISOBMFF: a leading 4-byte big-endian box size, then "ftyp",
  // then a 4-byte major brand, then zero or more 4-byte compatible brands
  // filling out the rest of the box (whose total size is the leading
  // 4-byte field). Accept if the major brand OR any compatible brand is
  // "avif" (still image) or "avis" (image sequence).
  if (bytes.size() >= 12 && bytes.mid(4, 4) == QByteArrayLiteral("ftyp")) {
    const quint32 boxSize = (static_cast<unsigned char>(bytes[0]) << 24) |
                            (static_cast<unsigned char>(bytes[1]) << 16) |
                            (static_cast<unsigned char>(bytes[2]) << 8) |
                            static_cast<unsigned char>(bytes[3]);
    const qint64 available = bytes.size();
    // Per ISO/IEC 14496-12, a box size of 0 means "this box extends to
    // the end of the enclosing file/buffer" -- NOT a zero-length box.
    // Treating it as zero-length would silently reject a spec-valid
    // AVIF `ftyp` box as a magic-bytes mismatch.
    const qint64 boxEnd =
        boxSize == 0 ? available : qMin<qint64>(boxSize, available);
    // A ftyp box must declare at least 12 bytes (4-byte size + "ftyp" +
    // 4-byte major_brand) to even contain a major_brand field. A nonzero
    // declared size smaller than that is a malformed/truncated box: it
    // must never be treated as ftyp data, because reading major_brand at
    // a fixed offset of 8 regardless of boxEnd would read past the
    // declared (invalid) box boundary into whatever bytes happen to
    // follow -- potentially misclassifying non-AVIF data as AVIF.
    if (boxEnd >= 12) {
      // Layout: [0..4) box size, [4..8) "ftyp", [8..12) major_brand,
      // [12..16) minor_version (a version number, NOT a brand -- it must
      // never be compared against "avif"/"avis"), [16..boxEnd) zero or
      // more 4-byte compatible_brands. Checking major_brand and then
      // skipping straight to compatible_brands (offset 16) avoids
      // misclassifying a crafted minor_version as a brand match.
      //
      // Compare directly against bytes.constData() rather than slicing
      // with mid(): a crafted box can declare boxEnd up to the full
      // capped download size (maxEncodedBytes, 20 MiB), which would
      // otherwise drive up to ~5 million loop iterations, each
      // allocating (and immediately discarding) a 4-byte QByteArray --
      // an avoidable CPU/allocation cost inflicted by network-controlled
      // bytes.
      const char *const data = bytes.constData();
      const auto isBrandAt = [data](qint64 offset, const char *brand) {
        return std::memcmp(data + offset, brand, 4) == 0;
      };
      if (isBrandAt(8, "avif") || isBrandAt(8, "avis")) {
        return AssetFormat::Avif;
      }
      for (qint64 offset = 16; offset + 4 <= boxEnd; offset += 4) {
        if (isBrandAt(offset, "avif") || isBrandAt(offset, "avis")) {
          return AssetFormat::Avif;
        }
      }
    }
  }
  return std::nullopt;
}

// Review item 10: Qt's bundled libjpeg-based decoder tolerates a
// premature end-of-file within entropy-coded scan data by *synthesising*
// a missing End-Of-Image (EOI, the two-byte marker 0xFF 0xD9) marker: it
// logs a "premature end of data segment" warning but still returns a
// non-null, seemingly-valid QImage. A response body truncated by a
// flaky/adversarial network mid-transfer can therefore decode
// "successfully" through QImageReader despite genuinely missing its
// tail. This performs an independent, bounded, single-pass scan of the
// JPEG marker structure to determine whether a *genuine* EOI marker is
// actually present in the supplied bytes, distinct from (and run before
// trusting) Qt's own decode.
//
// Trailing-data policy (review item 6, documented strict policy): a
// genuine EOI marker must be found AND must be the last thing in the
// buffer -- any byte following it (padding, a second concatenated JPEG
// stream, or anything else) is rejected. This is deliberately stricter
// than "an EOI exists somewhere"; a concatenated/trailer-appended body
// is never treated as a single complete, trusted codestream.
//
// This does not replace normal magic-byte or QImageReader
// validation -- a body that fails this check is prevented from ever
// reaching the decoder at all, and a body that passes still goes
// through the existing dimension/limit/decode checks below.
bool jpegCodestreamHasGenuineEoi(const QByteArray &bytes) {
  const qint64 size = bytes.size();
  const auto *const data =
      reinterpret_cast<const unsigned char *>(bytes.constData());
  // Magic-byte sniffing has already confirmed bytes[0..2] == FF D8 FF,
  // so start scanning markers right after the two-byte SOI.
  if (size < 2) {
    return false;
  }
  qint64 pos = 2;
  while (pos < size) {
    if (data[pos] != 0xFF) {
      // A marker was expected here but the byte isn't 0xFF: the
      // structure itself is malformed. Leave that diagnosis to the
      // normal QImageReader decode path (MalformedImage) rather than
      // misreporting it as a codestream-completeness failure.
      return false;
    }
    // Marker codes may be preceded by any number of 0xFF fill bytes.
    while (pos < size && data[pos] == 0xFF) {
      ++pos;
    }
    if (pos >= size) {
      return false; // ran out of bytes while skipping fill bytes
    }
    const unsigned char marker = data[pos];
    ++pos;
    if (marker == 0xD9) {
      // Genuine EOI found: strict policy requires it to be the very
      // last byte of the supplied body (see the trailing-data policy
      // comment above).
      return pos == size;
    }
    if (marker == 0x00) {
      return false; // a stuffed byte can only appear inside scan data
    }
    if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
      continue; // RSTn / TEM: standalone, no length field or payload
    }
    if (marker == 0xD8) {
      return false; // unexpected second top-level SOI: malformed
    }
    // Every other marker (SOF*, DHT, DQT, DRI, APPn, COM, SOS, ...) is
    // followed by a 2-byte big-endian length, INCLUDING those 2 bytes.
    if (pos + 1 >= size) {
      return false;
    }
    const int segmentLength =
        (static_cast<int>(data[pos]) << 8) | static_cast<int>(data[pos + 1]);
    if (segmentLength < 2) {
      return false; // a length that can't even cover its own field
    }
    if (marker != 0xDA) {
      // Non-scan segment: skip its declared payload wholesale.
      pos += segmentLength;
      if (pos > size) {
        return false; // declared length runs past the available bytes
      }
      continue;
    }
    // SOS (Start Of Scan): skip its own header, then scan the
    // entropy-coded data that follows byte-by-byte. Within scan data, a
    // 0xFF byte only terminates the scan if the following byte is a
    // "real" marker code -- 0xFF 0x00 is a stuffed literal 0xFF data
    // byte, 0xFF 0xD0-0xD7 is a restart marker (both stay inside the
    // scan), and a run of 0xFF fill bytes just keeps scanning.
    pos += segmentLength;
    if (pos > size) {
      return false;
    }
    while (pos < size) {
      if (data[pos] != 0xFF) {
        ++pos;
        continue;
      }
      if (pos + 1 >= size) {
        return false; // truncated exactly on a trailing 0xFF byte
      }
      const unsigned char next = data[pos + 1];
      if (next == 0x00 || (next >= 0xD0 && next <= 0xD7)) {
        pos += 2; // stuffed byte or restart marker: still scan data
        continue;
      }
      if (next == 0xFF) {
        ++pos; // fill byte: keep looking at the byte after it
        continue;
      }
      break; // a genuine marker follows: hand control back to the
             // outer loop, which will interpret it (possibly another
             // SOS for a progressive JPEG, or the final EOI).
    }
  }
  return false; // exhausted the buffer without ever finding a genuine EOI
}

// Round-4 review item 8 (PR #18 cumulative review): the structural EOI
// scan above (jpegCodestreamHasGenuineEoi()) proves the marker STRUCTURE
// of the supplied bytes is well-formed and ends in a genuine EOI -- but it
// cannot detect a truncated/corrupt *entropy-coded scan* (the
// Huffman-coded DCT data between SOS and EOI), because verifying THAT
// requires actually running the entropy decoder, which only libjpeg
// itself does. Qt's bundled qjpeg plugin already runs libjpeg's real
// entropy decoder, and libjpeg has its own long-standing, well-defined
// behaviour for exactly this case: when the entropy-coded data ends
// prematurely, libjpeg's default source manager (qt_fill_input_buffer()
// in qjpeghandler.cpp) synthesises a fake EOI marker "as per jpeglib
// recommendation" and libjpeg emits a WARNING-class message ("Corrupt
// JPEG data: premature end of data segment" from jdmarker.c's
// WARNMS/JWRN_JPEG_EOF path) rather than a fatal error -- decode continues
// to completion with the remainder of the image filled in from whatever
// state the decoder was last in, and QImageReader::read() returns a
// non-null QImage with no error at all. This is exactly the "silent
// recovery" this review item requires this project to reject outright:
// an attacker-truncated entropy stream with a forged EOI appended must
// not decode "successfully".
//
// Rather than vendoring/linking libjpeg directly and reimplementing
// QImageReader's decode call, this hooks Qt's own already-integrated
// libjpeg pipeline at its existing, stable seam: every libjpeg
// warning/error message the qjpeg plugin emits is routed through the
// public, documented Qt logging category "qt.gui.imageio.jpeg"
// (Q_LOGGING_CATEGORY(lcJpeg, "qt.gui.imageio.jpeg") in qjpeghandler.cpp;
// both the fatal-error path or non-fatal WARNMS-class messages funnel
// through the same qCWarning(lcJpeg, ...) call). A scoped message handler
// installed only for the duration of a single JPEG reader.read() call
// observes whether ANY message was logged under that exact category --
// matched by category name, never by message text, so this does not
// depend on libjpeg's exact wording being stable across Qt/libjpeg
// versions -- and if so, the decode is treated as having required silent
// recovery and is rejected regardless of whether QImageReader itself
// returned a seemingly-valid, non-null QImage. This deliberately also
// rejects the qjpeg plugin's one other, unrelated qCWarning(lcJpeg, ...)
// call site (a malformed EXIF orientation tag): a well-formed CDN-served
// JPEG has no reason to carry a malformed EXIF orientation tag either, and
// this project's "do not bless partial recovery" policy is intentionally
// strict rather than trying to enumerate which specific warning classes
// are "safe" to ignore.
//
// Every other Qt message logged during the scoped window (from this
// category or any other) is forwarded, unmodified, to whatever handler
// was previously installed (or Qt's default handler if none was) --
// nothing is ever swallowed. A QMutex guards the small amount of shared
// state (previous handler pointer + "warning seen" flag) purely as a
// defensive measure against a future regression that decodes JPEGs from
// more than one thread; this project has no QThread usage today; see
// AssetNetworkFetcher.h's class-level threading note in the header.
class ScopedJpegDecodeWarningDetector {
public:
  ScopedJpegDecodeWarningDetector() {
    QMutexLocker locker(&s_mutex);
    s_previousHandler = qInstallMessageHandler(&forwardingHandler);
    s_active = true;
    s_sawJpegPluginMessage = false;
  }

  ~ScopedJpegDecodeWarningDetector() {
    QMutexLocker locker(&s_mutex);
    qInstallMessageHandler(s_previousHandler);
    s_active = false;
    s_previousHandler = nullptr;
  }

  ScopedJpegDecodeWarningDetector(const ScopedJpegDecodeWarningDetector &) =
      delete;
  ScopedJpegDecodeWarningDetector &
  operator=(const ScopedJpegDecodeWarningDetector &) = delete;

  [[nodiscard]] bool sawJpegPluginMessage() const {
    QMutexLocker locker(&s_mutex);
    return s_sawJpegPluginMessage;
  }

private:
  static void forwardingHandler(QtMsgType type,
                                const QMessageLogContext &context,
                                const QString &msg) {
    QtMessageHandler previous = nullptr;
    {
      QMutexLocker locker(&s_mutex);
      if (s_active && context.category &&
          std::strcmp(context.category, "qt.gui.imageio.jpeg") == 0) {
        s_sawJpegPluginMessage = true;
      }
      previous = s_previousHandler;
    }
    if (previous) {
      previous(type, context, msg);
    } else {
      // No prior custom handler was installed (Qt's own built-in default
      // handler was in effect): there is no public API to invoke that
      // default handler directly, so approximate its behaviour closely
      // enough that messages are never silently dropped just because
      // this scope happened to be the first handler ever installed by
      // this process. Critically, Qt's real default handler does not
      // merely print QtFatalMsg to stderr -- it terminates the process
      // (see qlogging.cpp's qDefaultMessageHandler(), which calls
      // std::abort() after printing a fatal message). Silently
      // continuing execution past a qFatal() call inside this scope
      // would be a genuine, dangerous change to Qt's documented logging
      // semantics, so the fatal case is replicated exactly here.
      fprintf(stderr, "%s: %s\n",
              context.category ? context.category : "default", qPrintable(msg));
      if (type == QtFatalMsg) {
        fflush(stderr);
        std::abort();
      }
    }
  }

  static inline QMutex s_mutex;
  static inline QtMessageHandler s_previousHandler = nullptr;
  static inline bool s_active = false;
  static inline bool s_sawJpegPluginMessage = false;
};

} // namespace

[[noreturn]] void
AssetNetworkFetcher::triggerJpegDecodeWarningDetectorFatalMessageForTesting() {
  ScopedJpegDecodeWarningDetector detector;
  qFatal("AssetNetworkFetcher test-only fatal message: proving "
         "ScopedJpegDecodeWarningDetector's no-previous-handler fallback "
         "terminates the process exactly like Qt's real default handler "
         "would");
}

namespace {

// Round-4 review item 8: an independent, deterministic CRC-32 (ISO 3309 /
// zlib / PNG-standard polynomial 0xEDB88320) implementation. Qt's own
// qChecksum() deliberately does NOT compute this -- despite its
// Qt::ChecksumIso3309 enumerator name, it returns a 16-bit CRC-16 result
// (quint16), never the 32-bit value the PNG spec requires for chunk
// integrity -- so this is implemented directly rather than mis-using that
// unrelated function.
quint32 pngCrc32(const unsigned char *data, qint64 length) {
  static const auto table = [] {
    std::array<quint32, 256> t{};
    for (quint32 n = 0; n < 256; ++n) {
      quint32 c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[n] = c;
    }
    return t;
  }();
  quint32 crc = 0xFFFFFFFFu;
  for (qint64 i = 0; i < length; ++i) {
    crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

// Round-4 review item 8: an independent, deterministic PNG structural
// validator that walks the ENTIRE chunk sequence of a PNG body (the
// signature has already been confirmed by sniffMagicBytes()) and rejects
// anything QImageReader's underlying libpng-based decoder might otherwise
// tolerate or silently ignore:
//   - any chunk whose declared length or position would run past the end
//     of the supplied buffer (bounds safety, overflow-free: lengths are
//     capped to the PNG spec's own 31-bit limit before use in arithmetic);
//   - any chunk whose stored CRC-32 does not match the actual bytes (a
//     corrupt/tampered chunk is rejected outright rather than silently
//     accepted by a lenient decoder);
//   - any chunk type whose four bytes are not all ASCII letters (the only
//     structurally valid PNG chunk-type alphabet);
//   - a first chunk that is not exactly IHDR with a 13-byte payload;
//   - more than one IHDR chunk (multi-image is never valid in a single
//     bare PNG stream);
//   - any of the APNG-defining chunk types (acTL/fcTL/fdAT) -- an animated
//     PNG is a "multiple image" in the same sense the review item's AVIF
//     imageCount!=1 case is: this project only ever wants exactly one
//     still frame, and QImageReader can plausibly decode just the base
//     IDAT frame of an APNG while silently ignoring its animation frames,
//     which is exactly the kind of accept-a-subset-of-the-payload
//     behaviour this review item requires rejecting;
//   - a missing IDAT chunk (no image data at all);
//   - an IEND chunk that is not the exact final chunk of the buffer (IEND
//     must have zero-length data, and its CRC's last byte must be the
//     very last byte of the entire supplied body -- any trailing bytes
//     after IEND, whether padding, a second concatenated PNG, or anything
//     else, are rejected).
//
// This runs strictly BEFORE QImageReader ever sees the bytes; a body that
// fails this check never reaches the decoder at all. It does not replace
// magic-byte sniffing or the dimension/pixel-budget checks below -- a
// body that passes this check still goes through those unchanged.
bool pngChunksAreStrictlyValid(const QByteArray &bytes) {
  static constexpr qint64 kSignatureSize = 8;
  static constexpr qint64 kLengthFieldSize = 4;
  static constexpr qint64 kTypeFieldSize = 4;
  static constexpr qint64 kCrcFieldSize = 4;
  // The PNG spec restricts chunk data length to a 31-bit unsigned value
  // (the top bit of the 4-byte length field is reserved/must be zero);
  // enforcing that here means every length used below fits comfortably in
  // a qint64 with no overflow risk, however it is combined with the
  // buffer's own (also qint64) size.
  static constexpr qint64 kMaxChunkDataLength = 0x7FFFFFFF;

  const qint64 size = bytes.size();
  if (size < kSignatureSize) {
    return false; // sniffMagicBytes() already checked this, but be safe
  }
  const auto *const data =
      reinterpret_cast<const unsigned char *>(bytes.constData());

  auto isChunkTypeByte = [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  };

  qint64 pos = kSignatureSize;
  bool sawIhdr = false;
  bool sawIdat = false;
  bool sawIend = false;
  bool isFirstChunk = true;

  while (pos < size) {
    if (sawIend) {
      // IEND must be the final chunk; reaching here means more bytes
      // follow it.
      return false;
    }
    if (pos + kLengthFieldSize + kTypeFieldSize > size) {
      return false; // truncated chunk header
    }
    const quint32 declaredLength = (static_cast<quint32>(data[pos]) << 24) |
                                   (static_cast<quint32>(data[pos + 1]) << 16) |
                                   (static_cast<quint32>(data[pos + 2]) << 8) |
                                   static_cast<quint32>(data[pos + 3]);
    if (declaredLength > static_cast<quint32>(kMaxChunkDataLength)) {
      return false; // top bit set: not a spec-legal PNG chunk length
    }
    const qint64 length = static_cast<qint64>(declaredLength);
    const qint64 typeStart = pos + kLengthFieldSize;
    const unsigned char typeBytes[4] = {data[typeStart], data[typeStart + 1],
                                        data[typeStart + 2],
                                        data[typeStart + 3]};
    for (unsigned char typeByte : typeBytes) {
      if (!isChunkTypeByte(typeByte)) {
        return false; // not a structurally valid chunk-type alphabet
      }
    }
    const qint64 dataStart = typeStart + kTypeFieldSize;
    if (dataStart + length + kCrcFieldSize > size) {
      return false; // declared length runs past the available bytes
    }
    const quint32 storedCrc =
        (static_cast<quint32>(data[dataStart + length]) << 24) |
        (static_cast<quint32>(data[dataStart + length + 1]) << 16) |
        (static_cast<quint32>(data[dataStart + length + 2]) << 8) |
        static_cast<quint32>(data[dataStart + length + 3]);
    // CRC-32 (ISO 3309 / zlib) is computed over the type field followed by
    // the chunk data, exactly as PNG requires. See pngCrc32()'s doc
    // comment above for why Qt's own qChecksum() cannot be used here.
    const quint32 computedCrc =
        pngCrc32(&data[typeStart], kTypeFieldSize + length);
    if (storedCrc != computedCrc) {
      return false; // corrupt or tampered chunk
    }

    const QByteArrayView typeView(reinterpret_cast<const char *>(typeBytes), 4);
    if (isFirstChunk) {
      if (typeView != "IHDR"_ba || length != 13) {
        return false; // the very first chunk must be a well-formed IHDR
      }
      isFirstChunk = false;
    }
    if (typeView == "IHDR"_ba) {
      if (sawIhdr) {
        return false; // more than one IHDR: not a single still image
      }
      sawIhdr = true;
    } else if (typeView == "IDAT"_ba) {
      sawIdat = true;
    } else if (typeView == "IEND"_ba) {
      if (length != 0) {
        return false; // IEND must carry no data
      }
      sawIend = true;
    } else if (typeView == "acTL"_ba || typeView == "fcTL"_ba ||
               typeView == "fdAT"_ba) {
      // APNG animation chunks: this project only ever wants exactly one
      // still frame (see this function's doc comment above).
      return false;
    }

    pos = dataStart + length + kCrcFieldSize;
  }

  return sawIhdr && sawIdat && sawIend;
}

// Normalises a Content-Type header value to a bare, lowercase media type
// (e.g. "image/png; charset=binary" -> "image/png"), so a parameter suffix
// cannot defeat the comparison in either direction.
QString normalizeContentType(const QString &raw) {
  const qsizetype semi = raw.indexOf(u';');
  const QString mediaType = (semi >= 0) ? raw.left(semi) : raw;
  return mediaType.trimmed().toLower();
}

// Whether Qt's installed image plugins can decode `format`, and (if so)
// which exact plugin-key spelling QImageReader should be constructed
// with. These two questions are answered together, from the SAME
// QImageReader::supportedImageFormats() snapshot, specifically so the
// format-hint given to QImageReader can never diverge from the spelling
// that was actually confirmed supported: a fixed hint independent of this
// check (e.g. always "jpeg") could pass the support check on a Qt build
// that only registers the OTHER JPEG key ("jpg") yet still fail to
// decode, since QImageReader's format hint is matched by exact plugin key.
// JPEG is checked/hinted under both of Qt's registered plugin keys
// ("jpeg" and "jpg" -- both are advertised by the stock qjpeg plugin, but
// a build could plausibly register only one). PNG has exactly one Qt key.
//
// AVIF is deliberately NEVER routed through this function (see
// decodeAndValidate() and AssetAvifDecoder.h) -- it is decoded directly
// against libavif's own C API, independent of whatever Qt image plugins
// this build happens to have registered, so this function's answer for
// AVIF would never actually be consulted.
struct QtCodecSupport {
  bool supported{false};
  QByteArray formatHint;
};

QtCodecSupport resolveQtCodecSupport(AssetFormat format) {
  Q_ASSERT(format != AssetFormat::Avif);
  const QList<QByteArray> supported = QImageReader::supportedImageFormats();
  switch (format) {
  case AssetFormat::Avif:
    // Unreachable per the Q_ASSERT above in a debug build; in a release
    // build, fail closed rather than silently reporting support this
    // function never actually checked for AVIF.
    return {false, QByteArrayLiteral("avif")};
  case AssetFormat::Jpeg:
    if (supported.contains(QByteArrayLiteral("jpeg"))) {
      return {true, QByteArrayLiteral("jpeg")};
    }
    if (supported.contains(QByteArrayLiteral("jpg"))) {
      return {true, QByteArrayLiteral("jpg")};
    }
    // Neither key is registered: report unsupported. The hint value here
    // is never actually used to construct a QImageReader in that case
    // (the caller checks `supported` first), but is filled in for
    // completeness.
    return {false, QByteArrayLiteral("jpeg")};
  case AssetFormat::Png:
    return {supported.contains(QByteArrayLiteral("png")),
            QByteArrayLiteral("png")};
  }
  Q_UNREACHABLE_RETURN((QtCodecSupport{}));
}

void applyCommonRequestSettings(QNetworkRequest &request) {
  request.setRawHeader("Accept", "image/avif,image/jpeg,image/png");
  request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                       QNetworkRequest::AlwaysNetwork);
  request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
}

} // namespace

AssetOutcome<std::unique_ptr<AssetNetworkFetcher>>
AssetNetworkFetcher::create(Limits limits, std::chrono::milliseconds timeout,
                            QObject *parent) {
  if (std::optional<AssetError> error =
          validateConfiguration(limits, timeout)) {
    return AssetOutcome<std::unique_ptr<AssetNetworkFetcher>>(
        std::move(*error));
  }
  return AssetOutcome<std::unique_ptr<AssetNetworkFetcher>>(
      std::make_unique<AssetNetworkFetcher>(limits, timeout, parent));
}

AssetOutcome<std::unique_ptr<AssetNetworkFetcher>>
AssetNetworkFetcher::create(QNetworkAccessManager &nam, Limits limits,
                            std::chrono::milliseconds timeout,
                            QObject *parent) {
  if (std::optional<AssetError> error =
          validateConfiguration(limits, timeout)) {
    return AssetOutcome<std::unique_ptr<AssetNetworkFetcher>>(
        std::move(*error));
  }
  return AssetOutcome<std::unique_ptr<AssetNetworkFetcher>>(
      std::make_unique<AssetNetworkFetcher>(nam, limits, timeout, parent));
}

std::optional<AssetError>
AssetNetworkFetcher::validateConfiguration(const Limits &limits,
                                           std::chrono::milliseconds timeout) {
  // Review item 7: timeout must be strictly positive and bounded -- a
  // zero/negative value is rejected rather than being (as before)
  // silently reinterpreted as "disable the timeout entirely", and an
  // absurdly large value is rejected rather than risking overflow when
  // eventually handed to QTimer::start() (whose interval is ultimately a
  // plain `int` millisecond count).
  if (timeout <= std::chrono::milliseconds::zero() ||
      timeout > kMaxAllowedTimeout) {
    return AssetError{
        AssetErrorCode::InvalidConfiguration,
        QStringLiteral("asset fetcher timeout must be positive and at "
                       "most %1 ms")
            .arg(kMaxAllowedTimeout.count())};
  }
  if (limits.maxEncodedBytes <= 0 ||
      limits.maxEncodedBytes > kMaxAllowedEncodedBytes) {
    return AssetError{
        AssetErrorCode::InvalidConfiguration,
        QStringLiteral("asset fetcher maxEncodedBytes must be positive and "
                       "at most %1 bytes")
            .arg(kMaxAllowedEncodedBytes)};
  }
  if (limits.maxDimensionPixels <= 0 ||
      limits.maxDimensionPixels > kMaxAllowedDimensionPixels) {
    return AssetError{
        AssetErrorCode::InvalidConfiguration,
        QStringLiteral("asset fetcher maxDimensionPixels must be positive "
                       "and at most %1")
            .arg(kMaxAllowedDimensionPixels)};
  }
  if (limits.maxTotalPixels <= 0 ||
      limits.maxTotalPixels > kMaxAllowedTotalPixels) {
    return AssetError{
        AssetErrorCode::InvalidConfiguration,
        QStringLiteral("asset fetcher maxTotalPixels must be positive and "
                       "at most %1")
            .arg(kMaxAllowedTotalPixels)};
  }
  return std::nullopt;
}

AssetNetworkFetcher::AssetNetworkFetcher(Limits limits,
                                         std::chrono::milliseconds timeout,
                                         QObject *parent)
    : AssetNetworkFetcher(std::make_unique<QNetworkAccessManager>(), nullptr,
                          limits, timeout, parent) {}

AssetNetworkFetcher::AssetNetworkFetcher(QNetworkAccessManager &nam,
                                         Limits limits,
                                         std::chrono::milliseconds timeout,
                                         QObject *parent)
    : AssetNetworkFetcher(nullptr, &nam, limits, timeout, parent) {}

AssetNetworkFetcher::AssetNetworkFetcher(
    std::unique_ptr<QNetworkAccessManager> ownedNam,
    QNetworkAccessManager *borrowedNam, Limits limits,
    std::chrono::milliseconds timeout, QObject *parent)
    : QObject(parent), m_ownedNam(std::move(ownedNam)),
      m_nam(m_ownedNam ? *m_ownedNam : *borrowedNam), m_limits(limits),
      m_timeout(timeout),
      m_configurationError(validateConfiguration(limits, timeout)) {
  // Review item 5: this fetcher's QNetworkAccessManager must never send a
  // system/application-configured proxy's credentials, even though it is
  // otherwise a normal QNetworkAccessManager instance that would
  // otherwise inherit
  // QNetworkProxyFactory's/QNetworkProxy::applicationProxy()'s process-wide
  // default. Explicitly forcing NoProxy here means every request always goes
  // directly to the origin server named by its URL, regardless of what the
  // embedding process/environment has configured.
  m_nam.setProxy(QNetworkProxy::NoProxy);
  // Defence in depth: even with NoProxy explicitly set above, connect a
  // handler that never populates the QAuthenticator, so that even in a
  // hypothetical future where this manager's proxy is reconfigured, no
  // Proxy-Authorization credential value can ever be attached by this
  // class -- the request instead fails naturally with
  // QNetworkReply::ProxyAuthenticationRequiredError, reported as a typed
  // AssetErrorCode::Transport error via the normal
  // `!statusAttr.isValid()` branch in handleFinished().
  connect(&m_nam, &QNetworkAccessManager::proxyAuthenticationRequired, this,
          [](const QNetworkProxy &, QAuthenticator *) {
            // Deliberately left blank: never supply credentials.
          });
}

AssetNetworkFetcher::~AssetNetworkFetcher() {
  for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
    if (QTimer *timer = it.value().timer) {
      timer->stop();
    }
    QNetworkReply *reply = it.value().reply;
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }
  m_pending.clear();
}

AssetNetworkFetcher::FetchHandle AssetNetworkFetcher::fetch(
    const AssetFetchUrl &fetchUrl, AssetFormat expectedFormat,
    ConditionalHeaders conditional, FetchCallback callback) {
  // Review item 7: an invalid configuration (see validateConfiguration())
  // fails every fetch() the exact same way UnsupportedScheme does below:
  // queued, never synchronous, and without ever touching
  // QNetworkAccessManager or inserting anything into m_pending (so
  // cancel() on the returned invalid handle is correctly a safe no-op).
  if (m_configurationError) {
    QPointer<AssetNetworkFetcher> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, callback = std::move(callback),
         error = *m_configurationError]() mutable {
          if (self) {
            std::move(callback)(
                AssetOutcome<ConditionalFetchResult>(std::move(error)));
          }
        },
        Qt::QueuedConnection);
    return FetchHandle{};
  }

  const QUrl &url = fetchUrl.url();
  // Defence-in-depth only: AssetFetchUrl::validate() already guarantees
  // the scheme is http/https (see AssetNetworkFetcher.h's class comment)
  // -- this branch cannot be reached by any caller using the public API
  // as designed. Retained purely so a hypothetical future bug in
  // validate() would still fail closed here rather than ever reaching
  // QNetworkAccessManager with an unexpected scheme.
  const QString scheme = url.scheme();
  if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
    QPointer<AssetNetworkFetcher> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, callback = std::move(callback)]() mutable {
          if (self) {
            std::move(callback)(AssetOutcome<ConditionalFetchResult>(AssetError{
                AssetErrorCode::UnsupportedScheme,
                QStringLiteral("only http and https URLs may be fetched")}));
          }
        },
        Qt::QueuedConnection);
    // Copilot review (round 29, medium severity): this rejection is
    // dispatched asynchronously (to preserve fetch()'s always-async
    // callback contract) but nothing is ever inserted into m_pending for
    // it, so there is genuinely no in-flight operation cancel() could
    // intercept -- calling cancel() on a handle for this path could
    // therefore never actually cancel the still-queued UnsupportedScheme
    // delivery, which would contradict cancel()'s documented contract
    // that a successful cancellation always yields
    // AssetErrorCode::Cancelled instead of the operation's real outcome.
    // Returning an invalid handle (rather than consuming a real
    // m_nextHandle value) makes that contract unambiguous: callers can
    // tell from isValid() alone that this handle was never eligible for
    // cancel() to begin with, instead of having to learn via a stale/
    // unknown-handle no-op that happens to look identical to cancelling
    // an already-completed request.
    return FetchHandle{};
  }

  // Review round-4 item 1: re-assert NoProxy immediately before every
  // single request, not just once in the constructor. For the owned
  // manager (the only path production/composition code may use) this is
  // a harmless no-op re-application. For a TEST-ONLY borrowed manager,
  // this closes a TOCTOU gap: a test (or, if this constructor were ever
  // misused, a caller in the same process) holding its own reference to
  // the same QNetworkAccessManager could otherwise reconfigure its proxy
  // at any point after construction, silently reintroducing exactly the
  // credential/proxy leak the constructor's one-time NoProxy call was
  // meant to prevent.
  m_nam.setProxy(QNetworkProxy::NoProxy);

  QNetworkRequest request(url);
  applyCommonRequestSettings(request);
  if (!conditional.etag.isEmpty()) {
    request.setRawHeader("If-None-Match", conditional.etag.toUtf8());
  }
  if (!conditional.lastModified.isEmpty()) {
    request.setRawHeader("If-Modified-Since",
                         conditional.lastModified.toUtf8());
  }

  QNetworkReply *reply = m_nam.get(request);
  // Bound QNetworkReply's own internal socket-level read buffer to the
  // same cap this class enforces on Pending::buffer (plus one byte, to
  // match the "+1" over-read this class's readyRead handler already uses
  // to detect an overflow without ever letting its own buffer exceed the
  // cap -- see handleReadyRead()). Without this, a large/fast response
  // could still let Qt buffer well past maxEncodedBytes internally, in
  // its own reply object, in between this class's readyRead-driven reads
  // -- this makes the "never exceeds maxEncodedBytes" bound apply to the
  // whole pipeline, not just the copy this class keeps in Pending::buffer.
  reply->setReadBufferSize(m_limits.maxEncodedBytes + 1);
  const quint64 handle = m_nextHandle++;

  Pending pending;
  pending.reply = reply;
  pending.expectedFormat = expectedFormat;
  pending.conditionalRequested = !conditional.isEmpty();
  pending.callback = std::move(callback);

  QTimer *timer = nullptr;
  if (m_timeout.count() > 0) {
    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, handle]() {
      auto it = m_pending.find(handle);
      if (it == m_pending.end()) {
        return;
      }
      QNetworkReply *r = it.value().reply;
      // The timer that just fired is this very request's own timer: it
      // is single-shot (never fires again) but is NOT otherwise deleted
      // anywhere else on this path, so it must be cleaned up here --
      // otherwise every timed-out request leaks one QTimer for the
      // remaining lifetime of this AssetNetworkFetcher.
      QTimer *t = it.value().timer;
      QObject::disconnect(r, nullptr, this, nullptr);
      r->abort();
      r->deleteLater();
      if (t) {
        t->deleteLater();
      }
      completeWithError(handle,
                        AssetError{AssetErrorCode::Transport,
                                   QStringLiteral("request timed out")});
    });
  }
  pending.timer = timer;

  m_pending.insert(handle, std::move(pending));

  connect(reply, &QNetworkReply::readyRead, this,
          [this, handle]() { handleReadyRead(handle); });
  connect(reply, &QNetworkReply::finished, this,
          [this, handle]() { handleFinished(handle); });

  if (timer) {
    timer->start(m_timeout);
  }

  return FetchHandle{handle};
}

void AssetNetworkFetcher::handleReadyRead(quint64 handle) {
  auto it = m_pending.find(handle);
  if (it == m_pending.end()) {
    return;
  }
  Pending &pending = it.value();
  if (pending.overflowed) {
    return; // already aborting; ignore further readyRead delivery
  }
  // Never read (and therefore never buffer) more than the remaining
  // budget, plus exactly one extra byte solely to detect that the server
  // tried to send more than the cap allows: appending readAll() first and
  // checking the size afterwards would let pending.buffer briefly grow
  // past m_limits.maxEncodedBytes before this function aborts, weakening
  // the hard bound the class comment promises.
  const qint64 remaining = m_limits.maxEncodedBytes - pending.buffer.size();
  const QByteArray chunk = pending.reply->read(remaining + 1);
  if (chunk.size() > remaining) {
    // Abort immediately: never buffer past the configured cap, regardless
    // of how large a hostile or misconfigured server claims (or omits)
    // Content-Length to be.
    pending.buffer.append(chunk.left(remaining));
    pending.overflowed = true;
    // Stop the per-request timeout timer here, not just in
    // handleFinished() once the abort's finished() signal is actually
    // delivered: abort() does not guarantee finished() fires
    // synchronously, so without this a timeout whose interval elapses in
    // that gap would fire first and misreport this exact
    // ResponseTooLarge outcome as a generic Transport (timeout) error
    // instead.
    if (pending.timer) {
      pending.timer->stop();
    }
    pending.reply->abort();
    return;
  }
  pending.buffer.append(chunk);
}

void AssetNetworkFetcher::completeWithError(quint64 handle, AssetError error) {
  auto it = m_pending.find(handle);
  if (it == m_pending.end()) {
    return;
  }
  FetchCallback callback = std::move(it.value().callback);
  m_pending.erase(it);

  QPointer<AssetNetworkFetcher> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, callback = std::move(callback),
       error = std::move(error)]() mutable {
        if (self) {
          std::move(callback)(AssetOutcome<ConditionalFetchResult>(error));
        }
      },
      Qt::QueuedConnection);
}

void AssetNetworkFetcher::handleFinished(quint64 handle) {
  auto it = m_pending.find(handle);
  if (it == m_pending.end()) {
    return; // already handled (cancel/timeout/overflow completion path)
  }
  Pending pendingCopy = std::move(it.value());
  m_pending.erase(it);

  if (QTimer *t = pendingCopy.timer) {
    t->stop();
    t->deleteLater();
  }
  QNetworkReply *reply = pendingCopy.reply;
  reply->deleteLater();

  auto emitResult = [this, callback = std::move(pendingCopy.callback)](
                        AssetOutcome<ConditionalFetchResult> result) mutable {
    QPointer<AssetNetworkFetcher> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, callback = std::move(callback),
         result = std::move(result)]() mutable {
          if (self) {
            std::move(callback)(std::move(result));
          }
        },
        Qt::QueuedConnection);
  };

  if (pendingCopy.overflowed) {
    emitResult(AssetError{AssetErrorCode::ResponseTooLarge,
                          QStringLiteral("response body exceeded the "
                                         "configured encoded-size limit")});
    return;
  }

  const QVariant statusAttr =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
  if (!statusAttr.isValid()) {
    if (reply->error() == QNetworkReply::OperationCanceledError) {
      emitResult(AssetError{AssetErrorCode::Cancelled,
                            QStringLiteral("request was cancelled")});
    } else {
      emitResult(AssetError{AssetErrorCode::Transport,
                            QStringLiteral("network transport error: %1")
                                .arg(reply->errorString())});
    }
    return;
  }

  const int status = statusAttr.toInt();

  if (status == 304) {
    if (!pendingCopy.conditionalRequested) {
      emitResult(AssetError{
          AssetErrorCode::ConditionalWithoutCachedBody,
          QStringLiteral("server returned 304 for an unconditional "
                         "request; no cached body is available to reuse")});
      return;
    }
    ConditionalFetchResult result;
    result.notModified = true;
    // A 304 MAY carry refreshed validators (RFC 7232 S4.1) even with no
    // body; capture them so the coordinator can extend the cache entry's
    // validator instead of forever revalidating against a value the
    // origin may eventually stop recognising.
    result.refreshedEtag = QString::fromLatin1(reply->rawHeader("ETag"));
    result.refreshedLastModified =
        QString::fromLatin1(reply->rawHeader("Last-Modified"));
    emitResult(std::move(result));
    return;
  }

  if (status >= 300 && status < 400) {
    // Never auto-followed (ManualRedirectPolicy); every 3xx is explicit
    // failure so a redirect can never smuggle a request to another origin.
    emitResult(AssetError{AssetErrorCode::RedirectRejected,
                          QStringLiteral("server responded with a redirect; "
                                         "redirects are never followed")});
    return;
  }

  if (status == 404) {
    emitResult(AssetError{AssetErrorCode::NotFound,
                          QStringLiteral("asset was not found (404)")});
    return;
  }

  // Review item 4: a response body is only ever accepted as a successful
  // fetched asset for EXACTLY status 200. Every other status in the
  // 2xx/1xx/5xx range not already special-cased above (201/202/203/204/
  // 206/1xx/5xx and anything else) is rejected as UnexpectedStatus rather
  // than silently treated as success -- in particular, 206 Partial
  // Content must never be decoded/cached as if it were the complete
  // representation, and 204 No Content must never be treated as a valid
  // (empty) image.
  if (status != 200) {
    emitResult(AssetError{
        AssetErrorCode::UnexpectedStatus,
        QStringLiteral("server responded with unexpected HTTP status %1; "
                       "only exactly 200 (or 304 for a conditional "
                       "request) is accepted")
            .arg(status)});
    return;
  }

  if (reply->error() != QNetworkReply::NoError) {
    emitResult(AssetError{AssetErrorCode::Transport,
                          QStringLiteral("network transport error: %1")
                              .arg(reply->errorString())});
    return;
  }

  // Drain any final buffered bytes not yet delivered via readyRead, using
  // the same bounded-read-plus-one-overflow-byte technique as
  // handleReadyRead(): calling readAll() unconditionally and checking the
  // size afterward would let pendingCopy.buffer briefly grow past
  // m_limits.maxEncodedBytes before this check runs, contradicting the
  // "buffer never exceeds the cap" hard bound the class comment promises.
  const qint64 remainingBudget =
      m_limits.maxEncodedBytes - pendingCopy.buffer.size();
  const QByteArray tail = reply->read(remainingBudget + 1);
  if (tail.size() > remainingBudget) {
    emitResult(AssetError{AssetErrorCode::ResponseTooLarge,
                          QStringLiteral("response body exceeded the "
                                         "configured encoded-size limit")});
    return;
  }
  pendingCopy.buffer.append(tail);

  const QByteArray &body = pendingCopy.buffer;

  const QString declaredContentType = normalizeContentType(
      QString::fromLatin1(reply->rawHeader("Content-Type")));
  const QString expectedMime = assetFormatMimeType(pendingCopy.expectedFormat);
  if (declaredContentType != expectedMime) {
    emitResult(AssetError{
        AssetErrorCode::ContentTypeMismatch,
        QStringLiteral("declared Content-Type \"%1\" does not match the "
                       "expected asset format")
            .arg(declaredContentType)});
    return;
  }

  AssetOutcome<QImage> decodedOutcome =
      decodeAndValidate(body, pendingCopy.expectedFormat);
  if (!decodedOutcome) {
    emitResult(decodedOutcome.error());
    return;
  }

  FetchedAsset asset;
  asset.encodedBytes = body;
  asset.contentType = declaredContentType;
  asset.dimensions = decodedOutcome->size();
  asset.decodedImage = std::move(*decodedOutcome);
  asset.sha256Hex = QString::fromLatin1(
      QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex());
  asset.etag = QString::fromLatin1(reply->rawHeader("ETag"));
  asset.lastModified = QString::fromLatin1(reply->rawHeader("Last-Modified"));
  asset.httpStatus = status;

  ConditionalFetchResult result;
  result.notModified = false;
  result.asset = std::move(asset);
  emitResult(std::move(result));
}

AssetOutcome<QImage>
AssetNetworkFetcher::decodeAndValidate(const QByteArray &encodedBytes,
                                       AssetFormat expectedFormat) const {
  const std::optional<AssetFormat> sniffed = sniffMagicBytes(encodedBytes);
  if (!sniffed || *sniffed != expectedFormat) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MagicBytesMismatch,
        QStringLiteral("body's magic bytes do not match the expected "
                       "asset format")});
  }

  if (expectedFormat == AssetFormat::Jpeg &&
      !jpegCodestreamHasGenuineEoi(encodedBytes)) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("JPEG body has no genuine End-Of-Image marker "
                       "(response was likely truncated in transit)")});
  }
  if (expectedFormat == AssetFormat::Png &&
      !pngChunksAreStrictlyValid(encodedBytes)) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("PNG chunk structure is malformed, contains a "
                       "checksum mismatch, carries animation data, or has "
                       "trailing bytes after IEND")});
  }

  // AVIF is decoded directly against libavif's own C API (see
  // AssetAvifDecoder.h/.cpp) -- never through QImageReader/Qt's plugin
  // registry, so it never depends on whether this build happens to have a
  // Qt AVIF plugin registered (review item 4: Qt has no official AVIF
  // plugin at all, and card art defaults to AVIF, so a permanent
  // UnsupportedCodec here is not acceptable).
  if (expectedFormat == AssetFormat::Avif) {
    return decodeAvifImage(encodedBytes, m_limits.maxDimensionPixels,
                           m_limits.maxTotalPixels);
  }

  const QtCodecSupport codecSupport = resolveQtCodecSupport(expectedFormat);
  if (!codecSupport.supported) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::UnsupportedCodec,
        QStringLiteral("no installed Qt image plugin can decode this "
                       "asset's format")});
  }

  QBuffer buffer;
  buffer.setData(encodedBytes);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer, codecSupport.formatHint);

  const QSize declaredSize = reader.size();
  if (!declaredSize.isValid() || declaredSize.width() <= 0 ||
      declaredSize.height() <= 0) {
    return AssetOutcome<QImage>(
        AssetError{AssetErrorCode::MalformedImage,
                   QStringLiteral("image header is malformed or reports "
                                  "non-positive dimensions")});
  }
  if (declaredSize.width() > m_limits.maxDimensionPixels ||
      declaredSize.height() > m_limits.maxDimensionPixels) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::DimensionTooLarge,
        QStringLiteral("image dimension %1x%2 exceeds the configured cap "
                       "of %3 pixels per side")
            .arg(declaredSize.width())
            .arg(declaredSize.height())
            .arg(m_limits.maxDimensionPixels)});
  }
  // 64-bit multiplication: both operands are already bounded above by
  // maxDimensionPixels (a configurable but always-small int), so this can
  // never overflow even at the largest permitted configuration.
  const qint64 totalPixels =
      static_cast<qint64>(declaredSize.width()) * declaredSize.height();
  if (totalPixels > m_limits.maxTotalPixels) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::PixelBudgetExceeded,
        QStringLiteral("image totals %1 pixels, exceeding the configured "
                       "cap of %2")
            .arg(totalPixels)
            .arg(m_limits.maxTotalPixels)});
  }

  // Round-4 review item 8: for JPEG specifically, the actual decode call
  // is wrapped in a scoped detector (see ScopedJpegDecodeWarningDetector's
  // doc comment above) that observes whether Qt's bundled libjpeg-based
  // plugin needed to log ANY message under its "qt.gui.imageio.jpeg"
  // category while decoding -- which happens precisely when libjpeg had
  // to synthesise a fake EOI and/or otherwise recover from corrupt/
  // incomplete data rather than genuinely completing every MCU of the
  // entropy-coded scan. A "successful", non-null QImage obtained this way
  // is exactly the silent-recovery case this review item requires
  // rejecting outright, so it is treated as a decode failure regardless
  // of what QImageReader itself reports.
  std::optional<ScopedJpegDecodeWarningDetector> jpegDetector;
  if (expectedFormat == AssetFormat::Jpeg) {
    jpegDetector.emplace();
  }
  QImage decoded = reader.read();
  if (jpegDetector && jpegDetector->sawJpegPluginMessage()) {
    return AssetOutcome<QImage>(AssetError{
        AssetErrorCode::MalformedImage,
        QStringLiteral("JPEG decode required libjpeg to recover from "
                       "corrupt or incomplete entropy-coded data")});
  }
  if (decoded.isNull()) {
    if (reader.error() == QImageReader::UnsupportedFormatError) {
      return AssetOutcome<QImage>(AssetError{
          AssetErrorCode::UnsupportedCodec,
          QStringLiteral("installed Qt image plugin declined to decode "
                         "this asset")});
    }
    return AssetOutcome<QImage>(
        AssetError{AssetErrorCode::MalformedImage,
                   QStringLiteral("image body failed to decode: %1")
                       .arg(reader.errorString())});
  }

  return AssetOutcome<QImage>(std::move(decoded));
}

void AssetNetworkFetcher::cancel(FetchHandle handle) {
  if (!handle.isValid()) {
    return;
  }
  auto it = m_pending.find(handle.id);
  if (it == m_pending.end()) {
    return; // stale or already-completed handle: safe no-op
  }
  if (QTimer *timer = it.value().timer) {
    timer->stop();
    timer->deleteLater();
  }
  QNetworkReply *reply = it.value().reply;
  FetchCallback callback = std::move(it.value().callback);
  QObject::disconnect(reply, nullptr, this, nullptr);
  m_pending.erase(it);
  reply->abort();
  reply->deleteLater();

  QPointer<AssetNetworkFetcher> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, callback = std::move(callback)]() mutable {
        if (self) {
          std::move(callback)(AssetOutcome<ConditionalFetchResult>(
              AssetError{AssetErrorCode::Cancelled,
                         QStringLiteral("request was cancelled")}));
        }
      },
      Qt::QueuedConnection);
}

} // namespace Arkham
