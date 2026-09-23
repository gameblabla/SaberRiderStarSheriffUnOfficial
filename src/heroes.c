#include "heroes.h"
#include "assets.h"
#include "gfx.h"
#include "audio.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CRHC_APRIL 0x79260A58
#define CRHC_SABER_TAG 0x53414245   /* 'SABE': arbitrary cblock cache id, unrelated to CRHC_DEFAULT below */
#define CRHC_COLT_TAG  0x434F4C54   /* 'COLT': likewise (Colt's CRHC points at A332AB60, a copy of Fireball's cblock) */

static CBlock *april_sheet(void)
{
    static CBlock *cb; static bool tried;
    if (tried) return cb;
    tried = true;
    const char *path = asset_path("april.png");
    if (!path) { fprintf(stderr, "assets/april.png not found: April uses Fireball's sheet\n"); return NULL; }
    int w, h; uint32_t *px = png_load_rgba(path, &w, &h);
    if (!px) return NULL;
    cb = cblock_from_rgba(CRHC_APRIL, px, w, h, 64, 64);
    free(px);
    return cb;
}

/* Saber's CRHC (0x8403195A, the game's "default" player id) already ships an animation table byte-identical to
 * Fireball's own (see hero_apply below), so his sheet only has to fill Fireball's fixed 8x19 layout - no
 * AnimPatch table, unlike April (../heroes/build_saber_engine_sheet.py). */
static CBlock *saber_sheet(void)
{
    static CBlock *cb; static bool tried;
    if (tried) return cb;
    tried = true;
    const char *path = asset_path("saber.png");
    if (!path) { fprintf(stderr, "assets/saber.png not found: Saber Rider uses Fireball's sheet\n"); return NULL; }
    int w, h; uint32_t *px = png_load_rgba(path, &w, &h);
    if (!px) return NULL;
    cb = cblock_from_rgba(CRHC_SABER_TAG, px, w, h, 64, 64);
    free(px);
    return cb;
}

/* Colt: a re-skin of Fireball's cblock (../heroes/build_colt_engine_sheet.py) - his CRHC (26818B85) already carries
 * Fireball's table and points at a byte-identical copy of Fireball's sheet.  The run is Fireball's too (recoloured):
 * his native clip run had a different stride and opened a hip gap under Fireball's aim torsos. */
static CBlock *colt_sheet(void)
{
    static CBlock *cb; static bool tried;
    if (tried) return cb;
    tried = true;
    const char *path = asset_path("colt.png");
    if (!path) { fprintf(stderr, "assets/colt.png not found: Colt uses Fireball's sheet\n"); return NULL; }
    int w, h; uint32_t *px = png_load_rgba(path, &w, &h);
    if (!px) return NULL;
    cb = cblock_from_rgba(CRHC_COLT_TAG, px, w, h, 64, 64);
    free(px);
    return cb;
}

bool hero_available(int character)
{
    if (character == HERO_FIREBALL) return true;
    if (character == HERO_APRIL) return april_sheet() != NULL;
    if (character == HERO_SABER) return saber_sheet() != NULL;
    if (character == HERO_COLT) return colt_sheet() != NULL;
    return false;
}

const char *hero_name(int character)
{
    static const char *const names[4] = { "Saber Rider", "Fireball", "April", "Colt" };
    return (unsigned)character < 4 ? names[character] : "Fireball";
}

/* animation table changes: April's clip has a 6-frame idle sway followed by a 20-frame crouch + jumping jacks + wave
 * (10 fps; played once after 10 s of idling), 7 run frames (legs + torso overlay cells, both from the clip) and 8
 * death frames.  The "alert" pose (anims 4/7, held for
 * alert_time after shooting / landing) is the sway itself: her sheet has no clean alert art. */
typedef struct { int anim, first, last, loop; float frame_time; } AnimPatch;
#define APRIL_BORED_L 53
#define APRIL_BORED_R 54
static const AnimPatch APRIL_ANIMS[] = {
    { 1, 168, 173, 168, 0.10f }, { 2, 176, 181, 176, 0.10f },     /* idle L / R */
    { 4, 168, 173, 168, 0.10f }, { 7, 176, 181, 176, 0.10f },     /* alert L / R = idle */
    { APRIL_BORED_L, 184, 204, 204, 0.10f }, { APRIL_BORED_R, 208, 228, 228, 0.10f },   /* bored jump + wave L / R */
    { 36, 80, 86, 80, 0.10f }, { 37, 104, 110, 104, 0.10f },      /* run legs L / R */
    { 38, 64, 70, 64, 0.10f }, { 39, 88, 94, 88, 0.10f },         /* run torso overlays L / R (the clip frames' upper half + ponytail) */
    { 50, 152, 159, 159, 0.10f }, { 51, 160, 167, 167, 0.10f },   /* death L / R */
    { 52, 168, 168, 168, 4.0f },
};

/* April's grunts (../heroes/voice/generate.py) on the events Fireball's table 6 / 3-5 / 1-2 / 23 samples cover */
static void april_sfx(void)
{
    static const struct { int id; const char *files[3]; } G[] = {
        { 2,  { "voice/april_jump.wav" } },
        { 3,  { "voice/april_hurt1.wav", "voice/april_hurt2.wav", "voice/april_hurt3.wav" } },
        { 4,  { "voice/april_death1.wav", "voice/april_death2.wav" } },
        { 15, { "voice/april_fall.wav" } },
    };
    for (size_t i = 0; i < sizeof G / sizeof *G; i++) {
        const char *paths[3]; int n = 0;
        for (int k = 0; k < 3 && G[i].files[k]; k++) { const char *p = asset_path(G[i].files[k]); if (p) paths[n++] = strdup(p); }
        if (n) sfx_set_override(G[i].id, paths, n);
        for (int k = 0; k < n; k++) free((char *)paths[k]);
    }
}

/* Saber's grunts (../heroes/voice/generate.py --char saber), same events as April's / Fireball's */
static void saber_sfx(void)
{
    static const struct { int id; const char *files[3]; } G[] = {
        { 2,  { "voice/saber_jump.wav" } },
        { 3,  { "voice/saber_hurt1.wav", "voice/saber_hurt2.wav", "voice/saber_hurt3.wav" } },
        { 4,  { "voice/saber_death1.wav", "voice/saber_death2.wav" } },
        { 15, { "voice/saber_fall.wav" } },
    };
    for (size_t i = 0; i < sizeof G / sizeof *G; i++) {
        const char *paths[3]; int n = 0;
        for (int k = 0; k < 3 && G[i].files[k]; k++) { const char *p = asset_path(G[i].files[k]); if (p) paths[n++] = strdup(p); }
        if (n) sfx_set_override(G[i].id, paths, n);
        for (int k = 0; k < n; k++) free((char *)paths[k]);
    }
}

void hero_select_sfx(int character)
{
    /* the whole team's "OK!" (game sfx 8, the sample the briefing hands over with) whoever was picked: the demo's
     * select only had Fireball's own line (sfx 11) and the cloned April / Saber lines sounded out of place next to it */
    (void)character;
    sfx_play(8, 0);
}

#define CRHC_FIREBALL 0x9C8F9A9E
#define CRHC_COLT     0x26818B85
#define CRHC_DEFAULT  0x8403195A

static bool g_quiet;
void hero_quiet(bool quiet) { g_quiet = quiet; }

bool hero_apply(Character *c)
{
    if (c->crhc_id != CRHC_APRIL && c->crhc_id != CRHC_FIREBALL && c->crhc_id != CRHC_COLT && c->crhc_id != CRHC_DEFAULT) return false;   /* enemies */
    if (!g_quiet) sfx_clear_overrides();   /* the player is re-created on every level start; Fireball keeps the pack's samples */
    if (c->crhc_id == CRHC_DEFAULT) {
        if (!g_quiet) saber_sfx();
        CBlock *cb = saber_sheet();
        if (!cb) return false;
        c->cb = cb; c->spr = NULL;   /* table is already Fireball's own layout: no anim/hurtbox patches needed */
        /* but the run art is not Fireball's: the master's rows 10 / 11 are one figure cut at the hip per frame
         * (like April's clip halves), so torso frame k only fits on legs frame k; the figure bobs 0/2/4 px through
         * the stride (heroes/build_saber_engine_sheet.py prints it), which the packer takes out of the torso cells
         * so every torso overlay - run or aim / shot - follows the hip from here */
        static const int8_t saber_bob[8] = { 0, 2, 4, 0, 2, 4 };
        memcpy(c->torso_bob, saber_bob, sizeof c->torso_bob);
        c->ov_sync = true;
        return true;
    }
    if (c->crhc_id == CRHC_COLT) {
        CBlock *cb = colt_sheet();
        if (!cb) return false;
        c->cb = cb; c->spr = NULL;   /* every cell incl. the run is Fireball's art recoloured: table, bob and overlays stay Fireball's */
        return true;
    }
    if (c->crhc_id != CRHC_APRIL) return false;
    if (!g_quiet) april_sfx();
    CBlock *cb = april_sheet();
    if (!cb) return false;
    c->cb = cb; c->spr = NULL;
    memset(c->torso_bob, 0, sizeof c->torso_bob);   /* Fireball's run legs bob 1 px on cells 2 and 5; April's do not */
    c->ov_sync = true;         /* run torso frame k belongs on run legs frame k */
    c->walk_aim_ov = WALK_AIM_DIAG;   /* the run torso (clip pixels, gun held level) stays for level shots; up / down diagonals use the sheet's aim torsos over the clip legs */
    for (size_t i = 0; i < sizeof APRIL_ANIMS / sizeof *APRIL_ANIMS; i++) {
        const AnimPatch *p = &APRIL_ANIMS[i];
        AnimDef *a = &c->anims[p->anim];
        a->first = p->first; a->last = p->last; a->loop = p->loop; a->frame_time = p->frame_time;
    }
    c->hurt[APRIL_BORED_L] = c->hurt[1]; c->hurt[APRIL_BORED_R] = c->hurt[2];
    c->bored_anim[0] = APRIL_BORED_L; c->bored_anim[1] = APRIL_BORED_R; c->bored_time = 10.0f;
    if (SDL_getenv("SABER_BORED")) c->bored_time = (float)atof(SDL_getenv("SABER_BORED"));   /* debug */
    return true;
}
