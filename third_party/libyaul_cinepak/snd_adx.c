#include "snd_adx.h"
#include "film_snd.h"

#ifndef MIN
#  define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#endif

// CRI ADX (standard fixed-coefficient, 4-bit, 500Hz cutoff, 18-byte blocks:
// 2-byte BE scale + 16 bytes of nibbles = 32 samples/block). This was made
// by reverse engineering Sakura Wars 2 to see what Sega's official implmentation did.
// Originally it was really slow so I had Claude look at what the compiler output and
// make suggestions. A lot of the inline assembly came from this.

#define ADX_BLOCK_BYTES      18
#define ADX_BLOCK_DATA_BYTES 16
#define ADX_BLOCK_DATA_WORDS (ADX_BLOCK_DATA_BYTES / 2)
#define ADX_BLOCK_SAMPLES    32
#define ADX_BLOCK_PCM_BYTES  (ADX_BLOCK_SAMPLES * 2)

static bool adxHeaderPending = false;

static uint8_t adxCarryBuf[36] __aligned(4); // 2 * ADX_BLOCK_BYTES
static int32_t adxCarryLen = 0;
static const uint8_t *adxChunkEnd = NULL;

/* ffmpeg/CRI fixed ADX coefficients for the Saturn movie format we build:
 * 22050 Hz, 500 Hz high-pass, 13 fractional bits.  Integer truncation here
 * matches ffmpeg exactly (13138, -5268). */
#define ADX_COEF1  13138
#define ADX_COEF2 (-5268)
static int32_t adxHist1[2] = {0, 0};
static int32_t adxHist2[2] = {0, 0};

void film_audio_adx_reset_history(void) {
  adxHeaderPending = true;
  adxCarryLen = 0;
  adxChunkEnd = NULL;
  adxHist1[0] = adxHist1[1] = 0;
  adxHist2[0] = adxHist2[1] = 0;
}

static int32_t film_audio_adx_consume_header(const uint8_t *dataStart) {
  if (!adxHeaderPending) {
    return 0;
  }
  adxHeaderPending = false;
  int32_t offset2data = (int16_t) ((dataStart[2] << 8) | dataStart[3]);

  uint8_t format = dataStart[4];
  uint8_t blockSize = dataStart[5];
  uint8_t bitDepth = dataStart[6];
  uint16_t hpCutoff = (dataStart[16] << 8) | dataStart[17];

  if (format != 3 || blockSize != ADX_BLOCK_BYTES ||
    bitDepth != 4 || hpCutoff != 500) {
    /* Unsupported ADX layout: the build tool only emits 3/18/4/500. */
  }

  return offset2data + 4;
}

int32_t snd_adx_begin_chunk(uint8_t *dataStart, int32_t dataLength,
  int32_t channels, int32_t *outRemainingPcmBytes) {
  int32_t headerSize = film_audio_adx_consume_header(dataStart);
  uint8_t *readPos = dataStart + headerSize;
  int32_t remainingLength = dataLength - headerSize;
  int32_t blockGroupBytes = (channels == 2) ? (ADX_BLOCK_BYTES * 2) : ADX_BLOCK_BYTES;
  adxChunkEnd = readPos + remainingLength;
  int32_t availableForGroups = remainingLength + adxCarryLen;
  int32_t numGroups = availableForGroups / blockGroupBytes;
  *outRemainingPcmBytes = numGroups * ADX_BLOCK_PCM_BYTES;
  return headerSize;
}

__attribute__((always_inline)) static inline int16_t adx_clip16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t) v;
}

__attribute__((always_inline)) static inline void adx_decode_block(
  const uint8_t *src, int16_t *dest, int channel) {
  const int32_t scale = (int32_t) ((src[0] << 8) | src[1]);
  int32_t h1 = adxHist1[channel];
  int32_t h2 = adxHist2[channel];

  for (int32_t i = 2; i < ADX_BLOCK_BYTES; i++) {
    uint8_t packed = src[i];
    int32_t nib = (int32_t) (packed >> 4);
    if (nib & 8) nib -= 16;
    int32_t sample = (nib * scale * 8192 + ADX_COEF1 * h1 + ADX_COEF2 * h2) >> 13;
    sample = adx_clip16(sample);
    *dest++ = (int16_t) sample;
    h2 = h1; h1 = sample;

    nib = (int32_t) (packed & 15);
    if (nib & 8) nib -= 16;
    sample = (nib * scale * 8192 + ADX_COEF1 * h1 + ADX_COEF2 * h2) >> 13;
    sample = adx_clip16(sample);
    *dest++ = (int16_t) sample;
    h2 = h1; h1 = sample;
  }

  adxHist1[channel] = h1;
  adxHist2[channel] = h2;
}

static inline void adx_decode_n_blocks_mono(int16_t *dest, int32_t blocks,
  film_sample_cache_t *cache) {
  for (int32_t b = 0; b < blocks; b++) {
    if (adxCarryLen > 0) {
      int32_t need = ADX_BLOCK_BYTES - adxCarryLen;
      memcpy(adxCarryBuf + adxCarryLen, cache->readPos, need);
      cache->readPos += need;
      adxCarryLen = 0;
      adx_decode_block(adxCarryBuf, dest, 0);
    } else {
      adx_decode_block(cache->readPos, dest, 0);
      cache->readPos += ADX_BLOCK_BYTES;
    }
    dest += ADX_BLOCK_SAMPLES;
  }
}

static inline void adx_decode_n_blocks_stereo(int16_t *destL, int16_t *destR,
  int32_t blocks, film_sample_cache_t *cache) {
  for (int32_t b = 0; b < blocks; b++) {
    if (adxCarryLen > 0) {
      int32_t need = (ADX_BLOCK_BYTES * 2) - adxCarryLen;
      memcpy(adxCarryBuf + adxCarryLen, cache->readPos, need);
      cache->readPos += need;
      adxCarryLen = 0;
      adx_decode_block(adxCarryBuf, destL, 0);
      adx_decode_block(adxCarryBuf + ADX_BLOCK_BYTES, destR, 1);
    } else {
      adx_decode_block(cache->readPos, destL, 0);
      cache->readPos += ADX_BLOCK_BYTES;
      adx_decode_block(cache->readPos, destR, 1);
      cache->readPos += ADX_BLOCK_BYTES;
    }
    destL += ADX_BLOCK_SAMPLES;
    destR += ADX_BLOCK_SAMPLES;
  }
}

bool parseMonoADXAudio(decode_work_t *work) {
  film_sample_cache_t *cache = &work->stream.sampleCache;
  pollAudioConfirmTimer();

  int32_t readSize = film_audio_get_next_buffer_size();
  if (readSize <= 0) {
    return false;
  }

  int32_t blocksAvail = cache->remainingPcmBytes >> 6;
  int32_t blocksBudget = readSize >> 6;
  int32_t blocks = MIN(blocksAvail, blocksBudget);
  if (blocks <= 0) {
    return false;
  }

  uint16_t *writeLocation = film_audio_get_next_buffer_ptr(0);
  adx_decode_n_blocks_mono((int16_t *) writeLocation, blocks, cache);

  int32_t outputBytes = blocks * ADX_BLOCK_PCM_BYTES;
  removeReturnNoise((uint8_t *) writeLocation, baseSoundMemory, 16);
  film_audio_notify_read_buffer_bytes(outputBytes);
  cache->remainingPcmBytes -= outputBytes;

  if (!work->audioPlaying) {
    work->audioWaitingToStart = true;
  }

  if (cache->remainingPcmBytes == 0) {
    if (adxChunkEnd != NULL) {
      int32_t leftover = (int32_t) (adxChunkEnd - cache->readPos);
      adxCarryLen = leftover > 0 ? leftover : 0;
      if (adxCarryLen > 0) {
        memcpy(adxCarryBuf, cache->readPos, adxCarryLen);
      }
    }
    return true;
  }

  return false;
}

bool parseStereoADXAudio(decode_work_t *work) {
  film_sample_cache_t *cache = &work->stream.sampleCache;
  pollAudioConfirmTimer();

  int32_t readSize = film_audio_get_next_buffer_size();
  if (readSize <= 0) {
    return false;
  }

  int32_t blocksAvail = cache->remainingPcmBytes >> 6;
  int32_t blocksBudget = readSize >> 6;
  int32_t blocks = MIN(blocksAvail, blocksBudget);
  if (blocks <= 0) {
    return false;
  }

  uint16_t *writeLocation_L = film_audio_get_next_buffer_ptr(0);
  uint16_t *writeLocation_R = film_audio_get_next_buffer_ptr(1);
  adx_decode_n_blocks_stereo((int16_t *) writeLocation_L, (int16_t *) writeLocation_R,
    blocks, cache);

  int32_t outputBytes = blocks * ADX_BLOCK_PCM_BYTES;
  removeReturnNoise((uint8_t *) writeLocation_L, baseSoundMemory, 16);
  film_audio_notify_read_buffer_bytes(outputBytes);
  cache->remainingPcmBytes -= outputBytes;

  if (!work->audioPlaying) {
    work->audioWaitingToStart = true;
  }

  if (cache->remainingPcmBytes == 0) {
    if (adxChunkEnd != NULL) {
      int32_t leftover = (int32_t) (adxChunkEnd - cache->readPos);
      adxCarryLen = leftover > 0 ? leftover : 0;
      if (adxCarryLen > 0) {
        memcpy(adxCarryBuf, cache->readPos, adxCarryLen);
      }
    }
    return true;
  }

  return false;
}
