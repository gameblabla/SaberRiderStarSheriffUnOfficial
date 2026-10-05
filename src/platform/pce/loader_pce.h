#pragma once
#include "pce_config.h"
bool loader_ui(void);
bool loader_victory(uint8_t index);
bool loader_scene(uint8_t stage);
bool loader_font(void);
bool loader_voice(uint8_t hero);
extern uint8_t pce_stall;   /* set by audio_music: a CD-DA seek holds the loop up for dozens of frames; main drops the ticks it missed (main_pce.c) */
void audio_music(uint8_t track);
void audio_music_once(uint8_t track);
void audio_stop(void);
void audio_effect(uint8_t tone);
void audio_tick(void);
