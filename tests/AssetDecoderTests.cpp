#include <cstdio>

#include <jpeglib.h>

#include <avif/avif.h>

#include <algorithm>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <QBuffer>
#include <QtTest>

#include "AssetJpegDecoder.h"
#include "AssetTypes.h"

using Arkham::AssetDecodeErrorCode;
using Arkham::AssetDecodeLimits;
using Arkham::decodeAssetImage;

namespace {

QImage tinyImage() {
  QImage image(2, 2, QImage::Format_RGBA8888);
  image.fill(Qt::transparent);
  image.setPixelColor(0, 0, QColor(240, 20, 20, 255));
  image.setPixelColor(1, 0, QColor(20, 240, 20, 255));
  image.setPixelColor(0, 1, QColor(20, 20, 240, 255));
  image.setPixelColor(1, 1, QColor(250, 250, 20, 255));
  return image;
}

// A card-art-shaped gradient used by the sized-decode tests.
QImage cardImage(const QSize &size) {
  QImage image(size, QImage::Format_RGBA8888);
  for (int y = 0; y < size.height(); ++y) {
    for (int x = 0; x < size.width(); ++x) {
      image.setPixelColor(x, y,
                          QColor((x * 255) / size.width(),
                                 (y * 255) / size.height(), 120, 255));
    }
  }
  return image;
}

QByteArray encodePngWithQt(const QImage &source = tinyImage()) {
  QByteArray bytes;
  QBuffer buffer(&bytes);
  if (!buffer.open(QIODevice::WriteOnly)) {
    return {};
  }
  if (!source.save(&buffer, "PNG")) {
    return {};
  }
  return bytes;
}

avifImage *createAvifImage(const QImage &sourceImage) {
  const QImage source = sourceImage.convertToFormat(QImage::Format_RGBA8888);
  avifImage *image = avifImageCreate(static_cast<uint32_t>(source.width()),
                                     static_cast<uint32_t>(source.height()), 8,
                                     AVIF_PIXEL_FORMAT_YUV444);
  if (!image) {
    return nullptr;
  }

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  rgb.depth = 8;
  rgb.pixels = const_cast<uint8_t *>(source.constBits());
  rgb.rowBytes = static_cast<uint32_t>(source.bytesPerLine());

  if (avifImageRGBToYUV(image, &rgb) != AVIF_RESULT_OK) {
    avifImageDestroy(image);
    return nullptr;
  }
  return image;
}

QByteArray finishAvifEncode(avifEncoder *encoder) {
  avifRWData output = AVIF_DATA_EMPTY;
  const avifResult result = avifEncoderFinish(encoder, &output);

  QByteArray bytes;
  if (result == AVIF_RESULT_OK && output.data && output.size > 0) {
    bytes = QByteArray(reinterpret_cast<const char *>(output.data),
                       static_cast<qsizetype>(output.size));
  }
  avifRWDataFree(&output);
  return bytes;
}

QByteArray encodeAvif(const QImage &source = tinyImage()) {
  avifImage *image = createAvifImage(source);
  if (!image) {
    return {};
  }

  avifEncoder *encoder = avifEncoderCreate();
  if (!encoder) {
    avifImageDestroy(image);
    return {};
  }
  encoder->maxThreads = 1;
  encoder->minQuantizer = 0;
  encoder->maxQuantizer = 4;
  encoder->speed = AVIF_SPEED_FASTEST;

  QByteArray bytes;
  if (avifEncoderAddImage(encoder, image, 1, AVIF_ADD_IMAGE_FLAG_SINGLE) ==
      AVIF_RESULT_OK) {
    bytes = finishAvifEncode(encoder);
  }
  avifEncoderDestroy(encoder);
  avifImageDestroy(image);
  return bytes;
}

quint32 readBigEndian32(const char *bytes) {
  const auto *data = reinterpret_cast<const unsigned char *>(bytes);
  return (static_cast<quint32>(data[0]) << 24) |
         (static_cast<quint32>(data[1]) << 16) |
         (static_cast<quint32>(data[2]) << 8) | static_cast<quint32>(data[3]);
}

quint64 readBigEndian64(const char *bytes) {
  const auto *data = reinterpret_cast<const unsigned char *>(bytes);
  quint64 value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<quint64>(data[i]);
  }
  return value;
}

QByteArray renameTopLevelMetaBoxToFree(QByteArray avif) {
  qsizetype offset = 0;
  while (offset + 8 <= avif.size()) {
    quint64 boxSize = readBigEndian32(avif.constData() + offset);
    qsizetype headerSize = 8;
    if (boxSize == 1) {
      if (offset + 16 > avif.size()) {
        return {};
      }
      boxSize = readBigEndian64(avif.constData() + offset + 8);
      headerSize = 16;
    } else if (boxSize == 0) {
      boxSize = static_cast<quint64>(avif.size() - offset);
    }

    if (boxSize < static_cast<quint64>(headerSize) ||
        boxSize > static_cast<quint64>(avif.size() - offset)) {
      return {};
    }

    const qsizetype typeOffset = offset + 4;
    if (std::memcmp(avif.constData() + typeOffset, "meta", 4) == 0) {
      std::memcpy(avif.data() + typeOffset, "free", 4);
      return avif;
    }
    offset += static_cast<qsizetype>(boxSize);
  }
  return {};
}

struct JpegWriteErrorManager {
  jpeg_error_mgr pub;
  jmp_buf setjmpBuffer;
};

void jpegWriteErrorExit(j_common_ptr cinfo) {
  auto *errorManager = static_cast<JpegWriteErrorManager *>(cinfo->client_data);
  longjmp(errorManager->setjmpBuffer, 1);
}

QByteArray encodeJpeg(bool progressive = false,
                      J_COLOR_SPACE colorSpace = JCS_RGB, int width = 8,
                      int height = 8) {
  const int components = colorSpace == JCS_CMYK ? 4 : 3;
  std::vector<unsigned char> pixels(
      static_cast<size_t>(width * height * components));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t offset = static_cast<size_t>((y * width + x) * components);
      if (colorSpace == JCS_CMYK) {
        pixels[offset] = static_cast<unsigned char>(x * 24);
        pixels[offset + 1] = static_cast<unsigned char>(y * 24);
        pixels[offset + 2] = 0;
        pixels[offset + 3] = 0;
      } else {
        pixels[offset] = static_cast<unsigned char>((x * 32) % 256);
        pixels[offset + 1] = static_cast<unsigned char>((y * 32) % 256);
        pixels[offset + 2] = static_cast<unsigned char>(200);
      }
    }
  }

  jpeg_compress_struct cinfo{};
  JpegWriteErrorManager errorManager{};
  cinfo.err = jpeg_std_error(&errorManager.pub);
  errorManager.pub.error_exit = jpegWriteErrorExit;
  cinfo.client_data = &errorManager;

  unsigned char *output = nullptr;
  unsigned long outputSize = 0;
  if (setjmp(errorManager.setjmpBuffer)) {
    jpeg_destroy_compress(&cinfo);
    std::free(output);
    return {};
  }

  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &output, &outputSize);
  cinfo.image_width = static_cast<JDIMENSION>(width);
  cinfo.image_height = static_cast<JDIMENSION>(height);
  cinfo.input_components = components;
  cinfo.in_color_space = colorSpace;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 90, TRUE);
  if (progressive) {
    jpeg_simple_progression(&cinfo);
  }
  jpeg_start_compress(&cinfo, TRUE);

  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW rowPointer[1] = {pixels.data() +
                              static_cast<size_t>(cinfo.next_scanline) * width *
                                  static_cast<size_t>(components)};
    jpeg_write_scanlines(&cinfo, rowPointer, 1);
  }

  jpeg_finish_compress(&cinfo);
  QByteArray bytes(reinterpret_cast<const char *>(output),
                   static_cast<qsizetype>(outputSize));
  jpeg_destroy_compress(&cinfo);
  std::free(output);
  return bytes;
}

QByteArray truncatePngInsideIdat(const QByteArray &png) {
  const auto *data = reinterpret_cast<const unsigned char *>(png.constData());
  qsizetype pos = 8;
  while (pos + 12 <= png.size()) {
    const quint32 length = (static_cast<quint32>(data[pos]) << 24) |
                           (static_cast<quint32>(data[pos + 1]) << 16) |
                           (static_cast<quint32>(data[pos + 2]) << 8) |
                           static_cast<quint32>(data[pos + 3]);
    const qsizetype typeOffset = pos + 4;
    const qsizetype dataOffset = pos + 8;
    const qsizetype chunkEnd = dataOffset + static_cast<qsizetype>(length) + 4;
    if (chunkEnd > png.size()) {
      return png.left(std::max<qsizetype>(8, png.size() / 2));
    }
    if (std::memcmp(png.constData() + typeOffset, "IDAT", 4) == 0 &&
        length > 1) {
      return png.left(dataOffset + static_cast<qsizetype>(length / 2));
    }
    pos = chunkEnd;
  }
  return png.left(std::max<qsizetype>(8, png.size() / 2));
}

void expectDecodeError(const QByteArray &bytes, const AssetDecodeLimits &limits,
                       AssetDecodeErrorCode code) {
  const auto decoded = decodeAssetImage(bytes, limits);
  QVERIFY(!decoded);
  QCOMPARE(decoded.error().code, code);
}

void expectDecodeSuccess(const QByteArray &bytes, const QSize &size,
                         const QSize &bound = QSize()) {
  const auto decoded = decodeAssetImage(bytes, AssetDecodeLimits{}, bound);
  QVERIFY2(decoded.has_value(),
           decoded ? "" : qPrintable(decoded.error().message));
  QCOMPARE(decoded->size(), size);
}

} // namespace

class AssetDecoderTests final : public QObject {
  Q_OBJECT

private slots:
  void validPngRoundTrip();
  void validJpegRoundTrip();
  void validAvifRoundTrip();
  void progressiveJpegDecodesUnderDefaultScanCap();
  void rejectsWrongMagic();
  void rejectsInvalidLimits();
  void rejectsOversizeByteCount();
  void rejectsPngOversizeDimensions();
  void rejectsJpegOversizeDimensions();
  void rejectsPngPixelBudget();
  void rejectsJpegPixelBudget();
  void rejectsAvifPixelBudget();
  void capsProgressiveJpegScansWithTrailingGarbage();
  void rejectsPngTruncatedInsideIdat();
  void rejectsJpegTruncatedWithWarning();
  void rejectsAvifTruncated();
  void rejectsCmykJpeg();
  void avifWithoutMetaBoxIsRejectedAsMalformed();
  void fittedSizeKeepsAspectAndNeverUpscales();
  void pngDecodesAtRequestedBound();
  void jpegDecodesAtRequestedBound();
  void jpegScaleDenominatorPicksLargestCoveringReduction();
  void avifDecodesAtRequestedBound();
  void boundLargerThanSourceDoesNotUpscale();
  void emptyBoundDecodesFullSize();
  void boundedDecodeStillEnforcesDeclaredLimits();
};

void AssetDecoderTests::validPngRoundTrip() {
  const QByteArray png = encodePngWithQt();
  QVERIFY(!png.isEmpty());
  expectDecodeSuccess(png, QSize(2, 2));
}

void AssetDecoderTests::validJpegRoundTrip() {
  const QByteArray jpeg = encodeJpeg();
  QVERIFY(!jpeg.isEmpty());
  expectDecodeSuccess(jpeg, QSize(8, 8));
}

void AssetDecoderTests::validAvifRoundTrip() {
  const QByteArray avif = encodeAvif();
  QVERIFY(!avif.isEmpty());
  expectDecodeSuccess(avif, QSize(2, 2));
}

void AssetDecoderTests::progressiveJpegDecodesUnderDefaultScanCap() {
  const QByteArray jpeg = encodeJpeg(true);
  QVERIFY(!jpeg.isEmpty());
  expectDecodeSuccess(jpeg, QSize(8, 8));
}

void AssetDecoderTests::rejectsWrongMagic() {
  expectDecodeError(QByteArrayLiteral("not an image"), AssetDecodeLimits{},
                    AssetDecodeErrorCode::UnrecognizedFormat);
}

void AssetDecoderTests::rejectsInvalidLimits() {
  const QByteArray png = encodePngWithQt();
  QVERIFY(!png.isEmpty());

  AssetDecodeLimits limits;
  limits.maxEncodedBytes = 0;
  expectDecodeError(png, limits, AssetDecodeErrorCode::InvalidLimits);
}

void AssetDecoderTests::rejectsOversizeByteCount() {
  const QByteArray png = encodePngWithQt();
  QVERIFY(png.size() > 1);

  AssetDecodeLimits limits;
  limits.maxEncodedBytes = png.size() - 1;
  expectDecodeError(png, limits, AssetDecodeErrorCode::EncodedBytesTooLarge);
}

void AssetDecoderTests::rejectsPngOversizeDimensions() {
  const QByteArray png = encodePngWithQt();
  QVERIFY(!png.isEmpty());

  AssetDecodeLimits limits;
  limits.maxWidth = 1;
  expectDecodeError(png, limits, AssetDecodeErrorCode::DimensionTooLarge);
}

void AssetDecoderTests::rejectsJpegOversizeDimensions() {
  const QByteArray jpeg = encodeJpeg();
  QVERIFY(!jpeg.isEmpty());

  AssetDecodeLimits limits;
  limits.maxHeight = 1;
  expectDecodeError(jpeg, limits, AssetDecodeErrorCode::DimensionTooLarge);
}

void AssetDecoderTests::rejectsPngPixelBudget() {
  const QByteArray png = encodePngWithQt();
  QVERIFY(!png.isEmpty());

  AssetDecodeLimits limits;
  limits.maxPixelCount = 1;
  expectDecodeError(png, limits, AssetDecodeErrorCode::PixelBudgetExceeded);
}

void AssetDecoderTests::rejectsJpegPixelBudget() {
  const QByteArray jpeg = encodeJpeg();
  QVERIFY(!jpeg.isEmpty());

  AssetDecodeLimits limits;
  limits.maxPixelCount = 1;
  expectDecodeError(jpeg, limits, AssetDecodeErrorCode::PixelBudgetExceeded);
}

void AssetDecoderTests::rejectsAvifPixelBudget() {
  const QByteArray avif = encodeAvif();
  QVERIFY(!avif.isEmpty());

  AssetDecodeLimits limits;
  limits.maxPixelCount = 1;
  expectDecodeError(avif, limits, AssetDecodeErrorCode::PixelBudgetExceeded);
}

void AssetDecoderTests::capsProgressiveJpegScansWithTrailingGarbage() {
  QByteArray jpeg = encodeJpeg(true);
  QVERIFY(!jpeg.isEmpty());
  jpeg.append("\xFF\xE1\xFF\xFF", 4);

  AssetDecodeLimits limits;
  limits.maxProgressiveJpegScans = 1;
  expectDecodeError(jpeg, limits,
                    AssetDecodeErrorCode::ProgressiveScanLimitExceeded);
}

void AssetDecoderTests::rejectsPngTruncatedInsideIdat() {
  const QByteArray png = encodePngWithQt();
  QVERIFY(!png.isEmpty());

  expectDecodeError(truncatePngInsideIdat(png), AssetDecodeLimits{},
                    AssetDecodeErrorCode::MalformedImage);
}

void AssetDecoderTests::rejectsJpegTruncatedWithWarning() {
  const QByteArray jpeg = encodeJpeg();
  QVERIFY(jpeg.size() > 16);

  expectDecodeError(jpeg.left(jpeg.size() - 2), AssetDecodeLimits{},
                    AssetDecodeErrorCode::MalformedImage);
}

void AssetDecoderTests::rejectsAvifTruncated() {
  const QByteArray avif = encodeAvif();
  QVERIFY(avif.size() > 32);

  expectDecodeError(avif.left(avif.size() / 2), AssetDecodeLimits{},
                    AssetDecodeErrorCode::MalformedImage);
}

void AssetDecoderTests::rejectsCmykJpeg() {
  const QByteArray jpeg = encodeJpeg(false, JCS_CMYK);
  if (jpeg.isEmpty()) {
    QSKIP("this libjpeg build could not encode a CMYK fixture");
  }

  expectDecodeError(jpeg, AssetDecodeLimits{},
                    AssetDecodeErrorCode::UnsupportedCodec);
}

void AssetDecoderTests::avifWithoutMetaBoxIsRejectedAsMalformed() {
  const QByteArray avif = encodeAvif();
  QVERIFY(!avif.isEmpty());

  const QByteArray withoutMetaBox = renameTopLevelMetaBoxToFree(avif);
  QVERIFY(!withoutMetaBox.isEmpty());

  const auto decoded = decodeAssetImage(withoutMetaBox);
  QVERIFY(!decoded);
  // libavif 1.4.2 reports this well-formed top-level box rewrite as
  // AVIF_RESULT_TRUNCATED_DATA, which maps to MalformedImage. Older libavif
  // versions may instead report the absence of a primary item directly, which
  // maps to UnsupportedCodec; both are acceptable decoder rejections.
  const AssetDecodeErrorCode code = decoded.error().code;
  QVERIFY(code == AssetDecodeErrorCode::MalformedImage ||
          code == AssetDecodeErrorCode::UnsupportedCodec);
}

void AssetDecoderTests::fittedSizeKeepsAspectAndNeverUpscales() {
  using Arkham::fittedAssetDecodeSize;
  using Arkham::normalizedAssetDecodeBound;
  QCOMPARE(fittedAssetDecodeSize(QSize(600, 840), QSize(150, 210)),
           QSize(150, 210));
  QCOMPARE(fittedAssetDecodeSize(QSize(600, 840), QSize(300, 210)),
           QSize(150, 210));
  QCOMPARE(fittedAssetDecodeSize(QSize(600, 840), QSize(100, 1000)),
           QSize(100, 140));
  QCOMPARE(fittedAssetDecodeSize(QSize(2, 2), QSize(150, 210)), QSize(2, 2));
  QCOMPARE(fittedAssetDecodeSize(QSize(600, 840), QSize()), QSize(600, 840));
  QCOMPARE(fittedAssetDecodeSize(QSize(600, 840), QSize(150, 0)),
           QSize(600, 840));
  QCOMPARE(normalizedAssetDecodeBound(QSize(150, 210)), QSize(150, 210));
  QCOMPARE(normalizedAssetDecodeBound(QSize(150, 0)), QSize());
  QCOMPARE(normalizedAssetDecodeBound(QSize(-1, -1)), QSize());
}

void AssetDecoderTests::pngDecodesAtRequestedBound() {
  const QByteArray png = encodePngWithQt(cardImage(QSize(600, 840)));
  QVERIFY(!png.isEmpty());
  expectDecodeSuccess(png, QSize(150, 210), QSize(150, 210));
  expectDecodeSuccess(png, QSize(100, 140), QSize(100, 1000));
}

void AssetDecoderTests::jpegDecodesAtRequestedBound() {
  const QByteArray jpeg = encodeJpeg(false, JCS_RGB, 600, 840);
  QVERIFY(!jpeg.isEmpty());
  // 1/4 DCT scaling lands exactly on the bound.
  expectDecodeSuccess(jpeg, QSize(150, 210), QSize(150, 210));
  // 1/4 scaling (150x210) covers 100x140; 1/8 (75x105) would not, so the
  // final fit downsamples from the 1/4 output.
  expectDecodeSuccess(jpeg, QSize(100, 140), QSize(100, 140));
  // A bound that is not a power-of-two reduction of an odd-sized source.
  const QByteArray odd = encodeJpeg(true, JCS_RGB, 601, 841);
  QVERIFY(!odd.isEmpty());
  expectDecodeSuccess(odd, QSize(150, 210), QSize(150, 210));
}

void AssetDecoderTests::jpegScaleDenominatorPicksLargestCoveringReduction() {
  using Arkham::fittedAssetDecodeSize;
  using Arkham::detail::jpegScaleDenominator;
  // The same call decodeJpegImage() makes: the target is the fitted size.
  const auto denominator = [](const QSize &source, const QSize &bound) {
    return jpegScaleDenominator(static_cast<unsigned int>(source.width()),
                                static_cast<unsigned int>(source.height()),
                                fittedAssetDecodeSize(source, bound));
  };
  // 1/4 lands exactly on the cell; 1/8 (75x105) would not cover it.
  QCOMPARE(denominator(QSize(600, 840), QSize(150, 210)), 4U);
  // 1/8 (75x105) is still too small for 100x140, so 1/4 is the largest.
  QCOMPARE(denominator(QSize(600, 840), QSize(100, 140)), 4U);
  QCOMPARE(denominator(QSize(600, 840), QSize(300, 420)), 2U);
  // More than half the source: no reduction covers it, so full resolution.
  QCOMPARE(denominator(QSize(600, 840), QSize(599, 839)), 1U);
  // An odd source fits to 149x210. 1/4 gives ceil(599/4) x ceil(839/4) =
  // 150x210, which covers it only because libjpeg rounds up; rounding down
  // (149x209) would force 1/2.
  QCOMPARE(fittedAssetDecodeSize(QSize(599, 839), QSize(150, 210)),
           QSize(149, 210));
  QCOMPARE(denominator(QSize(599, 839), QSize(150, 210)), 4U);
}

void AssetDecoderTests::avifDecodesAtRequestedBound() {
  const QByteArray avif = encodeAvif(cardImage(QSize(120, 168)));
  QVERIFY(!avif.isEmpty());
  expectDecodeSuccess(avif, QSize(120, 168));
  expectDecodeSuccess(avif, QSize(30, 42), QSize(30, 42));
  expectDecodeSuccess(avif, QSize(30, 42), QSize(60, 42));
}

void AssetDecoderTests::boundLargerThanSourceDoesNotUpscale() {
  expectDecodeSuccess(encodePngWithQt(), QSize(2, 2), QSize(150, 210));
  expectDecodeSuccess(encodeJpeg(), QSize(8, 8), QSize(150, 210));
  expectDecodeSuccess(encodeAvif(), QSize(2, 2), QSize(150, 210));
  // Fitting one axis exactly must not grow the other.
  expectDecodeSuccess(encodeJpeg(), QSize(8, 8), QSize(8, 400));
}

void AssetDecoderTests::emptyBoundDecodesFullSize() {
  const QByteArray png = encodePngWithQt(cardImage(QSize(600, 840)));
  QVERIFY(!png.isEmpty());
  expectDecodeSuccess(png, QSize(600, 840), QSize());
  expectDecodeSuccess(png, QSize(600, 840), QSize(150, 0));
  const QByteArray jpeg = encodeJpeg(false, JCS_RGB, 600, 840);
  QVERIFY(!jpeg.isEmpty());
  expectDecodeSuccess(jpeg, QSize(600, 840), QSize(0, 210));
}

void AssetDecoderTests::boundedDecodeStillEnforcesDeclaredLimits() {
  const QSize bound(150, 210);
  AssetDecodeLimits limits;
  limits.maxWidth = 599;

  const QByteArray png = encodePngWithQt(cardImage(QSize(600, 840)));
  QVERIFY(!png.isEmpty());
  auto decoded = decodeAssetImage(png, limits, bound);
  QVERIFY(!decoded);
  QCOMPARE(decoded.error().code, AssetDecodeErrorCode::DimensionTooLarge);

  const QByteArray jpeg = encodeJpeg(false, JCS_RGB, 600, 840);
  QVERIFY(!jpeg.isEmpty());
  decoded = decodeAssetImage(jpeg, limits, bound);
  QVERIFY(!decoded);
  QCOMPARE(decoded.error().code, AssetDecodeErrorCode::DimensionTooLarge);

  AssetDecodeLimits pixelLimits;
  pixelLimits.maxPixelCount = 600 * 840 - 1;
  decoded = decodeAssetImage(jpeg, pixelLimits, bound);
  QVERIFY(!decoded);
  QCOMPARE(decoded.error().code, AssetDecodeErrorCode::PixelBudgetExceeded);

  const QByteArray avif = encodeAvif(cardImage(QSize(120, 168)));
  QVERIFY(!avif.isEmpty());
  AssetDecodeLimits avifLimits;
  avifLimits.maxHeight = 167;
  decoded = decodeAssetImage(avif, avifLimits, QSize(30, 42));
  QVERIFY(!decoded);
  QCOMPARE(decoded.error().code, AssetDecodeErrorCode::DimensionTooLarge);
}

QTEST_APPLESS_MAIN(AssetDecoderTests)

#include "AssetDecoderTests.moc"
