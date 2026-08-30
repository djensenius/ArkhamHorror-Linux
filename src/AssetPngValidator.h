#pragma once

#include <QByteArray>

// Round-4 review item 8 introduced an independent, deterministic PNG
// structural validator that runs strictly BEFORE QImageReader ever sees a
// PNG body, rejecting anything the underlying (Qt-build-specific, and
// therefore platform/version-variable) libpng-based decoder might
// otherwise tolerate or silently ignore. Round-5 review item 3 extended
// it to also reject a non-consecutive IDAT run. This round (PR #18,
// exact head 4a47ea34) extends it further to parse IHDR's own 13-byte
// payload (previously only checked for a correct type/length, never
// actually decoded) and to expose the concatenated IDAT chunk payload, so
// a companion function (pngIdatDecompressesToExactExpectedSize(), below)
// can validate that the IDAT run's zlib stream decompresses to EXACTLY
// the expected number of scanline bytes with no trailing/second-stream
// data -- something libpng/QImageReader tolerates (with at most a
// swallowed warning) but this project never accepts.
//
// This validator is deliberately exposed as its own free function (rather
// than kept file-local inside AssetNetworkFetcher.cpp) for exactly the
// same reason AssetAvifDecoder.h/.cpp and AssetJpegDecoder.h/.cpp expose
// their own decode entry points: it lets tests exercise this project's
// OWN structural policy directly and deterministically, independent of
// whatever a given host's bundled libpng version happens to itself
// accept or reject for the same crafted byte sequence (a genuinely
// nonconsecutive-IDAT payload may already be rejected end-to-end by a
// sufficiently strict libpng build, which would make an end-to-end HTTP
// fetch test unable to prove this project's OWN pre-check is what did
// the rejecting, rather than merely happening to agree with a decoder
// that might not be so strict on every target platform).
namespace Arkham {

// Everything pngChunksAreStrictlyValid() learns about a structurally
// valid PNG body's IHDR chunk and IDAT run, populated only when that
// function returns true -- letting a caller (or
// pngIdatDecompressesToExactExpectedSize(), below) reuse the results of the one
// chunk-walk pass it already performs rather than re-parsing the same bytes a
// second time.
struct PngStructuralInfo {
  quint32 width = 0;
  quint32 height = 0;
  quint8 bitDepth = 0;
  quint8 colorType = 0;
  // Every IDAT chunk's data bytes, concatenated in file order -- exactly
  // the input stream a conforming decoder would feed to zlib's inflate()
  // as one contiguous run.
  QByteArray idatPayload;
};

// Validates the full chunk structure of a PNG body whose 8-byte signature
// has already been confirmed by the caller (see
// AssetNetworkFetcher.cpp's sniffMagicBytes()). Returns false if:
//   - any chunk's declared length or position would run past the end of
//     `bytes` (bounds safety, overflow-free: lengths are capped to the
//     PNG spec's own 31-bit limit before use in arithmetic);
//   - any chunk's stored CRC-32 does not match its actual bytes;
//   - any chunk type's four bytes are not all ASCII letters;
//   - the first chunk is not exactly a well-formed 13-byte IHDR;
//   - IHDR's compression method or filter method is not exactly 0 (the
//     only PNG-spec-legal values), its interlace method is not exactly 0
//     (Adam7-interlaced PNGs are rejected outright -- this project has no
//     need for interlaced card art, and narrowing to the non-interlaced
//     case is what makes the exact-expected-scanline-size computation
//     below simple and unambiguous rather than requiring the substantial
//     extra complexity of Adam7's seven-pass sizing), or its bit
//     depth/colour type is not one of the PNG spec's valid combinations
//     (colour type 0: 1/2/4/8/16; type 2: 8/16; type 3: 1/2/4/8; type 4:
//     8/16; type 6: 8/16) or its colour type value is not one of 0/2/3/
//     4/6 at all;
//   - more than one IHDR chunk is present;
//   - any APNG-defining chunk type (acTL/fcTL/fdAT) is present -- an
//     animated PNG is a "multiple image" in the same sense this
//     project's AVIF imageCount!=1 rejection is;
//   - any DEFLATE-compressed ancillary chunk type is present (zTXt,
//     iCCP, or iTXt with its compression flag set) -- these chunks embed
//     an independently-compressed payload that QImageReader's underlying
//     libpng would decompress during header parsing, before this
//     project's own pixel/dimension budget checks ever run, making them a
//     "metadata bomb" vector even for a tiny, well-under-cap PNG body;
//   - no IDAT chunk is present at all;
//   - the IDAT chunks are not all consecutive (any chunk of a different
//     type appearing between two IDAT chunks, e.g.
//     IHDR,IDAT,tEXt,IDAT,IEND, closes the IDAT run; any further IDAT
//     chunk after that point is rejected);
//   - the final IEND chunk (zero-length data) is not the exact final
//     chunk of the buffer (any trailing byte after IEND -- padding, a
//     second concatenated PNG, or anything else -- is rejected).
//
// On success (return value true), if `info` is non-null it is populated
// with IHDR's width/height/bitDepth/colorType and the concatenated IDAT
// payload bytes.
[[nodiscard]] bool pngChunksAreStrictlyValid(const QByteArray &bytes,
                                             PngStructuralInfo *info = nullptr);

// Cumulative-review finding (PR #18, exact head 4a47ea34): a CRC-valid
// PNG IDAT run can contain a complete, valid image zlib stream followed
// by extra bytes or an entirely separate second zlib stream. libpng only
// warns about the trailing data (it does not fail the decode), and Qt's
// qpng plugin surfaces no way for this project to observe that warning,
// so QImageReader/QImage happily decodes and this project would happily
// cache the (still-image-correct, but not exactly-as-declared) result.
//
// This decompresses `idatPayload` (the exact concatenated IDAT bytes
// pngChunksAreStrictlyValid() already gathered) directly against zlib's
// own C API -- Qt's qUncompress()/qCompress() are NOT usable here: they
// require Qt's own wire format (a 4-byte big-endian uncompressed-length
// prefix ahead of the zlib stream), not the bare zlib stream PNG's IDAT
// chunks actually contain.
//
// Returns true only if the decompressed byte count is EXACTLY the
// expected non-interlaced scanline byte count computed from
// `width`/`height`/`bitDepth`/`colorType` (one leading filter-type byte
// per scanline, per the PNG spec -- see AssetPngValidator.cpp for the
// exact channel-count-per-colour-type mapping and the ceiling-division
// per-scanline byte computation), Z_STREAM_END is reached, and every
// input byte of `idatPayload` was consumed (no trailing bytes/second
// stream). The decompression itself is bounded: it can never allocate or
// produce more than `expectedBytes + 1` bytes of output regardless of
// how much `idatPayload` claims to decompress to, so a compressed IDAT
// run that inflates to substantially more than the declared image data
// is rejected the moment it exceeds that bound rather than being
// followed to completion.
[[nodiscard]] bool
pngIdatDecompressesToExactExpectedSize(const QByteArray &idatPayload,
                                       quint32 width, quint32 height,
                                       quint8 bitDepth, quint8 colorType);

} // namespace Arkham
