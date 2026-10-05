#include "audio_pcm.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "psg.h"
/* Native PSG effects (no DDA, no ADPCM): two voices, each a noise channel with a tone channel under it, stepped once every 1/60 s from audio_tick
 * and video_wait. Voice 0 (noise channel 5, tone channel 2) is the hero's shot; voice 1 (noise 4, tone 3) is the flying bosses' guns, so the
 * two never cut each other. The scripts (psg.h) are generated from the source recordings (tools/pce/build_audio.py). The DDA interrupt selects
 * channels 0 and 1 and never puts the selection back, so every access here is a short stretch with interrupts off (the road's scanline
 * interrupt has little slack: a step is a dozen register writes). $76: code here must not call a loader (pce_config.h PCE_X3). */
volatile uint8_t psg_live;                 /* bit v: voice v is playing (audio_pcm.c stop_all clears it) */
static const uint8_t *psg_ptr[2];
static uint8_t psg_tick;
static const uint8_t sine[32]={16,21,26,30,31,31,31,31,31,31,31,31,31,30,26,21,16,10,5,1,0,0,0,0,0,0,0,0,0,1,5,10};   /* a sine driven 1.7 times too hard and clipped: a fuller, louder body than the plain wave */
PCE_X3 void psg_init(void) {
    pce_cpu_irq_disable();
    *IO_PSG_VOLUME=0xff;
    for(uint8_t ch=2;ch<4;++ch) {
        *IO_PSG_CH_SELECT=ch;*IO_PSG_CH_CONTROL=0x40;*IO_PSG_CH_CONTROL=0;   /* the write index is reset by the DDA bit going up and down; samples are only taken with the channel off and DDA clear */
        for(uint8_t i=0;i<32;++i)*IO_PSG_CH_SAMPLE=sine[i];
        *IO_PSG_CH_CONTROL=0;
    }
    pce_cpu_irq_enable();
    psg_live=0;
}
uint8_t psg_voice,psg_script;   /* the call's arguments (loader_pce.c effect_body) */
PCE_X3 void psg_start(void) {
    uint8_t v=psg_voice;
    psg_ptr[v]=psg_scripts[psg_script];
    psg_live|=1<<v;
    psg_tick=pce_ticks-1;
}
PCE_X3 void psg_step(void) {
    if(!psg_live||pce_ticks==psg_tick)return;
    psg_tick=pce_ticks;
    for(uint8_t v=0;v<2;++v) {
        if(!(psg_live&(1<<v)))continue;
        const uint8_t *f=psg_ptr[v];
        uint8_t noise=v?4:5,tone=v?3:2;
        pce_cpu_irq_disable();
        if(f[0]==255) {
            *IO_PSG_CH_SELECT=noise;*IO_PSG_CH_CONTROL=0;
            *IO_PSG_CH_SELECT=tone;*IO_PSG_CH_CONTROL=0;
            psg_live&=~(1<<v);
        } else {
            *IO_PSG_CH_SELECT=noise;*IO_PSG_CH_CONTROL=0x80|f[0];*IO_PSG_CH_VOLUME=0xff;*IO_PSG_CH_NOISE=0x80|f[1];
            *IO_PSG_CH_SELECT=tone;*IO_PSG_CH_FREQ_FINE=f[3];*IO_PSG_CH_FREQ_COARSE=f[4];*IO_PSG_CH_CONTROL=f[2]?0x80|f[2]:0;*IO_PSG_CH_VOLUME=0xff;
            psg_ptr[v]=f+5;
        }
        pce_cpu_irq_enable();
    }
}
