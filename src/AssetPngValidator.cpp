#include "AssetPngValidator.h"

#include <algorithm>
#include <array>
#include <limits>

#include <zlib.h>

using namespace Qt::StringLiterals;

namespace Arkham {

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

// Cumulative-review finding (PR #18, exact head 4a47ea34): validates that
// (bitDepth, colorType) is one of the PNG spec's own legal combinations.
// Table 11.1 of the PNG specification: colour type 0 (greyscale) allows
// bit depths 1/2/4/8/16; type 2 (truecolour) and type 6
// (truecolour+alpha) allow only 8/16; type 3 (indexed/palette) allows
// 1/2/4/8; type 4 (greyscale+alpha) allows only 8/16. Any other colour
// type value (1, 5, 7, or >=8) is not defined by the spec at all.
bool pngBitDepthValidForColorType(quint8 bitDepth, quint8 colorType) {
  switch (colorType) {
  case 0:
    return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8 ||
           bitDepth == 16;
  case 2:
  case 6:
    return bitDepth == 8 || bitDepth == 16;
  case 3:
    return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8;
  case 4:
    return bitDepth == 8 || bitDepth == 16;
  default:
    return false;
  }
}

// The number of samples ("channels") per pixel for each PNG spec colour
// type: 0 (greyscale) -> 1, 2 (truecolour, RGB) -> 3, 3 (indexed) -> 1 (a
// single palette-index sample per pixel, regardless of the palette's own
// entry width), 4 (greyscale+alpha) -> 2, 6 (truecolour+alpha, RGBA) -> 4.
// pngBitDepthValidForColorType() above has already rejected every other
// colorType value by the time this is called.
int pngChannelsForColorType(quint8 colorType) {
  switch (colorType) {
  case 0:
    return 1;
  case 2:
    return 3;
  case 3:
    return 1;
  case 4:
    return 2;
  case 6:
    return 4;
  default:
    Q_UNREACHABLE();
  }
}

} // namespace

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
//   - IDAT chunks that are not all consecutive (round-5 review item 3): the
//     PNG spec requires every IDAT chunk belonging to the one image to
//     appear as an unbroken run; a chunk of any other type appearing
//     between two IDAT chunks (e.g. IHDR,IDAT,tEXt,IDAT,IEND) means a
//     conforming decoder must treat the second IDAT run as invalid/a
//     different logical stream, yet a CRC-valid decoder that merely
//     concatenates every IDAT payload it sees (ignoring ordering) would
//     silently accept it. This is rejected outright the first time a
//     non-IDAT chunk follows the closing chunk of an already-started
//     IDAT run;
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
bool pngChunksAreStrictlyValid(const QByteArray &bytes,
                               PngStructuralInfo *info) {
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
  // Round-5 review item 3: tracks whether the IDAT run has started and,
  // separately, whether it has already been closed by a subsequent
  // non-IDAT chunk. Any IDAT chunk seen after idatRunClosed is a
  // non-consecutive IDAT sequence and must be rejected.
  bool idatRunStarted = false;
  bool idatRunClosed = false;

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
      // IHDR's 13-byte payload (per the PNG spec): width (4 bytes, BE),
      // height (4 bytes, BE), bit depth (1 byte), colour type (1 byte),
      // compression method (1 byte), filter method (1 byte), interlace
      // method (1 byte). `length == 13` was already required above (the
      // "first chunk must be a well-formed IHDR" check), so every offset
      // below is in-bounds.
      const quint32 ihdrWidth =
          (static_cast<quint32>(data[dataStart]) << 24) |
          (static_cast<quint32>(data[dataStart + 1]) << 16) |
          (static_cast<quint32>(data[dataStart + 2]) << 8) |
          static_cast<quint32>(data[dataStart + 3]);
      const quint32 ihdrHeight =
          (static_cast<quint32>(data[dataStart + 4]) << 24) |
          (static_cast<quint32>(data[dataStart + 5]) << 16) |
          (static_cast<quint32>(data[dataStart + 6]) << 8) |
          static_cast<quint32>(data[dataStart + 7]);
      const quint8 ihdrBitDepth = data[dataStart + 8];
      const quint8 ihdrColorType = data[dataStart + 9];
      const quint8 ihdrCompressionMethod = data[dataStart + 10];
      const quint8 ihdrFilterMethod = data[dataStart + 11];
      const quint8 ihdrInterlaceMethod = data[dataStart + 12];

      if (ihdrWidth == 0 || ihdrHeight == 0) {
        return false; // non-positive dimension: caught structurally here,
                      // same as decodeAndValidate()'s own later check
      }
      // The PNG spec fixes compression method and filter method to
      // exactly 0 (the only methods it defines); any other value is not a
      // conforming PNG at all.
      if (ihdrCompressionMethod != 0 || ihdrFilterMethod != 0) {
        return false;
      }
      // Cumulative-review finding (PR #18, exact head 4a47ea34): Adam7
      // interlacing (method 1) is rejected outright -- see this file's
      // header comment for why narrowing to the simple non-interlaced
      // case is what makes the exact-expected-scanline-size computation
      // below tractable, and why this project has no actual need for
      // interlaced card art in the first place.
      if (ihdrInterlaceMethod != 0) {
        return false;
      }
      if (!pngBitDepthValidForColorType(ihdrBitDepth, ihdrColorType)) {
        return false;
      }
      if (info) {
        info->width = ihdrWidth;
        info->height = ihdrHeight;
        info->bitDepth = ihdrBitDepth;
        info->colorType = ihdrColorType;
      }
    } else if (typeView == "IDAT"_ba) {
      if (idatRunClosed) {
        return false; // non-consecutive IDAT run: see doc comment above
      }
      sawIdat = true;
      idatRunStarted = true;
      if (info) {
        info->idatPayload.append(
            reinterpret_cast<const char *>(&data[dataStart]),
            static_cast<qsizetype>(length));
      }
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
    } else if (typeView == "zTXt"_ba || typeView == "iCCP"_ba) {
      // Round-6 review item 1: both chunk types unconditionally embed a
      // zlib-DEFLATE-compressed payload (compressed text / a compressed
      // ICC colour profile respectively) that QImageReader's underlying
      // libpng decompresses during header parsing (png_read_info()),
      // strictly before this project's own pixel/dimension budget checks
      // ever run -- a "metadata bomb" vector independent of the actual
      // image's declared dimensions. Rejected outright: this project has
      // no use for embedded arbitrary text metadata or colour profiles on
      // card art.
      return false;
    } else if (typeView == "iTXt"_ba) {
      // iTXt's layout (per the PNG spec) is:
      //   Keyword\0 Compression-flag Compression-method Language-tag\0
      //   Translated-keyword\0 Text
      // The compression flag is NOT simply the chunk's first data byte
      // (that is always the first byte of Keyword, a text field, and is
      // therefore essentially always non-zero) -- it is the single byte
      // immediately following Keyword's own NUL terminator. Locating
      // that terminator correctly matters: treating the first byte as
      // the flag (an earlier version of this check did) makes every
      // non-empty iTXt chunk look "compressed" regardless of its actual
      // flag, silently rejecting legitimate uncompressed iTXt metadata
      // while still (only coincidentally) rejecting the compressed case
      // this check exists for.
      qint64 keywordNulPos = -1;
      for (qint64 i = 0; i < length; ++i) {
        if (data[dataStart + i] == 0) {
          keywordNulPos = i;
          break;
        }
      }
      if (keywordNulPos < 0) {
        return false; // no Keyword terminator at all: not a well-formed
                      // iTXt chunk
      }
      const qint64 compressionFlagOffset = keywordNulPos + 1;
      // A chunk truncated immediately after Keyword's terminator (no
      // compression-flag byte present at all) has no text and poses no
      // decompression risk either way -- exactly as truncated/empty
      // cases were already treated as harmless before this fix; only a
      // present, nonzero compression flag is rejected here, matching
      // zTXt/iCCP's blanket rejection above for exactly the same
      // "metadata bomb" reason once compression is actually in play.
      if (compressionFlagOffset < length &&
          data[dataStart + compressionFlagOffset] != 0) {
        return false;
      }
    }

    // Any chunk other than IDAT that appears once the IDAT run has begun
    // closes that run; a further IDAT chunk after this point is
    // non-consecutive and rejected above.
    if (typeView != "IDAT"_ba && idatRunStarted) {
      idatRunClosed = true;
    }

    pos = dataStart + length + kCrcFieldSize;
  }

  return sawIhdr && sawIdat && sawIend;
}

// Cumulative-review finding (PR #18, exact head 4a47ea34): see this
// function's doc comment in AssetPngValidator.h for the full rationale.
// Decompresses `idatPayload` directly against zlib's own C API (never
// Qt's qUncompress()/qCompress(), which use an incompatible wire format)
// and requires the result to be EXACTLY the non-interlaced scanline byte
// count IHDR's width/height/bitDepth/colorType imply -- no more, no less
// -- with the zlib stream reaching Z_STREAM_END and every input byte
// consumed.
bool pngIdatDecompressesToExactExpectedSize(const QByteArray &idatPayload,
                                            quint32 width, quint32 height,
                                            quint8 bitDepth, quint8 colorType) {
  // Defensive: pngChunksAreStrictlyValid() (the only production caller)
  // has already validated this combination while parsing IHDR, but a test
  // -- or any other future caller -- might invoke this function directly
  // with an arbitrary combination, and pngChannelsForColorType() below is
  // only well-defined for a colorType this already accepts.
  if (!pngBitDepthValidForColorType(bitDepth, colorType)) {
    return false;
  }
  // A sanity guard against QByteArray's qsizetype (signed) limits before
  // handing its length to zlib's own (unsigned int) avail_in field. This
  // project's caller only ever supplies an idatPayload bounded by the
  // encoded-body cap (a few tens of megabytes at most), but this keeps
  // the cast below well-defined regardless of who calls this function.
  if (idatPayload.size() < 0 ||
      static_cast<quint64>(idatPayload.size()) >
          static_cast<quint64>(std::numeric_limits<uInt>::max())) {
    return false;
  }

  const int channels = pngChannelsForColorType(colorType);
  // ceil(width * channels * bitDepth / 8): overflow-safe, since width fits
  // in a quint32, channels is at most 4, and bitDepth is at most 16 -- the
  // product fits comfortably in a quint64 with no overflow risk regardless
  // of width's own maximum possible value.
  const quint64 bitsPerScanline = static_cast<quint64>(width) *
                                  static_cast<quint64>(channels) *
                                  static_cast<quint64>(bitDepth);
  const quint64 bytesPerScanlineNoFilter = (bitsPerScanline + 7) / 8;
  // + 1: PNG's own leading filter-type byte, present on every scanline
  // regardless of colour type/bit depth.
  const quint64 bytesPerScanline = bytesPerScanlineNoFilter + 1;
  // Overflow-safe multiplication: only proceed if bytesPerScanline *
  // height cannot overflow quint64's range. In practice the caller
  // (AssetNetworkFetcher::decodeAndValidate()) has already rejected
  // width/height exceeding its own configured dimension/pixel-budget caps
  // by the time this runs, so this is a defensive belt-and-suspenders
  // check, not the primary enforcement mechanism.
  if (height != 0 && bytesPerScanline > (std::numeric_limits<quint64>::max)() /
                                            static_cast<quint64>(height)) {
    return false;
  }
  const quint64 expectedBytes = bytesPerScanline * static_cast<quint64>(height);

  // The output buffer is allocated at EXACTLY expectedBytes + 1 bytes --
  // one byte more than a fully-valid, exactly-sized stream could ever
  // legitimately need -- so a stream that would decompress to more than
  // expectedBytes is caught the moment it would need that
  // (expectedBytes + 1)-th byte of output space (inflate() returns
  // Z_OK/Z_BUF_ERROR with avail_out exhausted and no Z_STREAM_END, which
  // this function treats as failure below), without ever needing to grow
  // the buffer or otherwise follow a runaway decompression to completion.
  const quint64 boundedOutputCapacity = expectedBytes + 1;
  if (boundedOutputCapacity >
      static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
    return false; // astronomically large declared image; reject
  }

  QByteArray output(static_cast<qsizetype>(boundedOutputCapacity),
                    Qt::Uninitialized);

  z_stream stream{};
  stream.next_in =
      reinterpret_cast<Bytef *>(const_cast<char *>(idatPayload.constData()));
  stream.avail_in = static_cast<uInt>(idatPayload.size());
  stream.next_out = reinterpret_cast<Bytef *>(output.data());
  stream.avail_out = static_cast<uInt>(std::min<quint64>(
      boundedOutputCapacity, std::numeric_limits<uInt>::max()));

  // PNG's IDAT payload is a bare zlib stream (a 2-byte header and an
  // Adler-32 trailer around raw DEFLATE data) -- exactly what plain
  // inflateInit() expects, unlike inflateInit2() with a negative
  // windowBits (which would instead expect a raw, headerless DEFLATE
  // stream with no zlib framing at all).
  if (inflateInit(&stream) != Z_OK) {
    return false;
  }

  const int result = inflate(&stream, Z_FINISH);
  const quint64 totalOut = stream.total_out;
  const uInt remainingIn = stream.avail_in;
  inflateEnd(&stream);

  if (result != Z_STREAM_END) {
    // Either the stream was truncated/corrupt (never reached a valid
    // end), or it needed more output space than boundedOutputCapacity
    // allows (a stream that decompresses to more than the declared image
    // data, i.e. exactly the "second stream" attack this function
    // exists to reject).
    return false;
  }
  if (remainingIn != 0) {
    // zlib reached a valid Z_STREAM_END before consuming every byte of
    // idatPayload: trailing bytes (padding, or an entirely separate
    // second zlib stream) follow the legitimate image data.
    return false;
  }
  if (totalOut != expectedBytes) {
    // Reached Z_STREAM_END, consumed every input byte, but produced a
    // different byte count than IHDR's own width/height/bitDepth/
    // colorType imply -- e.g. a stream that is short by exactly the
    // final scanline. Never accepted: this project requires exact
    // agreement between the declared image shape and the actual pixel
    // data, not merely "some" successfully decompressed data.
    return false;
  }
  return true;
}

} // namespace Arkham
