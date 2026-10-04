#include "audio_pcm.h"
#include "overlay_pce.h"
#include "pcm.h"
#include "samples.h"
#define PCM_CODE __attribute__((noinline,section(".ram_bank117.text")))
/* Assembly reserves $2080-$20a0; the linker protects it from compiler ZP. */
static uint8_t request,voice_seed;
PceVoice pce_voice;
PCM_CODE static void stop_all(void) {
    pce_cpu_irq_disable();
    *IO_TIMER_CONTROL=0;*IO_IRQ_ACK=0;pce_pcm_active=0;
    for(uint8_t i=0;i<2;++i){pce_pcm_voices[i].left=0;pce_pcm_voices[i].loop=0;}
    for(uint8_t i=0;i<6;++i){*IO_PSG_CH_SELECT=i;*IO_PSG_CH_CONTROL=0;}
    pce_cpu_irq_enable();
}
PCM_CODE static void start(void) {
    uint8_t sample=request;
    uint8_t channel=sample==3?1:0;
    volatile PcePcmVoice *v=&pce_pcm_voices[channel];
    pce_cpu_irq_disable();
    if(sample==4) {v=&pce_pcm_voices[1];v->left=0;v->loop=0;pce_pcm_active&=1;*IO_PSG_CH_SELECT=1;*IO_PSG_CH_CONTROL=0;}
    else if(sample!=3||!v->left) {
        v->left=v->count=pcm_samples[sample][2];v->read=v->start=pcm_samples[sample][1];
        v->bank=pcm_samples[sample][0];v->last_sample=16;v->start_bank=v->bank;
        v->loop=sample==3;v->channel=channel;pce_pcm_active|=1<<channel;
        *IO_PSG_VOLUME=0xff;*IO_PSG_CH_SELECT=channel;
        *IO_PSG_CH_CONTROL=0;*IO_PSG_CH_VOLUME=0xff;
        *IO_PSG_CH_CONTROL=0xdf;*IO_PSG_CH_SAMPLE=16;
        if(!(*IO_TIMER_CONTROL&1)) {*IO_TIMER_COUNTER=0;*IO_IRQ_ACK=0;pce_irq_enable(IRQ_TIMER);*IO_TIMER_CONTROL=1;}
    }
    pce_cpu_irq_enable();
}
void audio_pcm_stop(void) {overlay_call(0x75,stop_all);}
void audio_pcm_init(void) {
    audio_pcm_stop();
    pce_cdb_irq_set(PCE_CDB_ID_IRQ_TIMER,pce_pcm_irq);
    pce_cdb_irq_enable(PCE_CDB_MASK_IRQ_TIMER);
}
void audio_pcm_play(uint8_t sample) {
    if(sample>2)return;
    request=sample;overlay_call(0x75,start);
}
__attribute__((noinline)) void audio_pcm_gallop(bool on) {request=on?3:4;overlay_call(0x75,start);}
/* tone (audio_effect) -> voice event: jump hurt death fall enemy_hit enemy_death alarm dialogue line, then the flying
 * bosses' engine pass, gun, rider's gun, both guns, blast and big bang (tones 12-17) */
PCM_CODE static void voice_pick(void) {
    static const uint8_t tone_event[18]={0,0,0,0,0,1,2,4,5,6,7,3,8,9,10,11,12,13};
    const uint8_t *group=voice_groups[tone_event[request]];
    uint8_t k=group[0];
    voice_seed=voice_seed*37+11;
    if(group[1]>1)k+=(voice_seed>>3)%group[1];
    pce_voice.address=voice_samples[pce_voice.hero][k][0];
    pce_voice.bytes=voice_samples[pce_voice.hero][k][1];
    pce_voice.priority=group[2];pce_voice.rate=group[3];
}
PCM_CODE static void voice_size(void) {pce_voice.bytes=voice_total[pce_voice.hero];}
void audio_pcm_voice(uint8_t tone) {request=tone;overlay_call(0x75,voice_pick);}
uint16_t audio_pcm_voice_bytes(uint8_t hero) {pce_voice.hero=hero;overlay_call(0x75,voice_size);return pce_voice.bytes;}
