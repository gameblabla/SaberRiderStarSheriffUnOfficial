#ifndef ZAMV_DC_HELPERS_H
#define ZAMV_DC_HELPERS_H
#include "zamv.h"
#include <stddef.h>
#include <stdint.h>

/* Bytes produced/consumed by one 16-line YUV420 macroblock row. */
size_t zamv_dc_mb_row_bytes(int width);

/* Pack macroblocks as PVR YUV-converter input: U64, V64, Y256 per MB. */
size_t zamv_dc_pack_mb_span(const zamv_frame_t *src, int mb_y, int mb_x, int mb_count, uint8_t *dst);
size_t zamv_dc_pack_mb_row(const zamv_frame_t *src, int mb_y, uint8_t *dst);

/* A small staging buffer is preferable to a full 15,360-byte 640px row because
   the latter nearly fills the SH-4's 16 KiB D-cache. Default is four MBs (1536 B). */
size_t zamv_dc_recommended_staging_bytes(int width);

/* Feed one or more complete 384-byte YUV420 macroblocks to the PVR YUV
   converter through an already-translated/locked SQ destination. The converter
   reuses the same 384-byte SQ aperture for every macroblock. */
void zamv_dc_sq_send_pvr(void *pvr_yuv_sq, const uint8_t *aligned_src, size_t bytes);

/* Configure the Dreamcast PVR YUV converter for an exact YUV420 image size.
   width/height must both be multiples of 16. pvr_texture is the destination
   VRAM pointer returned by pvr_mem_malloc(). Returns 0 on Dreamcast, negative
   for invalid parameters or when built on a non-Dreamcast host. */
int zamv_dc_configure_yuv420(void *pvr_texture, int width, int height);

/* Validate the SH4ZAM/FTRV inverse transform against fixed desktop-reference
   vectors. Run once during player startup; zero means exact agreement. */
int zamv_dc_transform_selftest(void);

/* Validate the SH-4 Store Queue gather against the normative packer by actually
   running it. Only meaningful on Dreamcast (returns -2 elsewhere), because the
   host test suite can never compile the SH-4 gather path. Zero means the bursts
   reproduce the converter layout byte for byte. */
int zamv_dc_gather_selftest(void);

typedef struct zamv_dc_row_uploader {
    void *pvr_yuv_port; /* translated SQ pointer while locked */
    uint8_t *staging;     /* 32-byte aligned, >=384 bytes; 1536 recommended */
    size_t staging_bytes;
    int sq_locked;
} zamv_dc_row_uploader_t;

/* Lock/configure the KOS store queues for PVR_TA_YUV_CONV. Call begin once
   immediately before decoding a frame and end immediately afterward. This avoids
   30 lock/QACR setup operations per 640x480 frame. */
int zamv_dc_row_uploader_begin(zamv_dc_row_uploader_t *u);
void zamv_dc_row_uploader_end(zamv_dc_row_uploader_t *u);

/* Install this as zamv_decoder_set_row_callback(dec, zamv_dc_upload_row_cb, &ctx).
   Each completed row is packed in small MB groups and pushed through SQ immediately. */
void zamv_dc_upload_row_cb(void *user, const zamv_frame_t *frame, int macroblock_row);

/* Experimental no-staging Dreamcast path. It gathers the U/V/Y rows directly
   from the reconstructed reference surface into 32-byte Store Queue bursts.
   This removes the intermediate 384/1536-byte pack buffer entirely and toggles
   SZ only once per macroblock row. Returns 0 on the real SH-4 backend. */
int zamv_dc_sq_send_row_direct(void *pvr_yuv_sq, const zamv_frame_t *frame, int macroblock_row);
void zamv_dc_upload_row_direct_cb(void *user, const zamv_frame_t *frame, int macroblock_row);

#endif
