#include "night_level.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A scene of a level-1 layer: source x range (16 px aligned) and whether it is
 * laid mirrored. src0 < 0 is empty space of width src1. */
typedef struct { int src0, src1; bool mirror; } Scene;

/* The play plane. Each cut sits on a level-1 column that no rock, car, pad or
 * house crosses in Playfield/Platforms/Cars/ForegroundStuff or the collision
 * grid (found from the decoded layers). The town (5056..8144) and the ruined
 * house (3904..4160) are left out; the level-1 boss fence yard is not used.
 *
 *     0  small rocks                 | 1792  the big rock wall with five pads
 *   304  pointed rock + pad (mirr.)  | 3040  wrecked car (mirr.)
 *   816  wrecked car                 | 3360  rock + van
 *  1200  signpost, loose rocks       | 3728  pointed rock + pad (mirr.)
 *                                    | 4176  loose rocks
 *                                    | 4384  the rock wall again (mirr.)
 *                                    | 5632  wrecked car (mirr.)
 *                                    | 6016  open ground: Hyperjumper's arena */
static const Scene PLAY[] = {
    { 3168, 3472, false }, { 1408, 1920, true  }, { 1024, 1408, false }, { 8144, 8736, false },
    { 1920, 3168, false }, { 3472, 3792, true  }, { 4224, 4592, false }, {  576, 1024, true  },
    { 4592, 4800, false }, { 1920, 3168, true  }, { 1024, 1408, true  }, {    0,  576, false },
};

/* MidBG (parallax 0.875): its rock-and-cactus stretches only (the houses sit at
 * 880..1120, 2736..3120 and from 4272 on). Long enough for the camera's range. */
static const Scene MIDBG[] = {
    {    0,  640, false }, { 1136, 2688, false }, { 3184, 3952, false }, { 1136, 2688, true },
    { 3184, 3952, true  }, {    0,  640, true  }, { 1136, 2688, false },
};

/* Cars MidBG (0.9): two wrecks out in the desert, no Ramrod and no town traffic. */
static const Scene CARS_MIDBG[] = {
    { -1, 1440, false }, { 896, 1104, false }, { -1, 1760, false }, { 1616, 1856, true },
    { -1, 1200, false }, { 896, 1104, true  }, { -1, 1600, false },
};

static int scenes_width(const Scene *s, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++) w += s[i].src0 < 0 ? s[i].src1 : s[i].src1 - s[i].src0;
    return w;
}

/* dest column -> (source column, mirrored); -1 = empty. unit = column width in px. */
static int scene_column(const Scene *s, int n, int dcol, int unit, bool *mirror)
{
    int x = dcol * unit;
    for (int i = 0; i < n; i++) {
        int w = s[i].src0 < 0 ? s[i].src1 : s[i].src1 - s[i].src0;
        if (x < w) {
            *mirror = s[i].mirror;
            if (s[i].src0 < 0) return -1;
            int k = x / unit;
            return s[i].mirror ? s[i].src1 / unit - 1 - k : s[i].src0 / unit + k;
        }
        x -= w;
    }
    *mirror = false;
    return -1;
}

static uint32_t *remap_map(const TileMap *m, const Scene *s, int n, int *out_w, bool periodic_ground)
{
    int tw = m->cb ? m->cb->tw : 16;
    int w = scenes_width(s, n) / tw;
    uint32_t *cells = calloc((size_t)w * (size_t)m->h, sizeof *cells);
    if (!cells) return NULL;
    for (int c = 0; c < w; c++) {
        bool mir;
        int sc = scene_column(s, n, c, tw, &mir);
        for (int r = 0; r < m->h; r++) {
            uint32_t v = 0;
            /* Playfield rows 13/14 are the dirt under the ground edge, a 6-tile
             * repeat across the whole level: lay them by the new column so no
             * scene boundary or mirrored scene breaks the pattern. */
            if (periodic_ground && (r == 13 || r == 14) && m->w >= 6) v = m->cells[r * m->w + c % 6];
            else if (sc >= 0 && sc < m->w) {
                v = m->cells[r * m->w + sc];
                if (v && mir) v |= STAGE3_CELL_FLIP;
            }
            cells[r * w + c] = v;
        }
    }
    *out_w = w;
    return cells;
}

static uint8_t mirror_cell(uint8_t v)
{
    /* bit 1 blocks moving right, bit 2 moving left: swap them in a mirrored scene */
    return (uint8_t)((v & ~3u) | ((v & 1u) << 1) | ((v & 2u) >> 1));
}

bool stage3_world_build(Level *L, Stage3World *w)
{
    memset(w, 0, sizeof *w);
    const int nplay = (int)(sizeof PLAY / sizeof PLAY[0]);
    int width = scenes_width(PLAY, nplay);
    if (L->cellw != 8 || L->cellh != 8) { fprintf(stderr, "stage3: unexpected collision cell size\n"); return false; }

    int cols = width / L->cellw, rows = L->rows;
    w->collision = calloc((size_t)cols * (size_t)rows, 1);
    if (!w->collision) return false;
    for (int c = 0; c < cols; c++) {
        bool mir;
        int sc = scene_column(PLAY, nplay, c, L->cellw, &mir);
        for (int r = 0; r < rows; r++) {
            uint8_t v = sc >= 0 && sc < L->cols ? L->collision[r * L->cols + sc] : 0;
            w->collision[r * cols + c] = mir ? mirror_cell(v) : v;
        }
    }

    int neww[LVL_MAX_LAYERS] = { 0 };
    for (int i = 0; i < L->nlayers; i++) {
        Layer *ly = &L->layers[i];
        if (!ly->is_tilemap || !ly->map) continue;
        const char *nm = ly->name;
        const TileMap *m = ly->map;
        uint32_t *cells = NULL;
        if (!strcmp(nm, "Playfield")) cells = remap_map(m, PLAY, nplay, &neww[i], true);
        else if (!strcmp(nm, "Platforms") || !strcmp(nm, "Cars") || !strcmp(nm, "ForegroundStuff"))
            cells = remap_map(m, PLAY, nplay, &neww[i], false);
        else if (!strcmp(nm, "MidBG")) cells = remap_map(m, MIDBG, (int)(sizeof MIDBG / sizeof MIDBG[0]), &neww[i], false);
        else if (!strcmp(nm, "Cars MidBG")) cells = remap_map(m, CARS_MIDBG, (int)(sizeof CARS_MIDBG / sizeof CARS_MIDBG[0]), &neww[i], false);
        else continue;   /* sky, mountains and the 1.2 foreground run as in level 1 */
        if (!cells) { stage3_world_free(w); return false; }
        w->cells[i] = cells;
    }

    /* everything allocated: switch the level over */
    for (int i = 0; i < L->nlayers; i++) {
        if (!w->cells[i]) continue;
        TileMap *m = L->layers[i].map;
        m->cells = w->cells[i];
        m->w = m->used_w = neww[i];
    }
    L->collision = w->collision;
    L->cols = cols;
    L->width = (float)width;
    w->width = (float)width;
    return true;
}

void stage3_world_free(Stage3World *w)
{
    for (int i = 0; i < LVL_MAX_LAYERS; i++) { free(w->cells[i]); w->cells[i] = NULL; }
    free(w->collision); w->collision = NULL;
}

/* ---- enemy triggers ----
 * Level-1 object semantics (FUN_00421070): the zone is x..x+zw (full height);
 * +-100000 spawns just outside that screen edge, anything else is a fixed spot.
 * Fixed spots sit on pads (y 83 / 35), car roofs (123) and the van (99), and
 * always 260+ px past their thin trigger so they are placed off screen and
 * scrolled into view, never popped in. Streams mix walkers, grunts and the
 * grunt variant, from ahead and from behind, with random intervals. */
#define R 100000.0f
#define LFT -100000.0f
typedef struct { int type; float x, zw; int nwp; float wp[3][2]; int interval_ms, loops, rand_n, delay_ms; } Stage3Trigger;
static const Stage3Trigger TRIGGERS[] = {
    /* the open start and the pointed rock */
    { 2,  180, 420, 1, { { R, 160 } },                              1500,  3, 40,    0 },
    { 8,  320,   8, 1, { { 592, 83 } },                                0,  1,  0,    0 },
    /* first wreck, the signpost flats */
    { 6,  760,   8, 1, { { 1048, 123 } },                              0,  1,  0,    0 },
    { 1,  900, 500, 2, { { LFT, 160 }, { R, 160 } },                1100, -1, 40,  500 },
    { 5, 1420, 300, 1, { { R, 160 } },                              1300,  3, 50,    0 },
    /* the rock wall: gunmen on the pads, grunts from ahead and behind */
    { 6, 2000,   8, 1, { { 2264, 83 } },                               0,  1,  0,    0 },
    { 7, 2180,   8, 1, { { 2496, 35 } },                               0,  1,  0,    0 },
    { 2, 1850, 900, 3, { { R, 160 }, { R, 160 }, { LFT, 160 } },    1700, -1, 60,  800 },
    { 9, 2480,   8, 1, { { 2776, 83 } },                               0,  1,  0,    0 },
    /* wrecks and the van */
    { 8, 2990,   8, 1, { { 3288, 123 } },                              0,  1,  0,    0 },
    { 1, 3100, 400, 2, { { R, 160 }, { LFT, 160 } },                1000, -1, 40,    0 },
    { 9, 3300,   8, 1, { { 3600, 99 } },                               0,  1,  0,    0 },
    { 5, 3650,   8, 1, { { LFT, 160 } },                             700,  2, 20,    0 },
    { 6, 3720,   8, 1, { { 4016, 83 } },                               0,  1,  0,    0 },
    { 2, 3800, 500, 1, { { R, 160 } },                              1400, -1, 50,  600 },
    /* the mirrored rock wall */
    { 9, 4360,   8, 1, { { 4648, 83 } },                               0,  1,  0,    0 },
    { 1, 4500, 900, 3, { { R, 160 }, { LFT, 160 }, { R, 160 } },    1200, -1, 50,  400 },
    { 7, 4620,   8, 1, { { 4928, 35 } },                               0,  1,  0,    0 },
    { 6, 4880,   8, 1, { { 5160, 83 } },                               0,  1,  0,    0 },
    /* last wreck before the arena */
    { 8, 5480,   8, 1, { { 5784, 123 } },                              0,  1,  0,    0 },
    { 5, 5600, 300, 1, { { R, 160 } },                              1200,  2, 40,    0 },
};
#undef R
#undef LFT

int stage3_triggers(LevelObject *out, int max, int player_layer)
{
    int n = 0;
    for (size_t i = 0; i < sizeof TRIGGERS / sizeof TRIGGERS[0] && n < max; i++) {
        const Stage3Trigger *t = &TRIGGERS[i];
        LevelObject *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->type = (uint32_t)t->type;
        o->x = t->x; o->y = 0;
        o->spawn_x = t->zw; o->spawn_y = 208;   /* zone size */
        o->n_wp = (uint8_t)t->nwp;
        for (int k = 0; k < t->nwp && k < 3; k++) { o->wp[k][0] = t->wp[k][0]; o->wp[k][1] = t->wp[k][1]; }
        o->layer = (uint32_t)player_layer;
        o->a = (uint16_t)t->interval_ms; o->loops = (int8_t)t->loops;
        o->c = (uint8_t)t->rand_n; o->b = (uint16_t)t->delay_ms;
    }
    return n;
}
