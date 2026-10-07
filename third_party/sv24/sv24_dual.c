#include <yaul.h>
#include <stdint.h>
#include <stddef.h>
#include "sv24_dual.h"
#include "sv24_slice.h"

#define MAX_JOBS SV24_V03_MAX_SLICES

typedef struct {
    uint32_t data;
    uint32_t size;
    uint16_t y0;
    uint8_t rows;
    uint8_t pad;
} dual_desc_t;

typedef struct {
    volatile uint32_t seq;
    volatile uint32_t done_seq;
    volatile uint32_t running_seq;
    volatile int32_t result;
    volatile uint32_t count;
    volatile uint32_t staging;
    volatile uint32_t plans;
    volatile uint32_t gap_pixels;
    dual_desc_t job[MAX_JOBS];
} dual_state_t;

static dual_state_t dual_state __aligned(32);
static uint32_t master_job[MAX_JOBS];
static uint32_t slave_job[MAX_JOBS];

volatile uint32_t sv24_dual_slave_frames = 0;
volatile uint32_t sv24_dual_slave_slices = 0;
volatile uint32_t sv24_dual_master_slices = 0;
volatile uint32_t sv24_dual_wait_spins = 0;
volatile uint32_t sv24_dual_decode_ticks_total = 0;
volatile uint32_t sv24_dual_decode_ticks_max = 0;
volatile uint32_t sv24_dual_decode_calls = 0;
volatile uint32_t sv24_dual_fault = 0;
#ifdef PLAT_SATURN
extern void sat_smpc_lock(void),sat_smpc_unlock(void);
#endif

static volatile dual_state_t *state_uncached(void)
{
    return (volatile dual_state_t *)(CPU_CACHE_THROUGH | (uintptr_t)&dual_state);
}

static void slave_entry(void)
{
    volatile dual_state_t *st = state_uncached();
    uint32_t seq = st->seq;
    st->running_seq=seq;
    uint32_t i;
    int r = 0;
    uint32_t *dst = (uint32_t *)(CPU_CACHE_THROUGH | (uintptr_t)st->staging);
    sv24_slice_dma_plan_t *plans = st->plans ? (sv24_slice_dma_plan_t *)(CPU_CACHE_THROUGH | (uintptr_t)st->plans) : 0;
    cpu_cache_purge();

    for (i = 0; i < st->count; i++) {
        const dual_desc_t *j = (const dual_desc_t *)&st->job[i];
        const uint8_t *src = (const uint8_t *)(uintptr_t)j->data;
        if (plans) r = sv24_decode_abs_slice_frame_plan(src, j->size, j->y0, j->rows, dst,
                                             &plans[j->pad], st->gap_pixels);
        else r = sv24_decode_abs_slice_frame(src, j->size, j->y0, j->rows, dst);
        if (r) break;
        sv24_dual_slave_slices++;
    }
    st->result = r;
    st->done_seq = seq;
    sv24_dual_slave_frames++;
}

void sv24_dual_init(void)
{
    volatile dual_state_t *st = state_uncached();
    st->seq = 0;
    st->done_seq = 0;
    st->running_seq = 0;
    st->result = 0;
    st->count = 0;
#ifndef PLAT_SATURN
    cpu_frt_init(CPU_FRT_CLOCK_DIV_128);
#endif
#ifndef PLAT_SATURN
    cpu_dual_comm_mode_set(CPU_DUAL_ENTRY_POLLING);
#endif
    /* The game already owns a running ICI slave. Swap its callback only;
     * restarting it each frame races the game's in-flight SMPC INTBACK. */
    cpu_dual_slave_set(slave_entry);
}

static void split_jobs(const sv24_v03_frame_t *fr, unsigned *mc, unsigned *sc)
{
    uint32_t load_m = 0, load_s = 0;
    unsigned used[MAX_JOBS] = {0};
    unsigned out_m = 0, out_s = 0;
    unsigned n;

    /* v25.4: the encoder can solve the 10-slice partition offline and put
       the exact master set in the formerly-reserved FRM3 word.  Old streams
       leave it zero and fall through to the hint-aware scheduler below. */
    if (fr->schedule_mask) {
        unsigned i;
        for (i = 0; i < fr->slice_count; i++) {
            if (fr->schedule_mask & (1u << i)) master_job[out_m++] = i;
            else slave_job[out_s++] = i;
        }
        *mc = out_m; *sc = out_s;
        return;
    }

    /* Greedy longest-slice-first assignment.  Compressed bytes plus changed-record count approximate both I/O and pixel work. */
    for (n = 0; n < fr->slice_count; n++) {
        unsigned i, best = 0;
        uint32_t best_sz = 0;
        for (i = 0; i < fr->slice_count; i++) {
            /* New smooth modes are deliberately tiny on the wire but still
               reconstruct a full 8x8 cell.  Include the changed-record count
               in the work estimate so dual-SH2 scheduling does not mistake a
               PLANE/LERP-heavy slice for a cheap one. */
            const uint8_t *d = fr->slice[i].data;
            uint32_t cc = (fr->slice[i].size >= 4u) ? (((uint32_t)d[2] << 8) | d[3]) : 0u;
            uint32_t work = fr->slice[i].work_hint ? (uint32_t)fr->slice[i].work_hint :
                            (fr->slice[i].size + cc * 36u);
            if (!used[i] && work >= best_sz) {
                best = i;
                best_sz = work;
            }
        }
        used[best] = 1;
        if (load_m * 99u <= load_s * 100u) {
            master_job[out_m++] = best;
            load_m += best_sz;
        } else {
            slave_job[out_s++] = best;
            load_s += best_sz;
        }
    }
    *mc = out_m;
    *sc = out_s;
}

int sv24_dual_decode_frame(const sv24_v03_frame_t *fr, uint32_t *staging,
                           sv24_slice_dma_plan_t *plans, unsigned max_gap_pixels)
{
    volatile dual_state_t *st = state_uncached();
    unsigned mc, sc, i;
    uint32_t seq;
    uint16_t tick0, tick1, dt;
    int r = 0;
    uint32_t *dst_uncached = (uint32_t *)(CPU_CACHE_THROUGH | (uintptr_t)staging);

    if (!fr || !staging || !plans) return -100;
    split_jobs(fr, &mc, &sc);
    tick0 = cpu_frt_count_get();

    st->staging = (uint32_t)(uintptr_t)staging;
    st->plans = (fr->coding_flags & SV24_FRM3_MAP_DMA_SAFE) ? 0u : (uint32_t)(uintptr_t)plans;
    st->gap_pixels = max_gap_pixels;
    st->count = sc;
    st->result = 0;
    for (i = 0; i < sc; i++) {
        const sv24_v03_slice_t *s = &fr->slice[slave_job[i]];
        st->job[i].data = (uint32_t)(uintptr_t)s->data;
        st->job[i].size = s->size;
        st->job[i].y0 = s->tile_y0;
        st->job[i].rows = s->tile_rows;
        st->job[i].pad = (uint8_t)slave_job[i];
    }
    seq = st->seq + 1u;
    st->seq = seq;

    if (sc) cpu_dual_slave_notify();

    cpu_cache_purge();

    for (i = 0; i < mc; i++) {
        const sv24_v03_slice_t *s = &fr->slice[master_job[i]];
        const uint8_t *src = (const uint8_t *)(uintptr_t)s->data;
        if (fr->coding_flags & SV24_FRM3_MAP_DMA_SAFE)
            r = sv24_decode_abs_slice_frame(src, s->size, s->tile_y0, s->tile_rows, dst_uncached);
        else
            r = sv24_decode_abs_slice_frame_plan(src,
                                             s->size, s->tile_y0, s->tile_rows,
                                             dst_uncached,
                                             &((sv24_slice_dma_plan_t *)(CPU_CACHE_THROUGH | (uintptr_t)plans))[master_job[i]],
                                             max_gap_pixels);
        if (r) break;
        sv24_dual_master_slices++;
    }

    if (sc) {
        uint16_t last=cpu_frt_count_get();uint32_t ticks=0,retry=0;
        while (st->done_seq != seq) {
            uint16_t now=cpu_frt_count_get();ticks+=(uint16_t)(now-last);last=now;
            sv24_dual_wait_spins++;
            if(st->running_seq!=seq&&ticks-retry>5000u){retry=ticks;cpu_dual_slave_notify();}
            if(ticks>200000u) {
                /* Stop before the caller releases memory a stalled slave
                 * might still write. The game restores its renderer slave. */
#ifdef PLAT_SATURN
                sat_smpc_lock();
#endif
                smpc_smc_sshoff_call();
#ifdef PLAT_SATURN
                sat_smpc_unlock();
#endif
                sv24_dual_fault=1;r=-95;break;
            }
        }
        if (!r) r = st->result;
    }
    tick1 = cpu_frt_count_get();
    dt = (uint16_t)(tick1 - tick0);
    sv24_dual_decode_ticks_total += dt;
    if ((uint32_t)dt > sv24_dual_decode_ticks_max) sv24_dual_decode_ticks_max = dt;
    sv24_dual_decode_calls++;
    return r;
}
