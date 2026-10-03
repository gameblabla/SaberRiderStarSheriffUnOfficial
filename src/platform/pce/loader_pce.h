#pragma once
#include "pce_config.h"
bool loader_scene(uint8_t stage);
bool loader_font(void);
bool loader_voice(uint8_t hero);
void audio_music(uint8_t track);
void audio_stop(void);
void audio_effect(uint8_t tone);
void audio_tick(void);
