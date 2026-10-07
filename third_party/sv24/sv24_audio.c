#include <yaul.h>
#include <stdint.h>
#include <string.h>
#include "sv24_audio.h"
#include "pcmsys.h"
#include "adx_dsp_sega.h"

#define ADX_BLOCK_BYTES 18u
#define ADX_GROUP_BYTES 36u
#define ADX_BLOCK_SAMPLES 32u
#define ADX_PCM_BLOCK_BYTES 64u

static uint32_t audio_buffer_size;
static uint32_t write_pos;
static uint8_t header_pending;

volatile uint32_t sv24_audio_groups = 0;
volatile uint32_t sv24_audio_input_bytes = 0;
volatile uint32_t sv24_audio_write_pos = 0;
volatile uint32_t sv24_audio_started = 0;
volatile uint32_t sv24_audio_slot0_reg16 = 0;
volatile uint32_t sv24_audio_slot1_reg16 = 0;
volatile uint32_t sv24_audio_slot2_reg16 = 0;
volatile uint32_t sv24_audio_slot3_reg16 = 0;
volatile uint32_t sv24_audio_ctrl0 = 0;
volatile uint32_t sv24_audio_ctrl1 = 0;
volatile uint32_t sv24_audio_loop_headers = 0;
volatile uint32_t sv24_audio_ca_hi = 0;
volatile uint32_t sv24_audio_lead_coarse = 0;
volatile uint32_t sv24_audio_min_lead_coarse = 0xFFFFFFFFu;

static int16_t clip16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Residual-only half of ADX.  The recursive filter/history is deliberately
   left to the SCSP DSP, matching the Sega-style path used by the reference
   Cinepak player. */
static void decode_residual_block(const uint8_t *src, int16_t *dst)
{
    int32_t scale = ((int32_t)src[0] << 8) | src[1];
    int32_t sp = (scale << 3) | 7;
    unsigned i;
    for (i = 0; i < 16u; i++) {
        int32_t hi = (int32_t)(src[2u+i] >> 4);
        int32_t lo = (int32_t)(src[2u+i] & 15u);
        if (hi & 8) hi -= 16;
        if (lo & 8) lo -= 16;
        *dst++ = clip16((hi * sp) >> 3);
        *dst++ = clip16((lo * sp) >> 3);
    }
}

int sv24_audio_init(const void *driver, uint32_t driver_len, uint16_t sample_rate, int16_t adx_coef1, int16_t adx_coef2)
{
#ifndef PLAT_SATURN
    if (!driver || !driver_len) return -1;
    pcmsys_load_driver((void *)driver, driver_len);
    pcmStreamInitialize();
#endif
    audio_buffer_size = adx_dsp_setup(sample_rate, adx_coef1, adx_coef2);
    if (!audio_buffer_size || (audio_buffer_size % ADX_PCM_BLOCK_BYTES)) return -2;
    sv24_audio_stream_reset();
    return 0;
}

void sv24_audio_stream_reset(void)
{
    write_pos = 0;
    header_pending = 1;
    sv24_audio_write_pos = 0;
    memset(getSlotAddress(ADX_DSP_FEEDER_SLOT_L), 0, getSlotSize());
    memset(getSlotAddress(ADX_DSP_FEEDER_SLOT_R), 0, getSlotSize());
    adx_dsp_reset_history();
}

void sv24_audio_reprime(void)
{
    sv24_audio_stream_reset();
    /* Mid-clip packets carry ADX blocks without another file header. */
    header_pending=0;
}

int sv24_audio_feed_adx(const uint8_t *data, uint32_t len)
{
    uint32_t p = 0;
    if (!data) return -3;
    sv24_audio_input_bytes += len;
    /* Packet 0 contains the standard ADX header.  In a looping SVM stream
       packet 0 returns every lap; skip it every time, but only require it on
       the very first feed.  Do NOT reset DSP history here: this packet is
       queued several video frames before the playback cursor reaches the loop
       point, so an early history reset would corrupt the end of the old lap. */
    if (len >= 20u && data[0] == 0x80u && data[1] == 0x00u) {
        uint32_t hs = (((uint32_t)data[2] << 8) | data[3]) + 4u;
        if (hs > len || data[4] != 3u || data[5] != 18u || data[6] != 4u || data[7] != 2u) return -5;
        p = hs;
        if (header_pending) header_pending = 0;
        else sv24_audio_loop_headers++;
    } else if (header_pending) {
        return -4;
    }
    if ((len - p) % ADX_GROUP_BYTES) return -6;
    while (p < len) {
        int16_t *l = (int16_t *)(getSlotAddress(ADX_DSP_FEEDER_SLOT_L) + write_pos);
        int16_t *r = (int16_t *)(getSlotAddress(ADX_DSP_FEEDER_SLOT_R) + write_pos);
        decode_residual_block(data + p, l);
        decode_residual_block(data + p + ADX_BLOCK_BYTES, r);
        p += ADX_GROUP_BYTES;
        write_pos += ADX_PCM_BLOCK_BYTES;
        if (write_pos >= audio_buffer_size) write_pos = 0;
        sv24_audio_groups++;
    }
    sv24_audio_write_pos = write_pos;
    return 0;
}

void sv24_audio_start(uint8_t volume)
{
    adx_dsp_play(volume);
    sv24_audio_started = 1;
    adx_dsp_remute_dry_output();
}

void sv24_audio_tick(void)
{
    {
        volatile uint16_t *mon = (volatile uint16_t *)(SNDRAM + 0x100408u);
        uint16_t md;
        uint32_t ca, wp, lead;
        *mon = (uint16_t)(ADX_DSP_FEEDER_SLOT_L << 11);
        md = *mon;
        sv24_audio_ca_hi = (uint32_t)((md >> 7) & 0x0Fu);
        ca = sv24_audio_ca_hi << 12;
        wp = write_pos >> 1;
        lead = (wp + (audio_buffer_size >> 1) - ca) % (audio_buffer_size >> 1);
        sv24_audio_lead_coarse = lead;
        /* Values above half the ring after a prior low value represent a
           producer underrun modulo-wrap, so only track the useful low half
           as a warning-watermark. */
        if (lead < (audio_buffer_size >> 2) && lead < sv24_audio_min_lead_coarse)
            sv24_audio_min_lead_coarse = lead;
    }
    if (sv24_audio_started) adx_dsp_remute_dry_output();
    sv24_audio_slot0_reg16 = *(volatile uint16_t *)(SNDRAM + 0x100000u + 0u*0x20u + 0x16u);
    sv24_audio_slot1_reg16 = *(volatile uint16_t *)(SNDRAM + 0x100000u + 1u*0x20u + 0x16u);
    sv24_audio_slot2_reg16 = *(volatile uint16_t *)(SNDRAM + 0x100000u + 2u*0x20u + 0x16u);
    sv24_audio_slot3_reg16 = *(volatile uint16_t *)(SNDRAM + 0x100000u + 3u*0x20u + 0x16u);
    sv24_audio_ctrl0 = ((uint32_t)m68k_com->pcmCtrl[0].sh2_permit << 24) |
                       ((uint32_t)m68k_com->pcmCtrl[0].pan << 16) |
                       ((uint32_t)m68k_com->pcmCtrl[0].volume << 8) |
                       (uint8_t)m68k_com->pcmCtrl[0].icsr_target;
    sv24_audio_ctrl1 = ((uint32_t)m68k_com->pcmCtrl[1].sh2_permit << 24) |
                       ((uint32_t)m68k_com->pcmCtrl[1].pan << 16) |
                       ((uint32_t)m68k_com->pcmCtrl[1].volume << 8) |
                       (uint8_t)m68k_com->pcmCtrl[1].icsr_target;
}

void sv24_audio_stop(void)
{
    if (sv24_audio_started) adx_dsp_stop();
    sv24_audio_started = 0;
}
