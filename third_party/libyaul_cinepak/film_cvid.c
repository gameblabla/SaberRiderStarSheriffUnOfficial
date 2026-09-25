#include "film_cvid.h"
#include "film_snd.h"

#ifndef CLAMP
#  define CLAMP(V) ((V) > (255) ? (255) : (V) < (0) ? (0) : (V))
#endif

strip_codebook_t *currentCodebook = 0;
strip_codebook_t *prevCodebook = 0;
bool copyingBlocks = false;
static uint8_t clampLUT24[512];
static uint8_t clampLUT15[512];

typedef struct {
  union {
    uint32_t l;
    uint8_t b[4];
  } flagsAndCvidLength;

  int16_t width;
  int16_t height;
  int16_t numStrips;
  int16_t padding;
} videoHeader;

void initClampLUT24(void) {
  for (int i = -128; i < 384; i++) {
    uint8_t clamped;
    if (i < 0) {
      clamped = 0;
    } else if (i > 255) {
      clamped = 255;
    } else {
      clamped = (uint8_t) i;
    }
    clampLUT24[i + 128] = clamped;
    clampLUT15[i + 128] = clamped >> 3;
  }
}


void codebookRGB_24(decode_work_t *work, strip_codebook_t *codebookPtr, bool isV4,
  uint16_t chunkID, int32_t remaining) {
  uint32_t *codebookRGBPtr = isV4 ?
    codebookPtr->v4RGB->color : codebookPtr->v1RGB->color;

  int32_t iter = 0;
  if (chunkID != 0x2000 && chunkID != 0x2200) {
    uint8_t *readPtr = work->stream.sampleCache.readPos;
    do {
      int32_t flags = (readPtr[0] << 24) | (readPtr[1] << 16) |
        (readPtr[2] << 8) | (readPtr[3]);
      readPtr += 4;
      iter += 4;
      int8_t shifts = 32;
      do {
        if (flags & 0x80000000) {
          codebook_t *book = (codebook_t *) readPtr;
          // | r |   | 1.0  0.0  2.0 | | y |
          // | g | = | 1.0 -0.5 -1.0 | | u |
          // | b |   | 1.0  2.0  0.0 | | v |

          int cr = (book->v << 1);
          int cg = -(book->u >> 1) - book->v;
          int cb = (book->u << 1);
          int y = book->y[0] + 128;

          codebookRGBPtr[0] = clampLUT24[y + cr] | clampLUT24[y + cb] << 0x10 |
            clampLUT24[y + cg] << 8 | 0x80000000;

          y = book->y[1] + 128;
          codebookRGBPtr[1] = clampLUT24[y + cr] | clampLUT24[y + cb] << 0x10 |
            clampLUT24[y + cg] << 8 | 0x80000000;

          y = book->y[2] + 128;
          codebookRGBPtr[2] = clampLUT24[y + cr] | clampLUT24[y + cb] << 0x10 |
            clampLUT24[y + cg] << 8 | 0x80000000;

          y = book->y[3] + 128;
          codebookRGBPtr[3] = clampLUT24[y + cr] | clampLUT24[y + cb] << 0x10 |
            clampLUT24[y + cg] << 8 | 0x80000000;

          readPtr += 6;
          iter += 6;
        }
        codebookRGBPtr+=4;
        if (remaining < iter) {
          return;
        }
        flags <<= 1;
        shifts--;
      } while (shifts != 0);
    } while (true);
  } else {
      codebook_t *book = (codebook_t *) work->stream.sampleCache.readPos;
    do {
      // | r |   | 1.0  0.0  2.0 | | y |
      // | g | = | 1.0 -0.5 -1.0 | | u |
      // | b |   | 1.0  2.0  0.0 | | v |

      int cr = (book->v << 1);
      int cg = -(book->u >> 1) - book->v;
      int cb = (book->u << 1);
      int y = book->y[0] + 128;

      codebookRGBPtr[0] = clampLUT24[y + cr] | clampLUT24[y + cb] << 0x10 |
        clampLUT24[y + cg] << 8 | 0x80000000;

      y = book->y[1] + 128;
      codebookRGBPtr[1] = clampLUT24[y + cr] | clampLUT24[y + cb] << 0x10 |
        clampLUT24[y + cg] << 8 | 0x80000000;

      y = book->y[2] + 128;
      codebookRGBPtr[2] = clampLUT24[y + cr] | clampLUT24[y + cb] << 0x10 |
        clampLUT24[y + cg] << 8 | 0x80000000;

      y = book->y[3] + 128;
      codebookRGBPtr[3] = clampLUT24[y + cr] | clampLUT24[y + cb] << 0x10 |
        clampLUT24[y + cg] << 8 | 0x80000000;

      book++;
      codebookRGBPtr+=4;
      iter += 6;
    } while (iter < remaining);
  }
}

static inline uint16_t codebook_rgb1555(const codebook_t *book, int idx) {
  int cr = (book->v << 1);
  int cg = -(book->u >> 1) - book->v;
  int cb = (book->u << 1);
  int y = book->y[idx] + 128;
  return (uint16_t) (0x8000 |
    clampLUT15[y + cr] |
    (clampLUT15[y + cg] << 5) |
    (clampLUT15[y + cb] << 10));
}

void codebookRGB_15(decode_work_t *work, strip_codebook_t *codebookPtr,
  bool isV4, uint16_t chunkID, int32_t remaining) {
  codebookRGB_t *entry = isV4 ? codebookPtr->v4RGB : codebookPtr->v1RGB;
  int32_t iter = 0;

  if (chunkID != 0x2000 && chunkID != 0x2200) {
    uint8_t *readPtr = work->stream.sampleCache.readPos;
    do {
      if (remaining - iter < 4) return;
      uint32_t flags = ((uint32_t) readPtr[0] << 24) |
        ((uint32_t) readPtr[1] << 16) | ((uint32_t) readPtr[2] << 8) |
        readPtr[3];
      readPtr += 4;
      iter += 4;
      for (int bit = 0; bit < 32; bit++, entry++, flags <<= 1) {
        if (flags & 0x80000000u) {
          if (remaining - iter < 6) return;
          const codebook_t *book = (const codebook_t *) readPtr;
          uint16_t p0 = codebook_rgb1555(book, 0);
          uint16_t p1 = codebook_rgb1555(book, 1);
          uint16_t p2 = codebook_rgb1555(book, 2);
          uint16_t p3 = codebook_rgb1555(book, 3);
          entry->color[0] = ((uint32_t) p0 << 16) | p1;
          entry->color[1] = ((uint32_t) p2 << 16) | p3;
          readPtr += 6;
          iter += 6;
        }
        if (iter >= remaining) return;
      }
    } while (true);
  } else {
    const codebook_t *book = (const codebook_t *) work->stream.sampleCache.readPos;
    while (iter + 6 <= remaining) {
      uint16_t p0 = codebook_rgb1555(book, 0);
      uint16_t p1 = codebook_rgb1555(book, 1);
      uint16_t p2 = codebook_rgb1555(book, 2);
      uint16_t p3 = codebook_rgb1555(book, 3);
      entry->color[0] = ((uint32_t) p0 << 16) | p1;
      entry->color[1] = ((uint32_t) p2 << 16) | p3;
      book++;
      entry++;
      iter += 6;
    }
  }
}

void stripdata_new(stripdata_t *data, uint32_t height, uint32_t width) {
  data->strip = 0;
  data->writeX = data->topX = 0;
  data->bottomX = width;
  data->writeY = data->topY = 0;
  data->bottomY = height;
}

void dmaCopyBlocksDone(void *data __unused) {
  cpu_cache_purge();
  copyingBlocks = false;
}

inline void stripdata_copyLastCodebooks(stripdata_t *data) {

  cpu_dmac_cfg_t cfg = {
    .channel = 0,
    .src_mode = CPU_DMAC_SOURCE_INCREMENT,
    .dst_mode = CPU_DMAC_DESTINATION_INCREMENT,
    .stride = CPU_DMAC_STRIDE_16_BYTES,
    .bus_mode = CPU_DMAC_BUS_MODE_BURST,
    .src = CPU_CACHE_THROUGH | (uint32_t) &data->codebooks[data->strip - 1],
    .dst = CPU_CACHE_THROUGH | (uint32_t) &data->codebooks[data->strip],
    .len = sizeof(strip_codebook_t),
    .ihr = dmaCopyBlocksDone,
    .ihr_work = NULL,
  };

  copyingBlocks = true;

  cpu_dmac_channel_config_set(&cfg);
  cpu_dmac_channel_start(0);
}

void stripdata_copyLastCodebooks_nodma(stripdata_t *data) {
  memcpy(&data->codebooks[1], &data->codebooks[0], sizeof(strip_codebook_t));
}

static void decodeIntra15(decode_work_t *work, strip_codebook_t *codebookPtr, uint16_t chunkDataLength,
  bool skipStripData, uint32_t quarterY, uint32_t quarterX) {
  int32_t remainingSectionBytes = chunkDataLength - 4;
  uint8_t *src = work->stream.sampleCache.readPos;
  int32_t vramwidth = work->filmHeader.fdsc.width;

  uint16_t rgb1, rgb2, rgb3, rgb4 = 0;

  int32_t flags = 0;
  int32_t shifts = 0;
  int32_t vramDelta = vramwidth * 3;
  int32_t xPos = quarterX;
  codebookRGB_t *codebook_rgbval;
  uint16_t *y0 = work->decodeParams->vramWritePos;
  uint16_t *y1 = y0 + vramwidth;
  uint16_t *y2 = y1 + vramwidth;
  uint16_t *y3 = y2 + vramwidth;
  int32_t isV4 = 0;

  if ((quarterY != 0) && (quarterX != 0)) {
    shifts = 1;
    xPos = quarterX;
    if (skipStripData == true) {
      shifts = -1;
      flags = 0;
    }
    do {
      do {
        shifts--;
        if (shifts == 0) {
          flags = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
          src += 4;
          shifts = 32;
        }
        isV4 = flags & 0x80000000;
        flags <<= 1;
        if (isV4 == 0) {
          codebook_rgbval = &codebookPtr->v1RGB[src[0]];
          codebook_rgbval->color[0];
          rgb1 = codebook_rgbval->color[0] & 0xFFFF;
          rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
          y0[0] = rgb2;
          y0[1] = rgb2;
          y1[0] = rgb2;
          y1[1] = rgb2;
          y0[2] = rgb1;
          y0[3] = rgb1;
          y1[2] = rgb1;
          y1[3] = rgb1;
          rgb1 = codebook_rgbval->color[1] & 0xFFFF;
          rgb2 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
          y2[0] = rgb2;
          y2[1] = rgb2;
          y3[0] = rgb2;
          y3[1] = rgb2;
          y2[2] = rgb1;
          y2[3] = rgb1;
          y3[2] = rgb1;
          y3[3] = rgb1;
          src++;
        } else {
          codebook_rgbval = &codebookPtr->v4RGB[src[0]];

          rgb1 = codebook_rgbval->color[0] & 0xFFFF;
          rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
          rgb3 = codebook_rgbval->color[1] & 0xFFFF;
          rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
          y0[0] = rgb2;
          y0[1] = rgb1;
          y1[0] = rgb4;
          y1[1] = rgb3;

          codebook_rgbval = &codebookPtr->v4RGB[src[1]];
          rgb1 = codebook_rgbval->color[0] & 0xFFFF;
          rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
          rgb3 = codebook_rgbval->color[1] & 0xFFFF;
          rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
          y0[2] = rgb2;
          y0[3] = rgb1;
          y1[2] = rgb4;
          y1[3] = rgb3;

          codebook_rgbval = &codebookPtr->v4RGB[src[2]];
          rgb1 = codebook_rgbval->color[0] & 0xFFFF;
          rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
          rgb3 = codebook_rgbval->color[1] & 0xFFFF;
          rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
          y2[0] = rgb2;
          y2[1] = rgb1;
          y3[0] = rgb4;
          y3[1] = rgb3;

          codebook_rgbval = &codebookPtr->v4RGB[src[3]];
          rgb1 = codebook_rgbval->color[0] & 0xFFFF;
          rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
          rgb3 = codebook_rgbval->color[1] & 0xFFFF;
          rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
          y2[2] = rgb2;
          y2[3] = rgb1;
          y3[2] = rgb4;
          y3[3] = rgb3;
          src += 4;
        }

        y0 += 4;
        y1 += 4;
        y2 += 4;
        y3 += 4;
        xPos--;
      } while (xPos != 0);
      y0 += vramDelta;
      y1 += vramDelta;
      y2 += vramDelta;
      y3 += vramDelta;
      quarterY--;
      xPos = quarterX;
    } while (quarterY != 0);
  }
  work->decodeParams->vramWritePos = y0;
  work->stream.sampleCache.readPos += chunkDataLength;
}

static void decodeInter15(decode_work_t *work, strip_codebook_t *codebookPtr,
  uint16_t chunkDataLength,
  int32_t quarterY, int32_t quarterX) {
  int32_t remainingSectionBytes = chunkDataLength - 4;
  uint8_t *src = work->stream.sampleCache.readPos;
  int32_t vramwidth = work->filmHeader.fdsc.width;

  int16_t rgb1, rgb2, rgb3, rgb4 = 0;

  int32_t flags = 0;
  int32_t shifts = 0;
  int32_t vramDelta = vramwidth * 3;

  int32_t xPos = quarterX;
  codebookRGB_t *codebook_rgbval;
  uint16_t *y0 = work->decodeParams->vramWritePos;
  uint16_t *y1 = y0 + vramwidth;
  uint16_t *y2 = y1 + vramwidth;
  uint16_t *y3 = y2 + vramwidth;
  int32_t isV4 = 0;

  if ((quarterY != 0) && (quarterX != 0)) {
    shifts = 1;
    xPos = quarterX;
    do {
      do {
        shifts--;
        if (shifts == 0) {
          flags = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
          src += 4;
          shifts = 32;
        }
        isV4 = flags & 0x80000000;
        flags <<= 1;
        if (isV4 != 0) {
          shifts--;
          if (shifts == 0) {
            flags = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
            src += 4;
            shifts = 32;
          }
          isV4 = flags & 0x80000000;
          flags <<= 1;
          if (isV4 == 0) {
            codebook_rgbval = &codebookPtr->v1RGB[src[0]];
            rgb1 = codebook_rgbval->color[0] & 0xFFFF;
            rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
            y0[0] = rgb2;
            y0[1] = rgb2;
            y1[0] = rgb2;
            y1[1] = rgb2;
            y0[2] = rgb1;
            y0[3] = rgb1;
            y1[2] = rgb1;
            y1[3] = rgb1;
            rgb1 = codebook_rgbval->color[1] & 0xFFFF;
            rgb2 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
            y2[0] = rgb2;
            y2[1] = rgb2;
            y3[0] = rgb2;
            y3[1] = rgb2;
            y2[2] = rgb1;
            y2[3] = rgb1;
            y3[2] = rgb1;
            y3[3] = rgb1;
            src++;
          } else {
            codebook_rgbval = &codebookPtr->v4RGB[src[0]];
            rgb1 = codebook_rgbval->color[0] & 0xFFFF;
            rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
            rgb3 = codebook_rgbval->color[1] & 0xFFFF;
            rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
            y0[0] = rgb2;
            y0[1] = rgb1;
            y1[0] = rgb4;
            y1[1] = rgb3;

            codebook_rgbval = &codebookPtr->v4RGB[src[1]];
            rgb1 = codebook_rgbval->color[0] & 0xFFFF;
            rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
            rgb3 = codebook_rgbval->color[1] & 0xFFFF;
            rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
            y0[2] = rgb2;
            y0[3] = rgb1;
            y1[2] = rgb4;
            y1[3] = rgb3;

            codebook_rgbval = &codebookPtr->v4RGB[src[2]];
            rgb1 = codebook_rgbval->color[0] & 0xFFFF;
            rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
            rgb3 = codebook_rgbval->color[1] & 0xFFFF;
            rgb4 = codebook_rgbval->color[1] >> 16 & 0xFFFF;
            y2[0] = rgb2;
            y2[1] = rgb1;
            y3[0] = rgb4;
            y3[1] = rgb3;

            codebook_rgbval = &codebookPtr->v4RGB[src[3]];
            rgb1 = codebook_rgbval->color[0] & 0xFFFF;
            rgb2 = codebook_rgbval->color[0] >> 16 & 0xFFFF;
            rgb3 = codebook_rgbval->color[1] & 0xFFFF;
            rgb4 = codebook_rgbval->color[1]  >> 16 & 0xFFFF;
            y2[2] = rgb2;
            y2[3] = rgb1;
            y3[2] = rgb4;
            y3[3] = rgb3;
            src += 4;
          }
        }
        y0 += 4;
        y1 += 4;
        y2 += 4;
        y3 += 4;
        xPos--;
      } while (xPos != 0);
      y0 += vramDelta;
      y1 += vramDelta;
      y2 += vramDelta;
      y3 += vramDelta;
      quarterY--;
      xPos = quarterX;
    } while (quarterY != 0);
  }
  work->decodeParams->vramWritePos = y0;
  work->stream.sampleCache.readPos += chunkDataLength;
}

void decodeInter24(decode_work_t *work, strip_codebook_t *codebookPtr,
  uint16_t chunkDataLength,
  uint32_t quarterY, uint32_t quarterX) {
    int32_t remainingSectionBytes = chunkDataLength - 4;
    uint8_t *src = work->stream.sampleCache.readPos;
    int32_t vramwidth = work->filmHeader.fdsc.width;
    int32_t rgb1, rgb2, rgb3, rgb4 = 0;

    int32_t flags = 0;
    int32_t shifts = 0;
    int32_t vramDelta = vramwidth * 3;

    int32_t xPos = quarterX;
    int32_t *codebook_rgbval;
    int32_t *y0 = work->decodeParams->vramWritePos;
    int32_t *y1 = y0 + vramwidth;
    int32_t *y2 = y1 + vramwidth;
    int32_t *y3 = y2 + vramwidth;
    int32_t isV4 = 0;

    if ((quarterY != 0) && (quarterX != 0)) {
      shifts = 1;
      xPos = quarterX;
      do {
          do {
          shifts--;
          if (shifts == 0) {
            flags = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
                src += 4;
                shifts = 32;
          }
          isV4 = flags & 0x80000000;
          flags <<= 1;
          if (isV4 != 0) {
            shifts--;
            if (shifts == 0) {
              flags = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
              src += 4;
              shifts = 32;
            }
            isV4 = flags & 0x80000000;
            flags <<= 1;
            if (isV4 == 0) {
                codebook_rgbval = &codebookPtr->v1RGB[src[0]].color;
                rgb1 = codebook_rgbval[0];
                rgb2 = codebook_rgbval[1];
                y0[0] = rgb1;
                y0[1] = rgb1;
                y0[2] = rgb2;
                y0[3] = rgb2;
                y1[0] = rgb1;
                y1[1] = rgb1;
                y1[2] = rgb2;
                y1[3] = rgb2;
                rgb1 = codebook_rgbval[2];
                rgb2 = codebook_rgbval[3];
                y2[0] = rgb1;
                y2[1] = rgb1;
                y2[2] = rgb2;
                y2[3] = rgb2;
                y3[0] = rgb1;
                y3[1] = rgb1;
                y3[2] = rgb2;
                y3[3] = rgb2;
                src++;
            } else {
              codebook_rgbval = &codebookPtr->v4RGB[src[0]].color;
              rgb1 = codebook_rgbval[0];
              rgb2 = codebook_rgbval[1];
              rgb3 = codebook_rgbval[2];
              rgb4 = codebook_rgbval[3];
              y0[0] = rgb1;
              y0[1] = rgb2;
              y1[0] = rgb3;
              y1[1] = rgb4;

             codebook_rgbval = &codebookPtr->v4RGB[src[1]].color;
              rgb1 = codebook_rgbval[0];
              rgb2 = codebook_rgbval[1];
              rgb3 = codebook_rgbval[2];
              rgb4 = codebook_rgbval[3];
             y0[2] = rgb1;
             y0[3] = rgb2;
             y1[2] = rgb3;
             y1[3] = rgb4;

             codebook_rgbval = &codebookPtr->v4RGB[src[2]].color;
              rgb1 = codebook_rgbval[0];
              rgb2 = codebook_rgbval[1];
              rgb3 = codebook_rgbval[2];
              rgb4 = codebook_rgbval[3];
             y2[0] = rgb1;
             y2[1] = rgb2;
             y3[0] = rgb3;
             y3[1] = rgb4;

             codebook_rgbval = &codebookPtr->v4RGB[src[3]].color;
              rgb1 = codebook_rgbval[0];
              rgb2 = codebook_rgbval[1];
              rgb3 = codebook_rgbval[2];
              rgb4 = codebook_rgbval[3];
             y2[2] = rgb1;
             y2[3] = rgb2;
             y3[2] = rgb3;
             y3[3] = rgb4;
             src += 4;
            }
          }
          y0 += 4;
          y1 += 4;
          y2 += 4;
          y3 += 4;
          xPos--;
          } while (xPos != 0);
          y0 += vramDelta;
          y1 += vramDelta;
          y2 += vramDelta;
          y3 += vramDelta;
          quarterY--;
          xPos = quarterX;
      } while (quarterY != 0);
    }
    work->decodeParams->vramWritePos = y0;
    work->stream.sampleCache.readPos += chunkDataLength;
}

void decodeIntra24(decode_work_t *work, strip_codebook_t *codebookPtr, uint16_t chunkDataLength,
  bool skipStripData, uint32_t quarterY, uint32_t quarterX) {

  int32_t remainingSectionBytes = chunkDataLength - 4;
  uint8_t *src = work->stream.sampleCache.readPos;
  int32_t vramwidth = work->filmHeader.fdsc.width;
  int32_t y = work->stripData.writeY;

  int32_t rgb1, rgb2, rgb3, rgb4 = 0;

  int32_t flags = 0;
  int32_t shifts = 0;
  int32_t vramDelta = vramwidth * 3;

  int32_t xPos = quarterX;
  int32_t *codebook_rgbval;
  int32_t *y0 = work->decodeParams->vramWritePos;
  int32_t *y1 = y0 + vramwidth;
  int32_t *y2 = y1 + vramwidth;
  int32_t *y3 = y2 + vramwidth;
  int32_t isV4 = 0;

  if ((quarterY != 0) && (quarterX != 0)) {
    shifts = 1;
    xPos = quarterX;
    if (skipStripData == true) {
      shifts = -1;
      flags = 0;
    }
    do {
      do {
        shifts--;
        if (shifts == 0) {
          flags = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
          src += 4;
          shifts = 32;
        }
        isV4 = flags & 0x80000000;
        flags <<= 1;
        if (isV4 == 0) {
            codebook_rgbval =&codebookPtr->v1RGB[src[0]].color;
            rgb1 = codebook_rgbval[0];
            rgb2 = codebook_rgbval[1];
            y0[0] = rgb1;
            y0[1] = rgb1;
            y0[2] = rgb2;
            y0[3] = rgb2;
            y1[0] = rgb1;
            y1[1] = rgb1;
            y1[2] = rgb2;
            y1[3] = rgb2;
            rgb1 = codebook_rgbval[2];
            rgb2 = codebook_rgbval[3];
            y2[0] = rgb1;
            y2[1] = rgb1;
            y2[2] = rgb2;
            y2[3] = rgb2;
            y3[0] = rgb1;
            y3[1] = rgb1;
            y3[2] = rgb2;
            y3[3] = rgb2;
            src++;
        } else {
            codebook_rgbval =&codebookPtr->v4RGB[src[0]].color;
            rgb1 = codebook_rgbval[0];
            rgb2 = codebook_rgbval[1];
            rgb3 = codebook_rgbval[2];
            rgb4 = codebook_rgbval[3];
            y0[0] = rgb1;
            y0[1] = rgb2;
            y1[0] = rgb3;
            y1[1] = rgb4;

            codebook_rgbval = &codebookPtr->v4RGB[src[1]].color;
            rgb1 = codebook_rgbval[0];
            rgb2 = codebook_rgbval[1];
            rgb3 = codebook_rgbval[2];
            rgb4 = codebook_rgbval[3];
            y0[2] = rgb1;
            y0[3] = rgb2;
            y1[2] = rgb3;
            y1[3] = rgb4;

            codebook_rgbval = &codebookPtr->v4RGB[src[2]].color;
            rgb1 = codebook_rgbval[0];
            rgb2 = codebook_rgbval[1];
            rgb3 = codebook_rgbval[2];
            rgb4 = codebook_rgbval[3];
            y2[0] = rgb1;
            y2[1] = rgb2;
            y3[0] = rgb3;
            y3[1] = rgb4;

            codebook_rgbval = &codebookPtr->v4RGB[src[3]].color;
            rgb1 = codebook_rgbval[0];
            rgb2 = codebook_rgbval[1];
            rgb3 = codebook_rgbval[2];
            rgb4 = codebook_rgbval[3];
            y2[2] = rgb1;
            y2[3] = rgb2;
            y3[2] = rgb3;
            y3[3] = rgb4;
            src += 4;
        }

        y0 += 4;
        y1 += 4;
        y2 += 4;
        y3 += 4;
        xPos--;
      } while (xPos != 0);
      y0 += vramDelta;
      y1 += vramDelta;
      y2 += vramDelta;
      y3 += vramDelta;
      quarterY--;
      xPos = quarterX;
    } while (quarterY != 0);
  }
  work->decodeParams->vramWritePos = y0;
  work->stream.sampleCache.readPos += chunkDataLength;
}

void parseVideo(decode_work_t *work) {

  videoHeader *cvidHeader;
  cvidHeader = work->stream.sampleCache.readPos;
  work->stream.sampleCache.readPos += sizeof(videoHeader);
  int16_t numStrips = cvidHeader->numStrips;
  int16_t stripNum = 0;
  const bool copyLastCodeBooks = !(cvidHeader->flagsAndCvidLength.b[0] & 0x1);
  int16_t lastBottomY = 0;

  uint8_t *sampleEnd = work->nextSample.offset + work->nextSample.length;

  int8_t colorDepth = work->decodeParams->decodeColorDepth;
  void (*decodeIntra)(decode_work_t *work, strip_codebook_t *codebookPtr, uint16_t chunkDataLength, bool skipStripData, uint32_t quarterY, uint32_t quarterX);
  void (*decodeInter)(decode_work_t * work, strip_codebook_t *codebookPtr, uint16_t chunkDataLength, uint32_t quarterY, uint32_t quarterX);
  void (*codebookRGB_new)(decode_work_t * work, strip_codebook_t *codebookPtr, bool isV4, uint16_t chunkID, uint32_t remaining);

  if (colorDepth == COLOR_DEPTH_24) {
    decodeIntra = &decodeIntra24;
    decodeInter = &decodeInter24;
    codebookRGB_new = &codebookRGB_24;
  } else {
    decodeIntra = &decodeIntra15;
    decodeInter = &decodeInter15;
    codebookRGB_new = &codebookRGB_15;
  }

  if (numStrips != 0) {
      do {
          work->stripData.strip = stripNum;

          if (stripNum > 0 && copyLastCodeBooks) {
            /* The next strip consumes this codebook immediately.  The original
             * asynchronous SH-2 DMAC copy can still be in flight (and can
             * deadlock in an application that also owns DMA channel 0), so do
             * the small copy synchronously. */
            stripdata_copyLastCodebooks_nodma(&work->stripData);
          }
          currentCodebook = &work->stripData.codebooks[stripNum > 0 ? 1 : 0];

          /* Cinepak strip lengths may be odd, so every strip after the first
           * is not guaranteed to be 16-bit aligned.  SH-2 faults on an
           * unaligned uint16_t load: parse the 12-byte strip header bytewise. */
          const uint8_t *tmpStripBuffer = work->stream.sampleCache.readPos;
          work->stream.sampleCache.readPos += 12;

          uint32_t stripDataLength = ((uint32_t)tmpStripBuffer[1] << 16) |
            ((uint32_t)tmpStripBuffer[2] << 8) | tmpStripBuffer[3];

          work->stripData.topY = work->stripData.writeY =
            (int32_t)(((uint16_t)tmpStripBuffer[4] << 8) | tmpStripBuffer[5]);
          work->stripData.topX = work->stripData.writeX =
            (int32_t)(((uint16_t)tmpStripBuffer[6] << 8) | tmpStripBuffer[7]);
          work->stripData.bottomY =
            (int32_t)(((uint16_t)tmpStripBuffer[8] << 8) | tmpStripBuffer[9]);
          work->stripData.bottomX =
            (int32_t)(((uint16_t)tmpStripBuffer[10] << 8) | tmpStripBuffer[11]);

          work->stripData.topY = work->stripData.writeY = lastBottomY;
          work->stripData.bottomY += lastBottomY;

          int32_t quarterY = (work->stripData.bottomY - work->stripData.writeY) >> 2;
          int32_t quarterX = work->stripData.bottomX >> 2;

          // Read the strip chunks
          int32_t stripLimit =
            work->stream.sampleCache.readPos + stripDataLength - 12;
          lastBottomY = work->stripData.bottomY;
              do {
                  pollAudioConfirmTimer();

                int32_t chunkID = (work->stream.sampleCache.readPos[0] << 8) | work->stream.sampleCache.readPos[1];
                  work->stream.sampleCache.readPos += 2;
                  int32_t chunkDataLength =
                    ((work->stream.sampleCache.readPos[0] << 8) |
                      work->stream.sampleCache.readPos[1]) - 4;

                  work->stream.sampleCache.readPos += 2;
                  //work->stream.sampleCache.readPos = readPtr;
                    bool isV4 = (chunkID == 0x2000 || chunkID == 0x2100);
                    switch (chunkID) {
                        // 12 bit V4 (0x2000) or V1(0x2200)
                        case 0x2000:
                        case 0x2200: {
                          codebookRGB_new(work, currentCodebook, isV4, chunkID, chunkDataLength);
                          work->stream.sampleCache.readPos += chunkDataLength;
                        } break;
                        // 12 bit V4 (0x2100) or V1 (0x2300) - Update only
                        case 0x2100:
                        case 0x2300: {
                          codebookRGB_new(work, currentCodebook, isV4, chunkID, chunkDataLength);
                          work->stream.sampleCache.readPos += chunkDataLength;
                        } break;
                        // 8 bit V4
                        case 0x2400:
                          break;
                        // 8 bit V1
                        case 0x2600:
                          break;
                        // vectors
                        case 0x3000:
                          decodeIntra(work, currentCodebook, chunkDataLength, false, quarterY, quarterX);
                          break;
                        // list of blocks from v1
                        case 0x3100:
                            decodeInter(work, currentCodebook, chunkDataLength, quarterY, quarterX);
                          break;
                        case 0x3200:
                            decodeIntra(work, currentCodebook, chunkDataLength, true, quarterY, quarterX);
                          break;
                        default: {
                          const int32_t chunkIdPos =
                            work->stream.sampleCache.readPos - 4;

                          int16_t *vdp2Image15Ptr =
                            work->decodeParams->vramBuffAddr;
                          int32_t *vdp2ImagePtr = work->decodeParams->vramBuffAddr;
                          if (work->filmHeader.fdsc.color_depth == COLOR_DEPTH_15) {
                            memset(vdp2Image15Ptr, 0,
                              work->decodeParams->vramBufferWidth *
                                work->decodeParams->vramBufferHeight *
                                sizeof(int32_t));
                          } else {
                            memset(vdp2ImagePtr, 0,
                              work->decodeParams->vramBufferWidth *
                                work->decodeParams->vramBufferHeight *
                                sizeof(int32_t));
                          }
                          logMessage("Unknown chunk id 0x%X at offset 0x%X\n",
                            chunkID, chunkIdPos);
                          vdp2_scrn_back_color_set(VDP2_VRAM_ADDR(3, 0x01FFFE),
                            RGB1555(1, 7, 0, 0));
                          break;
                        }
                    }

            } while (work->stream.sampleCache.readPos < stripLimit &&
              work->stream.sampleCache.readPos < sampleEnd);
          stripNum++;

      } while (stripNum < numStrips && work->stream.sampleCache.readPos < sampleEnd);
  }
}
