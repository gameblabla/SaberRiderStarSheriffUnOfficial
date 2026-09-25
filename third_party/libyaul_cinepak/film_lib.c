#include "film_lib.h"
#include "cd.h"
#include "film_buff.h"
#include "film_cvid.h"
#include "film_snd.h"
#include "snd_adx.h"
#include "pcmsys.h"

int32_t lwrambuffer = 80;

static uint32_t bytesNeededForCurrentSample = 0;

static inline bool sampleIsReady(decode_work_t *work) {
  return work->stream.sampleCache.currentSample <
      work->stream.sampleCache.numSamples &&
    film_buff_async_bytes_delivered() >= bytesNeededForCurrentSample;
}

static inline void advanceReadyThreshold(decode_work_t *work) {
  film_sample_cache_t *cache = &work->stream.sampleCache;
  if (cache->currentSample < cache->numSamples) {
    bytesNeededForCurrentSample += (uint32_t) cache->samples[cache->currentSample].length;
  }
}

/* Saber Rider already owns the SH-2 FRT.  Keep the Cinepak clock in
 * milliseconds on top of the game's free-running microsecond timer. */
extern uint32_t sat_timer_us(void);
static int32_t frtBaseMs;
static inline int32_t frtTimerEllapsed(void);

void init_film_start(cdfs_filelist_entry_t *fsEntry, decode_work_t *work,
  int32_t vdp_height __unused, int32_t vdp_width) {
  initClampLUT24();
  film_buff_reset_async_state();
  if (!film_buff_io_active())
    queueDiskRead(fsEntry->starting_fad, fsEntry->size);

  stream_new(&work->stream, fsEntry, work->decodeParams->sampleBuffAddr,
    work->decodeParams->sampleBuffSize);

  /* Read the fixed FILM header through the stream.  Saber Rider can provide
   * the stream through its own CDFS/stdio backend, avoiding ownership races
   * with the game's existing CD-block streamer. */
  film_header *streamHeader = (film_header *) work->decodeParams->sampleBuffAddr;
  stream_readbytes(&work->stream, (uint16_t *) streamHeader, sizeof(film_header));
  if (work->stream.eof || memcmp(streamHeader->film_str, "FILM", 4) != 0) {
    work->play_status = ERROR;
    return;
  }
  memcpy(&work->filmHeader, streamHeader, sizeof(film_header));

  /* Keep enough of sampleBuffAddr for the streaming ring after the STAB table.
   * A malformed/unsupported movie must not let film_sample_cache_new() walk
   * past the caller's DMA-able allocation. */
  if (work->filmHeader.stab.total_entries <= 0 ||
      work->filmHeader.stab.total_entries > FILM_MAX_SAMPLES ||
      (uint32_t) work->decodeParams->sampleBuffSize < (96u * 1024u)) {
    work->play_status = ERROR;
    return;
  }

  stripdata_new(&work->stripData, work->filmHeader.fdsc.height, work->filmHeader.fdsc.width);

  film_audio_setup(work, work->filmHeader.fdsc.sample_rate >> 16,
    work->filmHeader.fdsc.sound_channels,
    work->filmHeader.fdsc.sound_resolution);

  if (work->filmHeader.fdsc.sound_codec == FDSC_CODEC_ADX) {
    work->parseAudioFn = (work->filmHeader.fdsc.sound_channels == 2) ?
      parseStereoADXAudio : parseMonoADXAudio;
    film_audio_adx_reset_history();
  } else {
    work->parseAudioFn = (work->filmHeader.fdsc.sound_channels == 2) ?
      parseAudioStereo : parseAudioMono;
  }

  work->stream.sampleCache.currentSample = 0;
  work->stream.sampleCache.currentBuffSample = 0;
  work->stream.sampleCache.numSamples = work->filmHeader.stab.total_entries;
  work->stream.sampleCache.remainingPcmBytes = 0;
  if (work->filmHeader.fdsc.sound_resolution == 8) {
    work->stream.sampleCache.pcmBytesPerBlank = calculate_bytes_per_blank(
      work->filmHeader.fdsc.sample_rate >> 16, true, false);
  } else {
    work->stream.sampleCache.pcmBytesPerBlank = calculate_bytes_per_blank(
      work->filmHeader.fdsc.sample_rate >> 16, false, false);
  }

  film_sample_cache_new(&work->stream, work->filmHeader.stab.total_entries,
    work->sampleTable, (uint8_t *) work->decodeParams->sampleBuffAddr);
  if (work->stream.eof) {
    work->play_status = ERROR;
    return;
  }

  const film_sample_t *lastSample =
    &work->stream.sampleCache.samples[work->stream.sampleCache.numSamples - 1];
  work->lastSampleEndTime = (lastSample->time & 0x7FFFFFFFu) + lastSample->duration;
  bytesNeededForCurrentSample = (uint32_t) work->stream.sampleCache.samples[0].length;

  work->hasVideoSamples = false;
  for (int32_t i = 0; i < work->filmHeader.stab.total_entries; i++) {
    if (work->stream.sampleCache.samples[i].time != 0xFFFFFFFF) {
      work->hasVideoSamples = true;
      break;
    }
  }

  work->lastAudioSampleIndex = -1;
  for (int32_t i = work->filmHeader.stab.total_entries - 1; i >= 0; i--) {
    if (work->stream.sampleCache.samples[i].time == 0xFFFFFFFF) {
      work->lastAudioSampleIndex = i;
      break;
    }
  }

  cpu_divu_32_32_set((work->stream.sampleCache.samples[0].duration * 1000), work->filmHeader.stab.ticks_per_second);

  work->isDisplayReady = false;
  work->displayWaiting = false;

  work->videoStartY = 0;
  work->dma_delta = work->videoStartY * vdp_width;
  work->audioPlaying = false;
  readBytesIntoRingBuff(&work->stream);
  uint32_t syncPrefillBytes = 0;
  for (int32_t i = 0; i < work->stream.sampleCache.currentBuffSample; i++) {
    syncPrefillBytes += (uint32_t) work->stream.sampleCache.samples[i].length;
  }
  film_buff_credit_async_bytes_delivered(syncPrefillBytes);

  work->play_status = INIT;
}

inline static void frtTimerStart(uint16_t count) {
  frtBaseMs = (int32_t) (sat_timer_us() / 1000u) - (int32_t) count;
}

inline static int32_t frtTimerEllapsed() {
  return (int32_t) (sat_timer_us() / 1000u) - frtBaseMs;
}

static void handle_play(decode_work_t *work);

void cpk_play(decode_work_t *work) {
  // Ask the sound driver to stop the warm up sound and get ready to start
  // processing sounds
  film_audio_prepare_to_play();

  frtTimerStart(0);

  work->frtOverflowCount = 0;
  work->ticksUntilNextFrame = 0;
  work->tickCount = 0;
  work->copyingVideoFrame = 0;
  work->audioWaitingToStart = false;
  work->isDisplayReady = false;
  work->displayWaiting = false;
  work->tickStart = frtTimerEllapsed();
  work->lastFrameTime = frtTimerEllapsed();
  work->play_status = PLAY;
}

void cpk_task(decode_work_t *work) {
  switch (work->play_status) {
  case STOP:
    work->play_status = END;
    break;
  case PAUSE:
    // TODO: Implement pause logic.
    break;
  case PLAY:
    handle_play(work);
    break;
  case INIT:
    cpk_play(work);
    break;
  default:
    break;
  }
}

// Had claude make this from the disassembly of Sega's time keeping to make it exact.
static inline void mulWide32(int32_t a, int32_t b, int32_t *hi, int32_t *lo) {
  int32_t h, l;
  __asm__ volatile (
    "dmuls.l %[a], %[b]\n\t"
    "sts mach, %[h]\n\t"
    "sts macl, %[l]"
    : [h] "=&r" (h), [l] "=&r" (l)
    : [a] "r" (a), [b] "r" (b)
    : "mach", "macl");
  *hi = h;
  *lo = l;
}

//Mimics what Segas code does for time keeping, they apparently use the div unit and don't care about wasting 38 cycles waiting.
static int32_t cpk_GetTimeNative(decode_work_t *work) {
  int32_t hi, lo;
  mulWide32((int32_t) work->tickCount, work->filmHeader.stab.ticks_per_second, &hi, &lo);
  cpu_divu_64_32_set((uint32_t) hi, (uint32_t) lo, 1000);
  return (int32_t) cpu_divu_quotient_get();
}

static void handle_play(decode_work_t *work) {
  pollAudioConfirmTimer();
  asyncReadBytesIntoRingBuff(&work->stream);

  if (sampleIsReady(work)) {
        film_sample_t *sample =
          &work->stream.sampleCache.samples[work->stream.sampleCache.currentSample];
        if (sample->time == 0xFFFFFFFF) {
          if (work->stream.sampleCache.remainingPcmBytes == 0) {
            work->nextSample = *sample;
            work->stream.sampleCache.readPos = sample->offset;

            if (work->filmHeader.fdsc.sound_codec == FDSC_CODEC_ADX) {
              int32_t remainingPcmBytes;
              int32_t headerSize = snd_adx_begin_chunk(
                work->stream.sampleCache.readPos, work->nextSample.length,
                work->filmHeader.fdsc.sound_channels, &remainingPcmBytes);
              work->stream.sampleCache.readPos += headerSize;
              work->nextSample.length -= headerSize;
              work->stream.sampleCache.remainingPcmBytes = remainingPcmBytes;
            } else {
              work->stream.sampleCache.remainingPcmBytes =
                (work->filmHeader.fdsc.sound_channels == 2) ?
                  work->nextSample.length >> 1 : work->nextSample.length;
            }
          }
          if (work->parseAudioFn(work)) {
            work->stream.sampleCache.currentSample++;
            advanceReadyThreshold(work);
          }
        }
  }

  if (!work->displayWaiting) {
    if (sampleIsReady(work)) {
          film_sample_t *sample =
            &work->stream.sampleCache.samples[work->stream.sampleCache.currentSample];
          if (sample->time != 0xFFFFFFFF) {
            work->nextSample = *sample;
            work->stream.sampleCache.currentSample++;
            advanceReadyThreshold(work);
            work->stream.sampleCache.readPos = work->nextSample.offset;
            work->ticksUntilNextFrame = (work->nextSample.time & 0x7FFFFFFF);
            parseVideo(work);
            work->decodeParams->vramWritePos = work->decodeParams->vramBuffAddr;
            work->displayWaiting = true;
          }
    }
  }

  {
    int32_t ellapsed = frtTimerEllapsed();
    work->timeEllapsed = ellapsed;
    int32_t deltaTime = ellapsed - work->lastFrameTime;
    if (deltaTime > 0) {
      work->lastFrameTime = ellapsed;
      work->tickCount += deltaTime;
    }
    if (work->timeEllapsed >= 1000) {
      frtTimerStart(ellapsed - 1000);
      work->lastFrameTime = frtTimerEllapsed();
    }
  }

  if (work->stream.sampleCache.currentSample >=
    work->stream.sampleCache.numSamples) {
    if (!work->displayWaiting) {
      if (!work->hasVideoSamples) {
        film_audio_fill_silence(work);
        work->play_status = END;
      } else {

        if (cpk_GetTimeNative(work) >= work->lastSampleEndTime) {
          film_audio_fill_silence(work);
          work->play_status = END;
        }
      }
    }
  }

  if (work->displayWaiting) {
    if ((uint32_t) cpk_GetTimeNative(work) >= work->ticksUntilNextFrame) {
      if (work->audioWaitingToStart) {
        film_audio_play(work->decodeParams->pcmVolume);
        work->audioPlaying = true;
        work->audioWaitingToStart = false;
      }
      work->isDisplayReady = true;
    }
  }


}

bool cpk_display_ready(decode_work_t *work) { return work->isDisplayReady; }

void cpk_display_finished(decode_work_t *work) {
  work->isDisplayReady = false;
  work->displayWaiting = false;
}
