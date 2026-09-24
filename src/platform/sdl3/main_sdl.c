/* Saber Rider and the Star Sheriffs — demo reconstruction, the SDL3 (PC) program.
 * Reads the original demo data packs (.pck) directly. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../app.h"
#include "sdl_platform.h"

static void usage(const char *exe)
{
    fprintf(stderr, "usage: %s [data_dir] [--level N]\n"
                    "  data_dir    the demo's data/ folder with the .pck packs (default SaberRider/data)\n"
                    "  --level N   skip the front end and start at level N (1 the frontier town, 2 the All Galaxy Grand Prix, 3 Hyperjumper Pass, 4 the Red Palm Jungle, 5 the cave lab, 6 Ramrod)\n", exe);
}

/* the display's frame period: a 59.94 / 60.x Hz display counts as exactly one 60 Hz game step, so each shown frame runs
 * exactly one step instead of now and then none or two (the jitter of a measured frame time around 1/60 s did that) */
static double frame_period(SDL_Window *win)
{
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
    double hz = dm && dm->refresh_rate > 1.0f ? dm->refresh_rate : 60.0;
    if (hz > 59.0 && hz < 61.0) hz = 60.0;
    return 1.0 / hz;
}

static int debug_key(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_F1: return DBG_KEY_COLLISION;  case SDL_SCANCODE_F2: return DBG_KEY_FREECAM;
    case SDL_SCANCODE_LEFT: return DBG_KEY_LEFT;     case SDL_SCANCODE_RIGHT: return DBG_KEY_RIGHT;
    case SDL_SCANCODE_LSHIFT: return DBG_KEY_FAST;   default: return -1;
    }
}

int main(int argc, char **argv)
{
    const char *data_dir = plat_default_data_dir(); int start_level = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--level") || !strcmp(argv[i], "-l")) {
            if (i + 1 >= argc || (start_level = atoi(argv[++i])) < 1 || start_level > 6) { usage(argv[0]); return 2; }
        } else if (!strncmp(argv[i], "--level=", 8)) { start_level = atoi(argv[i] + 8); if (start_level < 1 || start_level > 6) { usage(argv[0]); return 2; } }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "unknown option %s\n", argv[i]); usage(argv[0]); return 2; }
        else data_dir = argv[i];
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win; SDL_Renderer *ren;
    int ww = APP_SCREEN_W * 3, wh = APP_SCREEN_H * 3;
    if (SDL_getenv("SABER_WINDOW")) sscanf(SDL_getenv("SABER_WINDOW"), "%dx%d", &ww, &wh);   /* debug: initial window size */
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");   /* asked for at creation too: not every backend takes it afterwards */
    if (!SDL_CreateWindowAndRenderer("Saber Rider and the Star Sheriffs", ww, wh, SDL_WINDOW_RESIZABLE, &win, &ren)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1;
    }
    SDL_SetRenderLogicalPresentation(ren, APP_SCREEN_W, APP_SCREEN_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    /* every present waits for the display's vertical blank (no tearing). A driver that refuses gets adaptive vsync (tears
     * only a late frame); with neither, the loop paces itself to the display's refresh */
    int vsync = SDL_SetRenderVSync(ren, 1) ? 1 : SDL_SetRenderVSync(ren, SDL_RENDERER_VSYNC_ADAPTIVE) ? -1 : 0;
    if (vsync != 1) SDL_Log("vsync %s (%s)", vsync ? "adaptive only" : "unavailable, pacing frames by the clock", SDL_GetError());
    double period = frame_period(win);
    if (SDL_getenv("SABER_FAST")) vsync = 1;   /* headless test runs go as fast as they can */
    input_sdl_init();
    if (!app_init((Ren *)ren, data_dir, start_level)) return 1;
    Game *g = app_game();

    bool running = true;
    Uint64 prev = SDL_GetTicksNS();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || ev.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED) period = frame_period(win);
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT)) {
                SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
                continue;
            }
            input_sdl_event(&g->in, &ev);
            if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) game_debug_key(g, debug_key(ev.key.scancode), ev.type == SDL_EVENT_KEY_DOWN);
        }
        Uint64 now = SDL_GetTicksNS();
        if (!vsync && now - prev < (Uint64)(period * 1e9)) {   /* no vsync: hold the frame rate at the display's */
            SDL_DelayPrecise((Uint64)(period * 1e9) - (now - prev));
            now = SDL_GetTicksNS();
        }
        double elapsed = (now - prev) / 1e9;
        if (vsync && elapsed > period * 0.8 && elapsed < period * 1.2) elapsed = period;   /* one refresh: timer noise */
        int steps = app_update(elapsed);
        Uint64 t_upd = SDL_GetTicksNS();
        app_draw();
        Uint64 t_draw = SDL_GetTicksNS();
        const char *shot = app_shot_path();
        if (shot) { plat_screenshot((Ren *)ren, shot); running = false; }
        SDL_RenderPresent(ren);
        if (app_perf_on()) app_perf((uint32_t)((t_upd - now) / 1000), (uint32_t)((t_draw - t_upd) / 1000), (uint32_t)((now - prev) / 1000), steps, -1);
        prev = now;
    }
    app_shutdown();
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
