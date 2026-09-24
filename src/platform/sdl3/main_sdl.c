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
    if (!SDL_CreateWindowAndRenderer("Saber Rider and the Star Sheriffs", ww, wh, SDL_WINDOW_RESIZABLE, &win, &ren)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1;
    }
    SDL_SetRenderLogicalPresentation(ren, APP_SCREEN_W, APP_SCREEN_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    SDL_SetRenderVSync(ren, 1);
    input_sdl_init();
    if (!app_init((Ren *)ren, data_dir, start_level)) return 1;
    Game *g = app_game();

    bool running = true;
    Uint64 prev = SDL_GetTicksNS();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT)) {
                SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
                continue;
            }
            input_sdl_event(&g->in, &ev);
            if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) game_debug_key(g, debug_key(ev.key.scancode), ev.type == SDL_EVENT_KEY_DOWN);
        }
        Uint64 now = SDL_GetTicksNS();
        app_update((now - prev) / 1e9); prev = now;
        app_draw();
        const char *shot = app_shot_path();
        if (shot) { plat_screenshot((Ren *)ren, shot); running = false; }
        SDL_RenderPresent(ren);
    }
    app_shutdown();
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
