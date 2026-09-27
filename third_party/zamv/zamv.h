#ifndef ZAMV_H
#define ZAMV_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZAMV_VERSION 5
#define ZAMV_MAGIC "ZAMV"
#define ZAMV_CHROMA_420 0

/* Decoder-facing frame surface. Planes are tightly packed. */
typedef struct zamv_frame {
    int width, height;
    uint8_t *storage;
    uint8_t *y, *u, *v;
} zamv_frame_t;

typedef struct zamv_file_header {
    uint16_t width, height;
    uint32_t fps_num, fps_den;
    uint32_t frame_count;
    uint32_t target_video_bps;
    uint32_t reserved_audio_bps;
    uint16_t keyint;
    uint8_t chroma;
    uint8_t version;
} zamv_file_header_t;

typedef struct zamv_encoder_config {
    int width, height;
    uint32_t fps_num, fps_den;
    uint32_t target_video_bps;
    uint32_t reserved_audio_bps;
    uint32_t max_total_bps;   /* encoder-only hard rolling budget, 0 disables */
    int vbv_window_ms;        /* encoder-only rolling window, e.g. 500 ms */
    int keyint;
    int search_range;       /* integer pixels */
    int qmin, qmax;         /* scalar quantizer, 1..255 */
    int qinitial;
    int enable_qpel;        /* bilinear quarter-pixel refinement */
    int enable_split8;      /* allow four 8x8 motion partitions */
    int golden_period;
} zamv_encoder_config_t;

typedef struct zamv_encoder zamv_encoder_t;
typedef struct zamv_decoder zamv_decoder_t;
/* Generic sequential input. The callback may return fewer than `want` bytes;
   zero bytes with status 0 is EOF. This permits CD/GD-ROM readers, network
   streams, or fixed-size sector buffers without FILE* in the decoder core. */
typedef int (*zamv_read_callback_t)(void *user, void *dst, size_t want, size_t *got);
typedef struct zamv_chunk_reader zamv_chunk_reader_t;

/* Chunk reader requests fixed sector-aligned blocks from its source and serves
   arbitrary decoder reads across chunk boundaries. chunk_bytes must be a
   multiple of 2048; 32768 or 65536 are recommended for CD streaming. */
zamv_chunk_reader_t *zamv_chunk_reader_create(zamv_read_callback_t source, void *source_user, size_t chunk_bytes);
void zamv_chunk_reader_destroy(zamv_chunk_reader_t *reader);
int zamv_chunk_reader_read(void *reader, void *dst, size_t want, size_t *got);
size_t zamv_chunk_reader_chunk_bytes(const zamv_chunk_reader_t *reader);
uint64_t zamv_chunk_reader_refills(const zamv_chunk_reader_t *reader);
uint64_t zamv_chunk_reader_source_bytes(const zamv_chunk_reader_t *reader);


typedef struct zamv_decoder_stats {
    uint64_t frames;
    uint64_t mb_skip, mb_inter, mb_intra, mb_fill;
    uint64_t mb_inter_seed_reuse; /* LAST+(0,0) predictor MBs; eager mode reuses pre-seed */
    uint64_t frame_seed_bytes;     /* whole-frame bytes copied by eager seed policy */
    uint64_t lazy_seed_bytes;      /* MB bytes copied by lazy seed policy */
    uint64_t golden_refresh_copy_bytes; /* old-policy full-frame GOLDEN refresh copies */
    uint64_t mv_regions_integer, mv_regions_hfrac, mv_regions_vfrac, mv_regions_xyfrac;
    uint64_t mv_phase_q4[16]; /* index=(fy&3)*4+(fx&3); desktop profiling aid */
    uint64_t residual_maps_sparse, residual_maps_dense;
    uint64_t blocks_zero, blocks_dc, blocks_1d, blocks_full;
    uint64_t blocks_1d_kind[6]; /* lowrow,row2,col0,col1,col2,col3 */
} zamv_decoder_stats_t;


typedef void (*zamv_row_callback_t)(void *user, const zamv_frame_t *frame, int macroblock_row);

int zamv_frame_alloc(zamv_frame_t *f, int width, int height);
void zamv_frame_free(zamv_frame_t *f);
size_t zamv_frame_bytes(int width, int height);

zamv_encoder_t *zamv_encoder_create(const zamv_encoder_config_t *cfg);
void zamv_encoder_destroy(zamv_encoder_t *enc);
int zamv_encoder_write_header(zamv_encoder_t *enc, FILE *out, uint32_t frame_count);
int zamv_encoder_encode(zamv_encoder_t *enc, const zamv_frame_t *src, FILE *out);

zamv_decoder_t *zamv_decoder_create(void);
void zamv_decoder_destroy(zamv_decoder_t *dec);
int zamv_decoder_read_header(zamv_decoder_t *dec, FILE *in, zamv_file_header_t *hdr);
int zamv_decoder_read_header_cb(zamv_decoder_t *dec, zamv_read_callback_t read_cb, void *read_user, zamv_file_header_t *hdr);
void zamv_decoder_set_row_callback(zamv_decoder_t *dec, zamv_row_callback_t cb, void *user);
void zamv_decoder_get_stats(const zamv_decoder_t *dec, zamv_decoder_stats_t *stats);
void zamv_decoder_reset_stats(zamv_decoder_t *dec);
/* returns 1 frame, 0 EOF, negative on error */
int zamv_decoder_decode(zamv_decoder_t *dec, FILE *in, zamv_frame_t *out);
int zamv_decoder_decode_cb(zamv_decoder_t *dec, zamv_read_callback_t read_cb, void *read_user, zamv_frame_t *out);

/* Pack one decoded YUV420 frame into Dreamcast PVR YUV-converter macroblocks:
   64 U, 64 V, 256 Y bytes per 16x16 MB, raster MB order. dst must hold
   ceil(width/16)*ceil(height/16)*384 bytes. Edges are replicated. */
size_t zamv_pvr420_packed_bytes(int width, int height);
void zamv_pack_pvr420(const zamv_frame_t *src, uint8_t *dst);

#ifdef __cplusplus
}
#endif
#endif
