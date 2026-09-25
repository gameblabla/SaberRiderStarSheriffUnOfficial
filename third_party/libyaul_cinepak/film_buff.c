#include "film_buff.h"
#include "cd.h"


#ifndef MIN
#  define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#endif

typedef struct {
  void *user;
  film_io_read_fn read_fn;
  film_io_available_fn available_fn;
} film_io_state_t;

static film_io_state_t filmIo = {0};

void film_buff_io_set(void *user, film_io_read_fn read_fn, film_io_available_fn available_fn) {
  filmIo.user = user;
  filmIo.read_fn = read_fn;
  filmIo.available_fn = available_fn;
}

void film_buff_io_clear(void) {
  filmIo = (film_io_state_t) {0};
}

bool film_buff_io_active(void) {
  return filmIo.read_fn != NULL;
}

static scu_dma_handle_t cd_dma = {.dnr = (uintptr_t) 0x25818000UL,
  .dnw = 0,
  .dnc = 0,
  .dnad = 0x00000002,
  .dnmd = 0x00000000};

// stream must be exactly at the start of the sample descriptions.
void film_sample_cache_new(binary_stream_t *stream, uint32_t totalNumSamples,
  film_sample_t *outputSamples, uint8_t *dmaScratch) {
  stream->sampleCache.samples = outputSamples;
  stream->sampleCache.ringBuffStart = dmaScratch;
  stream->sampleCache.readPos = dmaScratch;
  stream->sampleCache.writePos = dmaScratch;

  /* The STAB table is CPU-only.  Stage descriptors through the high-RAM ring
   * before copying them into decode_work_t.  This keeps the direct-CD backend
   * DMA-safe and uses the same layout with the host/game I/O backend. */
  for (uint32_t i = 0; i < totalNumSamples; i++) {
    stream_readbytes(stream, (uint16_t *) dmaScratch, sizeof(film_sample_t));
    film_sample_t sample;
    memcpy(&sample, dmaScratch, sizeof sample);

    film_sample_t *newSample = &outputSamples[i];
    newSample->offset = 0;
    newSample->length = sample.length;
    newSample->time = sample.time;
    newSample->duration = sample.duration;
  }

  initRingBuffer(stream);
}

void stream_new(binary_stream_t *stream, cdfs_filelist_entry_t *entry,
  void *sampleBuffer, uint32_t sampleBufferSize) {
  stream->startFAD = entry->starting_fad;
  stream->size = entry->size;
  stream->remainingSectors = numSectorsForSize(entry->size);
  stream->dataAvailable = 0;
  stream->offset = 0;

  stream->sampleCache.samples = NULL;
  stream->sampleCache.numSamples = 0;
  stream->sampleCache.currentSample = 0;
  stream->sampleCache.currentBuffSample = 0;
  stream->eof = false;
  stream->sampleCache.ringBuffEnd = (uint8_t *) sampleBuffer + sampleBufferSize;

  if (film_buff_io_active()) {
    stream->remainingSectors = 0;
    return;
  }

  int endStatus __unused = cd_block_cmd_data_transfer_end();

  const uint32_t sectorsReady =
    getSectorsReady(MIN(stream->remainingSectors, SECTORS_PREFETCH));

  int status;
  do {
    status = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);
  } while (status & CD_STATUS_WAIT);

  waitUntilCdDataIsAvailable();

  stream->dataAvailable = sectorsReady * CDFS_SECTOR_SIZE;
  stream->remainingSectors -= sectorsReady;
}

void triggerDataRequest(binary_stream_t *stream, uint32_t sectors) {
  // End previous transfers
  int status __unused = cd_block_cmd_data_transfer_end();

  const uint32_t sectorsReady =
    getSectorsReady(MIN(stream->remainingSectors, sectors));

  while (true) {
    status = cd_block_cmd_sector_data_get_delete(0, 0, sectorsReady);
    if (status & CD_STATUS_WAIT) {

    } else {
      break;
    }
  }

  bool ready __unused = false;
  for (uint32_t i = 0; i < 240000; i++) {
    if (MEMORY_READ(32, CD_BLOCK(HIRQ)) & DRDY) {
      ready = true;
      break;
    }
  }

  stream->dataAvailable = sectorsReady * CDFS_SECTOR_SIZE;
  stream->remainingSectors -= sectorsReady;
}

void initRingBuffer(binary_stream_t *stream) {
  film_sample_t nextSample = stream->sampleCache.samples[stream->sampleCache.currentBuffSample];

  /* The original player fills as much of the ring as possible here.  That is
   * fine when it owns the CD block, but Saber Rider's callback I/O makes the
   * whole prefill synchronous inside video_open(), stalling gameplay for
   * dozens of VBlanks on a power cut-in.  Prime only through the first video
   * sample; subsequent samples are fetched by asyncReadBytesIntoRingBuff(). */
  uint32_t prefillSamples = stream->sampleCache.numSamples;
  if (film_buff_io_active()) {
    prefillSamples = stream->sampleCache.numSamples ? 1u : 0u;
    for (uint32_t i = 0; i < stream->sampleCache.numSamples; i++) {
      if (stream->sampleCache.samples[i].time != 0xFFFFFFFFu) {
        prefillSamples = i + 1u;
        break;
      }
    }
  }

  while (stream->sampleCache.currentBuffSample < stream->sampleCache.numSamples &&
         stream->sampleCache.currentBuffSample < (int32_t)prefillSamples) {
    if (stream->sampleCache.writePos + nextSample.length >
      stream->sampleCache.ringBuffEnd) {
      stream->sampleCache.writePos = stream->sampleCache.ringBuffStart;
      break;
    } else {
      stream_readbytes(stream, (uint16_t *) stream->sampleCache.writePos, nextSample.length);
      stream->sampleCache.samples[stream->sampleCache.currentBuffSample].offset = stream->sampleCache.writePos;

      stream->sampleCache.writePos += nextSample.length;
      stream->sampleCache.currentBuffSample++;
      if (stream->sampleCache.currentBuffSample >= stream->sampleCache.numSamples)
        break;
      nextSample = stream->sampleCache.samples[stream->sampleCache.currentBuffSample];
    }
  }
}

void readBytesIntoRingBuff(binary_stream_t *stream) {
  uint8_t *writePtr = stream->sampleCache.writePos;
  film_sample_cache_t *cache = &stream->sampleCache;
  int32_t currIdx = cache->currentSample;
  int32_t buffIdx = cache->currentBuffSample;
  film_sample_t nextSample = cache->samples[buffIdx];
  film_sample_t currentSample = cache->samples[currIdx];
  int32_t nextLen = nextSample.length;
  uint8_t *stopPoint = currentSample.offset;

  if (currentSample.offset == 0) {
    currentSample = cache->samples[currIdx - 1];
    stopPoint = cache->ringBuffEnd;
  }

  if (writePtr + nextLen > cache->ringBuffEnd ||
    buffIdx <= currIdx) {
    writePtr = stream->sampleCache.ringBuffStart;
  }

  if (writePtr + nextLen < stopPoint &&
    buffIdx < cache->numSamples) {
    stream_readbytes(stream, (uint16_t *) writePtr, nextLen);
    cache->samples[buffIdx].offset = writePtr;

    writePtr += nextLen;
    buffIdx++;
  }
  stream->sampleCache.currentBuffSample = buffIdx;
  stream->sampleCache.writePos = writePtr;
}

void stream_readbytes(binary_stream_t *stream, uint16_t *destPtr, uint32_t len) {
  if (film_buff_io_active()) {
    uint8_t *dst = (uint8_t *) destPtr;
    uint32_t done = 0;
    while (done < len) {
      uint32_t got = filmIo.read_fn(filmIo.user, dst + done, len - done);
      if (got == 0) {
        stream->eof = true;
        break;
      }
      done += got;
    }
    stream->offset += done;
    return;
  }

  uint32_t remainingBytes = len;

  while (remainingBytes) {
    if (!stream->dataAvailable)
      triggerDataRequest(stream, SECTORS_PREFETCH);
    int32_t readSize = stream->dataAvailable;
    if (readSize > remainingBytes) {
      readSize = remainingBytes;
    }

    cd_dma.dnw = (uintptr_t) destPtr;
    cd_dma.dnc = readSize;

    scu_dma_config_set(0, SCU_DMA_START_FACTOR_ENABLE, &cd_dma, NULL);
    scu_dma_level_fast_start(0);
    scu_dma_level_wait(0);
    cpu_cache_purge();
    destPtr += readSize >> 1;
    stream->dataAvailable -= readSize;
    remainingBytes -= readSize;
  }

  stream->offset += len;
}

typedef struct {
  bool requestPending;
  uint32_t sectorsThisRequest;
  uint8_t *nextWritePtr;
  int32_t sampleBytesRemaining;
  uint8_t *sampleStartPtr;
  int32_t targetBuffIdx;
} async_cd_state_t;

static async_cd_state_t asyncCd = {0};
static uint32_t asyncBytesDelivered = 0;

uint32_t film_buff_async_bytes_delivered(void) {
  return asyncBytesDelivered;
}

void film_buff_credit_async_bytes_delivered(uint32_t bytes) {
  asyncBytesDelivered += bytes;
}

void film_buff_reset_async_state(void) {
  asyncCd = (async_cd_state_t) {0};
  asyncBytesDelivered = 0;
}
void asyncReadBytesIntoRingBuff(binary_stream_t *stream) {
  film_sample_cache_t *cache = &stream->sampleCache;

  if (film_buff_io_active()) {
    if (asyncCd.sampleBytesRemaining <= 0) {
      int32_t currIdx = cache->currentSample;
      int32_t buffIdx = cache->currentBuffSample;
      if (buffIdx >= cache->numSamples) return;

      film_sample_t nextSample = cache->samples[buffIdx];
      film_sample_t currentSample = cache->samples[currIdx];
      uint8_t *stopPoint = currentSample.offset;
      if (currentSample.offset == 0) {
        if (currIdx <= 0) return;
        currentSample = cache->samples[currIdx - 1];
        stopPoint = cache->ringBuffEnd;
      }

      uint8_t *writePtr = cache->writePos;
      if (writePtr + nextSample.length > cache->ringBuffEnd || buffIdx <= currIdx)
        writePtr = cache->ringBuffStart;
      if (!(writePtr + nextSample.length < stopPoint)) return;

      asyncCd.sampleBytesRemaining = (int32_t) nextSample.length;
      asyncCd.sampleStartPtr = writePtr;
      asyncCd.nextWritePtr = writePtr;
      asyncCd.targetBuffIdx = buffIdx;
      cache->writePos = writePtr;
    }

    uint32_t avail = filmIo.available_fn ? filmIo.available_fn(filmIo.user) :
      (uint32_t) asyncCd.sampleBytesRemaining;
    if (avail == 0) return;
    uint32_t want = (uint32_t) asyncCd.sampleBytesRemaining;
    if (want > avail) want = avail;
    uint32_t got = filmIo.read_fn(filmIo.user, asyncCd.nextWritePtr, want);
    if (got == 0) { stream->eof = true; return; }

    asyncCd.nextWritePtr += got;
    asyncCd.sampleBytesRemaining -= (int32_t) got;
    stream->offset += (int32_t) got;
    asyncBytesDelivered += got;

    if (asyncCd.sampleBytesRemaining <= 0) {
      cache->samples[asyncCd.targetBuffIdx].offset = asyncCd.sampleStartPtr;
      cache->currentBuffSample = asyncCd.targetBuffIdx + 1;
      cache->writePos = asyncCd.nextWritePtr;
      asyncCd.sampleBytesRemaining = 0;
    }
    return;
  }

  if (asyncCd.sampleBytesRemaining <= 0 && !asyncCd.requestPending) {
    int32_t currIdx = cache->currentSample;
    int32_t buffIdx = cache->currentBuffSample;
    if (buffIdx >= cache->numSamples) {
      return; // nothing left to prefetch
    }

    film_sample_t nextSample = cache->samples[buffIdx];
    film_sample_t currentSample = cache->samples[currIdx];
    int32_t nextLen = nextSample.length;
    uint8_t *stopPoint = currentSample.offset;

    if (currentSample.offset == 0) {
      currentSample = cache->samples[currIdx - 1];
      stopPoint = cache->ringBuffEnd;
    }

    uint8_t *writePtr = cache->writePos;
    if (writePtr + nextLen > cache->ringBuffEnd || buffIdx <= currIdx) {
      writePtr = cache->ringBuffStart;
    }

    if (!(writePtr + nextLen < stopPoint) || buffIdx >= cache->numSamples) {
      return; // no room yet - try again next tick
    }

    asyncCd.sampleBytesRemaining = nextLen;
    asyncCd.sampleStartPtr = writePtr;
    asyncCd.nextWritePtr = writePtr;
    asyncCd.targetBuffIdx = buffIdx;
    cache->writePos = writePtr; // reserve the space now, same as the sync path
    return; // start the actual CD request on the next call
  }

  if (asyncCd.sampleBytesRemaining <= 0) {
    return;
  }

  if (!asyncCd.requestPending) {
    if (stream->dataAvailable == 0) {
      if (stream->remainingSectors == 0) {
        return; // EOF - nothing more to fetch
      }

      uint32_t wantSectors = MIN(stream->remainingSectors, SECTORS_PREFETCH);

      int32_t sectorsReady = cd_block_cmd_sector_number_get(0);
      if ((uint32_t) sectorsReady < wantSectors) {
        return;
      }

      int status = cd_block_cmd_data_transfer_end();
      if (status & CD_STATUS_WAIT) {
        return;
      }
      status = cd_block_cmd_sector_data_get_delete(0, 0, wantSectors);
      if (status & CD_STATUS_WAIT) {
        return; // command port busy - try again next tick
      }

      asyncCd.sectorsThisRequest = wantSectors;
      asyncCd.requestPending = true;
    }
  }

  if (asyncCd.requestPending) {
    if (!(MEMORY_READ(16, CD_BLOCK(HIRQ)) & DRDY)) {
      return; // not ready yet - try again next tick
    }

    stream->dataAvailable += asyncCd.sectorsThisRequest * CDFS_SECTOR_SIZE;
    stream->remainingSectors -= asyncCd.sectorsThisRequest;
    asyncCd.requestPending = false;
  }

  if (stream->dataAvailable > 0) {
    int32_t readSize = stream->dataAvailable;
    if (readSize > asyncCd.sampleBytesRemaining) {
      readSize = asyncCd.sampleBytesRemaining;
    }

    volatile uint16_t *cdData = (volatile uint16_t *) CD_BLOCK_DATA_2;
    uint16_t *dst = (uint16_t *) asyncCd.nextWritePtr;
    uint32_t words = (uint32_t)readSize >> 1;
    for (uint32_t i = 0; i < words; ++i) {
      *dst++ = *cdData;
    }

    asyncCd.nextWritePtr += readSize;
    asyncCd.sampleBytesRemaining -= readSize;
    stream->dataAvailable -= readSize;
    stream->offset += readSize;
    asyncBytesDelivered += readSize;

    if (asyncCd.sampleBytesRemaining <= 0) {
      cache->samples[asyncCd.targetBuffIdx].offset = asyncCd.sampleStartPtr;
      cache->currentBuffSample = asyncCd.targetBuffIdx + 1;
      cache->writePos = asyncCd.nextWritePtr;
      asyncCd.sampleBytesRemaining = 0;
    }
  }
}
