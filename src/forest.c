#include "forest.h"

#include "assets.h"
#include "audio.h"
#include "font.h"
#include "gfx.h"
#include "pack.h"

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
static real f32(Rd *r) { return r_bits(u32(r)); }

void forest_dispose(Forest *f)
{
    for (int i = 0; i < FOREST_MAX_LAYERS; i++) { free(f->cells[i]); f->cells[i] = NULL; }
    free(f->collision); f->collision = NULL;
}

static CBlock *load_sheet(const char *dir, const char *png, uint32_t id)
{
    char name[64]; snprintf(name, sizeof name, "%s/%s", dir, png);
    const char *path = asset_path(name);
    if (!path) { fprintf(stderr, "%s: missing %s\n", dir, name); return NULL; }
    CBlock *cb = cblock_from_png(id, path, 16, 16);
    if (!cb) fprintf(stderr, "%s: can't decode %s\n", dir, name);
    return cb;
}

bool forest_init(Forest *f, Level *L) { return forest_load(f, L, "forest", "forest.lvl", 0x46520000u); }

bool forest_load(Forest *f, Level *L, const char *dir, const char *file, uint32_t sheet_ids)
{
    memset(f, 0, sizeof *f);
    /* level 1's tile layers give way to ours: their textures go first, so ours get the memory and palette entries */
    for (int i = 0; i < L->nlayers; i++) if (L->layers[i].is_tilemap && L->layers[i].map) cblock_unload(L->layers[i].map->cb);
    char lname[64]; snprintf(lname, sizeof lname, "%s/%s", dir, file);
    const char *path = asset_path(lname);
    size_t size = 0;
    uint8_t *blob = path ? file_read(path, &size) : NULL;
    if (!blob) { fprintf(stderr, "%s: assets/%s not found\n", dir, lname); return false; }
    Rd r = { blob, blob + size, true };
    const uint8_t *magic = take(&r, 4);
    uint32_t version = u32(&r);
    if (!magic || memcmp(magic, "FRST", 4) || version != 1) { fprintf(stderr, "%s: bad %s\n", dir, file); free(blob); return false; }
    f->width = r_int((int)u32(&r)); f->start_x = f32(&r); f->start_y = f32(&r); f->exit_x = f32(&r);
    int nl = (int)u32(&r);
    int slot[FOREST_MAX_LAYERS]; real par[FOREST_MAX_LAYERS];
    for (int k = 0; k < nl && r.ok; k++) {
        const uint8_t *sname = take(&r, 16), *png = take(&r, 16);
        real p = f32(&r); uint32_t w = u32(&r), h = u32(&r), tile = u32(&r);
        const uint8_t *cells = take(&r, (size_t)w * h * 4);
        if (!r.ok || k >= FOREST_MAX_LAYERS || tile != 16) { r.ok = false; break; }
        char sn[17] = { 0 }, pn[17] = { 0 }; memcpy(sn, sname, 16); memcpy(pn, png, 16);
        slot[k] = -1;
        for (int i = 0; i < L->nlayers; i++) if (L->layers[i].is_tilemap && !strcmp(L->layers[i].name, sn)) slot[k] = i;
        CBlock *cb = load_sheet(dir, pn, sheet_ids + (uint32_t)k);
        if (slot[k] < 0 || !cb) { fprintf(stderr, "%s: layer %s unusable\n", dir, sn); r.ok = false; break; }
        f->cells[k] = malloc((size_t)w * h * 4);
        if (!f->cells[k]) { r.ok = false; break; }
        memcpy(f->cells[k], cells, (size_t)w * h * 4);
        le32_to_host(f->cells[k], (size_t)w * h);
        TileMap *m = &f->maps[k];
        m->w = m->used_w = (int)w; m->h = (int)h; m->cblock_id = cb->id; m->cells = f->cells[k]; m->cb = cb;
        par[k] = p;
    }
    uint32_t cols = u32(&r), rows = u32(&r);
    const uint8_t *coll = take(&r, (size_t)cols * rows);
    int nt = (int)u32(&r);
    for (int i = 0; i < nt && r.ok; i++) {
        LevelObject o = { 0 };
        o.type = u32(&r); o.x = f32(&r); o.spawn_x = f32(&r); o.spawn_y = R(208);   /* zone: x .. x + zone_w, full height */
        uint32_t nwp = u32(&r);
        for (int k = 0; k < 3; k++) { o.wp[k][0] = f32(&r); o.wp[k][1] = f32(&r); }
        o.n_wp = (uint8_t)(nwp > 3 ? 3 : nwp);
        o.a = (uint16_t)u32(&r); o.loops = (int8_t)(int32_t)u32(&r); o.c = (uint8_t)u32(&r); o.b = (uint16_t)u32(&r);
        if (f->ntriggers < FOREST_MAX_TRIGGERS) f->triggers[f->ntriggers++] = o;
    }
    if (r.ok && coll) { f->collision = malloc((size_t)cols * rows); if (f->collision) memcpy(f->collision, coll, (size_t)cols * rows); }
    free(blob);
    if (!r.ok || !f->collision) { fprintf(stderr, "%s: truncated %s\n", dir, file); forest_dispose(f); return false; }

    /* the shield sniper's frames (type 30, enemies.c; forest/sniper.py; stage 5 has them too) */
    if (!sprite_from_png(SHIELD_SNIPER_SPRITE, asset_path("forest/sniper.png"), 64)) fprintf(stderr, "stage4: missing forest/sniper.png\n");

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

bool forest_on_deck(const Level *L, real x, real feet_y)
{
    int cx = r_floor(x / L->cellw);
    if (!L->collision || cx < 0 || cx >= L->cols) return false;
    for (int cy = 0; cy < L->rows; cy++) {
        uint8_t v = L->collision[cy * L->cols + cx];
        if (v & 0x10) return false;                            /* reached the ground: no deck above it here */
        if (v == 4) return feet_y <= r_int(cy * L->cellh) + R(1.0f) && feet_y > r_int(cy * L->cellh) - R(64.0f);
    }
    return false;
}

/* ---- the finale ---- */
#define FINALE_ONSLAUGHT_T R(24.0f)

/* Story: the morning after Hyperjumper Pass. On the way home Ramrod caught a garbled distress call from the ranger
 * station on Tropicus, the dinosaur reserve the Outriders raid in the series ("Oh Boy! Dinosaurs!": the ranger's
 * call gets through half jammed). Here the jamming comes from Outrider watchtowers strung along the Red Palm Jungle,
 * and Ramrod can't set down under the canopy, so the hero is dropped at the jungle's edge and walks in. The intro
 * doesn't hint at a boss (the quiet stretch before the clearing does that): the last clearing is a trap, and what
 * comes out of the Vapor Zone for it is a rebuilt Hyperjumper. */
/* every page fits one box (4 wrapped lines, whichever hero's name is swapped in: SABER_DLGCHECK) */
const char *const FOREST_SCRIPT_INTRO =
    "<|GREEN|>\n</dialog_avatar_april2/>\nFireball, that garbled distress call we caught after the pass came from the ranger station on Tropicus.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nThe dinosaur reserve? Reckon the Outriders are rustling big game for Nemesis again, pardner.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nThey've raised watchtowers all over this jungle to jam the ranger's signal - and our scanners.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nRamrod can't land under this canopy, so you're on foot again. Knock out those towers and we'll find the ranger.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nSo much for going home. Keep Ramrod's engines warm - this is one jungle safari I'll make quick.\n<<>>\n";
const char *const FOREST_SCRIPT_AMBUSH =
    "<|PURPLE|>\n</dialog_avatar_outrider/>\nEnd of the line, Star Sheriff! Every Outrider in the jungle is coming, and worse from the Vapor Zone!\n<<>>\n"
    "<|RED|>\n</dialog_avatar_april2/>\nFireball, they're phasing in all around you! Hold them off until Ramrod gets through the canopy!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nFirst that sniper up on the tower, then the high ground is mine. Come and get it, you phantoms!\n<<>>\n";
const char *const FOREST_SCRIPT_OUTRO =
    "<|GREEN|>\n</dialog_avatar_saber2/>\nHyperjumper won't be back this time, and that was their last watchtower. Outstanding work, Fireball.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nRemind me never to get on your bad side, pardner. Ramrod's coming down through the trees for you now.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nCommander Eagle says the frontier is safe again. The whole Cavalry Command owes you one!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nThen tell him I'll take it in time off. And this time we're REALLY going home!\n<<>>\n";
#define R_EDGE WP_OFF_R
#define L_EDGE WP_OFF_L

static void add_stream(Enemies *E, real x0, real w, int type, real wx0, real wx1, int every_ms, int delay_ms, int layer)
{
    LevelObject o = { 0 };
    o.type = (uint32_t)type; o.x = x0; o.spawn_x = w; o.y = R(-40); o.spawn_y = R(300); o.layer = (uint32_t)layer;
    o.wp[0][0] = wx0; o.wp[0][1] = R(160); o.wp[1][0] = wx1; o.wp[1][1] = R(160); o.n_wp = 2;
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

bool forest_finale_update(Forest *f, Enemies *E, Night *hj, Effects *fx, const Level *L, const PhysicsWorld *W,
                          const Player *pl, real cam_x, int sw, int sh, int layer, real dt)
{
    ForestFinale *F = &f->fin;
    bool alive = pl->ch.state != CS_DEAD;
    F->t += dt;
    switch (F->state) {
    case FF_WAIT:
        if (!alive || cam_x < L->width - r_int(sw) - R(0.5f)) break;
        F->state = FF_ONSLAUGHT; F->t = 0; F->scene_ambush = true;
        music_play(8, true);   /* the boss music for the whole finale (Hyperjumper's arrival keeps it going) */
        F->arena_x = L->width - r_int(sw); F->sw = sw;
        /* edge streams over the whole clearing (the spawner needs the hero in the zone: all of it, deck included) */
        F->tr0 = E->ntr;
        add_stream(E, F->arena_x, r_int(sw), 2, R_EDGE, R_EDGE, 2100, 300, layer);    /* gunmen running in from the right */
        add_stream(E, F->arena_x, r_int(sw), 1, L_EDGE, R_EDGE, 2500, 1400, layer);   /* chargers from both sides */
        add_stream(E, F->arena_x, r_int(sw), 32, L_EDGE, R_EDGE, 3000, 1900, layer);  /* blue stalkers: they take a firing
                                                                                          line on him, 45 degrees up to a deck */
        F->ntr_onslaught = E->ntr - F->tr0;
        add_stream(E, F->arena_x, r_int(sw), 1, R_EDGE, L_EDGE, 3400, 0, layer);      /* the boss round: thinner */
        add_stream(E, F->arena_x, r_int(sw), 2, L_EDGE, R_EDGE, 4600, 0, layer);
        add_stream(E, F->arena_x, r_int(sw), 32, R_EDGE, L_EDGE, 5200, 0, layer);
        F->ntr = E->ntr - F->tr0;
        streams(E, F, F->ntr_onslaught, F->ntr, false);
        if (plat_getenv("SABER_TRACE")) fprintf(stderr, "stage4: finale, arena %s..%s\n", RS(F->arena_x, 0), RS(F->arena_x + r_int(sw), 0));
        break;
    case FF_ONSLAUGHT:
    case FF_BOSS: {
        bool boss = F->state == FF_BOSS;
        if (F->t >= R(0.9f)) E->shield_wake = true;   /* the tower sniper opens up once the radio scene is over */
        int cap = boss ? 5 : 8;                    /* bodies on the field at once (the tower gunman and the snipers count) */
        E->spawner_enabled = on_field(E) < cap;
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
        /* the wreck has burnt out and the field is clear */
        if (hj->clear_ready && on_field(E) == 0 && alive && F->t > R(0.5f)) {
            F->state = FF_WON;
            if (plat_getenv("SABER_TRACE")) fprintf(stderr, "stage4: finale won\n");
        }
        break;
    case FF_WON:
        break;
    }
    return F->state == FF_WON;
}

void forest_finale_draw_hud(const Forest *f, const Enemies *E, const Night *hj, Ren *ren, int sw)
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
