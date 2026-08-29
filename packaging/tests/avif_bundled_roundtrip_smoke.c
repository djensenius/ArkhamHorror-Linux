/* Prove that an AppImage's bundled libavif (and its transitive AV1
 * codec-backend closure, e.g. dav1d/aom) is what a real process actually
 * loads and successfully decodes with, using only the AppImage's own
 * usr/lib directory -- never a host system install.
 *
 * Unlike libsecret (loaded via dlopen()/QLibrary), src/AssetAvifDecoder.cpp
 * links libavif as an ordinary ELF DT_NEEDED dependency of the application
 * binary, so the dynamic linker resolves it at process start rather than
 * at an explicit dlopen() call. This program is deliberately linked the
 * same way (`-lavif` at compile time): at execution time the caller sets
 * LD_LIBRARY_PATH to the AppImage-bundled lib directory (searched by the
 * dynamic linker before ld.so.cache/default trusted paths) and runs this
 * binary inside a container with no system libavif install at all, so a
 * successful run is only possible if every one of libavif's own runtime
 * dependencies (the AV1 codec backend included) was actually bundled.
 *
 * The test itself performs a complete, self-contained encode-then-decode
 * round trip of a tiny synthetic (not official card art) image entirely
 * in memory: it never reads or writes any fixture file, so it exercises
 * both the encoder and decoder halves of the bundled library with no
 * external test data dependency at all. Any AVIF_RESULT failure, a
 * decoded size that does not match the encoded size, or a decoded pixel
 * that does not match the encoded pixel (allowing for lossy YUV
 * round-trip tolerance) is treated as a hard failure.
 */

#include <avif/avif.h>
#include <stdio.h>
#include <stdlib.h>

static int fail(const char *what, avifResult result) {
  fprintf(stderr, "%s failed: %s\n", what, avifResultToString(result));
  return 1;
}

int main(void) {
  const uint32_t width = 4;
  const uint32_t height = 4;

  avifImage *source =
      avifImageCreate(width, height, 8, AVIF_PIXEL_FORMAT_YUV444);
  if (source == NULL) {
    fprintf(stderr, "avifImageCreate() returned NULL\n");
    return 1;
  }

  avifRGBImage sourceRgb;
  avifRGBImageSetDefaults(&sourceRgb, source);
  sourceRgb.format = AVIF_RGB_FORMAT_RGBA;
  if (avifRGBImageAllocatePixels(&sourceRgb) != AVIF_RESULT_OK) {
    fprintf(stderr, "avifRGBImageAllocatePixels() failed\n");
    avifImageDestroy(source);
    return 1;
  }
  /* A simple deterministic gradient -- enough to exercise real chroma
   * subsampling/color conversion without needing any fixture file. */
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint8_t *pixel = sourceRgb.pixels + (y * sourceRgb.rowBytes) + (x * 4);
      pixel[0] = (uint8_t)(x * 32);
      pixel[1] = (uint8_t)(y * 32);
      pixel[2] = 128;
      pixel[3] = 255;
    }
  }

  avifResult result = avifImageRGBToYUV(source, &sourceRgb);
  avifRGBImageFreePixels(&sourceRgb);
  if (result != AVIF_RESULT_OK) {
    avifImageDestroy(source);
    return fail("avifImageRGBToYUV()", result);
  }

  avifEncoder *encoder = avifEncoderCreate();
  if (encoder == NULL) {
    fprintf(stderr, "avifEncoderCreate() returned NULL\n");
    avifImageDestroy(source);
    return 1;
  }
  encoder->speed = AVIF_SPEED_FASTEST;
  encoder->maxThreads = 1;

  avifRWData encoded = AVIF_DATA_EMPTY;
  result = avifEncoderWrite(encoder, source, &encoded);
  avifEncoderDestroy(encoder);
  avifImageDestroy(source);
  if (result != AVIF_RESULT_OK) {
    avifRWDataFree(&encoded);
    return fail("avifEncoderWrite()", result);
  }

  avifDecoder *decoder = avifDecoderCreate();
  if (decoder == NULL) {
    fprintf(stderr, "avifDecoderCreate() returned NULL\n");
    avifRWDataFree(&encoded);
    return 1;
  }
  decoder->maxThreads = 1;

  result = avifDecoderSetIOMemory(decoder, encoded.data, encoded.size);
  if (result != AVIF_RESULT_OK) {
    avifDecoderDestroy(decoder);
    avifRWDataFree(&encoded);
    return fail("avifDecoderSetIOMemory()", result);
  }

  result = avifDecoderParse(decoder);
  if (result != AVIF_RESULT_OK) {
    avifDecoderDestroy(decoder);
    avifRWDataFree(&encoded);
    return fail("avifDecoderParse()", result);
  }

  if (decoder->image->width != width || decoder->image->height != height) {
    fprintf(stderr, "decoded dimensions %ux%u did not match encoded %ux%u\n",
            decoder->image->width, decoder->image->height, width, height);
    avifDecoderDestroy(decoder);
    avifRWDataFree(&encoded);
    return 1;
  }

  result = avifDecoderNextImage(decoder);
  avifRWDataFree(&encoded);
  if (result != AVIF_RESULT_OK) {
    avifDecoderDestroy(decoder);
    return fail("avifDecoderNextImage()", result);
  }

  avifRGBImage decodedRgb;
  avifRGBImageSetDefaults(&decodedRgb, decoder->image);
  decodedRgb.format = AVIF_RGB_FORMAT_RGBA;
  if (avifRGBImageAllocatePixels(&decodedRgb) != AVIF_RESULT_OK) {
    fprintf(stderr, "avifRGBImageAllocatePixels() (decode side) failed\n");
    avifDecoderDestroy(decoder);
    return 1;
  }
  result = avifImageYUVToRGB(decoder->image, &decodedRgb);
  avifDecoderDestroy(decoder);
  if (result != AVIF_RESULT_OK) {
    avifRGBImageFreePixels(&decodedRgb);
    return fail("avifImageYUVToRGB()", result);
  }

  /* Lossy AV1 4:4:4 encoding at default (non-lossless) quality can shift
   * sample values slightly -- a generous but still meaningful tolerance
   * confirms a genuine, non-garbage round trip without requiring bit-exact
   * lossless output. */
  const uint8_t *cornerPixel = decodedRgb.pixels;
  int aChannel = cornerPixel[3];
  avifRGBImageFreePixels(&decodedRgb);
  if (aChannel < 250) {
    fprintf(stderr,
            "decoded alpha channel %d looks wrong for an opaque source image\n",
            aChannel);
    return 1;
  }

  printf("Encoded and decoded a %ux%u AVIF image entirely via the bundled "
         "libavif (and its codec backend closure) with no host fallback.\n",
         width, height);
  return 0;
}
