#include "forest.h"

#include "assets.h"
#include "audio.h"
#include "font.h"
#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const uint8_t *p, *end; bool ok; } Rd;
static const uint8_t *take(Rd *r, size_t n)
{
    if (!r->ok || (size_t)(r->end - r->p) < n) { r->ok = false; return NULL; }
    const uint8_t *q = r->p; r->p += n; return q;
}
static uint32_t u32(Rd *r) { const uint8_t *q = take(r, 4); return q ? (uint32_t)(q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24) : 0; }
static float f32(Rd *r) { uint32_t v = u32(r); float f; memcpy(&f, &v, 4); return f; }

void forest_dispose(Forest *f)
{
    for (int i = 0; i < FOREST_MAX_LAYERS; i++) { free(f->cells[i]); f->cells[i] = NULL; }
    free(f->collision); f->collision = NULL;
}

static CBlock *load_sheet(const char *png, uint32_t id)
{
    char name[64]; snprintf(name, sizeof name, "forest/%s", png);
    const char *path = asset_path(name);
    if (!path) { fprintf(stderr, "stage4: missing %s\n", name); return NULL; }
    int w = 0, h = 0;
    uint32_t *px = png_load_rgba(path, &w, &h);
    if (!px) { fprintf(stderr, "stage4: can't decode %s\n", name); return NULL; }
    CBlock *cb = cblock_from_rgba(id, px, w, h, 16, 16);
    free(px);
    return cb;
}

bool forest_init(Forest *f, Level *L)
{
    memset(f, 0, sizeof *f);
    const char *path = asset_path("forest/forest.lvl");
    size_t size = 0;
    uint8_t *blob = path ? file_read(path, &size) : NULL;
    if (!blob) { fprintf(stderr, "stage4: assets/forest/forest.lvl not found\n"); return false; }
    Rd r = { blob, blob + size, true };
    const uint8_t *magic = take(&r, 4);
    uint32_t version = u32(&r);
    if (!magic || memcmp(magic, "FRST", 4) || version != 1) { fprintf(stderr, "stage4: bad forest.lvl\n"); free(blob); return false; }
    f->width = (float)u32(&r); f->start_x = f32(&r); f->start_y = f32(&r); f->exit_x = f32(&r);
    int nl = (int)u32(&r);
    int slot[FOREST_MAX_LAYERS]; float par[FOREST_MAX_LAYERS];
    for (int k = 0; k < nl && r.ok; k++) {
        const uint8_t *sname = take(&r, 16), *png = take(&r, 16);
        float p = f32(&r); uint32_t w = u32(&r), h = u32(&r), tile = u32(&r);
        const uint8_t *cells = take(&r, (size_t)w * h * 4);
        if (!r.ok || k >= FOREST_MAX_LAYERS || tile != 16) { r.ok = false; break; }
        char sn[17] = { 0 }, pn[17] = { 0 }; memcpy(sn, sname, 16); memcpy(pn, png, 16);
        slot[k] = -1;
        for (int i = 0; i < L->nlayers; i++) if (L->layers[i].is_tilemap && !strcmp(L->layers[i].name, sn)) slot[k] = i;
        CBlock *cb = load_sheet(pn, 0x46520000u + (uint32_t)k);
        if (slot[k] < 0 || !cb) { fprintf(stderr, "stage4: layer %s unusable\n", sn); r.ok = false; break; }
        f->cells[k] = malloc((size_t)w * h * 4);
        if (!f->cells[k]) { r.ok = false; break; }
        memcpy(f->cells[k], cells, (size_t)w * h * 4);   /* little-endian host assumed, as for the pack data */
        TileMap *m = &f->maps[k];
        m->w = m->used_w = (int)w; m->h = (int)h; m->cblock_id = cb->id; m->cells = f->cells[k]; m->cb = cb;
        par[k] = p;
    }
    uint32_t cols = u32(&r), rows = u32(&r);
    const uint8_t *coll = take(&r, (size_t)cols * rows);
    int nt = (int)u32(&r);
    for (int i = 0; i < nt && r.ok; i++) {
        LevelObject o = { 0 };
        o.type = u32(&r); o.x = f32(&r); o.spawn_x = f32(&r); o.spawn_y = 208;   /* zone: x .. x + zone_w, full height */
        uint32_t nwp = u32(&r);
        for (int k = 0; k < 3; k++) { o.wp[k][0] = f32(&r); o.wp[k][1] = f32(&r); }
        o.n_wp = (uint8_t)(nwp > 3 ? 3 : nwp);
        o.a = (uint16_t)u32(&r); o.loops = (int8_t)(int32_t)u32(&r); o.c = (uint8_t)u32(&r); o.b = (uint16_t)u32(&r);
        if (f->ntriggers < FOREST_MAX_TRIGGERS) f->triggers[f->ntriggers++] = o;
    }
    if (r.ok && coll) { f->collision = malloc((size_t)cols * rows); if (f->collision) memcpy(f->collision, coll, (size_t)cols * rows); }
    free(blob);
    if (!r.ok || !f->collision) { fprintf(stderr, "stage4: truncated forest.lvl\n"); forest_dispose(f); return false; }

    /* the shield sniper's frames (type 30, enemies.c; forest/sniper.py) */
    const char *sp = asset_path("forest/sniper.png");
    int sw_ = 0, sh_ = 0; uint32_t *spx = sp ? png_load_rgba(sp, &sw_, &sh_) : NULL;
    if (spx) { sprite_from_rgba(SHIELD_SNIPER_SPRITE, spx, sw_, sh_, sw_ / 64); free(spx); }
    else fprintf(stderr, "stage4: missing forest/sniper.png\n");

    /* everything loaded: switch the level over (the other tile layers are hidden) */
    for (int i = 0; i < L->nlayers; i++) if (L->layers[i].is_tilemap) L->layers[i].map = NULL;
    for (int k = 0; k < nl; k++) {
        Layer *ly = &L->layers[slot[k]];
        ly->map = &f->maps[k]; ly->parallax = par[k]; ly->extra = 0;
    }
    L->collision = f->collision; L->cols = (int)cols; L->rows = (int)rows; L->cellw = L->cellh = 8;
    L->width = f->width;
    return true;
}

int forest_triggers(const Forest *f, LevelObject *out, int max, int player_layer)
{
    int n = 0;
    for (int i = 0; i < f->ntriggers && n < max; i++) { out[n] = f->triggers[i]; out[n].layer = (uint32_t)player_layer; n++; }
    return n;
}

bool forest_on_deck(const Level *L, float x, float feet_y)
{
    int cx = (int)floorf(x / L->cellw);
    if (!L->collision || cx < 0 || cx >= L->cols) return false;
    for (int cy = 0; cy < L->rows; cy++) {
        uint8_t v = L->collision[cy * L->cols + cx];
        if (v & 0x10) return false;                            /* reached the ground: no deck above it here */
        if (v == 4) return feet_y <= cy * L->cellh + 1.0f && feet_y > cy * L->cellh - 64.0f;
    }
    return false;
}

/* ---- the finale ---- */
#define FINALE_ONSLAUGHT_T 24.0f

/* Story: after the Grand Prix and Hyperjumper Pass, the jungle is where the Outriders' watchtowers stand. The last
 * clearing is a trap, and what comes out of the Vapor Zone for it is a rebuilt Hyperjumper. */
/* every page fits one box (4 wrapped lines, whichever hero's name is swapped in: SABER_DLGCHECK) */
const char *const FOREST_SCRIPT_AMBUSH =
    "<|PURPLE|>\n</dialog_avatar_outrider/>\nEnd of the line, Star Sheriff! Every Outrider in the jungle is coming, and worse from the Vapor Zone!\n<<>>\n"
    "<|RED|>\n</dialog_avatar_april2/>\nFireball, they're phasing in all around you! Hold them off until Ramrod gets through the canopy!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nFirst that sniper up on the tower, then the high ground is mine. Come and get it, you phantoms!\n<<>>\n";
const char *const FOREST_SCRIPT_OUTRO =
    "<|GREEN|>\n</dialog_avatar_saber2/>\nHyperjumper won't be back this time, and that was their last watchtower. Outstanding work, Fireball.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nRemind me never to get on your bad side, pardner. Ramrod's coming down through the trees for you now.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nCommander Eagle says the frontier is safe again. The whole Cavalry Command owes you one!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nThen tell him I'll take it in time off. And this time we're REALLY going home!\n<<>>\n";
#define R_EDGE 100000.0f
#define L_EDGE -100000.0f

static int rnd(int n) { return n > 0 ? rand() % n : 0; }

static void add_stream(Enemies *E, float x0, float w, int type, float wx0, float wx1, int every_ms, int delay_ms, int layer)
{
    LevelObject o = { 0 };
    o.type = (uint32_t)type; o.x = x0; o.spawn_x = w; o.y = -40; o.spawn_y = 300; o.layer = (uint32_t)layer;
    o.wp[0][0] = wx0; o.wp[0][1] = 160; o.wp[1][0] = wx1; o.wp[1][1] = 160; o.n_wp = 2;
    o.a = (uint16_t)every_ms; o.c = 40; o.b = (uint16_t)delay_ms; o.loops = -1;
    enemies_add_trigger(E, &o);
}

static void streams(Enemies *E, const ForestFinale *F, int from, int to, bool on)
{
    for (int i = F->tr0 + from; i < F->tr0 + to && i < E->ntr; i++) E->tr[i].remaining = on ? -1 : 0;
}

static int on_field(const Enemies *E)
{
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) if (E->e[i].cls) n++;
    return n;
}

/* An Outrider materialising: its own death (vaporise) frames played backwards where it will stand, then it is
 * there. Sheet / frames of anims 0x32/0x33 by type (the art faces right). */
static void warp_begin(ForestFinale *F, Effects *fx, const Player *pl, int type, int layer)
{
    int k = 0; while (k < FINALE_WARPS && F->warp[k].on) k++;
    if (k == FINALE_WARPS) return;
    float px = pl->ch.body.x, x = 0;
    for (int tries = 0; tries < 12; tries++) {   /* somewhere on the ground clear of the hero */
        x = F->arena_x + 40 + rnd(F->sw - 80);
        if (fabsf(x - px) > 80) break;
    }
    uint32_t sheet = type == 1 ? 0x827CB6B8 : type == 5 ? 0x6338F34D : 0x9DA6D8D5;
    int first = type == 1 ? 23 : 59, last = type == 1 ? 18 : 54;
    AnimDef a = { 0, first, last, last, 0.0667f, 0 };
    Effect *ef = effects_spawn(fx, sheet, layer, &a, x, 176, 32, 32, 0);
    if (ef) ef->flip = px < x;                   /* faces the hero */
    F->warp[k].on = true; F->warp[k].x = x; F->warp[k].t = 0.40f; F->warp[k].type = type;
    sfx_play(6, 0);
}

static void warps_update(ForestFinale *F, Enemies *E, const Level *L, const PhysicsWorld *W, float cam_x, int sw, int sh, int layer, float dt)
{
    for (int k = 0; k < FINALE_WARPS; k++) {
        if (!F->warp[k].on || (F->warp[k].t -= dt) > 0) continue;
        F->warp[k].on = false;
        Trigger t = { 0 }; t.type = F->warp[k].type; t.layer = layer;
        enemy_spawn(E, &t, F->warp[k].x - 8, 208 - 52, L, W, cam_x, sw, sh);   /* body.x = x (+hx), feet on the ground */
    }
}

static bool warps_pending(const ForestFinale *F)
{
    for (int k = 0; k < FINALE_WARPS; k++) if (F->warp[k].on) return true;
    return false;
}

bool forest_finale_update(Forest *f, Enemies *E, Night *hj, Effects *fx, const Level *L, const PhysicsWorld *W,
                          const Player *pl, float cam_x, int sw, int sh, int layer, float dt)
{
    ForestFinale *F = &f->fin;
    bool alive = pl->ch.state != CS_DEAD;
    F->t += dt;
    if (F->state != FF_WAIT) warps_update(F, E, L, W, cam_x, sw, sh, layer, dt);
    switch (F->state) {
    case FF_WAIT:
        if (!alive || cam_x < L->width - sw - 0.5f) break;
        F->state = FF_ONSLAUGHT; F->t = 0; F->warp_t = 1.2f; F->scene_ambush = true;
        music_play(8, true);   /* the boss music for the whole finale (Hyperjumper's arrival keeps it going) */
        F->arena_x = L->width - sw; F->sw = sw;
        /* edge streams over the whole clearing (the spawner needs the hero in the zone: all of it, deck included) */
        F->tr0 = E->ntr;
        add_stream(E, F->arena_x, (float)sw, 2, R_EDGE, R_EDGE, 2100, 300, layer);    /* gunmen running in from the right */
        add_stream(E, F->arena_x, (float)sw, 1, L_EDGE, R_EDGE, 2500, 1400, layer);   /* chargers from both sides */
        add_stream(E, F->arena_x, (float)sw, 5, L_EDGE, R_EDGE, 3300, 2600, layer);
        F->ntr_onslaught = E->ntr - F->tr0;
        add_stream(E, F->arena_x, (float)sw, 1, R_EDGE, L_EDGE, 3400, 0, layer);      /* the boss round: thinner */
        add_stream(E, F->arena_x, (float)sw, 2, L_EDGE, R_EDGE, 4600, 0, layer);
        F->ntr = E->ntr - F->tr0;
        streams(E, F, F->ntr_onslaught, F->ntr, false);
        if (SDL_getenv("SABER_TRACE")) fprintf(stderr, "stage4: finale, arena %.0f..%.0f\n", F->arena_x, F->arena_x + sw);
        break;
    case FF_ONSLAUGHT:
    case FF_BOSS: {
        bool boss = F->state == FF_BOSS;
        if (F->t >= 0.9f) E->shield_wake = true;   /* the tower sniper opens up once the radio scene is over */
        int cap = boss ? 5 : 8;                    /* bodies on the field at once (the tower gunman and the snipers count) */
        E->spawner_enabled = on_field(E) < cap;
        if ((F->warp_t -= dt) <= 0 && on_field(E) < cap && alive) {
            static const int TYPES[4] = { 2, 1, 5, 2 };
            warp_begin(F, fx, pl, TYPES[rnd(4)], layer);
            F->warp_t = boss ? 4.5f + rnd(40) * 0.05f : 2.2f + rnd(30) * 0.05f;
        }
        if (!boss && F->t >= FINALE_ONSLAUGHT_T) {
            F->state = FF_BOSS; F->t = 0;
            streams(E, F, 0, F->ntr_onslaught, false); streams(E, F, F->ntr_onslaught, F->ntr, true);
            night_boss_summon(hj, F->arena_x, sw);
        }
        if (boss && hj->state >= HJ_DYING) {         /* it's going down: nobody new comes */
            F->state = FF_MOPUP; F->t = 0;
            streams(E, F, 0, F->ntr, false);
            E->spawner_enabled = true;               /* the level's own spots, if any are left, still stand */
        }
        break; }
    case FF_MOPUP:
        /* the wreck has burnt out and the field is clear (a warp-in already under way still arrives) */
        if (hj->clear_ready && !warps_pending(F) && on_field(E) == 0 && alive && F->t > 0.5f) {
            F->state = FF_WON;
            if (SDL_getenv("SABER_TRACE")) fprintf(stderr, "stage4: finale won\n");
        }
        break;
    case FF_WON:
        break;
    }
    return F->state == FF_WON;
}

void forest_finale_draw_hud(const Forest *f, const Enemies *E, const Night *hj, SDL_Renderer *ren, int sw)
{
    (void)ren;
    const ForestFinale *F = &f->fin;
    if (!(F->state == FF_MOPUP && hj->state == HJ_DONE)) return;   /* no timer in the ambush; Hyperjumper has its own bar */
    Font *fo = font_get(0x12072E60);
    if (!fo) return;
    int n = 0; for (int i = 0; i < MAX_ENEMIES; i++) if (E->e[i].cls && !E->e[i].dying) n++;   /* mopping up: how many are still standing */
    char buf[32]; snprintf(buf, sizeof buf, n ? "OUTRIDERS LEFT %d" : "AREA CLEAR", n);
    font_draw(fo, buf, sw - 10 - font_text_width(fo, buf), 8, 220, 226, 245);
}
