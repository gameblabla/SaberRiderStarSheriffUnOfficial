#include "film_snd.h"
#include "pcmsys.h"

#define AUDIO_CONFIRM_TIMER_CYCLE      EverySamples4
#define AUDIO_CONFIRM_SAMPLES_PER_TICK (256 * 4)
#define AUDIO_CONFIRM_MASTER_RATE      (44100L)

static cpu_dmac_cfg_t pcm_cfg = {
  .channel = 0,
  .src_mode = CPU_DMAC_SOURCE_INCREMENT,
  .dst_mode = CPU_DMAC_DESTINATION_INCREMENT,
  .stride = CPU_DMAC_STRIDE_16_BYTES,
  .bus_mode = CPU_DMAC_BUS_MODE_BURST,
  .src = 0,
  .dst = 0,
  .len = 0,
  .ihr = NULL,
  .ihr_work = NULL,
};

uint8_t *baseSoundMemory = NULL;
uint8_t *soundMemory = NULL;
uint8_t *soundMemoryLimit = NULL;
int32_t soundBufferSize = 0;
static uint8_t filmAudioChannels = 0;
static uint16_t filmAudioRate = 0;
static uint8_t filmAudioBits = 0;

/* The SH-2 sound RAM window is word addressable.  libc memset may issue byte
 * stores, which do not reliably clear SCSP sample data on Saturn hardware. */
static void sound_ram_clear(void *address, uint32_t bytes) {
  volatile uint16_t *p = (volatile uint16_t *) address;
  for (uint32_t i = 0; i < (bytes + 1u) / 2u; i++) p[i] = 0;
}

uint32_t audioConfirmedConsumedBytes = 0;
uint32_t audioTotalWrittenBytes = 0;
uint32_t audioBytesPerTimerTick = 0;
int32_t audioExpectedBytesPerMs = 0;

void pcm_MemcpyDword(int32_t *dst, int32_t *src, int32_t dwsize) {
  dst += dwsize;
  src += dwsize;
  while (--dwsize >= 0) {
    *--dst = *--src;
  }
}

void readStereoPcmBytesFromRingBuff(binary_stream_t *stream,
  uint16_t *destPtrL, uint16_t *destPtrR, int32_t len, int32_t channelOffset, int8_t xferMode) {
  if (xferMode == PCM_XFER_SH2_DMA) {
     uint32_t *srcL = CPU_CACHE_THROUGH | (uint32_t) stream->sampleCache.readPos;
     uint32_t *srcR = CPU_CACHE_THROUGH | (uint32_t) stream->sampleCache.readPos + channelOffset;
     pcm_cfg.src = srcL;
     pcm_cfg.dst = CPU_CACHE_THROUGH | (uint32_t) destPtrL;
     pcm_cfg.len = len;

     cpu_dmac_channel_config_set(&pcm_cfg);
     cpu_dmac_channel_start(0);
     /* Channel 0 is reused for the right channel below; drain the left copy
        before reconfiguring it or config_set will stop it mid-transfer. */
     cpu_dmac_channel_wait(0);

     pcm_cfg.src = srcR;
     pcm_cfg.dst = CPU_CACHE_THROUGH | (uint32_t) destPtrR;
     pcm_cfg.len = len;
     cpu_dmac_channel_config_set(&pcm_cfg);
     cpu_dmac_channel_start(0);
     /* The SH-2 DMAC is asynchronous.  The caller modifies this sound-RAM
        range and accounts it as ready immediately after this function; wait
        here so neither operation can race the copy (or truncate it when the
        next PCM chunk reconfigures channel 0). */
     cpu_dmac_channel_wait(0);
     cpu_cache_purge();

  } else {
    pcm_MemcpyDword(destPtrL, stream->sampleCache.readPos, len >> 2);
    destPtrL += (channelOffset >> 2);
    pcm_MemcpyDword(destPtrR, stream->sampleCache.readPos + channelOffset,
      len >> 2);
    destPtrR += (len >> 2);
  }
  stream->sampleCache.readPos += len;
}

void readPcmBytesFromRingBuff(
  binary_stream_t *stream, uint16_t *destPtr, int32_t len, int8_t xferMode) {

   uint32_t *dest = destPtr;
  if (xferMode == PCM_XFER_SH2_DMA) {
     pcm_cfg.src = CPU_CACHE_THROUGH | (uint32_t) stream->sampleCache.readPos;
     pcm_cfg.dst = CPU_CACHE_THROUGH | (uint32_t) destPtr;
     pcm_cfg.len = len;

     cpu_dmac_channel_config_set(&pcm_cfg);
     cpu_dmac_channel_start(0);
     /* See readStereoPcmBytesFromRingBuff: don't let the decoder process or
        announce this range until it has reached sound RAM. */
     cpu_dmac_channel_wait(0);
     cpu_cache_purge();
  } else {
    pcm_MemcpyDword(destPtr, stream->sampleCache.readPos, len >> 2);
    destPtr += (len >> 2);
  }
  stream->sampleCache.readPos += len;
}

void pollAudioConfirmTimer(void) {
  if (SndCpuInterruptPending->timerC) {
    SndCpuInterruptReset->timerC = 1;
    SndTimerRegisterC->countData = 0;

    audioConfirmedConsumedBytes += audioBytesPerTimerTick;
  }
}

int32_t film_audio_get_next_buffer_size() {

  int32_t size = (int32_t) (soundMemoryLimit - soundMemory);
  if (size == 0) {
    soundMemory = baseSoundMemory;
    size = (int32_t) (soundMemoryLimit - soundMemory);
  }

  return size;
}

static inline void film_audio_get_next_buffer_info(int32_t *contigSpace, int32_t *freeSpace) {
  int32_t untilWrap = (int32_t) (soundMemoryLimit - soundMemory);
  if (untilWrap <= 0) {
    soundMemory = baseSoundMemory;
    untilWrap = soundBufferSize;
  }

  int32_t inFlight = (int32_t) (audioTotalWrittenBytes - audioConfirmedConsumedBytes);
  int32_t free = soundBufferSize - inFlight;

  if (free < 0) {
    free = 0;
  } else if (free > soundBufferSize) {
    free = soundBufferSize;
  }

  *freeSpace = free;
  *contigSpace = (untilWrap < free) ? untilWrap : free;
}

uint16_t *film_audio_get_next_buffer_ptr(uint8_t slot) {
  return (uint16_t *) (soundMemory + (slot * soundBufferSize));
}

void film_audio_notify_read_buffer_bytes(int32_t length) {
  soundMemory += length;
  audioTotalWrittenBytes += length;

  if (soundMemory >= soundMemoryLimit) {
    soundMemory = baseSoundMemory;
  }
}

void film_audio_play(uint8_t volume) {
  for (uint8_t i = 0; i < filmAudioChannels; i++)
    film_pcm_start(i, volume);
}

void film_audio_fill_silence(decode_work_t *work) {
  int32_t contigSpace, freeSpace;
  film_audio_get_next_buffer_info(&contigSpace, &freeSpace);
  if (contigSpace > freeSpace) {
    contigSpace = freeSpace;
  }
  if (freeSpace <= 0) {
    return;
  }

  sound_ram_clear(film_audio_get_next_buffer_ptr(0), (uint32_t)contigSpace);
  if (work->filmHeader.fdsc.sound_channels == 2) {
    sound_ram_clear(film_audio_get_next_buffer_ptr(1), (uint32_t)contigSpace);
  }
  film_audio_notify_read_buffer_bytes(contigSpace);

  int32_t remainder = freeSpace - contigSpace;
  if (remainder > 0) {
    sound_ram_clear(film_audio_get_next_buffer_ptr(0), (uint32_t)remainder);
    if (work->filmHeader.fdsc.sound_channels == 2) {
      sound_ram_clear(film_audio_get_next_buffer_ptr(1), (uint32_t)remainder);
    }
    film_audio_notify_read_buffer_bytes(remainder);
  }
}

void removeReturnNoise(uint8_t *writeLocation,
  uint8_t *channelBase, int32_t numBits) {
  if (writeLocation != channelBase) {
    return;
  }

  if (numBits == 16) {
    uint16_t *lastSample = (uint16_t *) (channelBase + soundBufferSize) - 1;
    *lastSample = *(uint16_t *) channelBase;
  } else {
    uint8_t *lastSample = (channelBase + soundBufferSize) - 1;
    *lastSample = *channelBase;
  }
}

void film_audio_setup(decode_work_t *work, int16_t frequency,
  int32_t channels, int32_t numBits) {
  /* Saber Rider: only the bookkeeping and the ring here (a clip is set up while the game still plays: video_sat.c
     preloads). The SCSP is the game's sound driver's until the clip starts: film_audio_hw_begin. */
  if (work->filmHeader.fdsc.sound_codec == FDSC_CODEC_ADX) numBits = 16;   /* snd_adx.c decodes ADX to 16-bit PCM */
  filmAudioChannels = channels == 2 ? 2 : 1;
  filmAudioRate = (uint16_t)frequency;
  filmAudioBits = (uint8_t)numBits;
  soundBufferSize = work->decodeParams->audioBufferSize;
  if (soundBufferSize <= 0) soundBufferSize = 32768;
  baseSoundMemory = (uint8_t *)(uintptr_t)work->decodeParams->audioBufferAddr;
  soundMemory = baseSoundMemory;
  soundMemoryLimit = baseSoundMemory + soundBufferSize;
  for (uint8_t i = 0; i < filmAudioChannels; i++)
    sound_ram_clear(baseSoundMemory + i * soundBufferSize, (uint32_t)soundBufferSize);

  audioConfirmedConsumedBytes = 0;
  audioTotalWrittenBytes = 0;
}

/* The clip starts: its slots (the platform's film_pcm_*, the 68000 stopped) and SCSP timer C, which counts what the
   slots have played. */
void film_audio_hw_begin(void) {
  if (!filmAudioChannels) return;
  cpu_divu_32_32_set((int32_t) AUDIO_CONFIRM_SAMPLES_PER_TICK * filmAudioRate,
    AUDIO_CONFIRM_MASTER_RATE);
  for (uint8_t i = 0; i < filmAudioChannels; i++) {
    /* mono: pan centre; stereo: left / right */
    uint8_t pan = filmAudioChannels == 2 ? (i ? PCM_PAN_RIGHT : PCM_PAN_LEFT) : 0;
    uint32_t off = (uint32_t)((uintptr_t)(baseSoundMemory + i * soundBufferSize) - (uintptr_t)SNDRAM);
    film_pcm_configure(i, off, (uint32_t)soundBufferSize, filmAudioRate, filmAudioBits, pan);
  }

  audioConfirmedConsumedBytes = 0;
  audioTotalWrittenBytes = 0;

  const int32_t bytesPerSample = filmAudioBits >> 3;
  audioBytesPerTimerTick = cpu_divu_quotient_get() * bytesPerSample;

  cpu_divu_32_32_set((int32_t) filmAudioRate * bytesPerSample, 1000);
  audioExpectedBytesPerMs = cpu_divu_quotient_get();

  setIncrement(SndTimerRegisterC, AUDIO_CONFIRM_TIMER_CYCLE);
  SndTimerRegisterC->countData = 0;
  SndCpuInterruptReset->timerC = 1;
  SndCpuInterruptEnable->timerC = 1;
}

void film_audio_prepare_to_play() {}

void film_audio_reset() {
  /* Drain any final PCM copy before the movie buffer is freed or the game
     sound driver takes the SCSP back. */
  cpu_dmac_channel_wait(0);
  SndCpuInterruptEnable->timerC = 0;
  for (uint8_t i = 0; i < filmAudioChannels; i++) film_pcm_stop(i);
  filmAudioChannels = 0;
  baseSoundMemory = NULL;
  soundMemory = NULL;
  soundMemoryLimit = NULL;
  soundBufferSize = 0;
  audioConfirmedConsumedBytes = 0;
  audioTotalWrittenBytes = 0;
}

bool parseAudioMono(decode_work_t *work) {
  pollAudioConfirmTimer();

  film_sample_cache_t *cache = &work->stream.sampleCache;

  int32_t contigSpace, freeSpace;
  film_audio_get_next_buffer_info(&contigSpace, &freeSpace);
  if (freeSpace <= 0) {
    return false;
  }

  int32_t needed = freeSpace;
  if (needed > cache->remainingPcmBytes) {
    needed = cache->remainingPcmBytes;
  }
  if (needed <= 0) {
    return false;
  }

  int32_t firstPiece = (contigSpace < needed) ? contigSpace : needed;

  uint16_t *writeLocation = film_audio_get_next_buffer_ptr(0);
  readPcmBytesFromRingBuff(&work->stream, writeLocation, firstPiece,
    work->decodeParams->pcmTransferMode);
  removeReturnNoise((uint8_t *) writeLocation, baseSoundMemory,
    work->filmHeader.fdsc.sound_resolution);
  film_audio_notify_read_buffer_bytes(firstPiece);
  cache->remainingPcmBytes -= firstPiece;

  int32_t remainder = needed - firstPiece;
  if (remainder > 0) {
    uint16_t *writeLocation2 = film_audio_get_next_buffer_ptr(0);
    readPcmBytesFromRingBuff(&work->stream, writeLocation2, remainder,
      work->decodeParams->pcmTransferMode);
    removeReturnNoise((uint8_t *) writeLocation2, baseSoundMemory,
      work->filmHeader.fdsc.sound_resolution);
    film_audio_notify_read_buffer_bytes(remainder);
    cache->remainingPcmBytes -= remainder;
  }

  if (!work->audioPlaying) {
    work->audioWaitingToStart = true;
  }

  return cache->remainingPcmBytes == 0;
}

bool parseAudioStereo(decode_work_t *work) {
  pollAudioConfirmTimer();

  film_sample_cache_t *cache = &work->stream.sampleCache;

  int32_t contigSpace, freeSpace;
  film_audio_get_next_buffer_info(&contigSpace, &freeSpace);
  if (freeSpace <= 0) {
    return false;
  }

  int32_t needed = freeSpace;
  if (needed > cache->remainingPcmBytes) {
    needed = cache->remainingPcmBytes;
  }
  if (needed <= 0) {
    return false;
  }

  // Per-channel distance from L to R in the source ring stays constant
  // for the whole sample, so the same value applies to both pieces.
  int32_t perChannelLength = work->nextSample.length >> 1;
  int32_t firstPiece = (contigSpace < needed) ? contigSpace : needed;

  uint16_t *writeLocation_L = film_audio_get_next_buffer_ptr(0);
  uint16_t *writeLocation_R = film_audio_get_next_buffer_ptr(1);
  readStereoPcmBytesFromRingBuff(&work->stream, writeLocation_L,
    writeLocation_R, firstPiece, perChannelLength, work->decodeParams->pcmTransferMode);
  removeReturnNoise((uint8_t *) writeLocation_L, baseSoundMemory,
    work->filmHeader.fdsc.sound_resolution);
  film_audio_notify_read_buffer_bytes(firstPiece);
  cache->remainingPcmBytes -= firstPiece;

  int32_t remainder = needed - firstPiece;
  if (remainder > 0) {
    uint16_t *writeLocation_L2 = film_audio_get_next_buffer_ptr(0);
    uint16_t *writeLocation_R2 = film_audio_get_next_buffer_ptr(1);
    readStereoPcmBytesFromRingBuff(&work->stream, writeLocation_L2,
      writeLocation_R2, remainder, perChannelLength, work->decodeParams->pcmTransferMode);
    removeReturnNoise((uint8_t *) writeLocation_L2, baseSoundMemory,
      work->filmHeader.fdsc.sound_resolution);
    film_audio_notify_read_buffer_bytes(remainder);
    cache->remainingPcmBytes -= remainder;
  }

  if (!work->audioPlaying) {
    work->audioWaitingToStart = true;
  }

  return cache->remainingPcmBytes == 0;
}
