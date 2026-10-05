#pragma once
#include "pce_config.h"
void presentation_frame(void);
void foreground_prepare(void);
void foreground_draw(void);
void foreground_reset(void);
void presentation_draw(void);
void video_panel(uint8_t x,uint8_t y,uint8_t w,uint8_t h);
void video_panel_restore_prepare(uint8_t y);
void video_panel_restore_apply(void);
void video_cells_apply(uint8_t y,uint16_t column);   /* a dialogue's panel cells from buffer+1024 (story_pce.c), right after a VBlank */
extern uint16_t pce_panel_column;
void story_graphics_restore(void);
