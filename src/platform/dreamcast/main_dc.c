/* Saber Rider and the Star Sheriffs — demo reconstruction, the Dreamcast (KallistiOS) program.
 * The disc holds the demo's packs in /cd/data and everything converted by tools/dc/build_disc.py. */
#include <kos.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include "../../app.h"
#include "pvr_internal.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

Ren *rdc_renderer(void);
bool dc_reset_combo(void);
size_t rdc_vram_used(void);
int rdc_prims(void);
void rdc_header_stats(int *sent, int *asked);

static void report_memory(const char *when)
{
    struct mallinfo mi = mallinfo();
    printf("[mem] %s: heap in use %d KB, arena %d KB; vram textures %u KB, free %u KB\n", when,
           mi.uordblks / 1024, mi.arena / 1024, (unsigned)(rdc_vram_used() / 1024), (unsigned)(pvr_mem_available() / 1024));
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    vid_set_mode(DM_640x480, PM_RGB565);
    pvr_init_params_t params = {
        .opb_sizes = { PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_0 },   /* everything is in the translucent list */
        .vertex_buf_size = 512 * 1024,
        .dma_enabled = 0,
        .fsaa_enabled = 0,
        .autosort_disabled = 1,       /* draw in submission order, like the 2D renderer it replaces */
        .opb_overflow_count = 2,
    };
    pvr_init(&params);
    pvr_set_bg_color(0, 0, 0);
    rdc_init();
    fs_chdir("/cd");
    report_memory("boot");

    const char *lv = plat_getenv("SABER_LEVEL");
    if (!app_init(rdc_renderer(), plat_default_data_dir(), lv ? atoi(lv) : 0)) {
        printf("app_init failed\n");
        return 1;
    }
    report_memory("init");

    uint64_t prev = timer_ns_gettime64(), last_report = 0;
    for (;;) {
        if (dc_reset_combo()) break;
        uint64_t now = timer_ns_gettime64();
        uint64_t delta_ns = now - prev;
        /* Convert a bounded microsecond delta via 32-bit float (cheap on the
         * SH-4), then let the shared fixed-step app accumulator run. */
        float elapsed = (float)(uint32_t)(delta_ns / 1000) * 0.000001f;
        app_update(elapsed); prev = now;
        rdc_frame_begin();
        app_draw();
        rdc_frame_end();
        if (now - last_report > 10000000000ull) {
            Game *g = app_game(); int hs, ha; rdc_header_stats(&hs, &ha);
            printf("[state] level=%d stage=%d state=%d menu=%d title=%d draws=%d headers=%d/%d\n",
                   g->in_level, g->stage, g->state, g->menu.state, g->title_on, rdc_prims(), hs, ha);
            report_memory("run"); last_report = now;
        }
    }
    app_shutdown();
    pvr_shutdown();
    return 0;
}
