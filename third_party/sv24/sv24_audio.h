#ifndef SV24_AUDIO_H
#define SV24_AUDIO_H
#include <stdint.h>
#define SV24_AUDIO_RING_SAMPLES 32768u
#define SV24_AUDIO_PCM_BASE 0x60000u
int sv24_audio_init(const void *driver, uint32_t driver_len, uint16_t sample_rate, int16_t adx_coef1, int16_t adx_coef2);
void sv24_audio_stream_reset(void);
void sv24_audio_reprime(void);
int sv24_audio_feed_adx(const uint8_t *data, uint32_t len);
void sv24_audio_start(uint8_t volume);
void sv24_audio_tick(void);
void sv24_audio_stop(void);
extern volatile uint32_t sv24_audio_groups;
extern volatile uint32_t sv24_audio_input_bytes;
extern volatile uint32_t sv24_audio_write_pos;
extern volatile uint32_t sv24_audio_started;
extern volatile uint32_t sv24_audio_loop_headers;
extern volatile uint32_t sv24_audio_ca_hi;
extern volatile uint32_t sv24_audio_lead_coarse;
extern volatile uint32_t sv24_audio_min_lead_coarse;
#endif
