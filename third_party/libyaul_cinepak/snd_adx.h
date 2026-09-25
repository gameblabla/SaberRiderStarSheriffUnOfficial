#ifndef SND_ADX_H
#define SND_ADX_H

#include "film_lib.h"

extern void film_audio_adx_reset_history(void);

extern int32_t snd_adx_begin_chunk(uint8_t *dataStart, int32_t dataLength,
  int32_t channels, int32_t *outRemainingPcmBytes);

extern bool parseMonoADXAudio(decode_work_t *work);

extern bool parseStereoADXAudio(decode_work_t *work);

#endif // SND_ADX_H
