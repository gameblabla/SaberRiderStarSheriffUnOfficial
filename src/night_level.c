#include "night_level.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A stretch of a level-1 background layer: source x range (16 px aligned) and
 * whether it is laid mirrored. src0 < 0 is empty space of width src1. */
typedef struct { int src0, src1; bool mirror; } Scene;

/* ---- the play plane: level-1 objects placed one by one ----
 * Each object is a level-1 tile rectangle (found from the decoded layers: a
 * connected group of tiles with nothing else touching it), stamped into the
 * new maps over a plain ground strip. Pads can be put at any height; pads and
 * wrecks bring their collision with them. Nothing is placed where level 1 had
 * it: the rocks, pads, wrecks and signposts make new scenes. */
enum { L_PLAYFIELD, L_PLATFORMS, L_CARS, L_FOREGROUND };
static const char *const LAYER_NAME[] = { "Playfield", "Platforms", "Cars", "ForegroundStuff" };

typedef struct { int layer, src0, src1, row0, row1, surface; } Obj;   /* surface: a pad's standing y in level 1 */
enum {
    CACTUS_A, CACTUS_B, CACTUS_C, CACTUS_D,
    ROCK_S1, ROCK_S2, ROCK_S3, ROCK_S4, ROCK_S5, ROCK_S6, ROCK_S7, ROCK_S8, ROCK_M1, ROCK_M2,
    CLUSTER_A, CLUSTER_B, PEAK_A, PEAK_B, PILE, MOUND, WALL,
    PAD64, PAD80, PAD96, PAD128,
    TAXI, CAR, VAN, SIGN_A, SIGN_B,
};
static const Obj OBJ[] = {
    [CACTUS_A] = { L_PLAYFIELD,   64,   96, 0, 13, 0 }, [CACTUS_B] = { L_PLAYFIELD,  352,  384, 0, 13, 0 },
    [CACTUS_C] = { L_PLAYFIELD, 3424, 3456, 0, 13, 0 }, [CACTUS_D] = { L_PLAYFIELD, 4608, 4640, 0, 13, 0 },
    [ROCK_S1]  = { L_PLAYFIELD,    0,   48, 0, 13, 0 }, [ROCK_S2]  = { L_PLAYFIELD,  128,  192, 0, 13, 0 },
    [ROCK_S3]  = { L_PLAYFIELD,  224,  288, 0, 13, 0 }, [ROCK_S4]  = { L_PLAYFIELD, 1280, 1344, 0, 13, 0 },
    [ROCK_S5]  = { L_PLAYFIELD, 3200, 3264, 0, 13, 0 }, [ROCK_S6]  = { L_PLAYFIELD, 3296, 3360, 0, 13, 0 },
    [ROCK_S7]  = { L_PLAYFIELD, 4736, 4800, 0, 13, 0 }, [ROCK_S8]  = { L_PLAYFIELD, 4816, 4864, 0, 13, 0 },
    [ROCK_M1]  = { L_PLAYFIELD, 1088, 1152, 0, 13, 0 }, [ROCK_M2]  = { L_PLAYFIELD, 8672, 8736, 0, 13, 0 },
    [CLUSTER_A] = { L_PLAYFIELD,  400,  560, 0, 13, 0 }, [CLUSTER_B] = { L_PLAYFIELD, 3568, 3728, 0, 13, 0 },
    [PEAK_A]   = { L_PLAYFIELD,  592,  944, 0, 13, 0 }, [PEAK_B]   = { L_PLAYFIELD, 1456, 1808, 0, 13, 0 },
    [PILE]     = { L_PLAYFIELD, 8752, 9200, 0, 13, 0 }, [MOUND]    = { L_PLAYFIELD, 4240, 4592, 0, 13, 0 },
    [WALL]     = { L_PLAYFIELD, 2032, 3152, 0, 13, 0 },
    [PAD64]    = { L_PLATFORMS,  704,  768, 7, 10, 128 }, [PAD80]  = { L_PLATFORMS, 2352, 2432, 7, 10, 128 },
    [PAD96]    = { L_PLATFORMS, 1584, 1680, 7, 10, 128 }, [PAD128] = { L_PLATFORMS, 2560, 2688, 4, 7, 80 },
    [TAXI]     = { L_CARS,     1200, 1328, 8, 14, 0 }, [CAR] = { L_CARS, 3488, 3616, 8, 14, 0 },
    [VAN]      = { L_CARS,     4400, 4576, 8, 14, 0 },
    [SIGN_A]   = { L_FOREGROUND, 4864, 4944, 6, 15, 0 }, [SIGN_B] = { L_FOREGROUND, 8416, 8496, 6, 15, 0 },
};

/* x: left edge; y: a pad's standing height (0 = as in level 1) */
typedef struct { int obj, x; bool mirror; int y; } Place;
#define STAGE3_WIDTH 7040
static const Place LAYOUT[] = {
    /* the badlands: loose rocks, a first wreck to fight from */
    { ROCK_S2, 32, false, 0 }, { CACTUS_A, 144, false, 0 }, { CLUSTER_A, 288, true, 0 }, { TAXI, 512, false, 0 }, { CACTUS_B, 688, false, 0 }, { ROCK_S5, 752, false, 0 },
    /* a stair of hover pads up the face of the rock pile (every pad is bolted to rock, as in level 1) */
    { ROCK_S3, 832, false, 0 }, { PILE, 912, false, 0 }, { CLUSTER_B, 1360, false, 0 }, { CACTUS_C, 1536, false, 0 }, { ROCK_S6, 1584, false, 0 },
    { PAD64, 944, false, 160 }, { PAD80, 1056, false, 128 }, { PAD96, 1184, false, 96 }, { PAD64, 1376, true, 144 },
    /* twin peaks: a climb to the first summit, the valley, a pad on the second */
    { PEAK_A, 1696, false, 0 }, { PEAK_B, 2080, true, 0 }, { CACTUS_D, 2464, false, 0 },
    { PAD64, 1744, false, 144 }, { PAD128, 1824, false, 80 }, { PAD64, 2176, true, 112 },
    /* the wreck yard: cover to fight across */
    { ROCK_S3, 2592, false, 0 }, { VAN, 2688, false, 0 }, { CAR, 2960, true, 0 }, { SIGN_A, 3120, false, 0 }, { TAXI, 3248, true, 0 }, { ROCK_S4, 3408, false, 0 },
    /* the mound, pads zig-zagging in front of it */
    { MOUND, 3552, false, 0 }, { PAD80, 3568, false, 144 }, { PAD96, 3680, false, 96 }, { PAD64, 3808, true, 144 },
    { CACTUS_A, 3952, false, 0 }, { CLUSTER_A, 4032, false, 0 }, { ROCK_S7, 4224, true, 0 },
    /* the rock wall, a long climb along its face */
    { WALL, 4320, true, 0 }, { PAD64, 4368, false, 144 }, { PAD80, 4480, false, 112 }, { PAD128, 4624, false, 80 },
    { PAD96, 4848, true, 112 }, { PAD64, 4992, false, 144 }, { PAD80, 5152, true, 96 }, { CAR, 5312, false, 0 },
    /* the crossroads: signpost, the rock pile */
    { SIGN_B, 5520, false, 0 }, { ROCK_M2, 5632, false, 0 }, { CACTUS_B, 5712, false, 0 }, { PILE, 5792, true, 0 }, { PAD96, 5952, false, 128 },
    { ROCK_S8, 6272, false, 0 },
    /* open ground: Hyperjumper's arena, only low rocks at the back */
    { ROCK_S4, 6432, false, 0 }, { CACTUS_C, 6592, false, 0 }, { ROCK_S2, 6784, true, 0 }, { CACTUS_D, 6944, false, 0 },
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

/* a map cell that draws nothing (level 1 keeps ids for fully empty tiles) */
static bool blank_cell(const TileMap *m, uint32_t v)
{
    if (!v) return true;
    int n = m->cb ? cblock_ncells(m->cb) : 0;
    return n > 0 && m->cb->cells[(v - 1) % (uint32_t)n] == 0xFFFF;
}

static bool plain_ground(int row, uint32_t v)
{
    /* level 1's bare ground edge: rows 11/12 repeat 8..13 / 56..61 */
    return (row == 11 && v >= 8 && v <= 13) || (row == 12 && v >= 56 && v <= 61);
}

/* a column with nothing above the ground rows (only a rock's foot, or bare ground) */
static bool column_bare(const TileMap *m, int sc)
{
    for (int r = 0; r < 11 && r < m->h; r++) if (!blank_cell(m, m->cells[r * m->w + sc])) return false;
    return true;
}

static bool column_plain(const TileMap *m, int sc)
{
    for (int r = 11; r <= 12 && r < m->h; r++) {
        uint32_t v = m->cells[r * m->w + sc];
        if (!blank_cell(m, v) && !plain_ground(r, v)) return false;
    }
    return true;
}

/* Which side's rock a lone foot column of level 1 belongs to: -1 left, +1 right, 0 unknown.
 * A foot shades the ground on one side of its rock, so a foot tile (by its row-11 id) always
 * belongs to the same side. Where only one neighbour is an object that side owns it; where
 * both are (a foot between two objects, e.g. ROCK_S1 | foot | CACTUS_A) the tile's side
 * elsewhere in the level decides. */
static int foot_owner(const TileMap *m, int sc)
{
    if (sc < 0 || sc >= m->w || !column_bare(m, sc) || column_plain(m, sc)) return 0;
    uint32_t id = m->cells[11 * m->w + sc];
    int left = 0, right = 0;
    for (int c = 1; c + 1 < m->w; c++) {
        if (m->cells[11 * m->w + c] != id || !column_bare(m, c)) continue;
        bool l = !column_bare(m, c - 1), r = !column_bare(m, c + 1);
        if (l && !r) left++;
        if (r && !l) right++;
    }
    return left > right ? -1 : right > left ? 1 : 0;
}

bool stage3_world_build(Level *L, Stage3World *w)
{
    memset(w, 0, sizeof *w);
    if (L->cellw != 8 || L->cellh != 8) { fprintf(stderr, "stage3: unexpected collision cell size\n"); return false; }
    const int width = STAGE3_WIDTH, tcols = width / 16, cols = width / L->cellw, rows = L->rows;
    int li[4] = { -1, -1, -1, -1 };
    for (int i = 0; i < L->nlayers; i++)
        for (int k = 0; k < 4; k++)
            if (L->layers[i].is_tilemap && L->layers[i].map && !strcmp(L->layers[i].name, LAYER_NAME[k])) li[k] = i;
    for (int k = 0; k < 4; k++) if (li[k] < 0) { fprintf(stderr, "stage3: level-1 layer %s missing\n", LAYER_NAME[k]); return false; }

    w->collision = calloc((size_t)cols * (size_t)rows, 1);
    for (int k = 0; k < 4; k++) w->cells[li[k]] = calloc((size_t)tcols * (size_t)L->layers[li[k]].map->h, sizeof(uint32_t));
    int neww[LVL_MAX_LAYERS] = { 0 };
    for (int i = 0; i < L->nlayers; i++) {
        const Layer *ly = &L->layers[i];
        if (!ly->is_tilemap || !ly->map) continue;
        if (!strcmp(ly->name, "MidBG")) w->cells[i] = remap_map(ly->map, MIDBG, (int)(sizeof MIDBG / sizeof MIDBG[0]), &neww[i], false);
        else if (!strcmp(ly->name, "Cars MidBG")) w->cells[i] = remap_map(ly->map, CARS_MIDBG, (int)(sizeof CARS_MIDBG / sizeof CARS_MIDBG[0]), &neww[i], false);
        else continue;
        if (!w->cells[i]) { stage3_world_free(w); return false; }
    }
    bool ok = w->collision != NULL;
    for (int k = 0; k < 4; k++) ok = ok && w->cells[li[k]];
    if (!ok) { stage3_world_free(w); return false; }

    /* The source dirt is only six tiles wide. Repeating it unchanged makes the
     * same large pebble clusters form conspicuous vertical bands across the
     * desert. Reverse selected whole motifs, including their individual tile
     * pixels, so each motif remains internally continuous while the long run
     * has a less mechanical rhythm. Collision remains a plain solid floor. */
    const TileMap *pf = L->layers[li[L_PLAYFIELD]].map;
    uint32_t *g = w->cells[li[L_PLAYFIELD]];
    for (int c = 0; c < tcols; c++) {
        unsigned motif = (unsigned)c / 6;
        bool reverse = ((motif * 1103515245u + 12345u) >> 29) & 1u;
        int sc = 114 + (reverse ? 5 - c % 6 : c % 6);
        for (int r = 11; r <= 14 && r < pf->h; r++) {
            uint32_t v = pf->cells[r * pf->w + sc];
            g[r * tcols + c] = reverse && v ? v | STAGE3_CELL_FLIP : v;
        }
    }
    int floor_row = 208 / L->cellh;
    for (int r = floor_row; r < rows; r++) for (int c = 0; c < cols; c++) w->collision[r * cols + c] = 15;

    for (size_t i = 0; i < sizeof LAYOUT / sizeof LAYOUT[0]; i++) {
        const Place *p = &LAYOUT[i];
        const Obj *o = &OBJ[p->obj];
        const TileMap *m = L->layers[li[o->layer]].map;
        uint32_t *dst = w->cells[li[o->layer]];
        int dy = o->surface && p->y ? p->y - o->surface : 0;   /* pads: px, a multiple of 16 */
        int n = (o->src1 - o->src0) / 16, dc0 = p->x / 16;
        /* the rocks' feet spill up to a tile past the object's columns (only into the ground rows): bring that
         * column along or the rock ends in a flat cut */
        bool rocks = o->layer == L_PLAYFIELD;
        for (int k = rocks ? -1 : 0; k < (rocks ? n + 1 : n); k++) {
            int sc = p->mirror ? o->src1 / 16 - 1 - k : o->src0 / 16 + k, dc = dc0 + k;
            if (dc < 0 || dc >= tcols || sc < 0 || sc >= m->w) continue;
            bool foot = k < 0 || k >= n;
            /* only a lone foot (nothing of the column above the ground), and only this object's: the column
             * next to an object can hold its neighbour's foot instead, a wedge cut flat on one side */
            if (foot && foot_owner(m, sc) != ((k < 0) != p->mirror ? 1 : -1)) continue;
            for (int r = foot ? 11 : o->row0; r < (foot ? 13 : o->row1) && r < m->h; r++) {
                uint32_t v = m->cells[r * m->w + sc];
                int dr = r + dy / 16;
                if (!v || dr < 0 || dr >= m->h) continue;
                if (o->layer == L_PLAYFIELD && plain_ground(r, v)) continue;   /* keep the strip's own phase */
                if (foot && !plain_ground(dr, dst[dr * tcols + dc] & ~STAGE3_CELL_FLIP)) continue;   /* never over a neighbour */
                dst[dr * tcols + dc] = p->mirror ? v | STAGE3_CELL_FLIP : v;
            }
        }
        if (o->layer != L_PLATFORMS && o->layer != L_CARS) continue;   /* only pads and wrecks collide */
        for (int k = 0; k < n * 2; k++) {
            int sc = p->mirror ? o->src1 / 8 - 1 - k : o->src0 / 8 + k, dc = p->x / 8 + k;
            if (dc < 0 || dc >= cols || sc >= L->cols) continue;
            for (int r = 0; r < floor_row; r++) {
                uint8_t v = L->collision[r * L->cols + sc];
                int dr = r + dy / 8;
                if (v && dr >= 0 && dr < floor_row) w->collision[dr * cols + dc] = p->mirror ? mirror_cell(v) : v;
            }
        }
    }

    /* everything allocated: switch the level over */
    for (int k = 0; k < 4; k++) neww[li[k]] = tcols;
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
 * Fixed spots stand on pads (pad y - 45), wreck roofs (123) and the van (99), and
 * always 260+ px past their thin trigger so they are placed off screen and
 * scrolled into view, never popped in. Streams mix walkers, grunts and the
 * grunt variant, from ahead and from behind, with random intervals. */
#define RGT 100000.0f
#define LFT -100000.0f
typedef struct { int type; float x, zw; int nwp; float wp[3][2]; int interval_ms, loops, rand_n, delay_ms; } Stage3Trigger;
static const Stage3Trigger TRIGGERS[] = {
    /* badlands: grunts run in, a kneeler waits behind the first wreck */
    { 2,  160, 400, 1, { { RGT, 160 } },                              1500,  3, 40,    0 },
    { 8,  330,   8, 1, { { 664, 160 } },                               0,  1,  0,    0 },
    /* the pad stair: a sniper on the top pad, walkers from both sides, a kneeler on the last pad */
    { 7,  900,   8, 1, { { 1232, 51 } },                               0,  1,  0,    0 },
    { 1,  880, 520, 2, { { LFT, 160 }, { RGT, 160 } },                1100, -1, 40,  400 },
    { 9, 1080,   8, 1, { { 1408, 99 } },                               0,  1,  0,    0 },
    /* twin peaks: a sniper on the summit pad, grunts from behind, a kneeler on the far pad */
    { 6, 1580,   8, 1, { { 1888, 35 } },                               0,  1,  0,    0 },
    { 5, 1900,   8, 1, { { LFT, 160 } },                             700,  2, 20,    0 },
    { 8, 1940,   8, 1, { { 2208, 67 } },                               0,  1,  0,    0 },
    { 2, 2100, 400, 3, { { RGT, 160 }, { RGT, 160 }, { LFT, 160 } },    1500, -1, 60,  600 },
    /* the wreck yard: gunmen on all three roofs, walkers both ways */
    { 9, 2480,   8, 1, { { 2780, 99 } },                               0,  1,  0,    0 },
    { 1, 2650, 700, 2, { { RGT, 160 }, { LFT, 160 } },                1300, -1, 50,  800 },
    { 6, 2760,   8, 1, { { 3048, 123 } },                              0,  1,  0,    0 },
    { 8, 3040,   8, 1, { { 3336, 123 } },                              0,  1,  0,    0 },
    /* the mound */
    { 7, 3420,   8, 1, { { 3728, 51 } },                               0,  1,  0,    0 },
    { 5, 3560, 500, 1, { { RGT, 160 } },                              1200,  4, 40,    0 },
    { 8, 3860,   8, 1, { { 4160, 160 } },                              0,  1,  0,    0 },
    /* the wall climb */
    { 2, 4350, 900, 3, { { RGT, 160 }, { LFT, 160 }, { RGT, 160 } },    1400, -1, 50,  500 },
    { 6, 4390,   8, 1, { { 4688, 35 } },                               0,  1,  0,    0 },
    { 9, 4900,   8, 1, { { 5192, 51 } },                               0,  1,  0,    0 },
    { 7, 5060,   8, 1, { { 5352, 123 } },                              0,  1,  0,    0 },
    { 5, 5100,   8, 1, { { LFT, 160 } },                             700,  2, 20,    0 },
    /* the crossroads before the arena */
    { 1, 5450, 700, 2, { { LFT, 160 }, { RGT, 160 } },                1100, -1, 40,  300 },
    { 9, 5700,   8, 1, { { 6000, 83 } },                               0,  1,  0,    0 },
    { 6, 5980,   8, 1, { { 6280, 160 } },                              0,  1,  0,    0 },
    { 2, 6050, 300, 1, { { RGT, 160 } },                              1200,  2, 40,    0 },
};
#undef RGT
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
