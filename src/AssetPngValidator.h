#pragma once

#include <QByteArray>

// Round-4 review item 8 introduced an independent, deterministic PNG
// structural validator that runs strictly BEFORE QImageReader ever sees a
// PNG body, rejecting anything the underlying (Qt-build-specific, and
// therefore platform/version-variable) libpng-based decoder might
// otherwise tolerate or silently ignore. Round-5 review item 3 extended
// it to also reject a non-consecutive IDAT run.
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

// Validates the full chunk structure of a PNG body whose 8-byte signature
// has already been confirmed by the caller (see
// AssetNetworkFetcher.cpp's sniffMagicBytes()). Returns false if:
//   - any chunk's declared length or position would run past the end of
//     `bytes` (bounds safety, overflow-free: lengths are capped to the
//     PNG spec's own 31-bit limit before use in arithmetic);
//   - any chunk's stored CRC-32 does not match its actual bytes;
//   - any chunk type's four bytes are not all ASCII letters;
//   - the first chunk is not exactly a well-formed 13-byte IHDR;
//   - more than one IHDR chunk is present;
//   - any APNG-defining chunk type (acTL/fcTL/fdAT) is present -- an
//     animated PNG is a "multiple image" in the same sense this
//     project's AVIF imageCount!=1 rejection is;
//   - no IDAT chunk is present at all;
//   - the IDAT chunks are not all consecutive (any chunk of a different
//     type appearing between two IDAT chunks, e.g.
//     IHDR,IDAT,tEXt,IDAT,IEND, closes the IDAT run; any further IDAT
//     chunk after that point is rejected);
//   - the final IEND chunk (zero-length data) is not the exact final
//     chunk of the buffer (any trailing byte after IEND -- padding, a
//     second concatenated PNG, or anything else -- is rejected).
[[nodiscard]] bool pngChunksAreStrictlyValid(const QByteArray &bytes);

} // namespace Arkham
