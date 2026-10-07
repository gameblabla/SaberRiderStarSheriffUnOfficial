#include <yaul.h>

#include "scsp_dsp.h"

void scsp_dsp_wait_sample(void) {
  SndCpuInterruptReset->sampleFs = 1;
  while (!SndCpuInterruptPending->sampleFs) {}
}

// Writes one step as 4 explicit 16-bit stores, MSB word first, matching the
// hardware's real register layout (100800h=bits[63:48] ... 100806h=[15:0]).
static void scsp_dsp_write_step(unsigned int step, uint64_t word) {
  DSP_MPRO16[step * 4 + 0] = (uint16_t) (word >> 48);
  DSP_MPRO16[step * 4 + 1] = (uint16_t) (word >> 32);
  DSP_MPRO16[step * 4 + 2] = (uint16_t) (word >> 16);
  DSP_MPRO16[step * 4 + 3] = (uint16_t) (word >>  0);
}

void scsp_dsp_load_program(const uint64_t program[DSP_STEP_COUNT]) {
  for (unsigned int i = 0; i < DSP_STEP_COUNT; i++) {
    scsp_dsp_write_step(i, program[i] & ~DSP_TWT);
  }

  scsp_dsp_wait_sample();
  scsp_dsp_wait_sample();

  /* TEMP is addressed through the DSP's rotating MDEC counter.  Clearing
     only nominal history indices is insufficient after the program has run
     for a while, because logical TRA/TWA offsets no longer correspond to
     fixed physical TEMP cells.  With every TWT masked for two complete
     samples, no DSP step can race these CPU zero stores, so clearing the
     entire 128-entry TEMP file gives a deterministic predictor reset. */
  for (unsigned int i = 0; i < DSP_TEMP_COUNT; i++) {
    DSP_TEMP[i] = 0;
  }

  for (unsigned int i = 0; i < DSP_STEP_COUNT; i++) {
    scsp_dsp_write_step(i, program[i]);
  }
}
