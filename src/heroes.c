#include "heroes.h"
#include "assets.h"
#include "gfx.h"
#include "audio.h"
#include "platform/render.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CRHC_APRIL 0x79260A58
#define CRHC_FIREBALL 0x9C8F9A9E
#define CRHC_SABER_TAG 0x53414245   /* 'SABE': arbitrary cblock cache id, unrelated to CRHC_DEFAULT below */
#define CRHC_COLT_TAG  0x434F4C54   /* 'COLT': likewise (Colt's CRHC points at A332AB60, a copy of Fireball's cblock) */

static CBlock *april_sheet(void)
{
    static CBlock *cb; static bool tried;
    if (tried) return cb;
    tried = true;
    const char *path = asset_path("april.png");
    if (!path) { fprintf(stderr, "assets/april.png not found: April uses Fireball's sheet\n"); return NULL; }
    cb = cblock_from_png(CRHC_APRIL, path, 64, 64);   /* evictable: the select screen no longer holds every sheet */
    return cb;
}

/* Saber's CRHC (0x8403195A, the game's "default" player id) ships Fireball's animation table. His sheet fills
 * its original 8x19 layout plus breathing / salute cells (tools/build_saber_idle.py);
 * hero_patch_def selects his idle cycle and separate falling aim torsos. */
static CBlock *saber_sheet(void)
{
    static CBlock *cb; static bool tried;
    if (tried) return cb;
    tried = true;
    const char *path = asset_path("saber.png");
    if (!path) { fprintf(stderr, "assets/saber.png not found: Saber Rider uses Fireball's sheet\n"); return NULL; }
    cb = cblock_from_png(CRHC_SABER_TAG, path, 64, 64);   /* evictable: the select screen no longer holds every sheet */
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
    cb = cblock_from_png(CRHC_COLT_TAG, path, 64, 64);   /* evictable: the select screen no longer holds every sheet */
    return cb;
}

bool hero_available(int character)
{
    if (character == HERO_FIREBALL) return true;
    /* only whether the sheet ships: decoding all three here (the select screen asks every frame) filled the
     * Dreamcast's texture memory before the level had loaded */
    static const char *const sheet[4] = { "saber.png", NULL, "april.png", "colt.png" };
    static int8_t ships[4];   /* 0 = not looked yet, 1 = yes, -1 = no */
    if ((unsigned)character >= 4 || !sheet[character]) return false;
    if (!ships[character]) ships[character] = asset_path(sheet[character]) ? 1 : -1;
    return ships[character] > 0;
}

const char *hero_name(int character)
{
    static const char *const names[4] = { "Saber Rider", "Fireball", "April", "Colt" };
    return (unsigned)character < 4 ? names[character] : "Fireball";
}

/* animation table changes: April's clip has a 6-frame idle sway followed by a 20-frame crouch + jumping jacks + wave
 * (10 fps; played once after 10 s of idling), 6 run frames (legs + torso overlay cells, both from the clip) and 8
 * death frames.  The "alert" pose (anims 4/7, held for
 * alert_time after shooting / landing) is the sway itself: her sheet has no clean alert art. */
typedef struct { int anim, first, last, loop; real frame_time; } AnimPatch;
#define APRIL_BORED_L 53
#define APRIL_BORED_R 54
/* Slots are per character definition, so Saber can share April's bored slots. */
#define SABER_BORED_L 53
#define SABER_BORED_R 54
static const AnimPatch SABER_ANIMS[] = {
    { 1, 152, 163, 152, R(0.10f) }, { 2, 168, 179, 168, R(0.10f) },
    { 36, 80, 85, 80, R(0.10f) }, { 37, 104, 109, 104, R(0.10f) },
    { 38, 64, 69, 64, R(0.10f) }, { 39, 88, 93, 88, R(0.10f) },
    { SABER_BORED_L, 184, 196, 196, R(0.10f) },
    { SABER_BORED_R, 200, 212, 212, R(0.10f) },
};
static const AnimPatch APRIL_ANIMS[] = {
    { 1, 168, 173, 168, R(0.10f) }, { 2, 176, 181, 176, R(0.10f) },     /* idle L / R */
    { 4, 168, 173, 168, R(0.10f) }, { 7, 176, 181, 176, R(0.10f) },     /* alert L / R = idle */
    { APRIL_BORED_L, 184, 204, 204, R(0.10f) }, { APRIL_BORED_R, 208, 228, 228, R(0.10f) },   /* bored jump + wave L / R */
    /* The seventh recovered clip cell repeats the sixth pose with codec noise. Including it holds that stride
     * twice before wrapping; the actual cycle is the first six poses. */
    { 36, 80, 85, 80, R(0.10f) }, { 37, 104, 109, 104, R(0.10f) },      /* run legs L / R */
    { 38, 64, 69, 64, R(0.10f) }, { 39, 88, 93, 88, R(0.10f) },         /* matching run torso overlays */
    { 50, 152, 159, 159, R(0.10f) }, { 51, 160, 167, 167, R(0.10f) },   /* death L / R */
    { 52, 168, 168, 168, R(4.0f) },
};

/* April's grunts (../heroes/voice/generate.py) on the events Fireball's table 6 / 3-5 / 1-2 / 23 samples cover */
static void april_sfx(void)
{
    static const struct { int id; const char *files[4]; } G[] = {
        { 2,  { "voice/april_jump.wav" } },
        { 3,  { "voice/april_hurt1.wav", "voice/april_hurt2.wav", "voice/april_hurt3.wav", "voice/april_huh_anime_hurt.wav" } },
        { 4,  { "voice/april_death1.wav", "voice/april_death2.wav" } },
        { 15, { "voice/april_fall.wav" } },
    };
    for (size_t i = 0; i < sizeof G / sizeof *G; i++) {
        const char *paths[4]; int n = 0;
        for (int k = 0; k < 4 && G[i].files[k]; k++) { const char *p = asset_path(G[i].files[k]); if (p) paths[n++] = strdup(p); }
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

/* Colt's series-dialogue clone uses Fireball's same jump/hurt/death/fall events. */
static void colt_sfx(void)
{
    static const struct { int id; const char *files[3]; } G[] = {
        { 2,  { "voice/colt_jump.wav" } },
        { 3,  { "voice/colt_hurt1.wav", "voice/colt_hurt2.wav", "voice/colt_hurt3.wav" } },
        { 4,  { "voice/colt_death1.wav", "voice/colt_death2.wav" } },
        { 15, { "voice/colt_fall.wav" } },
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

#define CRHC_COLT     0x26818B85
#define CRHC_DEFAULT  0x8403195A

static bool g_quiet;
void hero_quiet(bool quiet) { g_quiet = quiet; }

void hero_patch_def(CharDef *d)
{
    if (d->crhc_id == CRHC_DEFAULT) {
        /* Standing still must breathe immediately, including after landing or
         * releasing input. Anims 4/7 remain gun-ready for explicit CS_AIM. */
        d->alert_time = 0;
        for (size_t i = 0; i < sizeof SABER_ANIMS / sizeof *SABER_ANIMS; i++) {
            const AnimPatch *p = &SABER_ANIMS[i];
            AnimDef *a = &d->anims[p->anim];
            a->first = p->first; a->last = p->last; a->loop = p->loop; a->frame_time = p->frame_time;
        }
        d->hurt[SABER_BORED_L] = d->hurt[1]; d->hurt[SABER_BORED_R] = d->hurt[2];
    }
    if (d->crhc_id == CRHC_APRIL || d->crhc_id == CRHC_DEFAULT) {
        /* Falling needs the aim torso that fits the fall legs, rather than the first running torso. Fireball's
         * original art reuses that run cell, but the reconstructed sheets have different hip positions. */
        d->anims[20].first = d->anims[20].last = d->anims[20].loop = 72;
        d->anims[23].first = d->anims[23].last = d->anims[23].loop = 96;
    }
    if (d->crhc_id != CRHC_APRIL) return;
    for (size_t i = 0; i < sizeof APRIL_ANIMS / sizeof *APRIL_ANIMS; i++) {
        const AnimPatch *p = &APRIL_ANIMS[i];
        AnimDef *a = &d->anims[p->anim];
        a->first = p->first; a->last = p->last; a->loop = p->loop; a->frame_time = p->frame_time;
    }
    d->hurt[APRIL_BORED_L] = d->hurt[1]; d->hurt[APRIL_BORED_R] = d->hurt[2];
}

bool hero_apply(Character *c)
{
    if (c->crhc_id != CRHC_APRIL && c->crhc_id != CRHC_FIREBALL && c->crhc_id != CRHC_COLT && c->crhc_id != CRHC_DEFAULT) return false;   /* enemies */
    /* The full stride's leg span, measured across the six sheet cells: use one center per direction so the
     * shadow follows the artwork without wobbling between poses. The stride width sets the footprint for every
     * pose, including standing and crouching. Fireball and Colt share the same leg art. */
    c->run_shadow_x[0] = R(9.5f); c->run_shadow_x[1] = R(-3.5f); c->shadow_half = R(17.5f);
    c->idle_shadow_x[0] = c->idle_shadow_x[1] = c->box_ox;
    c->jump_shadow_x[0] = c->jump_shadow_x[1] = c->box_ox;
    c->fall_shadow_x[0] = c->fall_shadow_x[1] = c->box_ox;
    c->crouch_shadow_x[0] = c->crouch_shadow_x[1] = c->box_ox;
    if (!g_quiet) sfx_clear_overrides();   /* the player is re-created on every level start; Fireball keeps the pack's samples */
    if (c->crhc_id == CRHC_DEFAULT) {
        if (!g_quiet) saber_sfx();
        CBlock *cb = saber_sheet();
        if (!cb) return false;
        c->cb = cb; c->spr = NULL;   /* inherited layout, with falling torso cells selected by hero_patch_def */
        /* Native clip run (tools/build_saber_run.py): each torso belongs to its
         * own legs frame. The extractor removes the 0/1/2 px bob from torsos;
         * character_draw restores it for both native and diagonal aim torsos. */
        static const int8_t saber_bob[8] = { 0, 1, 2, 0, 1, 2 };
        memcpy(c->torso_bob, saber_bob, sizeof c->torso_bob);
        c->ov_sync = true;
        c->walk_aim_ov = WALK_AIM_DIAG;   /* the native run already holds his gun level, including while firing */
        /* The fall pelvis sits 3 px toward the back from the torso's belt.
         * Its top is row 39, so the belt's row 37 must move down one pixel. */
        c->fall_torso_x[0] = R(3); c->fall_torso_x[1] = R(-3);
        c->fall_torso_y[0] = c->fall_torso_y[1] = R(1);
        c->fall_vertical_y[0] = c->fall_vertical_y[1] = R(4);
        c->run_shadow_x[0] = R(5); c->run_shadow_x[1] = R(-5); c->shadow_half = R(21);
        /* Idle feet occupy columns [17,45) / [19,47), with the sprite origin at 32. */
        c->idle_shadow_x[0] = R(-1); c->idle_shadow_x[1] = R(1);
        /* Crouch feet: [24,44) left, [21,43) right; origin column 32. */
        c->crouch_shadow_x[0] = R(2); c->crouch_shadow_x[1] = 0;
        c->bored_anim[0] = SABER_BORED_L; c->bored_anim[1] = SABER_BORED_R; c->bored_time = R(10.0f);
        if (plat_getenv("SABER_BORED")) c->bored_time = r_parse(plat_getenv("SABER_BORED"), NULL);
        /* Jump-cycle union: [16,47) / [15,47); falling legs: [24,40) in both directions. */
        c->jump_shadow_x[0] = R(-0.5f); c->jump_shadow_x[1] = R(-1);
        c->fall_shadow_x[0] = c->fall_shadow_x[1] = 0;
        return true;
    }
    if (c->crhc_id == CRHC_COLT) {
        if (!g_quiet) colt_sfx();
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
    /* Belt ends at row 39; fall pelvis starts at row 42 L / 41 R.
     * The up/down torso's bottom is row 34 after UPBASE/DOWNBASE. */
    c->fall_torso_y[0] = R(3); c->fall_torso_y[1] = R(2);
    c->fall_vertical_y[0] = R(7); c->fall_vertical_y[1] = R(6);
    c->run_shadow_x[0] = R(5); c->run_shadow_x[1] = R(-4.5f); c->shadow_half = R(19);
    /* Both idle sways (also used for alert) plant the feet in columns [19,45), centered at the origin. */
    c->idle_shadow_x[0] = c->idle_shadow_x[1] = 0;
    /* Crouch feet: [24,44) left, [22,40) right; origin column 32. */
    c->crouch_shadow_x[0] = R(2); c->crouch_shadow_x[1] = R(-1);
    /* Jump-cycle union: [17,48) / [16,47); falling legs: [25,39) / [25,40).
     * Measure the legs, so aiming a gun does not pull the airborne shadow sideways. */
    c->jump_shadow_x[0] = R(0.5f); c->jump_shadow_x[1] = R(-0.5f);
    c->fall_shadow_x[0] = 0; c->fall_shadow_x[1] = R(0.5f);
    memset(c->torso_bob, 0, sizeof c->torso_bob);   /* Fireball's run legs bob 1 px on cells 2 and 5; April's do not */
    c->ov_sync = true;         /* run torso frame k belongs on run legs frame k */
    c->walk_aim_ov = WALK_AIM_DIAG;   /* the run torso (clip pixels, gun held level) stays for level shots; up / down diagonals use the sheet's aim torsos over the clip legs */
    c->bored_anim[0] = APRIL_BORED_L; c->bored_anim[1] = APRIL_BORED_R; c->bored_time = R(10.0f);
    if (plat_getenv("SABER_BORED")) c->bored_time = r_parse(plat_getenv("SABER_BORED"), NULL);   /* debug */
    return true;
}

void hero_align_muzzle(Character *c)
{
    if (c->crhc_id != CRHC_DEFAULT && c->crhc_id != CRHC_APRIL) return;
    if (!(c->aim & 1) && c->aim != AIM_D) return;
    /* Barrel-tip pixels in assets/saber.png and assets/april.png. Standing
     * cells and split torsos differ, as do the aim and recoil frames. These
     * are coordinates in the 64x64 cell, before its draw offset and run bob. */
    static const struct { uint8_t cell, saber_x, saber_y, april_x, april_y; } tips[] = {
        {18, 8,42, 13,46}, {19, 8,38, 14,44},
        {20,12, 8, 15,13}, {21,17,12, 18,13},
        {42,54,42, 46,47}, {43,52,39, 45,45},
        {44,46,11, 44,13}, {45,44,12, 40,14},
        /* Hand-reconstructed straight-down torsos (tools/replace_down_aim.py). */
        {46,32,57, 32,47}, {47,32,57, 32,47},
        {74, 9,41, 13,46}, {75,11,39, 14,44},
        {76,16,14, 15,13}, {77,18,12, 18,13},
        {98,54,41, 47,46}, {99,52,39, 46,44},
        {100,47,14,44,13}, {101,45,12,40,14},
    };
    int frame = c->overlay ? c->ov_frame : c->frame;
    for (size_t i = 0; i < sizeof tips / sizeof *tips; i++) {
        if (tips[i].cell != frame) continue;
        bool saber = c->crhc_id == CRHC_DEFAULT;
        c->muzzle_x = r_int(saber ? tips[i].saber_x : tips[i].april_x) - c->origin_x;
        c->muzzle_y = r_int(saber ? tips[i].saber_y : tips[i].april_y) - c->origin_y;
        if (c->overlay) {
            c->muzzle_x += c->base_ox; c->muzzle_y += c->base_oy;
            int k = c->frame - c->def->anims[c->anim].first;
            if (c->walk_bob && k >= 0 && k < 8) c->muzzle_y += r_int(c->torso_bob[k]);
        }
        return;
    }
}
