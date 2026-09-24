/* platform/render.h with no output: textures only remember their size, draws are counted. For headless tests and the
 * bring-up of a new port (the Saturn's first milestone runs the whole game on it). Baked blocks ("PVT1" / "SAT1")
 * are accepted: their header gives the size (the pixels are not looked at). */
#include "../render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SABER_DRAWLOG=file (the headless build, main_null.c): every draw call of every frame, one line each, for comparing
 * two builds' rendering (texture tag, source and destination rectangles, angle, colour state) */
static FILE *dlog; static int dlog_on = -1;
static bool dl(void)
{
    if (dlog_on < 0) { const char *p = getenv("SABER_DRAWLOG"); dlog = p ? fopen(p, "w") : NULL; dlog_on = dlog != NULL; }
    return dlog_on;
}
void rnull_frame_end(unsigned frame) { if (dl()) fprintf(dlog, "F %u\n", frame); }
#define R4(q) (q) ? (double)(q)->x : -1.0, (q) ? (double)(q)->y : -1.0, (q) ? (double)(q)->w : -1.0, (q) ? (double)(q)->h : -1.0

struct Ren { int prims; uint8_t r, g, b, a; int blend; };
struct RTex { int w, h; Ren *r; uint32_t tag; uint8_t mr, mg, mb, ma; int blend; };
struct RFloor { int unused; };
static Ren ren;

Ren *rnull_renderer(void) { return &ren; }
int  rnull_prims(void) { int n = ren.prims; ren.prims = 0; return n; }

static RTex *make(Ren *r, int w, int h)
{
    RTex *t = calloc(1, sizeof *t);
    if (t) { t->w = w; t->h = h; t->r = r; t->mr = t->mg = t->mb = t->ma = 255; t->blend = R_BLEND_BLEND; }
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
void  rtex_set_color_mod(RTex *t, uint8_t r, uint8_t g, uint8_t b) { if (t) { t->mr = r; t->mg = g; t->mb = b; } }
void  rtex_set_alpha_mod(RTex *t, uint8_t a) { if (t) t->ma = a; }
void  rtex_set_blend(RTex *t, RBlend b) { if (t) t->blend = b; }
void  rtex_set_scale(RTex *t, RScale s) { (void)t; (void)s; }
void  rtex_set_tag(RTex *t, uint32_t tag) { if (t) t->tag = tag; }
Ren  *rtex_renderer(const RTex *t) { return t ? t->r : &ren; }
void  r_set_evict_hook(bool (*hook)(void)) { (void)hook; }

void r_set_draw_color(Ren *r, uint8_t R, uint8_t G, uint8_t B, uint8_t A) { r->r = R; r->g = G; r->b = B; r->a = A; }
void r_set_draw_blend(Ren *r, RBlend b) { r->blend = b; }
void r_set_clip(Ren *r, const RRect *c) { (void)r; if (dl()) { if (c) fprintf(dlog, "clip %d %d %d %d\n", c->x, c->y, c->w, c->h); else fprintf(dlog, "clip -\n"); } }
void r_set_viewport(Ren *r, const RRect *v) { (void)r; if (dl()) { if (v) fprintf(dlog, "vp %d %d %d %d\n", v->x, v->y, v->w, v->h); else fprintf(dlog, "vp -\n"); } }
bool r_rect_intersect(const RRect *a, const RRect *b, RRect *out)
{
    int x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
    int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w, y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    if (x1 <= x0 || y1 <= y0) { if (out) *out = (RRect){ 0, 0, 0, 0 }; return false; }
    if (out) *out = (RRect){ x0, y0, x1 - x0, y1 - y0 };
    return true;
}
void r_clear(Ren *r) { r->prims++; if (dl()) fprintf(dlog, "clear %u %u %u\n", r->r, r->g, r->b); }
void r_fill_rect(Ren *r, const RFRect *q)
{
    r->prims++;
    if (dl()) fprintf(dlog, "fill %.2f %.2f %.2f %.2f c %u %u %u %u b%d\n", R4(q), r->r, r->g, r->b, r->a, r->blend);
}
void r_fill_rects(Ren *r, const RFRect *q, int n) { for (int i = 0; i < n; i++) r_fill_rect(r, &q[i]); }
void r_rect(Ren *r, const RFRect *q) { r->prims++; if (dl()) fprintf(dlog, "rect %.2f %.2f %.2f %.2f c %u %u %u %u\n", R4(q), r->r, r->g, r->b, r->a); }
void r_line(Ren *r, float x0, float y0, float x1, float y1) { r->prims++; if (dl()) fprintf(dlog, "line %.2f %.2f %.2f %.2f\n", (double)x0, (double)y0, (double)x1, (double)y1); }
void r_point(Ren *r, float x, float y) { r->prims++; if (dl()) fprintf(dlog, "point %.2f %.2f\n", (double)x, (double)y); }
static void log_tex(const char *op, const RTex *t, const RFRect *s, const RFRect *d, double a, const RFPoint *c, RFlip f)
{
    if (!dl()) return;
    fprintf(dlog, "%s %08X s %.2f %.2f %.2f %.2f d %.2f %.2f %.2f %.2f", op, t ? (unsigned)t->tag : 0u, R4(s), R4(d));
    if (a != 0 || c || f) fprintf(dlog, " a %.3f c %.2f %.2f f%d", a, c ? (double)c->x : -1.0, c ? (double)c->y : -1.0, (int)f);
    if (t && (t->mr & t->mg & t->mb & t->ma) != 255) fprintf(dlog, " m %u %u %u %u", t->mr, t->mg, t->mb, t->ma);
    if (t && t->blend != R_BLEND_BLEND) fprintf(dlog, " b%d", t->blend);
    fputc('\n', dlog);
}
void r_tex(Ren *r, RTex *t, const RFRect *s, const RFRect *d) { r->prims++; log_tex("tex", t, s, d, 0, NULL, R_FLIP_NONE); }
void r_tex_rot(Ren *r, RTex *t, const RFRect *s, const RFRect *d, double a, const RFPoint *c, RFlip f) { r->prims++; log_tex("rot", t, s, d, a, c, f); }
void r_tex_batch(Ren *r, RTex *t, const RFRect *s, const RFRect *d, int n) { r->prims += n; for (int i = 0; i < n; i++) log_tex("tile", t, &s[i], &d[i], 0, NULL, R_FLIP_NONE); }
void r_geometry(Ren *r, RTex *t, const RVertex *v, int nv, const int *idx, int ni)
{
    r->prims += (idx ? ni : nv) / 3;
    if (!dl()) return;
    fprintf(dlog, "geom %08X %d", t ? (unsigned)t->tag : 0u, idx ? ni : nv);
    for (int i = 0; i < (idx ? ni : nv); i++) { const RVertex *p = &v[idx ? idx[i] : i]; fprintf(dlog, " %.2f,%.2f", (double)p->position.x, (double)p->position.y); }
    fputc('\n', dlog);
}

RFloor *r_floor_create(Ren *r, const RFloorDesc *d) { (void)r; (void)d; return calloc(1, sizeof(RFloor)); }
void    r_floor_cells_changed(RFloor *f) { (void)f; }
void    r_floor_draw(Ren *r, RFloor *f, const RFloorView *v)
{
    (void)f; r->prims++;
    if (dl()) fprintf(dlog, "floor cam %.2f %.2f dir %.4f %.4f h %.2f focal %.2f horizon %.2f rows %d %d\n", (double)v->cam_x, (double)v->cam_y,
                      (double)v->fx, (double)v->fy, (double)v->cam_h, (double)v->focal, (double)v->horizon, v->y0, v->y1);
}
void    r_floor_destroy(RFloor *f) { free(f); }
bool    r_layer(Ren *r, uint32_t level, int layer, float cam_x, float cam_y) { (void)r; (void)level; (void)layer; (void)cam_x; (void)cam_y; return false; }
void    r_set_depth(Ren *r, int layer) { (void)r; (void)layer; }
