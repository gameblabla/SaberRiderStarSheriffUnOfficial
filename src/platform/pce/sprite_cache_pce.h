#pragma once
#include "pce_config.h"
extern uint16_t sprite_ids[48],sprite_words[48];
extern uint8_t sprite_used[48],sprite_pinned[48],pattern_owner[54];
extern uint8_t sprite_slot_of[480],sprite_count[48],sprite_len[48],sprite_p0[48],sprite_p1[48],sprite_p2[48];
extern uint8_t sprite_pb_hi[48],sprite_attr[48];
uint8_t sprite_slot(uint16_t id,uint8_t count);
extern uint16_t sprite_cache_foreground_first;
extern uint8_t sprite_cache_stage;

extern uint8_t sprite_stamp[48],sprite_epoch;
