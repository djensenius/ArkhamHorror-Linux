#include <cstdio>

#include <jpeglib.h>

#include <avif/avif.h>

#include <csetjmp>
#include <cstdlib>
#include <vector>

#include <QBuffer>
#include <QtTest>

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

QByteArray encodeWithQt(const char *format) {
  QByteArray bytes;
  QBuffer buffer(&bytes);
  if (!buffer.open(QIODevice::WriteOnly)) {
    return {};
  }
  if (!tinyImage().save(&buffer, format)) {
    return {};
  }
  return bytes;
}

QByteArray encodeAvif() {
  const QImage source = tinyImage().convertToFormat(QImage::Format_RGBA8888);
  avifImage *image = avifImageCreate(static_cast<uint32_t>(source.width()),
                                     static_cast<uint32_t>(source.height()), 8,
                                     AVIF_PIXEL_FORMAT_YUV444);
  if (!image) {
    return {};
  }

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  rgb.depth = 8;
  rgb.pixels = const_cast<uint8_t *>(source.constBits());
  rgb.rowBytes = static_cast<uint32_t>(source.bytesPerLine());

  avifResult result = avifImageRGBToYUV(image, &rgb);
  if (result != AVIF_RESULT_OK) {
    avifImageDestroy(image);
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

  avifRWData output = AVIF_DATA_EMPTY;
  result = avifEncoderAddImage(encoder, image, 1, AVIF_ADD_IMAGE_FLAG_SINGLE);
  if (result == AVIF_RESULT_OK) {
    result = avifEncoderFinish(encoder, &output);
  }

  QByteArray bytes;
  if (result == AVIF_RESULT_OK && output.data && output.size > 0) {
    bytes = QByteArray(reinterpret_cast<const char *>(output.data),
                       static_cast<qsizetype>(output.size));
  }
  avifRWDataFree(&output);
  avifEncoderDestroy(encoder);
  avifImageDestroy(image);
  return bytes;
}

struct JpegWriteErrorManager {
  jpeg_error_mgr pub;
  jmp_buf setjmpBuffer;
};

void jpegWriteErrorExit(j_common_ptr cinfo) {
  auto *errorManager = static_cast<JpegWriteErrorManager *>(cinfo->client_data);
  longjmp(errorManager->setjmpBuffer, 1);
}

QByteArray encodeProgressiveJpeg() {
  constexpr int width = 8;
  constexpr int height = 8;
  std::vector<unsigned char> pixels(static_cast<size_t>(width * height * 3));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t offset = static_cast<size_t>((y * width + x) * 3);
      pixels[offset] = static_cast<unsigned char>(x * 32);
      pixels[offset + 1] = static_cast<unsigned char>(y * 32);
      pixels[offset + 2] = static_cast<unsigned char>(200);
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
  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 90, TRUE);
  jpeg_simple_progression(&cinfo);
  jpeg_start_compress(&cinfo, TRUE);

  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW rowPointer[1] = {
        pixels.data() + static_cast<size_t>(cinfo.next_scanline) * width * 3};
    jpeg_write_scanlines(&cinfo, rowPointer, 1);
  }

  jpeg_finish_compress(&cinfo);
  QByteArray bytes(reinterpret_cast<const char *>(output),
                   static_cast<qsizetype>(outputSize));
  jpeg_destroy_compress(&cinfo);
  std::free(output);
  return bytes;
}

void expectDecodeError(const QByteArray &bytes, const AssetDecodeLimits &limits,
                       AssetDecodeErrorCode code) {
  const auto decoded = decodeAssetImage(bytes, limits);
  QVERIFY(!decoded);
  QCOMPARE(decoded.error().code, code);
}

} // namespace

class AssetDecoderTests final : public QObject {
  Q_OBJECT

private slots:
  void validPngRoundTrip();
  void validJpegRoundTrip();
  void validAvifRoundTrip();
  void rejectsTruncatedInput();
  void rejectsWrongMagic();
  void rejectsOversizeByteCount();
  void rejectsPngOversizeDimensions();
  void rejectsJpegOversizeDimensions();
  void capsProgressiveJpegScans();
  void rejectsAvifOversizeDimensions();
};

void AssetDecoderTests::validPngRoundTrip() {
  const QByteArray png = encodeWithQt("PNG");
  QVERIFY(!png.isEmpty());

  const auto decoded = decodeAssetImage(png);
  QVERIFY2(decoded.has_value(), qPrintable(decoded.error().message));
  QCOMPARE(decoded->size(), QSize(2, 2));
}

void AssetDecoderTests::validJpegRoundTrip() {
  const QByteArray jpeg = encodeWithQt("JPEG");
  QVERIFY(!jpeg.isEmpty());

  const auto decoded = decodeAssetImage(jpeg);
  QVERIFY2(decoded.has_value(), qPrintable(decoded.error().message));
  QCOMPARE(decoded->size(), QSize(2, 2));
}

void AssetDecoderTests::validAvifRoundTrip() {
  const QByteArray avif = encodeAvif();
  QVERIFY(!avif.isEmpty());

  const auto decoded = decodeAssetImage(avif);
  QVERIFY2(decoded.has_value(), qPrintable(decoded.error().message));
  QCOMPARE(decoded->size(), QSize(2, 2));
}

void AssetDecoderTests::rejectsTruncatedInput() {
  const QByteArray png = encodeWithQt("PNG");
  QVERIFY(png.size() > 8);

  expectDecodeError(png.left(8), AssetDecodeLimits{},
                    AssetDecodeErrorCode::MalformedImage);
}

void AssetDecoderTests::rejectsWrongMagic() {
  expectDecodeError(QByteArrayLiteral("not an image"), AssetDecodeLimits{},
                    AssetDecodeErrorCode::UnrecognizedFormat);
}

void AssetDecoderTests::rejectsOversizeByteCount() {
  const QByteArray png = encodeWithQt("PNG");
  QVERIFY(png.size() > 1);

  AssetDecodeLimits limits;
  limits.maxEncodedBytes = png.size() - 1;
  expectDecodeError(png, limits, AssetDecodeErrorCode::EncodedBytesTooLarge);
}

void AssetDecoderTests::rejectsPngOversizeDimensions() {
  const QByteArray png = encodeWithQt("PNG");
  QVERIFY(!png.isEmpty());

  AssetDecodeLimits limits;
  limits.maxWidth = 1;
  expectDecodeError(png, limits, AssetDecodeErrorCode::DimensionTooLarge);
}

void AssetDecoderTests::rejectsJpegOversizeDimensions() {
  const QByteArray jpeg = encodeWithQt("JPEG");
  QVERIFY(!jpeg.isEmpty());

  AssetDecodeLimits limits;
  limits.maxHeight = 1;
  expectDecodeError(jpeg, limits, AssetDecodeErrorCode::DimensionTooLarge);
}

void AssetDecoderTests::capsProgressiveJpegScans() {
  const QByteArray jpeg = encodeProgressiveJpeg();
  QVERIFY(!jpeg.isEmpty());

  AssetDecodeLimits limits;
  limits.maxProgressiveJpegScans = 1;
  expectDecodeError(jpeg, limits,
                    AssetDecodeErrorCode::ProgressiveScanLimitExceeded);
}

void AssetDecoderTests::rejectsAvifOversizeDimensions() {
  const QByteArray avif = encodeAvif();
  QVERIFY(!avif.isEmpty());

  AssetDecodeLimits limits;
  limits.maxWidth = 1;
  expectDecodeError(avif, limits, AssetDecodeErrorCode::DimensionTooLarge);
}

QTEST_APPLESS_MAIN(AssetDecoderTests)

#include "AssetDecoderTests.moc"
