#ifndef __ADX_DSP_SEGA_H__
#define __ADX_DSP_SEGA_H__

#include <yaul.h> // uint8_t/uint16_t/uint32_t come from here (via bios.h), not a direct stdint.h include

// SCSP-DSP-based ADX recursive-filter offload, prototype #2.
//
// This is a from-scratch replacement for adx_dsp.c/adx_dsp.h, built to
// replicate the STRUCTURE Sega's own SCSP DSP program actually uses for
// Sakura Wars 2's ADX movie audio (see SCSP_ADX_FILTER_FINDINGS.md), not
// the earlier attempt's collapsed single-stage approximation. Ignore the
// old adx_dsp.c/h entirely - this is a clean rebuild, not a patch.
//
// Confirmed-real coefficients (from live SCSP captures, steps 110-115 of
// Sega's actual DSP program): a two-stage filter, 4 terms into an
// intermediate register then 2 terms into the final output, not a single
// flat MAC. See adx_dsp_sega.c for the per-term breakdown and what's
// confirmed vs. our own filled-in design choice.
//
// Same plumbing as before (unchanged, not part of what was wrong): the SH2
// side keeps computing the ADX residual (nibble*scale, no history
// dependency) and feeds it to the DSP via two silent PCM "feeder" voices
// routed into the DSP's MIXS input bus.

// Slots 2/3 do NOT fit in sound RAM: getSlotAddressOffset(slot) is
// 0x10000 + slot*getSlotSize() (128KB/slot), and sound RAM is only 512KB
// (0x80000) total - slot 3 alone would span 0x70000-0x90000, 64KB past the
// end (confirmed real bug, not theoretical: caused audio corruption/silence
// on real hardware). Reusing 0/1 is safe instead: a movie is always either
// ADX-via-DSP or plain PCM, never both at once (see film_audio_setup()'s
// codec branch in decoder.c), so nothing else needs these slots while ADX
// is active.
#define ADX_DSP_FEEDER_SLOT_L (0)
#define ADX_DSP_FEEDER_SLOT_R (1)

// Loads COEF, uploads the filter microprogram, and configures/mutes/routes
// the two feeder voices for the given sample rate. Returns the feeder
// buffer size (bytes), same convention as pcmStreamBufferSize().
uint32_t adx_dsp_setup(uint16_t sampleRate, int16_t stream_coef1, int16_t stream_coef2);

// Clears the DSP-side filter history (all 3 history taps, both channels).
// Call once per fresh ADX stream.
void adx_dsp_reset_history(void);

// Keys on both feeder voices so the DSP starts receiving residuals.
void adx_dsp_play(uint8_t volume);

// Re-asserts DISDL=0 (muted dry output) on both feeder voices - the 68K
// driver's own play-command handling clobbers this back to nonzero
// sometime after adx_dsp_play(). Call every tick.
void adx_dsp_remute_dry_output(void);

// Keys off both feeder voices (pcm_cease) and re-asserts DISDL=0 one more
// time immediately after. Call when an ADX stream ends. Mirrors the same
// clobbering race adx_dsp_remute_dry_output() fights during playback, just
// at the other end - pcm_cease()'s sh2_permit=0 is processed asynchronously
// by the 68K driver, so there's a window after this call where the voice
// may still be audible and briefly unmuted again before it actually keys
// off. If real hardware testing shows this one-shot call doesn't reliably
// win that race (same as a one-shot remute didn't reliably win the
// startup race - see adx_dsp_remute_dry_output()'s history), the fix is
// the same shape: keep calling adx_dsp_remute_dry_output() every tick for
// a few ticks after stopping, not just once here.
void adx_dsp_stop(void);

#endif
