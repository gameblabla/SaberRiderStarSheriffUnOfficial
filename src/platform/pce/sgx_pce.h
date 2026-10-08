#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* "SGX1" carries the block version; flags bits 0-2 are active, priority,
       and paired, while bits 3-7 shadow the current VDC1 register index. */
    uint8_t magic[4], flags, paired_screen, colors, uploads, failures;
    uint16_t bytes_uploaded;
    /* Appended so existing SGX1 offsets remain stable. */
    uint8_t vdc1_max_units, vdc1_sat_count;
} PceSgxTelemetry;

typedef struct __attribute__((packed)) {
    uint32_t tiles,map;
    uint16_t bytes; /* Sky: pattern count. Space-near record: pattern bytes. */
    uint8_t bat_cols;
    uint32_t foreground;
    uint16_t nforeground;
    uint32_t foreground_slow;
    uint16_t nforeground_slow;
    uint16_t foreground_first;
    uint16_t cols;
    uint16_t speed; /* Q8 pixels per source frame (wrapped) or camera pixel. */
    uint8_t wrap;
    uint32_t near_map;
    uint16_t near_cols,near_first_tile,near_speed;
    uint8_t split_row;
    uint32_t moon_patterns,moon_palette;
} PceSgxSkyRecord;
_Static_assert(sizeof(PceSgxSkyRecord)==49,"SGX sky archive layout changed");

#define PCE_SGX_ACTIVE       0x01
#define PCE_SGX_PRIORITY     0x02
#define PCE_SGX_PAIR_ACTIVE  0x04
#define PCE_SGX_SPACE_NEAR   0x08
#define PCE_SGX_GAMEPLAY     0x80
#define PCE_SGX_STATIC_SKY   0x40
#define PCE_SGX_ARENA_SPRITES 0x20
#define PCE_SGX_SPACE_SPRITES 0x10
#define PCE_SGX_ARENA_BG_PAGE 0x08
#define PCE_SGX_ARENA_BG_READY 0x10

#ifdef PCE_SGX
void pce_sgx_copy(void *dst,const void *src,uint16_t bytes);
extern volatile PceSgxTelemetry pce_sgx_metrics;
#define pce_sgx_active ((pce_sgx_metrics.flags & PCE_SGX_ACTIVE) != 0)
#define pce_sgx_vdc2_index ((pce_sgx_metrics.flags >> 3) & 0x1f)
#define PCE_SGX_RECORD_VDC2_INDEX(reg) \
    (pce_sgx_metrics.flags = (uint8_t)((pce_sgx_metrics.flags & 0x07) | (((reg) & 0x1f) << 3)))

static inline bool pce_sgx_gameplay(void) {
    return pce_sgx_active && (pce_sgx_metrics.paired_screen & PCE_SGX_GAMEPLAY) != 0;
}
static inline __attribute__((always_inline)) bool pce_sgx_vdc1_sprites(void) {
    return pce_sgx_gameplay();
}
static inline __attribute__((always_inline)) bool pce_sgx_arena_sprites(void) {
    return pce_sgx_active &&
        (pce_sgx_metrics.paired_screen & PCE_SGX_ARENA_SPRITES) != 0;
}

extern volatile uint8_t pce_sgx_sat1_alt, pce_sgx_sat1_pending,pce_sgx_sat1_ready;
extern volatile uint8_t pce_sgx_vdc1_hidden;
extern volatile uint8_t pce_sgx_arena_bg_pending_page;
extern volatile uint16_t pce_sgx_sprite_id;
extern volatile uint8_t pce_sgx_sprite_slot, pce_sgx_sprite_upload_ok;
extern uint8_t pce_sgx_hero_first;
extern uint8_t pce_sgx_split_active,pce_sgx_split_count,pce_sgx_split_last;
extern uint8_t pce_sgx_actor_plane[8],pce_sgx_actor_stage;

void pce_sgx_detect_init(void);
void pce_sgx_display_on_body(void);
void pce_sgx_ui_load_body(void);
void pce_sgx_ui_end_body(void);
void pce_sgx_gameplay_begin_body(void);
void pce_sgx_gameplay_end_body(void);
void pce_sgx_sky_load_body(void);
void pce_sgx_column_write_body(void);
void pce_sgx_sky_scroll_body(void);
void pce_sgx_sky_stream_body(void);
void pce_sgx_ui_mode_body(void);
void pce_sgx_vdc1_sat_upload_body(void);
void pce_sgx_vdc1_stats_body(void);
void pce_sgx_sprite_upload_body(void);
void pce_sgx_vdc1_hide_body(void);
void pce_sgx_hull_retire_body(void);
void pce_sgx_moon_draw_body(void);
void pce_sgx_story_world_body(void);
void pce_sgx_platform_actor_pass_body(void);
#else
static inline bool pce_sgx_gameplay(void) { return false; }
static inline bool pce_sgx_vdc1_sprites(void) { return false; }
static inline bool pce_sgx_arena_sprites(void) { return false; }
static inline void pce_sgx_platform_actor_pass_body(void) {}
#endif
