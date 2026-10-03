#pragma once
#include "pce_config.h"
bool floor_init(void);
void floor_update(uint16_t x,uint16_t y,uint8_t heading,uint8_t phase);
void floor_present(void);
