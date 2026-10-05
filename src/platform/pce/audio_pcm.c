#include "audio_pcm.h"
#include "overlay_pce.h"
#include "pcm.h"
#include "samples.h"
#define PCM_CODE __attribute__((noinline,section(".ram_bank117.text")))
/* Assembly reserves $2080-$20a0; the linker protects it from compiler ZP. */
static uint8_t request,voice_seed;
PceVoice pce_voice;
extern volatile uint8_t psg_live;
PCM_CODE static void stop_all(void) {
    pce_cpu_irq_disable();
    *IO_TIMER_CONTROL=0;*IO_IRQ_ACK=0;pce_pcm_active=0;
    for(uint8_t i=0;i<2;++i){pce_pcm_voices[i].left=0;pce_pcm_voices[i].loop=0;}
    for(uint8_t i=0;i<6;++i){*IO_PSG_CH_SELECT=i;*IO_PSG_CH_CONTROL=0;}
    psg_live=0;
    pce_cpu_irq_enable();
}
/* The road's per-scanline interrupt has about 300 cycles of slack, so nothing may keep interrupts off for longer than that: the sample's
 * numbers are read and the voice's record is written with them on (the voice is idle then, which is all the timer handler checks),
 * and only the PSG channel's set-up, which the handler's channel select would otherwise split, is done with them off. */
PCM_CODE static void start(void) {
    uint8_t sample=request;
    uint8_t channel=(sample==PCM_SAMPLE_GALLOP||sample==PCM_SAMPLE_STOP_LOOP||sample==PCM_SAMPLE_TURBO_LOOP)?1:0;
    volatile PcePcmVoice *v=&pce_pcm_voices[channel];
    if(sample==PCM_SAMPLE_STOP_LOOP) {
        pce_cpu_irq_disable();
        v=&pce_pcm_voices[1];v->left=0;v->loop=0;pce_pcm_active&=1;*IO_PSG_CH_SELECT=1;*IO_PSG_CH_CONTROL=0;
        pce_cpu_irq_enable();
        return;
    }
    if((sample==PCM_SAMPLE_GALLOP||sample==PCM_SAMPLE_TURBO_LOOP)&&v->left)return;
    const uint16_t *table=pcm_samples[sample];
    uint8_t bank=table[0];uint16_t begin=table[1],count=table[2];
    pce_cpu_irq_disable();pce_pcm_active&=~(1<<channel);pce_cpu_irq_enable();
    v->left=v->count=count;v->read=v->start=begin;
    v->bank=bank;v->last_sample=16;v->start_bank=bank;
    v->loop=sample==PCM_SAMPLE_GALLOP||sample==PCM_SAMPLE_TURBO_LOOP;v->channel=channel;
    pce_cpu_irq_disable();
    pce_pcm_active|=1<<channel;
    *IO_PSG_VOLUME=0xff;*IO_PSG_CH_SELECT=channel;
    *IO_PSG_CH_CONTROL=0;*IO_PSG_CH_VOLUME=0xff;
    *IO_PSG_CH_CONTROL=0xdf;*IO_PSG_CH_SAMPLE=16;
    if(!(*IO_TIMER_CONTROL&1)) {*IO_TIMER_COUNTER=0;*IO_IRQ_ACK=0;pce_irq_enable(IRQ_TIMER);*IO_TIMER_CONTROL=1;}
    pce_cpu_irq_enable();
}
void audio_pcm_stop(void) {overlay_call(0x75,stop_all);}
void psg_init(void);
void psg_start(void);
extern uint8_t psg_voice,psg_script;
void audio_pcm_init(void) {
    audio_pcm_stop();overlay_call(0x75,psg_init);
    pce_cdb_irq_set(PCE_CDB_ID_IRQ_TIMER,pce_pcm_irq);
    pce_cdb_irq_enable(PCE_CDB_MASK_IRQ_TIMER);
}
void audio_pcm_play(uint8_t sample) {
    if(sample>2)return;
    request=sample;overlay_call(0x75,start);
}
void audio_pcm_tick(void) {psg_voice=0;psg_script=6;overlay_call(0x75,psg_start);}   /* the CONTINUE? countdown's tick (sfx 0): pure PSG script 6, no DDA/ADPCM */
__attribute__((noinline)) void audio_pcm_gallop(bool on) {request=on?PCM_SAMPLE_GALLOP:PCM_SAMPLE_STOP_LOOP;overlay_call(0x75,start);}
void audio_pcm_power_intro(void) {request=PCM_SAMPLE_POWER_INTRO;overlay_call(0x75,start);}
void audio_pcm_turbo_start(void) {request=PCM_SAMPLE_TURBO_START;overlay_call(0x75,start);}
void audio_pcm_turbo_loop(bool on) {request=on?PCM_SAMPLE_TURBO_LOOP:PCM_SAMPLE_STOP_LOOP;overlay_call(0x75,start);}
/* ---- The hardware ADPCM controller, reset before every voice play ----
 *
 * $180a (the ADPCM RAM data port) is also the length counter: every write *increments* it, and it does not saturate at $ffff - it wraps with a
 * 17-bit mask, and the Super CD verificator (tst_adpcm.c, "writes to 180A increment len counter ... counter wraps, mask is 1FFFF / seems if counter
 * will be leave at 0x1xxxx range it may broke proper flags work") found that a counter left in $10000-$1ffff breaks the END and "32k" flags. That
 * is exactly the state every stage leaves behind: loader_voice() fills the 37-43 KB hero bank through the BIOS AD_TRANS, so one transfer counts tens
 * of thousands of bytes into that counter.
 *
 * Once the flags are broken, ADPCM_AD_END ($180c bit 0 - the BIOS ad_stat's "not playing") can read set with nothing playing, and the IFU raises IRQ2
 * the instant ad_play re-enables IFU_INT_END (tst_adpcm.c, tst_ad_irq: "should fire immediately if enable it while ad_play_end state"). The System
 * Card handler then clears ADPCM_PLAY, so each voice is cut a fraction of a millisecond after it starts and the channel stays silent until something
 * resets the controller - the mute that outlives a single effect.
 *
 * So take clean ownership before each play: the controller's reset pulse (which clears both length flags, the length counter and the read/write
 * pointers), no CD DMA left over from a transfer, the RAM port back in byte mode, and the BIOS fader cancelled ($180f bit 3 cancels a fade; bit 2
 * would otherwise leave ADPCM itself faded to silence). $1802 is deliberately NOT touched: HuC's ad_reset clears it, but that would also drop the
 * CD-DA driver's own IFU enables (cdda_pce.c keeps its own snapshot). */
#define IFU_ADPCM_DMA (*(volatile uint8_t*)0x180b)
#define IFU_ADPCM_CTL (*(volatile uint8_t*)0x180d)
#define IFU_ADPCM_SPD (*(volatile uint8_t*)0x180e)
#define IFU_AUDIO_FADE (*(volatile uint8_t*)0x180f)
PCM_CODE void audio_adpcm_reset(void) {
    IFU_ADPCM_CTL=0x80;                 /* ADPCM_RESET, held one opcode as the verificator does */
    __asm__ __volatile__("nop");
    IFU_ADPCM_CTL=0x00;
    IFU_ADPCM_DMA=0x00;                 /* stop ADPCM DMA from CD */
    IFU_AUDIO_FADE=0x00;                /* byte mode for the RAM port, and cancel any BIOS fade */
    IFU_ADPCM_SPD=0x00;                 /* ad_play sets the sample's own rate next */
}
/* tone (audio_effect) -> voice event: jump hurt death fall enemy_hit enemy_death alarm dialogue line, then the flying
 * bosses' engine pass, gun, rider's gun, both guns, blast and big bang (tones 12-17), the cruiser's cannon gathering and firing (18, 19) */
PCM_CODE static void voice_pick(void) {
    static const uint8_t tone_event[20]={0,0,0,0,0,1,2,4,5,6,7,3,8,9,10,11,12,13,14,15};
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
