/* Saber Rider and the Star Sheriffs — demo reconstruction, the Sega Saturn (libyaul) program.
 * The disc holds the demo's packs and the ones tools/saturn/build_disc.py bakes, all in the ISO root (cd_sat.c). */
#include <yaul.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../app.h"
#include "sat_internal.h"

#ifdef SAT_RENDER_NULL   /* bring-up / CPU profiles: platform/null/render_null.c, nothing on screen */
Ren *rnull_renderer(void);
int  rnull_prims(void);
#define rsat_renderer rnull_renderer
#define rsat_prims    rnull_prims
static void rsat_init(void) { }
static void rsat_frame_begin(void) { }
static void rsat_frame_end(void) { }
static void rsat_timing(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) { *a = *b = *c = *d = 0; }
#endif

static void vblank_out(void *work)
{
    (void)work;
    smpc_peripheral_intback_issue();
    sat_vblank_tick();
}

void user_init(void)
{
    vdp2_tvmd_display_res_set(VDP2_TVMD_INTERLACE_NONE, VDP2_TVMD_HORZ_NORMAL_A, VDP2_TVMD_VERT_224);
    vdp2_scrn_back_color_set(VDP2_VRAM_ADDR(3, 0x01FFFE), RGB1555(1, 0, 0, 0));
    vdp_sync_vblank_out_set(vblank_out, NULL);
    cd_block_init();
    smpc_peripheral_init();
    vdp2_tvmd_display_set();
}

/* The boot stack (IP.BIN: down from 0x06004000, the program's start) has ~8 KB above the slave's; the core needs more.
 * main() moves the master SH-2 onto this one (interrupts run on it too) and paints it to measure the high-water mark. */
#define STACK_SIZE (48 * 1024)
static uint32_t game_stack[STACK_SIZE / 4] __attribute__((aligned(16)));

static unsigned stack_used(void)
{
    unsigned i = 0;
    while (i < STACK_SIZE / 4 && game_stack[i] == 0x5AB3A5A5u) i++;
    return STACK_SIZE - i * 4;
}

static void report_memory(const char *when)
{
    size_t hw_free, lw_free, hw_used, lw_used;
    sat_heap_stats(&hw_free, &lw_free, &hw_used, &lw_used);
    printf("[mem] %s: high RAM heap %u KB used / %u KB free, low RAM %u KB used / %u KB free, stack %u / %u KB\n", when,
           (unsigned)(hw_used / 1024), (unsigned)(hw_free / 1024), (unsigned)(lw_used / 1024), (unsigned)(lw_free / 1024),
           stack_used() / 1024, STACK_SIZE / 1024);
}

static void __attribute__((noreturn, noinline)) game_main(void)
{
    sat_timer_init();
    printf("saber rider: saturn build " __DATE__ " " __TIME__ "\n");
    cd_sat_init();
    rsat_init();
#ifndef SAT_RENDER_NULL
    if (plat_getenv("SABER_RBENCH")) rsat_bench();
#endif
    report_memory("boot");

    const char *lv = plat_getenv("SABER_LEVEL");
    if (!app_init(rsat_renderer(), plat_default_data_dir(), lv ? atoi(lv) : 0)) {
        printf("app_init failed\n");
        for (;;) vdp2_sync(), vdp2_sync_wait();
    }
    report_memory("init");

    uint32_t prev_vb = sat_vblanks(), last_report = 0, frames = 0;
    bool no_draw = plat_getenv("SABER_NODRAW") != NULL;   /* debug: profile the update alone */
    for (;;) {
        smpc_peripheral_process();
        if (sat_reset_combo()) bios_cd_player_execute();
        uint32_t vb = sat_vblanks(), t0 = sat_timer_us();
        /* one displayed field is one fixed game step (59.94 Hz fields counted as 60 Hz steps, as on the Dreamcast) */
        uint32_t fields = vb - prev_vb; prev_vb = vb;
        if (fields == 0) fields = 1;
        int steps = app_update_fields((int)fields);
        uint32_t t_upd = sat_timer_us();
        rsat_frame_begin();
        if (!no_draw) app_draw();
        rsat_frame_end();
        uint32_t t_draw = sat_timer_us();
        if (app_perf_on()) {
            app_perf(t_upd - t0, t_draw - t_upd, fields * 16683u, steps, rsat_prims());
            static uint32_t last_split, fsum, nsum;
            fsum += fields; nsum++;
            if (t0 - last_split > 1000000u) {   /* the draw's split (µs a frame): planes, waiting for VDP1, command DMA */
                uint32_t pl, w, pu, n; rsat_timing(&pl, &w, &pu, &n);
                if (n) printf("[perf] split: planes %u wait-vdp1 %u put %u us a frame, %u.%02u fields a frame\n", (unsigned)(pl / n),
                              (unsigned)(w / n), (unsigned)(pu / n), (unsigned)(fsum / nsum), (unsigned)(fsum * 100 / nsum % 100));
                fsum = nsum = 0; last_split = t0;
            }
        }
        vdp2_sync();
        vdp2_sync_wait();
        frames++;
        if (t0 - last_report > 10000000u) {
            Game *g = app_game(); unsigned long reads, bytes, seeks; cd_sat_stats(&reads, &bytes, &seeks);
            printf("[state] t=%us frame=%u level=%d stage=%d state=%d menu=%d title=%d cd reads=%lu (%lu KB, %lu seeks)\n",
                   (unsigned)(t0 / 1000000u), (unsigned)frames, g->in_level, g->stage, g->state, g->menu.state, g->title_on,
                   reads, bytes / 1024, seeks);
            report_memory("run");
            last_report = t0;
        }
    }
}

int main(void)
{
    for (unsigned i = 0; i < STACK_SIZE / 4 - 64; i++) game_stack[i] = 0x5AB3A5A5u;   /* the top 256 bytes: in use below */
    __asm__ volatile ("mov %0, r15\n\tjmp @%1\n\tnop" : : "r"(game_stack + STACK_SIZE / 4), "r"(game_main) : "memory");
    __builtin_unreachable();
}
