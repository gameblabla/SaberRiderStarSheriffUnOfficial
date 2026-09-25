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
#define FILM_PCM_CONTROL_BASE 2

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

     pcm_cfg.src = srcR;
     pcm_cfg.dst = CPU_CACHE_THROUGH | (uint32_t) destPtrR;
     pcm_cfg.len = len;
     cpu_dmac_channel_config_set(&pcm_cfg);
     cpu_dmac_channel_start(0);
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
    pcmsys_pcm_start(FILM_PCM_CONTROL_BASE + i, volume);
  sound_notify_driver();
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
  cpu_divu_32_32_set((int32_t) AUDIO_CONFIRM_SAMPLES_PER_TICK * frequency,
    AUDIO_CONFIRM_MASTER_RATE);

  /* ADX is decoded to 16-bit PCM by snd_adx.c.  Using the ordinary pcmsys
     stream avoids the experimental SCSP-DSP predictor path and is cheap at
     22.05 kHz (two integer MACs per output sample). */
  clearDspRam();
  if (work->filmHeader.fdsc.sound_codec == FDSC_CODEC_ADX) numBits = 16;
  filmAudioChannels = channels == 2 ? 2 : 1;
  soundBufferSize = work->decodeParams->audioBufferSize;
  if (soundBufferSize <= 0) soundBufferSize = 32768;
  baseSoundMemory = (uint8_t *)(uintptr_t)work->decodeParams->audioBufferAddr;
  soundMemory = baseSoundMemory;
  soundMemoryLimit = baseSoundMemory + soundBufferSize;
  for (uint8_t i = 0; i < filmAudioChannels; i++) {
    /* DIPAN 0 is the driver's neutral/default setting and is what the old
       one-channel pcmStream path used.  Keep mono movie audio at that setting;
       stereo uses the explicit left/right assignments below. */
    uint8_t pan = filmAudioChannels == 2 ? (i ? PCM_PAN_RIGHT : PCM_PAN_LEFT) : 0;
    uint32_t off = (uint32_t)((uintptr_t)(baseSoundMemory + i * soundBufferSize) - (uintptr_t)SNDRAM);
    pcmsys_pcm_configure(FILM_PCM_CONTROL_BASE + i, off, (uint32_t)soundBufferSize,
      (uint16_t)frequency, (uint8_t)numBits, PCM_FWD_LOOP, pan);
    sound_ram_clear(baseSoundMemory + i * soundBufferSize, (uint32_t)soundBufferSize);
  }

  audioConfirmedConsumedBytes = 0;
  audioTotalWrittenBytes = 0;

  const int32_t bytesPerSample = numBits >> 3;
  audioBytesPerTimerTick = cpu_divu_quotient_get() * bytesPerSample;

  cpu_divu_32_32_set((int32_t) frequency * bytesPerSample, 1000);
  audioExpectedBytesPerMs = cpu_divu_quotient_get();

  setIncrement(SndTimerRegisterC, AUDIO_CONFIRM_TIMER_CYCLE);
  SndTimerRegisterC->countData = 0;
  SndCpuInterruptReset->timerC = 1;
  SndCpuInterruptEnable->timerC = 1;
}

void film_audio_prepare_to_play() {}

void film_audio_reset() {
  SndCpuInterruptEnable->timerC = 0;
  for (uint8_t i = 0; i < filmAudioChannels; i++) pcmsys_pcm_stop(FILM_PCM_CONTROL_BASE + i);
  if (filmAudioChannels) sound_notify_driver();
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
