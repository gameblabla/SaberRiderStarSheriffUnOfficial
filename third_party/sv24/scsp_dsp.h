#ifndef __SCSP_DSP_H__
#define __SCSP_DSP_H__

//THIS WAS LIFTED FROM CELERIAC's SCSP ADPCM DRIVER AND MODIFIED BY CLAUDE.

// uint8_t/uint16_t/uint32_t/uint64_t come from yaul.h (via bios.h) - callers
// must include yaul.h before this header, same convention pcmsys.h already
// relies on.
#include "pcmsys.h"

// SCSP onboard DSP register/buffer map (SH-2 absolute addresses, relative to
// SNDRAM). Confirmed against the SCSP User's Manual register map (chapter 4)
// and cross-checked against celeriyacon/scspadpcm, a working Saturn homebrew
// driver that programs this same DSP - https://github.com/celeriyacon/scspadpcm

#define DSP_COEF_COUNT  (64)
#define DSP_MADRS_COUNT (32)
#define DSP_STEP_COUNT  (128)
#define DSP_TEMP_COUNT  (128)
#define DSP_MEMS_COUNT  (32)
#define DSP_EFREG_COUNT (16)

static volatile uint16_t *const DSP_COEF  = (volatile uint16_t *) (SNDRAM + 0x100700);
static volatile uint16_t *const DSP_MADRS = (volatile uint16_t *) (SNDRAM + 0x100780);
// DSP_MPRO lives at DSPRAM (pcmsys.h) - pcmsys_load_driver()'s existing DSP RAM
// clear already zeroes exactly this region (0x400 bytes = 128 steps * 8 bytes).
// Declared as uint16_t (4 words/step), NOT uint64_t: the SH2 can't do atomic
// 64-bit stores, and the hardware itself is documented as 4 separate 16-bit
// registers per step (100800h=bits[63:48], 100802h=[47:32], ...) - relying
// on however the compiler happens to decompose a 64-bit store into smaller
// ones (byte/word order unverified) risks scrambling which bits land where.
// scsp_dsp_load_program() writes each step as 4 explicit 16-bit stores.
static volatile uint16_t *const DSP_MPRO16 = (volatile uint16_t *) (DSPRAM);
static volatile uint32_t *const DSP_TEMP  = (volatile uint32_t *) (SNDRAM + 0x100C00);
static volatile uint32_t *const DSP_MEMS  = (volatile uint32_t *) (SNDRAM + 0x100E00);
static volatile uint16_t *const DSP_EFREG = (volatile uint16_t *) (SNDRAM + 0x100EC0);

// Instruction word field macros - bit offsets ported verbatim from
// scspadpcm's dsp-macros.h (independently confirmed against the manual's
// Figure 5.1 DSP configuration diagram).
#define DSP_NXADDR  ((uint64_t) (      1) <<  0)
#define DSP_ADRGB   ((uint64_t) (      1) <<  1)
#define DSP_MASA(v) ((uint64_t) ((v)&0x1F) <<  2)
#define DSP_NOFL    ((uint64_t) (      1) <<  8)
#define DSP_CRA(v)  ((uint64_t) ((v)&0x3F) <<  9)
#define DSP_BSEL(v) ((uint64_t) ((v)&0x01) << 16)
#define DSP_ZERO    ((uint64_t) (      1) << 17)
#define DSP_NEGB    ((uint64_t) (      1) << 18)
#define DSP_YRL     ((uint64_t) (      1) << 19)
#define DSP_SHFT0   ((uint64_t) (      1) << 20)
#define DSP_SHFT1   ((uint64_t) (      1) << 21)
#define DSP_FRCL    ((uint64_t) (      1) << 22)
#define DSP_ADRL    ((uint64_t) (      1) << 23)
#define DSP_EWA(v)  ((uint64_t) ((v)&0x0F) << 24)
#define DSP_EWT     ((uint64_t) (      1) << 28)
#define DSP_MRT     ((uint64_t) (      1) << 29)
#define DSP_MWT     ((uint64_t) (      1) << 30)
#define DSP_TABLE   ((uint64_t) (      1) << 31)
#define DSP_IWA(v)  ((uint64_t) ((v)&0x1F) << 32)
#define DSP_IWT     ((uint64_t) (      1) << 37)
#define DSP_IRA(v)  ((uint64_t) ((v)&0x3F) << 38)
#define DSP_YSEL(v) ((uint64_t) ((v)&0x03) << 45)
#define DSP_XSEL(v) ((uint64_t) ((v)&0x01) << 47)
#define DSP_TWA(v)  ((uint64_t) ((v)&0x7F) << 48)
#define DSP_TWT     ((uint64_t) (      1) << 55)
#define DSP_TRA(v)  ((uint64_t) ((v)&0x7F) << 56)

// COEF is a 13-bit signed field left-justified in its 16-bit register.
#define DSP_MAKE_COEF(v) ((uint16_t) ((v) << 3))

// MEMS is a 24-bit value split across two 16-bit register halves in a
// byte-swizzled layout - this composes a plain value into that layout for a
// single 32-bit CPU-side write. (TEMP uses the same split layout, but is
// only ever touched by the DSP program itself, never the CPU, so no
// equivalent MAKE_TEMP helper is provided.)
#define DSP_MAKE_MEMS(v) \
  (((uint32_t) ((v) & 0xFF) << 16) | (((v) >> 8) & 0xFFFF))

// Blocks until the next SCSP sample-rate interrupt fires. Used to serialize
// DSP reprogramming against the DSP's own execution cycle.
void scsp_dsp_wait_sample(void);

// Uploads a 128-step microprogram safely: writes every step with its TWT
// (TEMP-write-trigger) bit masked off first, waits a couple of sample
// periods, then writes the real program. Prevents the DSP from executing a
// step with a torn/half-written instruction mid-upload. Mirrors the
// load sequence in scspadpcm's InitADPCM().
void scsp_dsp_load_program(const uint64_t program[DSP_STEP_COUNT]);

#endif
