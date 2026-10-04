#pragma once
#include "pce_config.h"
/* Layout shared with the assembly IRQ; two independent decoder states. */
typedef struct __attribute__((packed)) {
    uint16_t left,read,predictor;
    uint8_t index,packed,phase,bank,loop,channel;
    uint16_t start,count;
} PcePcmVoice;
_Static_assert(sizeof(PcePcmVoice)==16,"IRQ voice stride");
extern volatile PcePcmVoice pce_pcm_voices[2];
extern volatile uint8_t pce_pcm_active;
void pce_pcm_irq(void);
void audio_pcm_init(void);
void audio_pcm_stop(void);
void audio_pcm_play(uint8_t sample);
void audio_pcm_gallop(bool on);
