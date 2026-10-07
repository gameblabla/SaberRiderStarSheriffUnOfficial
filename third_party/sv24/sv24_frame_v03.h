#ifndef SV24_FRAME_V03_H
#define SV24_FRAME_V03_H
#include <stddef.h>
#include <stdint.h>
#define SV24_V03_MAX_SLICES 10u
#define SV24_FRM3_MAP_DMA_SAFE 0x01u
typedef struct {const uint8_t *data;uint32_t size;uint16_t tile_y0;uint8_t tile_rows;uint8_t work_hint;} sv24_v03_slice_t;
typedef struct {uint32_t frame_no,pts;uint16_t flags,changed_tiles,schedule_mask;uint8_t slice_count,coding_flags;sv24_v03_slice_t slice[SV24_V03_MAX_SLICES];} sv24_v03_frame_t;
int sv24_v03_frame_parse(const uint8_t *f,size_t bytes,sv24_v03_frame_t *o);
#endif
