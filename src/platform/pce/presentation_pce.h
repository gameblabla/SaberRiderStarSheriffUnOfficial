#pragma once
#include "pce_config.h"
extern volatile uint8_t pce_ui_state;
void frontend_start(void);
void presentation_frame(void);
void foreground_draw(void);
void presentation_draw(void);
void video_panel(uint8_t x,uint8_t y,uint8_t w,uint8_t h);
