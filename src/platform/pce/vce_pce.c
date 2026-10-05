#define VCE_RAW
#include "pce_config.h"
#include "video_pce.h"
#include <string.h>
/* The VCE (palette RAM) writes of pce_config.h. $75: the audio bank, never overwritten by a load. The queue is a ring of four palettes that
 * irq.S writes at the next VBlank (vce_q_head is the consumer's, vce_q_tail ours; each is one byte stored last, so neither side needs the
 * interrupts masked). */
uint8_t vce_arg_index,vce_arg_count;
const void *vce_arg_ptr;
uint16_t vce_arg_value;
extern volatile uint8_t vce_q_head,vce_q_tail,vce_q_idx[4],vce_q_data[4][32];
/* With the display off: write what is queued now (the VBlank interrupt may be disabled during a disc load, so it is not waited for). */
PCE_X3 static void drain(void) {
    pce_cpu_irq_disable();
    while(vce_q_head!=vce_q_tail) {
        pce_vce_copy_palette(vce_q_idx[vce_q_head],(const void*)vce_q_data[vce_q_head],1);
        vce_q_head=(vce_q_head+1)&3;
    }
    pce_cpu_irq_enable();
}
PCE_X3 void vce_copy_body(void) {
    uint8_t index=vce_arg_index,count=vce_arg_count;const uint8_t *source=vce_arg_ptr;
    if(pce_display_on&&count==1) {
        uint8_t t=vce_q_tail,next=(t+1)&3;
        if(next==vce_q_head){video_wait();t=vce_q_tail;next=(t+1)&3;}
        uint8_t *d=(uint8_t*)vce_q_data[t];
        memcpy(d,source,16);memcpy(d+16,source+16,16);   /* 16 bytes a block move: the road's raster interrupt is never held back */
        vce_q_idx[t]=index;vce_q_tail=next;
        return;
    }
    if(pce_display_on)video_wait();else drain();
    pce_vce_copy_palette(index,source,count);
}
PCE_X3 void vce_set_body(void) {
    if(pce_display_on)video_wait();else drain();
    pce_vce_set_color(((uint16_t)vce_arg_index<<4)|vce_arg_count,vce_arg_value);
}
PCE_X3 void vce_read_body(void) {
    if(pce_display_on)video_wait();else drain();
    pce_vce_copy_palette_to_ram((void*)vce_arg_ptr,vce_arg_index,vce_arg_count);
}
