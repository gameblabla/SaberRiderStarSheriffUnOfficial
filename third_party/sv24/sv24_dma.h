#ifndef SV24_DMA_H
#define SV24_DMA_H
#include <stdint.h>
#include "sv24_frame_v03.h"
#include "sv24_slice.h"

void sv24_dma_init(void);
int sv24_dma_prepare_plans(const uint32_t *staging,
                           const sv24_slice_dma_plan_t *plans,
                           unsigned plan_count,
                           unsigned max_gap_pixels);
int sv24_dma_prepare_maps(const uint32_t *staging, const sv24_v03_frame_t *fr,
                           unsigned max_gap_pixels);
int sv24_dma_prepare_dirty_cells(const uint32_t *staging, const uint8_t *dirty,
                                  unsigned max_gap_pixels);
int sv24_dma_upload_prepared_band(unsigned band);
int sv24_dma_upload_band_full(const uint32_t *staging, unsigned band);

extern volatile uint32_t sv24_dma_submissions;
extern volatile uint32_t sv24_dma_runs;
extern volatile uint32_t sv24_dma_bytes;
extern volatile uint32_t sv24_dma_fallback_bands;
extern volatile uint32_t sv24_dma_prepare_ticks_total, sv24_dma_prepare_ticks_max, sv24_dma_prepare_calls;
extern volatile uint32_t sv24_dma_upload_ticks_total, sv24_dma_upload_ticks_max, sv24_dma_upload_calls;
extern volatile uint32_t sv24_dma_last_runs[3];
extern volatile uint32_t sv24_dma_last_bytes[3];
extern volatile uint32_t sv24_dma_max_runs[3];
extern volatile uint32_t sv24_dma_max_bytes[3];
#endif
