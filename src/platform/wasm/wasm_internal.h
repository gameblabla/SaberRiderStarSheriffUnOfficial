#pragma once
/* The WASM port's own interface between the module and the page (src/platform/wasm).
 *
 * One direction only for the data: the page pushes the files, the decoded images, the input state and the SABER_*
 * switches into the module, then drives it with wasm_boot once and wasm_frame per animation frame, and reads the
 * framebuffer and the audio ring back out. The module calls into JS only through the `env` imports declared in
 * wasm_main.c - the clock and the console - so a frame is a straight call from JS into wasm and back. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct Ren Ren;

/* ---- the page's imports (see web/saber-wasm.js) ----
 * Declared with the import attribute so wasm-ld knows they are provided by the host rather than missing: the
 * clock (performance.now, in milliseconds) and the console (the core's 105 debug fprintf(stderr) lines and the
 * SABER_PERF report go to console.log / console.warn). Everything else crosses the boundary as an export. */
#define WASM_IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
extern WASM_IMPORT(js_now_ms) double js_now_ms(void);
extern WASM_IMPORT(js_log) void js_log(int is_error, const char *text, uint32_t len);

/* ---- the heap and linear memory (libc_wasm.c) ---- */
size_t heap_free_bytes(void);
size_t memory_used(void);
size_t memory_total(void);
void   set_heap_hook(bool (*hook)(void));   /* gfx.c's texture eviction, on an allocation that will not fit */
#ifdef SABER_WASM_SELFTEST
/* the heap's own two predicates, for heap_test.c: is this pointer one of ours, and is the block list walkable
 * from end to end? (they are static in libc_wasm.c, so the test asks through these) */
bool wasm_ptr_is_heap(void *p);
bool wasm_heap_list_ok(void);
#endif

/* ---- the file system the core reads through stdio (vfs_wasm.c) ----
 * The page fetches each file the game may ask for, copies it into the module's memory and calls wasm_vfs_add,
 * which takes that block over. fopen finds a file by name, so pack.c's fseek/fread/ftell and assets.c's file_read
 * work on the packs unchanged. The names are the ones the core builds: "data/<pack>.pck" and "assets/<name>". */
int    wasm_vfs_add(const char *name, uint8_t *data, uint32_t size);
int    vfs_count(void);
size_t vfs_bytes(void);
/* an image the page decoded (a PNG turned into RGBA8888, R first in memory, the layout render.h asks for) */
int    wasm_vfs_add_image(const char *path, uint32_t w, uint32_t h, uint32_t *rgba);

/* ---- the screen (render_wasm.c) ----
 * The internal resolution is fixed at WASM_SCREEN_W x WASM_SCREEN_H and the page scales the canvas to fit, so a
 * frame costs the same whatever the window is. The framebuffer is malloc'd once, so its address never changes,
 * but a JS view over the module's memory is detached by a memory.grow, so the page re-makes its view whenever
 * memory_total() changes. */
#define WASM_SCREEN_W 426
#define WASM_SCREEN_H 240

uint32_t *wasm_framebuffer(void);
int  wasm_frame_width(void);
int  wasm_frame_height(void);
int  prims_this_frame(void);       /* draws this frame, for the status panel (SABER_PERF) */
uint32_t floor_us_total(void);     /* the Mode 7 floor's own time, for the status panel */
void floor_us_reset(void);

/* ---- the renderer the core draws through ---- */
Ren *rwasm_renderer(void);
void rwasm_present(void);        /* the frame is ready for the page to read */
int  rwasm_drawn_width(void);    /* the game's logical width this frame (426, or 320 in 4:3) */

/* ---- audio (aud_wasm.c) ----
 * The mixer writes interleaved S16 stereo into a ring in the module's memory; the page copies out whatever is
 * ready after each frame and posts it to its AudioWorklet, the arrangement the reference WASM port uses. The ring
 * holds half a second, so a late frame is slack rather than a click. */
#define WASM_AUDIO_RATE 44100
#define WASM_AUDIO_RING_FRAMES (WASM_AUDIO_RATE / 2)

/* aud.h's own names, so audio.c (core) finds them: aud_init / aud_shutdown */
bool  aud_init(void);
void  aud_shutdown(void);
void  audio_pump(int frames);      /* mix n output frames (1/60 s each) into the ring */
int16_t *audio_ring(void);
int16_t *audio_read_ptr(void);   /* the oldest unread frame: what the page copies out (the run may wrap) */
int   audio_contig(void);        /* contiguous frames available from audio_read_ptr */
int   audio_pending(void);         /* frames ready to copy out */
int   audio_take(int frames);
void  audio_set_master(float v);   /* the page's volume control, 0..1 */
float audio_peak(void);            /* the limiter's envelope, for the page's meter */

/* ---- input (input_wasm.c) ----
 * The page owns the bindings - its sidebar is the controls panel - so it resolves the keyboard, the gamepad and
 * the touch overlay into the game's nine buttons, the two shoulders and the analog stick and pushes the result
 * here. plat_input_poll only reports that state (and puts the stick through the core's own dead zone, so the
 * game's SENSITIVITY option still works). */
void input_set(uint32_t buttons, bool shoulder_l, bool shoulder_r, int stick_x, int stick_y, bool stick_active);
void input_debug_key(int key, bool down);   /* game.h's DBG_KEY_*, for the page's F1/F2 and the arrows */

/* ---- the SABER_* debug switches (plat_wasm.c) ----
 * The console ports read a NAME=value file off the disc; here the page fills this table (from the sidebar's
 * developer box, or the URL's query string), and the core's 104 plat_getenv calls read it. */
int  env_set(const char *name, const char *value);
const char *env_get(const char *name);
uint64_t set_clock(double now_ms);   /* the page's performance.now(), for plat_ticks_ms */
