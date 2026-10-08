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
    uint16_t bytes;
    uint8_t bat_cols;
    uint32_t foreground_slow;
    uint16_t nforeground_slow;
} PceSgxSkyRecord;

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

extern volatile uint8_t pce_sgx_sat1_alt, pce_sgx_sat1_pending;
extern volatile uint8_t pce_sgx_vdc1_hidden;
extern volatile uint8_t pce_sgx_arena_bg_pending_page;
extern volatile uint16_t pce_sgx_sprite_id;
extern volatile uint8_t pce_sgx_sprite_slot, pce_sgx_sprite_upload_ok;

void pce_sgx_detect_init(void);
void pce_sgx_display_on_body(void);
void pce_sgx_ui_load_body(void);
void pce_sgx_ui_end_body(void);
void pce_sgx_gameplay_begin_body(void);
void pce_sgx_gameplay_end_body(void);
void pce_sgx_sky_load_body(void);
void pce_sgx_column_write_body(void);
void pce_sgx_sky_scroll_body(void);
void pce_sgx_ui_mode_body(void);
void pce_sgx_vdc1_sat_upload_body(void);
void pce_sgx_vdc1_stats_body(void);
void pce_sgx_sprite_upload_body(void);
void pce_sgx_vdc1_hide_body(void);
void pce_sgx_platform_actor_pass_body(void);
#else
static inline bool pce_sgx_gameplay(void) { return false; }
static inline bool pce_sgx_vdc1_sprites(void) { return false; }
static inline bool pce_sgx_arena_sprites(void) { return false; }
static inline void pce_sgx_platform_actor_pass_body(void) {}
#endif
