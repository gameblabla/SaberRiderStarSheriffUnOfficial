#ifndef SV24_SCU_H
#define SV24_SCU_H
#include <stdint.h>
extern volatile uint32_t sv24_scu_frames;
extern volatile uint32_t sv24_scu_jobs;
extern volatile uint32_t sv24_scu_errors;
extern volatile uint32_t sv24_scu_last_jobs;
int sv24_scu_init(void);
/* Begin/end split lets SCU-DSP exact-tile reconstruction overlap the dual-SH2
   slice decoder.  DSP-owned SV24 records use MODE_DSP_SKIP and therefore do
   not touch staging on either SH-2. */
int sv24_scu_begin(const uint8_t *sidecar,uint32_t size,uint32_t *staging);
int sv24_scu_end(uint32_t *staging);
int sv24_scu_apply(const uint8_t *sidecar,uint32_t size,uint32_t *staging,uint32_t budget_jobs);
#endif
