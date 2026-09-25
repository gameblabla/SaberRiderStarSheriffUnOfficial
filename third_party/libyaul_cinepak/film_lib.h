#ifndef FILM_LIB_H
#define FILM_LIB_H
#include "base.h"

#define SECTORS_PREFETCH         8

#define COLOR_DEPTH_15 15
#define COLOR_DEPTH_24 24

#define MAX_STRIPS 2
#define FILM_MAX_SAMPLES 2048

// fdsc.sound_codec values
#define FDSC_CODEC_ADX 2

typedef enum {
  ERROR = -1,
  STOP = 0,
  PAUSE = 1,
  PLAY = 2,
  END = 3,
  INIT = 4
} playback_status_t;

typedef enum {
    PCM_XFER_CPU = 0,
    PCM_XFER_SH2_DMA = 1,
    PCM_XFER_SCU_DMA = 2, // Currently not supported
    PCM_XFER_SCU_DSP_DMA = 3, // Currently not supported.
} pcm_transfer_mode_t;

typedef struct {
  uint8_t y[4];
  int8_t u;
  int8_t v;
} codebook_t;

typedef struct {
  int32_t color[4];
} codebookRGB_t;

typedef struct {
  codebookRGB_t v1RGB[256];
  codebookRGB_t v4RGB[256];
} __aligned(16) strip_codebook_t;

// Sample data can be stored as a single uint32_t with the MSB telling us if its
// a video (0) or audio sample (1). If it is a video sample, the rest of the
// data holds the 'info2' field from the STAB data and if its audio, it holds
// the sample length.
typedef struct {
  uint8_t *offset;
  uint32_t length;
  uint32_t time;
  uint32_t duration;
} __packed __aligned(4) film_sample_t;

typedef struct {
  int32_t numSamples;
  int32_t currentSample;
  int32_t currentBuffSample;
  int32_t pcmBytesPerBlank;
  int32_t remainingPcmBytes;
  int32_t pcmPlayPosition;
  uint8_t *ringBuffStart;
  uint8_t *ringBuffEnd;
  uint8_t *writePos;
  uint8_t *readPos;
  uint32_t totalBytesWritten;
  uint32_t totalBytesConsumed;
  int32_t maxSampleLength;
  film_sample_t *samples;
  uint32_t readyThreshold;
  uint32_t OffsetWrite;
  uint32_t OffsetRead;
  uint8_t *AddrWrite;
  uint8_t *AddrRead;
} __packed __aligned(4) film_sample_cache_t;

typedef struct {
  int32_t startFAD;
  int32_t size;
  int32_t remainingSectors;

  int32_t dataAvailable;
  int32_t offset;

  film_sample_cache_t sampleCache;

  bool eof;

} __packed __aligned(4) binary_stream_t;

typedef struct {
  int32_t strip;
  int32_t topX;
  int32_t writeX;
  int32_t bottomX;
  int32_t topY;
  int32_t writeY;
  int32_t bottomY;
  strip_codebook_t codebooks[MAX_STRIPS];

} __packed __aligned(4) stripdata_t;

typedef struct {
  int32_t offset;
  uint32_t size;
  uint32_t play_tick;
  uint32_t duration_ticks;
} __packed __aligned(
  4) stab_entry; // same as cd_film_sample_t, but this is what the actual
                 // variables are referred to in SBL.

typedef struct {
  char stab_str[4];
  int32_t stab_size;
  int32_t ticks_per_second;
  int32_t total_entries;
  film_sample_t entries[0];
} __packed __aligned(4) stab_table;

typedef struct {
  char fdsc_str[4];
  int32_t fdsc_size;
  char fourcc[4];
  int32_t height;
  int32_t width;
  int8_t color_depth;      // Usually 24
  int8_t sound_channels;   // 1 or 2
  int8_t sound_resolution; // 8 bit or 16 bit
  int8_t sound_codec;      // 0 = PCM, 1 = Sega ADPCM, 2= ADX
  int32_t sample_rate;
  int32_t chroma_key;
} __packed __aligned(4) frame_description;

typedef struct {
  char film_str[4];
  int32_t header_size;
  int32_t version;
  int32_t reserved;
  frame_description fdsc;
  stab_table stab;
} __packed __aligned(4) film_header;

typedef struct {
  uint32_t *sampleBuffAddr;
  int32_t sampleBuffSize;
  uint32_t *vramBuffAddr;
  uint32_t *vramWritePos;
  int32_t vramBuffSize;
  int16_t vramBufferWidth;
  int16_t vramBufferHeight;
  int32_t vramDelta;
  int32_t audioBufferAddr;
  int32_t audioBufferSize;
  int8_t decodeColorDepth;
  bool audioEnable;
  int8_t pcmChannels;
  int8_t pcmVolume;
  int8_t pcmPan;
  pcm_transfer_mode_t pcmTransferMode;
} __packed __aligned(4) decode_param_t;

typedef struct decode_work_t {
  decode_param_t *decodeParams;
  playback_status_t play_status;
  int32_t timeEllapsed;
  int32_t frtOverflowCount;
  uint32_t ticksUntilNextFrame;
  uint32_t tickCount;
  uint32_t lastSampleEndTime;
  int32_t tickStart;
  int32_t lastFrameTime;
  int32_t targetAudioBytes;
  int32_t copyingVideoFrame;
  int32_t videoStartY;
  int32_t dma_delta;
  bool isDisplayReady;
  bool displayWaiting;
  bool audioPlaying;
  bool audioWaitingToStart;
  stripdata_t stripData;
  int32_t padding;
  film_header filmHeader;
  binary_stream_t stream;
  /* Sample descriptors are CPU-only; keep them with decode_work_t so the
   * high-work-RAM sample buffer is entirely available as the DMA ring. */
  film_sample_t sampleTable[FILM_MAX_SAMPLES];
  film_sample_t nextSample;
  bool hasVideoSamples;
  int32_t lastAudioSampleIndex;
  bool (*parseAudioFn)(struct decode_work_t *work);
} __packed __aligned(4) decode_work_t;

// Return 0 to stop film playback
extern int film_loop_handler();

extern void init_film_start(cdfs_filelist_entry_t *fsEntry, decode_work_t *work,
  int32_t vdp_height, int32_t vdp_width);

extern void cpk_play(decode_work_t *work);

extern void cpk_task(decode_work_t *work);

extern bool cpk_display_ready(decode_work_t *work);

extern void cpk_display_finished(decode_work_t *work);

extern void cpk_vbl_task();


#endif // FILM_LIB_H
