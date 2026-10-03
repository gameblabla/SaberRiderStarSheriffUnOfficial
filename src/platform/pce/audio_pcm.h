#pragma once
#include "pce_config.h"
extern volatile uint8_t pce_pcm_bankid;
extern volatile uint16_t pce_pcm_left;
extern volatile uint8_t pce_pcm_read[];
void pce_pcm_irq(void);
void audio_pcm_init(void);
void audio_pcm_stop(void);
void audio_pcm_play(uint8_t sample);
