#include "AssetPngValidator.h"

#include <array>

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
    } else if (typeView == "IDAT"_ba) {
      if (idatRunClosed) {
        return false; // non-consecutive IDAT run: see doc comment above
      }
      sawIdat = true;
      idatRunStarted = true;
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

} // namespace Arkham
