#pragma once
#include "pce_config.h"
extern uint16_t sprite_ids[48],sprite_words[48];
extern uint8_t sprite_used[48],sprite_pinned[48],pattern_owner[48];
uint8_t sprite_slot(uint16_t id,uint8_t count);
