/* SCSP feeder adapter. aud_movie_begin owns/reset the chip and leaves the
 * 68000 running its idle loop. Never replace or clear the resident SFX bank. */
#include <yaul.h>
#include "pcmsys.h"
static sysComPara controls;
sysComPara *m68k_com = &controls;
uint32_t getSlotAddressOffset(uint32_t slot) { return 0x78000u + slot * 16384u; }
uint8_t *getSlotAddress(uint32_t slot) { return (uint8_t *)(SNDRAM + getSlotAddressOffset(slot)); }
uint32_t getSlotSize(void) { return 16384u; }
uint32_t pcmStreamBufferSize(uint8_t bits, uint16_t rate) { (void)bits; (void)rate; return 16384u; }
short convert_bitrate_to_pitchword(uint16_t rate)
{
    uint32_t base=44100u; int oct=0;
    while (rate < base && oct > -8) { base >>= 1; oct--; }
    uint32_t fns=((uint32_t)rate-base)*1024u/base;
    return (short)(((oct & 15) << 11) | (fns & 1023u));
}
