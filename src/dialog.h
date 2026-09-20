#pragma once
/* In-level dialog / cutscene text (E2DM_Hlevel_Dialog). Script format (data blobs, e.g. C3B6D081):
 *   line 1: optional sfx name (or empty / "<<>>")
 *   pages separated by "<<>>": "<|COLOR|>" text colour, "</dialog_avatar_x/>" avatar sprite name, "<r>" line break, text */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdbool.h>
#include "input.h"

#define DLG_MAX_PAGES 8
typedef struct {
    uint32_t avatar_id; int color; char text[512];
} DialogPage;

typedef struct {
    bool active;
    DialogPage pages[DLG_MAX_PAGES]; int npages, page;
    float t, chars;       /* typewriter */
    int frame;            /* open animation frames */
    bool closing;
} Dialog;

enum { DLG_GREEN, DLG_PURPLE, DLG_RED, DLG_BLUE };
bool dialog_open(Dialog *d, uint32_t text_id);
bool dialog_open_text(Dialog *d, const char *text, int color);   /* plain text (no script header), e.g. the mission briefing */
bool dialog_text_done(const Dialog *d);
void dialog_close(Dialog *d);                                    /* start the close animation */                          /* typewriter finished on the last page */
void dialog_update(Dialog *d, const Input *in, float dt);   /* advances pages, sets active=false when done */
void dialog_draw(const Dialog *d, SDL_Renderer *r, int sw, int sh);
