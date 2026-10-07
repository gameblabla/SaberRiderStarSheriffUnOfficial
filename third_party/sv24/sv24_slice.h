#ifndef SV24_SLICE_H
#define SV24_SLICE_H
#include <stddef.h>
#include <stdint.h>

#define SV24_TILES_X 44u
#define SV24_TILES_Y 30u
#define SV24_TILE_PIXELS 64u
#define SV24_FRAME_TILES (SV24_TILES_X * SV24_TILES_Y)
#define SV24_SLICE_MAX_TILES 132u /* v0.3 = 3 rows x 44 */
/* Wire maps retain 44 columns. Independent absolute records may decode into
 * a narrower RAM surface; full-width motion/gradient streams use 44. */
extern unsigned sv24_output_tiles_x;
extern unsigned sv24_output_tile_rows;

/* v25.4 transfer plan: one 44-bit changed-cell bitmap per slice row.
 * The decoder writes only these tiny masks; SCU-DMA runs are materialized
 * afterward.  Two u32 words keep all operations native on SH-2. */
typedef struct {
    uint16_t y0;
    uint8_t rows;
    uint8_t reserved;
    uint32_t row_lo[3];  /* x 0..31 */
    uint32_t row_hi[3];  /* x 32..43 in bits 0..11 */
} sv24_slice_dma_plan_t;

/* Compact changed-cell output, useful for tests or alternative renderers. */
typedef struct {
    uint16_t count;
    uint16_t tile_id[SV24_SLICE_MAX_TILES];
    uint32_t pixels[SV24_SLICE_MAX_TILES * SV24_TILE_PIXELS];
} sv24_slice_output_t;

int sv24_decode_abs_slice(const uint8_t *slice, size_t slice_size,
                          uint16_t tile_y0, uint8_t tile_rows,
                          sv24_slice_output_t *out);

int sv24_decode_abs_slice_frame(const uint8_t *slice, size_t slice_size,
                                uint16_t tile_y0, uint8_t tile_rows,
                                uint32_t *frame_cells);

int sv24_decode_abs_slice_frame_plan(const uint8_t *slice, size_t slice_size,
                                     uint16_t tile_y0, uint8_t tile_rows,
                                     uint32_t *frame_cells,
                                     sv24_slice_dma_plan_t *plan,
                                     unsigned max_gap_pixels);

int sv24_decode_abs_slice_frame_motion(const uint8_t *slice, size_t slice_size,
                                       uint16_t tile_y0, uint8_t tile_rows,
                                       uint32_t *frame_cells,
                                       const uint32_t *motion_ref,
                                       int global_mv_x, int global_mv_y);

int sv24_decode_abs_slice_frame_plan_motion(const uint8_t *slice, size_t slice_size,
                                            uint16_t tile_y0, uint8_t tile_rows,
                                            uint32_t *frame_cells,
                                            sv24_slice_dma_plan_t *plan,
                                            unsigned max_gap_pixels,
                                            const uint32_t *motion_ref,
                                            int global_mv_x, int global_mv_y);
#endif

void sv24_slice_init_tables(void);
