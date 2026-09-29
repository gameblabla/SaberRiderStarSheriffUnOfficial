/* The WASM module's entry points: the page's side of the boundary.
 *
 * The page owns the canvas, the clock, the downloads and the audio device, so this file is a thin shell. The page
 * instantiates the module, pushes the files and the SABER_* switches in, calls wasm_boot once, and then calls
 * wasm_frame from its own requestAnimationFrame with the elapsed time. Nothing calls back into JS during a frame
 * except the two imports a frame needs (the clock, and the console for the core's debug output), so a frame is a
 * straight call from JS into wasm and back.
 *
 * The framebuffer is read straight out of the module's memory: wasm_frame_ptr hands over the address and the page
 * makes a Uint8ClampedArray view on it, which it re-makes whenever wasm_mem_total changes (a memory.grow detaches
 * every view over the old buffer). The game itself is driven through the same app.h every other port uses:
 * app_init once, app_update with the elapsed seconds, app_draw per frame, app_shutdown at the end.
 *
 * The exports are named wasm_* to keep them clear of the C library's, and everything is plain C calling the core -
 * there is no JavaScript engine in here and nothing is interpreted. */
#include "../../app.h"
#include "../plat.h"
#include "../../input.h"
#include "wasm_internal.h"
#include <stdio.h>
#include <string.h>

static char error_text[256];
static bool booted, running;
static unsigned frame_prims;

const char *wasm_error_text(void) { return error_text; }

#define EXPORT(name) __attribute__((export_name(#name)))
/* The developer's half of the boundary. wasm_env_put / wasm_env_get_ptr are how the core's SABER_* switches are
 * set - the level select's ?level=, the debug box, the collision overlay, the free camera - and wasm_input_key is
 * the debug-key path (F1, F2, the camera arrows, fast forward). A distributable build is compiled with
 * SABER_WASM_REDIST and leaves them out of the export list, so they are not merely hidden by the page: nothing
 * outside the module can reach them, and with them unreferenced --gc-sections drops their code. */
#ifdef SABER_WASM_REDIST
#define EXPORT_DEBUG(name)
#else
#define EXPORT_DEBUG(name) __attribute__((export_name(#name)))
#endif

/* stamped at compile time, so a bug report can say which module it ran (the page logs it at boot) */
static const char build_id[] = "saber-wasm " __DATE__ " " __TIME__;
EXPORT(wasm_build_id) unsigned wasm_build_id(void) { return (unsigned)(uintptr_t)build_id; }

/* ------------------------------------------------------------------ boot */
EXPORT(wasm_boot) int wasm_boot(int start_level, int sample_rate)
{
    (void)sample_rate;   /* the mix rate is fixed (WASM_AUDIO_RATE); the page resamples if the device differs */
    error_text[0] = 0;
    Ren *ren = rwasm_renderer();
    if (!ren) { snprintf(error_text, sizeof error_text, "no framebuffer"); return 0; }
    /* the page has pushed the files and the switches by now */
    if (!app_init(ren, plat_default_data_dir(), start_level)) {
        snprintf(error_text, sizeof error_text, "app_init failed (are the data packs loaded?)");
        return 0;
    }
    aud_init();
    booted = running = true;
    return 1;
}

/* ------------------------------------------------------------------ a frame
 * `dt_seconds` is the page's elapsed time. app_update runs the fixed 60 Hz steps (none, one or two, per its
 * accumulator) and app_draw draws the frame; the page then reads the framebuffer. */
EXPORT(wasm_frame) int wasm_frame(double dt_seconds)
{
    if (!running) return 0;
    set_clock(js_now_ms());
    floor_us_reset();
    app_update(dt_seconds);
    app_draw();
    rwasm_present();
    frame_prims = (unsigned)prims_this_frame();
    return running ? 1 : 0;   /* 0 once SABER_SHOT has asked the run to end */
}

EXPORT(wasm_shutdown) void wasm_shutdown(void)
{
    if (booted) app_shutdown();
    aud_shutdown();
    booted = running = false;
}

/* ---- what the page reads each frame ----
 * wasm_frame_ptr is the framebuffer: WASM_SCREEN_W * WASM_SCREEN_H pixels of RGBA8888, R first in memory, which
 * is exactly the order ImageData wants, so the page makes one Uint8ClampedArray view and putImageData's it. */
EXPORT(wasm_frame_ptr) unsigned wasm_frame_ptr(void) { return (unsigned)(uintptr_t)wasm_framebuffer(); }
EXPORT(wasm_frame_w) int wasm_frame_w(void) { return wasm_frame_width(); }
EXPORT(wasm_frame_h) int wasm_frame_h(void) { return wasm_frame_height(); }
EXPORT(wasm_prims) int wasm_prims(void) { return (int)frame_prims; }
EXPORT(wasm_floor_us) unsigned wasm_floor_us(void) { return floor_us_total(); }

/* ---- handing the module its data ----
 * The page has no allocator of its own that the module can see, so it asks for a block here, fills it, and hands
 * it over with wasm_vfs_add / wasm_vfs_add_image, which take ownership. wasm_alloc is the same heap malloc uses. */
EXPORT(wasm_alloc) unsigned wasm_alloc(unsigned bytes) { return (unsigned)(uintptr_t)malloc(bytes ? bytes : 1); }
#ifdef SABER_WASM_SELFTEST
size_t wasm_heap_walk(unsigned *buf, unsigned cap);
EXPORT(wasm_heap_walk) unsigned wasm_heap_walk_(unsigned buf, unsigned cap) { return (unsigned)wasm_heap_walk((unsigned *)(uintptr_t)buf, cap); }
#endif
EXPORT(wasm_error_msg) unsigned wasm_error_msg(void) { return (unsigned)(uintptr_t)error_text; }

/* ---- the heap and the linear memory, for the status panel ---- */
EXPORT(wasm_heap_free) unsigned wasm_heap_free(void) { return (unsigned)heap_free_bytes(); }
EXPORT(wasm_mem_used) unsigned wasm_mem_used(void) { return (unsigned)memory_used(); }
EXPORT(wasm_mem_total) unsigned wasm_mem_total(void) { return (unsigned)memory_total(); }
EXPORT(wasm_vfs_count) int wasm_vfs_count(void) { return vfs_count(); }
EXPORT(wasm_vfs_bytes) unsigned wasm_vfs_bytes(void) { return (unsigned)vfs_bytes(); }

/* ---- audio ----
 * The mixer writes into a ring; the page copies the unread run (which starts at the read index and may wrap,
 * so it goes through the read pointer and the contiguous count, not the ring's base) and takes what it took. */
EXPORT(wasm_audio_ready) int wasm_audio_ready(void) { return audio_pending(); }
EXPORT(wasm_audio_get_ptr) unsigned wasm_audio_get_ptr(void) { return (unsigned)(uintptr_t)audio_read_ptr(); }
EXPORT(wasm_audio_contig) int wasm_audio_contig(void) { return audio_contig(); }
EXPORT(wasm_audio_take) int wasm_audio_take(int frames) { return audio_take(frames); }
EXPORT(wasm_audio_set_volume) void wasm_audio_set_volume(float v) { audio_set_master(v); }

/* ---- input ---- */
EXPORT(wasm_input_push) void wasm_input_push(uint32_t buttons, int sh_l, int sh_r, int sx, int sy, int active)
{
    input_set(buttons, sh_l != 0, sh_r != 0, sx, sy, active != 0);
}
EXPORT_DEBUG(wasm_input_key) void wasm_input_key(int key, int down) { input_debug_key(key, down != 0); }
EXPORT(wasm_input_sens) void wasm_input_sens(int s) { input_set_sensitivity(s); }   /* input.h: the core's own, so the in-game SENSITIVITY option and the page agree */

/* ---- the SABER_* switches (the sidebar's developer box and the URL's query string) ---- */
EXPORT_DEBUG(wasm_env_put) int wasm_env_put(unsigned name_ptr, unsigned name_len, unsigned value_ptr, unsigned value_len)
{
    char name[64], value[256];
    if (name_len >= sizeof name || value_len >= sizeof value) return 0;
    memcpy(name, (const void *)(uintptr_t)name_ptr, name_len);
    name[name_len] = 0;
    memcpy(value, (const void *)(uintptr_t)value_ptr, value_len);
    value[value_len] = 0;
    return env_set(name, value);
}
EXPORT_DEBUG(wasm_env_get_ptr) unsigned wasm_env_get_ptr(unsigned name_ptr, unsigned name_len, unsigned buf_ptr, unsigned buf_cap)
{
    char name[64];
    if (name_len >= sizeof name) return 0;
    memcpy(name, (const void *)(uintptr_t)name_ptr, name_len);
    name[name_len] = 0;
    const char *v = env_get(name);
    if (!v) return 0;
    size_t n = strlen(v);
    if (n >= buf_cap) n = buf_cap - 1;
    memcpy((void *)(uintptr_t)buf_ptr, v, n);
    return (unsigned)n;
}

/* ---- the game state, for the status panel ---- */
EXPORT(wasm_state) int wasm_state(void)
{
    Game *g = app_game();
    if (!g) return 0;
    return (g->stage & 0xff) | ((g->state & 0xff) << 8) | ((g->in_level ? 1 : 0) << 16);
}
