#pragma once
#include "pce_config.h"
#include "assets.h"

typedef struct {
    uint8_t magic[4], version, ready, arcade_ports, stage;
    uint16_t frames, floor_commits, uploads, dropped_cosmetic;
    uint16_t essential_overflow, disc_reads, forbidden_reads, load_error;
    uint8_t max_units, sat_count, hero, phase;
    uint16_t player_x, player_y, camera_x, hp;
} PceTelemetry;
extern volatile PceTelemetry pce_metrics;
extern volatile uint8_t pce_ticks, pce_raster_enabled, pce_floor_page;
extern volatile uint8_t pce_vdc_index;
void video_init(void);
void video_wait(void);
void video_mode_ui(void);
void video_display(bool enable);
void video_scroll(uint16_t x, uint16_t y);
extern volatile uint16_t pce_scroll_x;
extern volatile uint8_t pce_scroll_hold;
void video_scene(const PceScene *scene);
bool video_background(uint16_t camera);
void video_restore(void);
void video_text(uint8_t x, uint8_t y, const char *text);
void video_number(uint8_t x, uint8_t y, uint16_t value);
void video_sat_begin(void);
bool video_sprite(uint16_t id, int16_t x, int16_t y, bool flip, uint8_t scale);
bool video_sprite_optional(uint16_t id,int16_t x,int16_t y,bool flip,uint8_t scale);
void video_front_mark(void);
void video_front_begin(void);
void video_sat_end(void);
void video_vdc(uint8_t index, uint16_t value);
void video_race_init(void);
void video_floor_row(uint8_t page, uint8_t row, const uint8_t *pairs);
bool video_race_sky(void);
void pce_vblank(void);
void pce_hblank(void);
