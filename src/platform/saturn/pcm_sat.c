/* One PCM stream on the SCSP, driven from the SH-2 (plan 7.2 has the full driver; this is what the videos need): slot 0
 * loops over a ring of 16-bit mono samples in sound RAM, the SH-2 writes the samples ahead of it. No 68000 program:
 * the 68000 is stopped (SMPC SNDOFF) and the slot registers are written directly.
 *
 * The ring is RING samples at sound RAM offset RING_OFF; the slot plays it at `rate` (44.1 kHz x 2^OCT x
 * (1 + FNS / 1024)) from the key-on. Where the playback is comes from the SH-2's timer (the SCSP only reports its
 * position to 4096 samples): played = time since key-on x rate. A writer keeps at most RING - MARGIN samples ahead. */
#include "sat_internal.h"
#include <yaul.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SND_RAM    ((volatile uint16_t *)0x25A00000u)
#define SLOT(n, r) (*(volatile uint16_t *)(0x25B00000u + (uint32_t)(n) * 0x20u + (uint32_t)(r) * 2u))
#define SCSP_MVOL  (*(volatile uint16_t *)0x25B00400u)
#define RING_OFF   0x10000u         /* bytes into sound RAM */
#define RING       32768            /* samples (the loop end register is 16 bits); 1.49 s at 22050 Hz */
#define MARGIN     1024             /* samples kept clear of the playback position (its interpolation reads ahead) */

static struct {
    bool on, finishing;
    int rate;
    uint32_t t0;                    /* sat_timer_us at the key-on */
    uint32_t written;               /* samples written since the start */
    int16_t *tail; int tail_n, tail_at;
    uint32_t end; bool ended;       /* finishing: the last sample's position once the tail is written */
    unsigned underruns;
} P;

void pcm_sat_init(void)
{
    smpc_smc_sndoff_call();                     /* the 68000 stays off: the SH-2 drives the slots */
    SCSP_MVOL = 0x0200 | 0x000F;                /* 4 Mbit sound RAM, master volume 15 */
    for (int n = 0; n < 32; n++) {
        SLOT(n, 0) = 0x1000;                    /* key off (KYONEX) */
        for (int r = 1; r < 16; r++) SLOT(n, r) = 0;
    }
}

static void put(const int16_t *s, int n)
{
    uint32_t at = P.written % RING;
    volatile uint16_t *ring = SND_RAM + RING_OFF / 2;
    for (int i = 0; i < n; i++) {
        ring[at] = (uint16_t)s[i];
        if (++at == RING) at = 0;
    }
    P.written += (uint32_t)n;
}

static void put_zero(int n)
{
    uint32_t at = P.written % RING;
    volatile uint16_t *ring = SND_RAM + RING_OFF / 2;
    for (int i = 0; i < n; i++) { ring[at] = 0; if (++at == RING) at = 0; }
    P.written += (uint32_t)n;
}

static void key(bool on)
{
    const uint16_t sa_hi = (uint16_t)((RING_OFF >> 16) & 0xF);
    SLOT(0, 0) = (uint16_t)(0x1000 | (on ? 0x0800 : 0) | (1 << 5) | sa_hi);   /* KYONEX, KYONB, LPCTL normal loop, 16-bit */
}

/* OCT / FNS for a rate: rate = 44100 x 2^oct x (1024 + fns) / 1024 */
static uint16_t pitch(int rate)
{
    int oct = 0;
    uint32_t r = (uint32_t)rate;
    while (r < 44100u && oct > -8) { r <<= 1; oct--; }
    while (r >= 88200u && oct < 7) { r >>= 1; oct++; }
    uint32_t fns = (r * 1024u) / 44100u - 1024u;   /* r in [44100, 88200) */
    return (uint16_t)(((unsigned)oct & 0xF) << 11 | (fns & 0x3FF));
}

uint32_t pcm_sat_played(void)
{
    if (!P.on) return 0;
    uint32_t us = sat_timer_us() - P.t0;
    return (uint32_t)(((uint64_t)us * (uint32_t)P.rate) / 1000000u);
}

bool pcm_sat_start(int rate, const int16_t *lead, int lead_samples)
{
    pcm_sat_stop();
    if (rate < 4000 || rate > 44100) return false;
    P.rate = rate; P.written = 0; P.underruns = 0;
    if (lead_samples > RING - MARGIN) lead_samples = RING - MARGIN;
    put_zero(RING);                             /* silence everywhere the playback can reach before the writer */
    P.written = 0;
    put(lead, lead_samples);
    SLOT(0, 1) = (uint16_t)(RING_OFF & 0xFFFF);  /* SA */
    SLOT(0, 2) = 0;                             /* LSA */
    SLOT(0, 3) = (uint16_t)RING;                /* LEA: the loop is [LSA, LEA) (mednafen scsp.inc) */
    SLOT(0, 4) = 0x001F;                        /* AR 31 (instant), D1R 0, D2R 0: full level while keyed */
    SLOT(0, 5) = (uint16_t)(0xF << 10 | 0x1F);  /* KRS off, DL 0, RR 31 */
    SLOT(0, 6) = 0;                             /* TL 0 dB */
    SLOT(0, 7) = 0;
    SLOT(0, 8) = pitch(rate);
    SLOT(0, 9) = 0;
    SLOT(0, 10) = 0;
    SLOT(0, 11) = (uint16_t)(7 << 13);          /* direct send 0 dB, pan centre */
    P.on = true; P.finishing = false;
    key(true);
    P.t0 = sat_timer_us();
    return true;
}

int pcm_sat_space(void)
{
    if (!P.on) return 0;
    uint32_t played = pcm_sat_played();
    if (P.written < played) {   /* the writer fell behind: what it lost is gone, go on from where the playback is */
        if (!P.underruns++) printf("pcm: underrun (%u samples)\n", (unsigned)(played - P.written));
        P.written = played + 256;
    }
    int32_t s = (int32_t)(played + RING - MARGIN - P.written);
    return s > 0 ? s : 0;
}

void pcm_sat_write(const int16_t *s, int n)
{
    if (!P.on || n <= 0) return;
    int room = pcm_sat_space();
    if (n > room) n = room;   /* never over what hasn't been played */
    put(s, n);
}

void pcm_sat_finish(const int16_t *tail, int n)
{
    if (!P.on) return;
    free(P.tail); P.tail = NULL; P.tail_n = P.tail_at = 0;
    if (tail && n > 0 && (P.tail = malloc((size_t)n * 2))) { memcpy(P.tail, tail, (size_t)n * 2); P.tail_n = n; }
    P.finishing = true; P.ended = false;
    pcm_sat_poll();
}

void pcm_sat_stop(void)
{
    if (P.on) key(false);
    P.on = P.finishing = false;
    free(P.tail); P.tail = NULL; P.tail_n = P.tail_at = 0;
}

void pcm_sat_poll(void)
{
    if (!P.on || !P.finishing) return;
    int room = pcm_sat_space();
    if (P.tail_at < P.tail_n) {
        int n = P.tail_n - P.tail_at; if (n > room) n = room;
        put(P.tail + P.tail_at, n); P.tail_at += n;
        return;
    }
    if (!P.ended) { P.end = P.written; P.ended = true; }
    if (pcm_sat_played() >= P.end) { pcm_sat_stop(); return; }
    /* zeros behind the end, so the loop plays silence (not the ring's old samples) until the key-off */
    if (room > 2048) put_zero(2048);
}
