#pragma once
/* In-level dialog / cutscene text (E2DM_Hlevel_Dialog). Script format (data blobs, e.g. C3B6D081):
 *   line 1: optional sfx name (or empty / "<<>>")
 *   pages separated by "<<>>": "<|COLOR|>" text colour, "</dialog_avatar_x/>" avatar sprite name, "<r>" line break, text */
#include "platform/render.h"
#include "platform/plat.h"
#include <stdint.h>
#include <stdbool.h>
#include "input.h"

#define DLG_MAX_PAGES 16
typedef struct {
    uint32_t avatar_id; int color; char text[512];
} DialogPage;

typedef struct {
    bool active;
    DialogPage pages[DLG_MAX_PAGES]; int npages, page;
    int t;                /* page timer (DAT_00ac9bd8): >0 frames since the page opened, <0 closing countdown (-22..-1) */
    int box;              /* box frame (+0x50): 0 hidden, 1..11 growing, >=12 open with text; -12..-1 shrinking */
    float chars;          /* typewriter progress in characters (50/12 per frame, x3 while a button is held) */
    bool done;            /* all text shown, waiting for the player */
    int frame;            /* |t|, kept for the briefing video unfold (FUN_0042ba20 uses the same counter) */
    bool closing;         /* t < 0 */
    uint32_t pending_sfx; /* DAT_00ac9aa0: the script's voice sample, played by the first update (FUN_0042b250), not on open */
} Dialog;

enum { DLG_GREEN, DLG_PURPLE, DLG_RED, DLG_BLUE };
/* the hero the player picked: the level-1 scripts were written for Fireball, so with April the lines that name
 * him are rewritten for her and the two avatars swap roles (April speaks to Fireball instead of the other way round) */
void dialog_set_hero(int character);
bool dialog_open(Dialog *d, uint32_t text_id);
bool dialog_open_script(Dialog *d, const char *script);   /* the same script format from a string (our own stages) */
bool dialog_open_text(Dialog *d, const char *text, int color);   /* plain text (no script header), e.g. the mission briefing */
bool dialog_text_done(const Dialog *d);
void dialog_close(Dialog *d);                                    /* start the close animation */                          /* typewriter finished on the last page */
void dialog_update(Dialog *d, const Input *in, float dt);   /* advances pages, sets active=false when done */
void dialog_draw(const Dialog *d, Ren *r, int sw, int sh);
