#ifndef SV24_VDP2_LAYOUT_H
#define SV24_VDP2_LAYOUT_H
#include <stdint.h>
#include "sv24_slice.h"
#define SV24_VRAM_BANK_BYTES       0x20000u
#define SV24_VRAM_A0_OFFSET        0x00000u
#define SV24_VRAM_A1_OFFSET        0x20000u
#define SV24_VRAM_B0_OFFSET        0x40000u
#define SV24_VRAM_B1_OFFSET        0x60000u
#define SV24_BAND_TILE_ROWS        10u
#define SV24_BAND_TILES            (SV24_TILES_X * SV24_BAND_TILE_ROWS)
#define SV24_CELL_BYTES            256u
#define SV24_PND_PAGE_TILES        64u
#define SV24_PND_WORDS_PER_ENTRY   2u
#define SV24_PND_PAGE_BYTES        (SV24_PND_PAGE_TILES * SV24_PND_PAGE_TILES * 4u)
#define SV24_PND_VRAM_OFFSET       SV24_VRAM_B1_OFFSET
#define SV24_DMA_MAX_RUNS          SV24_SLICE_MAX_TILES

typedef struct {uint16_t src_cell;uint16_t cell_count;uint32_t vram_byte_offset;} sv24_dma_run_t;
typedef struct {uint16_t count;sv24_dma_run_t run[SV24_DMA_MAX_RUNS];} sv24_dma_plan_t;
uint32_t sv24_vdp2_tile_byte_offset(uint16_t tile_id);
uint16_t sv24_vdp2_tile_charno(uint16_t tile_id);
void sv24_vdp2_build_pnd_page(uint16_t *dst_words);
int sv24_vdp2_make_slice_dma_plan(const uint8_t *slice,uint32_t slice_size,
                                  uint16_t tile_y0,uint8_t tile_rows,
                                  sv24_dma_plan_t *plan);
#endif
