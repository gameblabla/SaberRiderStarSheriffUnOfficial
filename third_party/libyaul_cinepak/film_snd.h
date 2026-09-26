#ifndef FILM_SND_H
#define FILM_SND_H

#include "film_lib.h"

extern uint8_t *baseSoundMemory;

extern int32_t film_audio_get_next_buffer_size();

extern uint16_t *film_audio_get_next_buffer_ptr(uint8_t slot);

extern void film_audio_notify_read_buffer_bytes(int32_t length);

extern void film_audio_play(uint8_t volume);

extern void film_audio_setup(decode_work_t *work,
  int16_t frequency, int32_t numChannels, int32_t sampleResolution);

extern void film_audio_prepare_to_play();

/* Saber Rider: the SCSP side once the clip starts (the slots, timer C) */
extern void film_audio_hw_begin(void);
/* the platform's slots (src/platform/saturn/aud_sat.c): channel ch loops over bytes of PCM at sound RAM offset off */
extern void film_pcm_configure(int ch, uint32_t off, uint32_t bytes, uint16_t rate, uint8_t bits, uint8_t pan);
extern void film_pcm_start(int ch, uint8_t volume);
extern void film_pcm_stop(int ch);

extern void film_audio_reset();

extern void film_audio_fill_silence(decode_work_t *work);

extern void pollAudioConfirmTimer(void);

extern void removeReturnNoise(uint8_t *writeLocation,
  uint8_t *channelBase, int32_t numBits);

extern bool parseAudioMono(decode_work_t *work);

extern bool parseAudioStereo(decode_work_t *work);

#endif // FILM_SND_H
