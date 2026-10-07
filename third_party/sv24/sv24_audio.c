#include <yaul.h>
#include <stdint.h>
#include <string.h>
#include "sv24_audio.h"
#include "pcmsys.h"


#define ADX_BLOCK_BYTES 18u
#define ADX_GROUP_BYTES 36u
#define ADX_BLOCK_SAMPLES 32u
#define ADX_PCM_BLOCK_BYTES 64u

static uint32_t audio_buffer_size;
static uint32_t write_pos;
static uint8_t header_pending;
static int32_t history1[2],history2[2],coef1,coef2;
#define PCM_BASE SV24_AUDIO_PCM_BASE
#define PCM_BYTES (SV24_AUDIO_RING_SAMPLES*2u)
#define REG(slot, off) (*(volatile uint16_t *)(SNDRAM+0x100000u+(slot)*0x20u+(off)))
static uint8_t *pcm_address(unsigned channel) { return (uint8_t *)(SNDRAM+PCM_BASE+channel*PCM_BYTES); }
static void pcm_key(bool on)
{
    /* EG bypass makes key-off alone an unreliable output gate. */
    if(!on){REG(0,22)=0;REG(1,22)=0;}
    uint16_t a=REG(0,0)&0x07ffu,b=REG(1,0)&0x07ffu;
    if(on){a|=0x0800u;b|=0x0800u;}
    REG(0,0)=a;REG(1,0)=b;REG(0,0)=a|0x1000u;
    if(on) {
        REG(0,22)=(7u<<13)|(0x1fu<<8);
        REG(1,22)=(7u<<13)|(0x0fu<<8);
    }
}

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

/* Reconstruct once per ADX source sample. SCSP then resamples ordinary
 * PCM; its 44.1 kHz DSP clock must never advance the ADX predictor. */
static void decode_block(const uint8_t *src, volatile int16_t *dst,unsigned channel)
{
    int32_t scale=((int32_t)src[0]<<8)|src[1];
    int32_t h1=history1[channel],h2=history2[channel];
    for(unsigned i=0;i<32u;i++) {
        int32_t nib=(src[2u+i/2u]>>(i&1u?0:4))&15u;
        if(nib&8)nib-=16;
        int32_t sample=clip16(nib*scale+((coef1*h1+coef2*h2)>>13));
        *dst++=(int16_t)sample;h2=h1;h1=sample;
    }
    history1[channel]=h1;history2[channel]=h2;
}

int sv24_audio_init(const void *driver, uint32_t driver_len, uint16_t sample_rate, int16_t adx_coef1, int16_t adx_coef2)
{
#ifndef PLAT_SATURN
    if (!driver || !driver_len) return -1;
    pcmsys_load_driver((void *)driver, driver_len);
    pcmStreamInitialize();
#endif
    /* The SVM reader supplies the encoder's source-sample Q13 coefficients. */
    coef1=adx_coef1;coef2=adx_coef2;
    audio_buffer_size=PCM_BYTES;
    for(unsigned ch=0;ch<2;ch++) {
        uint32_t base=PCM_BASE+ch*PCM_BYTES;
        REG(ch,0)=0x0020u|(base>>16);REG(ch,2)=(uint16_t)base;
        REG(ch,4)=0;REG(ch,6)=SV24_AUDIO_RING_SAMPLES-1u;
        REG(ch,8)=0x001fu;REG(ch,10)=0x001fu;REG(ch,12)=0x0100u;
        REG(ch,14)=0;REG(ch,16)=(uint16_t)convert_bitrate_to_pitchword(sample_rate);
        REG(ch,18)=0;REG(ch,20)=0;
        REG(ch,22)=0; /* remain silent during prefill */
    }
    sv24_audio_stream_reset();
    return 0;
}

void sv24_audio_stream_reset(void)
{
    write_pos = 0;
    header_pending = 1;
    sv24_audio_write_pos = 0;
    memset(pcm_address(0),0,PCM_BYTES);
    memset(pcm_address(1),0,PCM_BYTES);
    memset(history1,0,sizeof history1);memset(history2,0,sizeof history2);
}

void sv24_audio_reprime(void)
{
    /* Only clear PCM storage. Predictor history belongs to the continuous
     * compressed stream and must survive a delayed CD read. */
    write_pos=0;sv24_audio_write_pos=0;
    memset(pcm_address(0),0,PCM_BYTES);memset(pcm_address(1),0,PCM_BYTES);
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
       the very first feed.  Do NOT reset predictor history here: this packet is
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
        int16_t *l = (int16_t *)(pcm_address(0) + write_pos);
        int16_t *r = (int16_t *)(pcm_address(1) + write_pos);
        decode_block(data+p,l,0);
        decode_block(data+p+ADX_BLOCK_BYTES,r,1);
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
    (void)volume;pcm_key(true);
    sv24_audio_started = 1;
}

void sv24_audio_tick(void)
{
    {
        volatile uint16_t *mon = (volatile uint16_t *)(SNDRAM + 0x100408u);
        uint16_t md;
        uint32_t ca, wp, lead;
        *mon = 0u;
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
    if (sv24_audio_started) pcm_key(false);
    sv24_audio_started = 0;
}
