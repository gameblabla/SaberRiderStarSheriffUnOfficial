#ifndef FILM_BUFF_OLD_H
#define FILM_BUFF_OLD_H

#include "film_lib.h"

typedef uint32_t (*film_io_read_fn)(void *user, void *dst, uint32_t len);
typedef uint32_t (*film_io_available_fn)(void *user);

/* Optional host/game I/O backend.  When set, the Cinepak decoder reads the
 * FILM stream through these callbacks instead of owning the CD block. */
extern void film_buff_io_set(void *user, film_io_read_fn read_fn, film_io_available_fn available_fn);
extern void film_buff_io_clear(void);
extern bool film_buff_io_active(void);
extern void film_sample_cache_new(binary_stream_t *stream, uint32_t totalNumSamples,
  film_sample_t *outputSamples, uint8_t *dmaScratch);

extern void stream_new(binary_stream_t *stream, cdfs_filelist_entry_t *entry,
  void *sampleBuffer, uint32_t sampleBufferSize);

extern void triggerDataRequest(binary_stream_t *stream, uint32_t sectors);

extern void stream_readbytes(binary_stream_t *stream, uint16_t *destPtr, uint32_t len);

extern void initRingBuffer(binary_stream_t *stream);

extern void readBytesIntoRingBuff(binary_stream_t *stream);

extern void asyncReadBytesIntoRingBuff(binary_stream_t *stream);
extern void film_buff_reset_async_state(void);

extern uint32_t film_buff_async_bytes_delivered(void);
extern void film_buff_credit_async_bytes_delivered(uint32_t bytes);

#endif // FILM_BUFF_OLD_H
