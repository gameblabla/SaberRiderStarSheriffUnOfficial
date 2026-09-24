/* texprep: every graphic of the demo's packs as the game builds it, for tools/dc/texbake.py to bake into the Dreamcast's
 * texture pack. Runs the game's own gfx.c / font.c (so the tile sheets and sprite strips are exactly the runtime ones)
 * over a renderer that only keeps the RGBA of the texture being created.
 *   texprep <data_dir> <out_dir>   -> <out>/<ID>.srgb for every sprite, cblock and font glyph sprite (their ids on stdout):
 *        "SRGB", u32 kind (1 sprite, 2 cblock), u32 w, h, u32 meta_len, meta, w x h RGBA
 *        sprite meta: u16 frame w, h, frames, 0      cblock meta: u16 frames, cols, rows, tw, th, ntiles, sheet_cols, 0,
 *                                                                  u16 cells[frames * cols * rows]
 * Build: cc -O2 -std=gnu11 -Isrc tools/dc/texprep.c src/gfx.c src/font.c src/pack.c src/lzo1z.c src/assets.c src/namehash.c */
#include "gfx.h"
#include "font.h"
#include "pack.h"
#include "platform/render.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the capturing renderer ---- */
struct Ren { int unused; };
struct RTex { int w, h; };
static struct Ren ren;
static uint32_t *cap_px; static int cap_w, cap_h;

static RTex *capture(int w, int h)
{
    RTex *t = calloc(1, sizeof *t);
    t->w = w; t->h = h;
    free(cap_px); cap_px = calloc((size_t)w * h, 4); cap_w = w; cap_h = h;
    return t;
}
RTex *rtex_create(Ren *r, int w, int h, RTexAccess a, const uint32_t *px)
{
    (void)r; (void)a;
    RTex *t = capture(w, h);
    if (px) memcpy(cap_px, px, (size_t)w * h * 4);
    return t;
}
RTex *rtex_create_rows(Ren *r, int w, int h, RTexRows rows, void *ud)
{
    (void)r;
    RTex *t = capture(w, h);
    for (int y = 0; y < h; y += 32) rows(ud, y, h - y < 32 ? h - y : 32, cap_px + (size_t)y * w);
    return t;
}
RTex *rtex_create_baked(Ren *r, uint8_t *b, size_t n) { (void)r; (void)b; (void)n; return NULL; }
void rtex_destroy(RTex *t) { free(t); }
void rtex_size(const RTex *t, int *w, int *h) { *w = t ? t->w : 0; *h = t ? t->h : 0; }
void rtex_update(RTex *t, const uint32_t *px, int pitch) { (void)t; (void)px; (void)pitch; }
void rtex_set_color_mod(RTex *t, uint8_t r, uint8_t g, uint8_t b) { (void)t; (void)r; (void)g; (void)b; }
void rtex_set_alpha_mod(RTex *t, uint8_t a) { (void)t; (void)a; }
void rtex_set_blend(RTex *t, RBlend b) { (void)t; (void)b; }
void rtex_set_scale(RTex *t, RScale s) { (void)t; (void)s; }
void rtex_set_tag(RTex *t, uint32_t tag) { (void)t; (void)tag; }
Ren *rtex_renderer(const RTex *t) { (void)t; return &ren; }
void r_set_evict_hook(bool (*hook)(void)) { (void)hook; }
void r_tex(Ren *r, RTex *t, const RFRect *s, const RFRect *d) { (void)r; (void)t; (void)s; (void)d; }
void r_tex_rot(Ren *r, RTex *t, const RFRect *s, const RFRect *d, double a, const RFPoint *c, RFlip f) { (void)r; (void)t; (void)s; (void)d; (void)a; (void)c; (void)f; }
void r_tex_batch(Ren *r, RTex *t, const RFRect *s, const RFRect *d, int n) { (void)r; (void)t; (void)s; (void)d; (void)n; }
void r_set_draw_color(Ren *r, uint8_t R, uint8_t G, uint8_t B, uint8_t A) { (void)r; (void)R; (void)G; (void)B; (void)A; }
void r_fill_rect(Ren *r, const RFRect *q) { (void)r; (void)q; }
void r_fill_rects(Ren *r, const RFRect *q, int n) { (void)r; (void)q; (void)n; }
void r_set_draw_blend(Ren *r, RBlend b) { (void)r; (void)b; }

/* ---- the platform bits gfx.c / assets.c reach ---- */
const char *plat_getenv(const char *name) { return getenv(name); }
uint64_t plat_ticks_ms(void) { return 0; }
const char *plat_base_path(void) { return NULL; }
uint32_t *plat_image_load_rgba(const char *path, int *w, int *h) { (void)path; (void)w; (void)h; return NULL; }

static void put32(FILE *f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((v >> (8 * i)) & 255, f); }

static void save(const char *out, uint32_t id, int kind, const uint8_t *meta, size_t mlen)
{
    char path[1024]; snprintf(path, sizeof path, "%s/%08X.srgb", out, id);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite("SRGB", 1, 4, f); put32(f, (uint32_t)kind); put32(f, (uint32_t)cap_w); put32(f, (uint32_t)cap_h); put32(f, (uint32_t)mlen);
    fwrite(meta, 1, mlen, f);
    fwrite(cap_px, 4, (size_t)cap_w * cap_h, f);
    fclose(f);
    printf("%08X\n", id);   /* the order they come in the packs (the baked pack keeps it: a stage's graphics sit together) */
}

static void save_sprite(const char *out, uint32_t id, const Sprite *s)
{
    uint8_t m[8] = { 0 };
    m[0] = s->w & 255; m[1] = s->w >> 8; m[2] = s->h & 255; m[3] = s->h >> 8; m[4] = s->frames & 255; m[5] = s->frames >> 8;
    save(out, id, 1, m, 8);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: texprep <data_dir> <out_dir>\n"); return 2; }
    static const char *const PACKS[] = { "pack.pck", "common.pck", "levels.pck", "menu.pck", "level1.pck" };
    if (!packs_open(argv[1], PACKS, 5)) return 1;
    gfx_init(&ren);
    int n = 0;
    for (size_t pi = 0; pi < sizeof PACKS / sizeof *PACKS; pi++) {
        char path[1024]; snprintf(path, sizeof path, "%s/%s", argv[1], PACKS[pi]);
        Pack pk;
        if (!pack_load(&pk, path)) return 1;
        for (int i = 0; i < pk.count; i++) {
            uint32_t id = pk.entries[i].id;
            ResType t = pk.entries[i].type;
            if (t == RES_SPRITE) {
                Sprite *s = sprite_get(id);
                if (s && cap_px) { save_sprite(argv[2], id, s); n++; }
            } else if (t == RES_CBLOCK) {
                CBlock *c = cblock_get(id);
                if (!c || !cap_px) continue;
                int cells = c->frames * c->cols * c->rows;
                size_t mlen = 16 + (size_t)cells * 2;
                uint8_t *m = calloc(1, mlen);
                int v[7] = { c->frames, c->cols, c->rows, c->tw, c->th, c->ntiles, c->sheet_cols };
                for (int k = 0; k < 7; k++) { m[k * 2] = v[k] & 255; m[k * 2 + 1] = (v[k] >> 8) & 255; }
                for (int k = 0; k < cells; k++) { m[16 + k * 2] = c->cells[k] & 255; m[17 + k * 2] = c->cells[k] >> 8; }
                save(argv[2], id, 2, m, mlen);
                free(m); n++;
            } else if (t == RES_FONT) {
                Font *f = font_get(id);
                if (f && f->spr && cap_px) { save_sprite(argv[2], id, f->spr); n++; }
            } else continue;
            free(cap_px); cap_px = NULL;
            gfx_flush();
        }
        pack_free(&pk);
    }
    fprintf(stderr, "texprep: %d graphics\n", n);
    return 0;
}
