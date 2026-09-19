/* Saber Rider and the Star Sheriffs — demo reconstruction (C11 + SDL3).
 * Reads the original demo data packs (.pck) directly. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pack.h"
#include "gfx.h"
#include "level.h"
#include "game.h"
#include "audio.h"

#define SCREEN_W 426
#define SCREEN_H 240

int main(int argc, char **argv)
{
    const char *data_dir = argc > 1 ? argv[1] : "SaberRider/data";
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win; SDL_Renderer *ren;
    if (!SDL_CreateWindowAndRenderer("Saber Rider and the Star Sheriffs", SCREEN_W * 3, SCREEN_H * 3, SDL_WINDOW_RESIZABLE, &win, &ren)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1;
    }
    SDL_SetRenderLogicalPresentation(ren, SCREEN_W, SCREEN_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    SDL_SetRenderVSync(ren, 1);
    gfx_init(ren);

    static const char *const base[] = { "pack.pck", "common.pck", "levels.pck", "menu.pck", "level1.pck" };
    if (!packs_open(data_dir, base, 5)) return 1;

    audio_init();
    Game g;
    if (!game_init(&g, ren, SCREEN_W, SCREEN_H)) return 1;

    /* debug: SABER_SCRIPT="60:R,20:RJ,40:" drives the input for N fixed steps each (L R U D J S A P) */
    const char *script = SDL_getenv("SABER_SCRIPT");
    int script_n = 0; char script_keys[16] = "";
    bool running = true;
    Uint64 prev = SDL_GetTicksNS();
    double acc = 0;
    const double step = 1.0 / 60.0;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            game_event(&g, &ev);
        }
        Uint64 now = SDL_GetTicksNS();
        acc += (now - prev) / 1e9; prev = now;
        if (acc > 0.25) acc = 0.25;
        while (acc >= step) {
            if (script) {
                if (script_n == 0 && *script) {
                    int used = 0; sscanf(script, "%d:%15[A-Z]%n", &script_n, script_keys, &used);
                    if (used == 0) { sscanf(script, "%d:%n", &script_n, &used); script_keys[0] = 0; }
                    script += used; if (*script == ',') script++;
                    if (script_n <= 0) script_n = 1;
                }
                if (script_n > 0) {
                    script_n--;
                    memset(g.in.raw, 0, sizeof g.in.raw);
                    for (const char *k = script_keys; *k; k++) switch (*k) {
                        case 'L': g.in.raw[BTN_LEFT] = true; break;  case 'R': g.in.raw[BTN_RIGHT] = true; break;
                        case 'U': g.in.raw[BTN_UP] = true; break;    case 'D': g.in.raw[BTN_DOWN] = true; break;
                        case 'J': g.in.raw[BTN_JUMP] = true; break;  case 'S': g.in.raw[BTN_SHOOT] = true; break;
                        case 'A': g.in.raw[BTN_AIM] = true; break;   case 'P': g.in.raw[BTN_PAUSE] = true; break; }
                }
            }
            game_update(&g, (float)step); audio_update(); acc -= step;
        }
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        game_draw(&g);
        /* debug: SABER_SHOT=path,camx,frames -> save a screenshot after N frames and quit */
        static int shot_frames = -1; static char shot_path[256];
        if (shot_frames < 0) {
            const char *s = SDL_getenv("SABER_SHOT");
            shot_frames = 0;
            if (s) { float cx = -1; int n = 1; sscanf(s, "%255[^,],%f,%d", shot_path, &cx, &n); if (cx >= 0) g.cam_x = cx; shot_frames = n; }
        }
        if (shot_frames > 0 && --shot_frames == 0) {
            SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
            if (sf) { SDL_SaveBMP(sf, shot_path); SDL_DestroySurface(sf); }
            running = false;
        }
        SDL_RenderPresent(ren);
    }
    audio_shutdown();
    packs_close();
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
