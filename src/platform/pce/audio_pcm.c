#include "audio_pcm.h"
volatile uint8_t pce_pcm_bankid;
volatile uint16_t pce_pcm_left;
void audio_pcm_stop(void) {
    pce_cpu_irq_disable();
    *IO_TIMER_CONTROL=0;*IO_IRQ_ACK=0;
    pce_pcm_left=0;
    for(uint8_t i=0;i<6;++i){*IO_PSG_CH_SELECT=i;*IO_PSG_CH_CONTROL=0;}
    pce_cpu_irq_enable();
}
void audio_pcm_init(void) {
    audio_pcm_stop();
    pce_cdb_irq_set(PCE_CDB_ID_IRQ_TIMER,pce_pcm_irq);
    pce_cdb_irq_enable(PCE_CDB_MASK_IRQ_TIMER);
}
