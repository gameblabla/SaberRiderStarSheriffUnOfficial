#ifndef SV24_DUAL_H
#define SV24_DUAL_H
#include <stdint.h>
#include "sv24_frame_v03.h"
#include "sv24_slice.h"
extern volatile uint32_t sv24_dual_fault;

void sv24_dual_init(void);
int sv24_dual_decode_frame(const sv24_v03_frame_t *fr, uint32_t *staging,
                           sv24_slice_dma_plan_t *plans, unsigned max_gap_pixels);
extern volatile uint32_t sv24_dual_slave_frames;
extern volatile uint32_t sv24_dual_slave_slices;
extern volatile uint32_t sv24_dual_master_slices;
extern volatile uint32_t sv24_dual_wait_spins;
#endif
