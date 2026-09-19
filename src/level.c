#include "level.h"
#include "pack.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p) { uint32_t v = rd32(p); float f; memcpy(&f, &v, 4); return f; }
static int32_t rds32(const uint8_t *p) { return (int32_t)rd32(p); }

bool level_load(Level *L, uint32_t id)
{
    memset(L, 0, sizeof *L);
    const PackEntry *e = packs_find(id);
    if (!e || memcmp(e->data, "LEVL", 4)) { fprintf(stderr, "level %08X not found\n", id); return false; }
    const uint8_t *d = e->data, *p;
    L->id = id;
    if (rd32(d + 4) != 0) { fprintf(stderr, "level %08X: not TILE mode\n", id); return false; }
    L->npacks = rd32(d + 8); L->nobjs = rd32(d + 12);
    p = d + 0x10;
    for (int i = 0; i < L->npacks && i < 4; i++) memcpy(L->packs[i], p + i * 12, 12);
    p += L->npacks * 12;
    if (L->nobjs > LVL_MAX_OBJECTS) L->nobjs = LVL_MAX_OBJECTS;
    for (int i = 0; i < L->nobjs; i++, p += 128) {
        LevelObject *o = &L->objs[i];
        o->raw = p; o->type = rd32(p); o->x = rdf(p + 4); o->y = rdf(p + 8);
        for (int k = 0; k < 3; k++) { o->wp[k][0] = rdf(p + 0x0c + k * 8); o->wp[k][1] = rdf(p + 0x10 + k * 8); }
        o->spawn_x = rdf(p + 0x4c); o->spawn_y = rdf(p + 0x50); o->layer = rd32(p + 0x54);
        o->a = (uint16_t)(p[0x58] | p[0x59] << 8); o->n_wp = p[0x5a]; o->loops = (int8_t)p[0x5b];
        o->b = (uint16_t)(p[0x5c] | p[0x5d] << 8); o->c = p[0x5e]; o->d = p[0x5f];
    }
    L->nlayers = rd32(p); p += 4;
    if (L->nlayers > LVL_MAX_LAYERS) L->nlayers = LVL_MAX_LAYERS;
    const uint8_t *flags = p, *par = p + 64, *extra = p + 128, *names = p + 192;
    for (int i = 0; i < L->nlayers; i++) {
        Layer *ly = &L->layers[i];
        ly->is_tilemap = rds32(flags + i * 4) != 0;
        ly->parallax = rdf(par + i * 4);
        ly->extra = rds32(extra + i * 4);
        memcpy(ly->name, names + i * 16, 15);
    }
    p += 192 + 15 * 16 + 16;
    /* collision map header */
    p += 4;
    L->width = rdf(p); L->height = rdf(p + 4);
    L->cols = rd32(p + 8); L->rows = rd32(p + 12); L->cellw = rd32(p + 16); L->cellh = rd32(p + 20);
    p += 24;
    L->collision = p; p += (size_t)L->cols * L->rows * 2;   /* second grid unused */
    /* tile maps */
    const uint8_t *end = d + e->size;
    int mi = 0;
    for (int i = 0; i < L->nlayers && p + 12 <= end; i++) {
        if (!L->layers[i].is_tilemap) continue;
        TileMap *m = &L->maps[mi];
        m->w = rd32(p); m->h = rd32(p + 4); m->cblock_id = rd32(p + 8);
        m->cells = (const uint32_t *)(p + 12);
        p += 12 + (size_t)m->w * m->h * 4;
        m->cb = cblock_get(m->cblock_id);
        L->layers[i].map = m;
        mi++;
    }
    L->nmaps = mi;
    return true;
}

uint8_t level_cell(const Level *L, int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= L->cols || cy >= L->rows) return 0;
    return L->collision[cy * L->cols + cx];
}

/* Port of saber_engine::TileMapRenderer::render. Non-wrapping layers scroll by cam*parallax;
 * the wrap layer (extra==1, SkyBG) repeats horizontally. */
void level_draw_layer(const Level *L, int li, float cam_x, float cam_y, int sw, int sh)
{
    const Layer *ly = &L->layers[li];
    const TileMap *m = ly->map;
    if (!m || !m->cb) return;
    const CBlock *cb = m->cb;
    int tw = cb->tw, th = cb->th;
    float ox = cam_x * ly->parallax, oy = cam_y * ly->parallax;
    bool wrap = ly->extra == 1;
    int ncells = cblock_ncells(cb);
    int mapw_px = m->w * tw;
    if (wrap && mapw_px > 0) ox = fmodf(ox, (float)mapw_px), oy = 0;
    if (ox < 0 && !wrap) ox = 0;
    if (oy < 0) oy = 0;
    int cx0 = (int)floorf(ox / tw), cy0 = (int)floorf(oy / th);
    int ncx = sw / tw + 2, ncy = sh / th + 2;
    for (int cy = cy0; cy < cy0 + ncy; cy++) {
        if (cy < 0 || cy >= m->h) continue;
        for (int cx = cx0; cx < cx0 + ncx; cx++) {
            int mx = cx;
            if (wrap) { mx = ((cx % m->w) + m->w) % m->w; }
            else if (cx < 0 || cx >= m->w) continue;
            uint32_t v = m->cells[cy * m->w + mx];
            if (!v) continue;
            int ci = (int)((v - 1) % (uint32_t)ncells);
            uint16_t t = cb->cells[ci];
            if (t == 0xFFFF) continue;
            cblock_draw_tile(cb, t, floorf(cx * tw - ox), floorf(cy * th - oy), false);
        }
    }
}
