/* platform/render.h with no output: textures only remember their size, draws are counted. For headless tests and the
 * bring-up of a new port (the Saturn's first milestone runs the whole game on it). Baked blocks ("PVT1" / "SAT1")
 * are accepted: their header gives the size (the pixels are not looked at). */
#include "../render.h"
#include <stdlib.h>
#include <string.h>

struct Ren { int prims; };
struct RTex { int w, h; Ren *r; };
struct RFloor { int unused; };
static Ren ren;

Ren *rnull_renderer(void) { return &ren; }
int  rnull_prims(void) { int n = ren.prims; ren.prims = 0; return n; }

static RTex *make(Ren *r, int w, int h)
{
    RTex *t = calloc(1, sizeof *t);
    if (t) { t->w = w; t->h = h; t->r = r; }
    return t;
}
RTex *rtex_create(Ren *r, int w, int h, RTexAccess a, const uint32_t *px) { (void)a; (void)px; return make(r, w, h); }
RTex *rtex_create_rows(Ren *r, int w, int h, RTexRows rows, void *ud) { (void)rows; (void)ud; return make(r, w, h); }
RTex *rtex_create_baked(Ren *r, uint8_t *b, size_t n)
{
    if (n < 32 || (memcmp(b, "PVT1", 4) && memcmp(b, "SAT1", 4))) return NULL;
    return make(r, b[4] | b[5] << 8, b[6] | b[7] << 8);
}
void  rtex_update(RTex *t, const uint32_t *px, int pitch) { (void)t; (void)px; (void)pitch; }
void  rtex_destroy(RTex *t) { free(t); }
void  rtex_size(const RTex *t, int *w, int *h) { if (w) *w = t ? t->w : 0; if (h) *h = t ? t->h : 0; }
void  rtex_set_color_mod(RTex *t, uint8_t r, uint8_t g, uint8_t b) { (void)t; (void)r; (void)g; (void)b; }
void  rtex_set_alpha_mod(RTex *t, uint8_t a) { (void)t; (void)a; }
void  rtex_set_blend(RTex *t, RBlend b) { (void)t; (void)b; }
void  rtex_set_scale(RTex *t, RScale s) { (void)t; (void)s; }
void  rtex_set_tag(RTex *t, uint32_t tag) { (void)t; (void)tag; }
Ren  *rtex_renderer(const RTex *t) { return t ? t->r : &ren; }
void  r_set_evict_hook(bool (*hook)(void)) { (void)hook; }

void r_set_draw_color(Ren *r, uint8_t R, uint8_t G, uint8_t B, uint8_t A) { (void)r; (void)R; (void)G; (void)B; (void)A; }
void r_set_draw_blend(Ren *r, RBlend b) { (void)r; (void)b; }
void r_set_clip(Ren *r, const RRect *c) { (void)r; (void)c; }
void r_set_viewport(Ren *r, const RRect *v) { (void)r; (void)v; }
bool r_rect_intersect(const RRect *a, const RRect *b, RRect *out)
{
    int x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
    int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w, y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    if (x1 <= x0 || y1 <= y0) { if (out) *out = (RRect){ 0, 0, 0, 0 }; return false; }
    if (out) *out = (RRect){ x0, y0, x1 - x0, y1 - y0 };
    return true;
}
void r_clear(Ren *r) { r->prims++; }
void r_fill_rect(Ren *r, const RFRect *q) { (void)q; r->prims++; }
void r_fill_rects(Ren *r, const RFRect *q, int n) { (void)q; r->prims += n; }
void r_rect(Ren *r, const RFRect *q) { (void)q; r->prims++; }
void r_line(Ren *r, float x0, float y0, float x1, float y1) { (void)x0; (void)y0; (void)x1; (void)y1; r->prims++; }
void r_point(Ren *r, float x, float y) { (void)x; (void)y; r->prims++; }
void r_tex(Ren *r, RTex *t, const RFRect *s, const RFRect *d) { (void)t; (void)s; (void)d; r->prims++; }
void r_tex_rot(Ren *r, RTex *t, const RFRect *s, const RFRect *d, double a, const RFPoint *c, RFlip f) { (void)t; (void)s; (void)d; (void)a; (void)c; (void)f; r->prims++; }
void r_tex_batch(Ren *r, RTex *t, const RFRect *s, const RFRect *d, int n) { (void)t; (void)s; (void)d; r->prims += n; }
void r_geometry(Ren *r, RTex *t, const RVertex *v, int nv, const int *idx, int ni) { (void)t; (void)v; (void)nv; (void)idx; r->prims += (idx ? ni : nv) / 3; }

RFloor *r_floor_create(Ren *r, const RFloorDesc *d) { (void)r; (void)d; return calloc(1, sizeof(RFloor)); }
void    r_floor_cells_changed(RFloor *f) { (void)f; }
void    r_floor_draw(Ren *r, RFloor *f, const RFloorView *v) { (void)f; (void)v; r->prims++; }
void    r_floor_destroy(RFloor *f) { free(f); }
bool    r_layer(Ren *r, uint32_t level, int layer, float cam_x, float cam_y) { (void)r; (void)level; (void)layer; (void)cam_x; (void)cam_y; return false; }
void    r_set_depth(Ren *r, int layer) { (void)r; (void)layer; }
