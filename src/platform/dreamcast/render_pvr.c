/* platform/render.h on the Dreamcast's PowerVR through KOS direct rendering: every draw call of a frame is written
 * straight into the store queues and fired at the tile accelerator (pvr_dr_target / pvr_dr_commit, as in KOS's
 * sh4zam "Bruce's Balls" example), all of it in the translucent list with autosort off, so the PVR draws in
 * submission order exactly like a 2D painter. A header goes out only when the texture / blend / filter changes.
 *
 * Textures are converted from RGBA8888 to the smallest 16-bit format that keeps them intact (RGB565 when opaque,
 * ARGB1555 for cut-outs, ARGB4444 when they carry real translucency) and stored non-twiddled with a power-of-two
 * row length, only as many rows as the image has. Anything wider or taller than 1024 is split into pages.
 * The logical screen (426x240 wide / 320x240 4:3) is scaled to 640x480 (plat_apply_screen). */
#include "pvr_internal.h"
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>

PvrView pvr_view = { 2.0f, 2.0f, 0.0f, 0.0f, 320, 240 };

struct Ren { int unused; };
static struct Ren the_ren;
Ren *rdc_renderer(void) { return &the_ren; }

/* ------------------------------------------------------------------ textures */
#define PAGE_MAX 1024
typedef struct {
    pvr_poly_hdr_t hdr[3][2];      /* [blend][linear], compiled on first use */
    pvr_ptr_t mem; size_t bytes;
    int x0, y0, w, h;              /* the part of the image this page holds */
    int tw, th;                    /* power-of-two size given to the PVR */
    uint8_t have;                  /* bit b*2+l: hdr[b][l] valid */
} Page;

struct RTex {
    int w, h; uint32_t fmt; bool streaming;
    int npx, npy; Page *pages;
    uint8_t r, g, b, a; RBlend blend; RScale scale; uint32_t tag;
};

static bool (*evict_hook)(void);
void r_set_evict_hook(bool (*hook)(void)) { evict_hook = hook; }

static size_t vram_used;
pvr_ptr_t rdc_vram_alloc(size_t bytes)
{
    for (int tries = 0; tries < 4096; tries++) {
        pvr_ptr_t p = pvr_mem_malloc(bytes);
        if (p) { vram_used += bytes; return p; }
        if (!evict_hook || !evict_hook()) break;
    }
    printf("vram: out of texture memory (%u bytes wanted, %u free, %u in use)\n", (unsigned)bytes, (unsigned)pvr_mem_available(), (unsigned)vram_used);
    return NULL;
}
static void vram_free(pvr_ptr_t p, size_t bytes) { if (p) { pvr_mem_free(p); vram_used -= bytes; } }
void rdc_vram_free(pvr_ptr_t p, size_t bytes) { vram_free(p, bytes); }
/* the header sent last this frame, by contents: floor levels and FMV frames recompile theirs in place, and a
 * freed page's memory can come back as another texture's header, so its address alone says nothing */
static pvr_poly_hdr_t last_hdr __attribute__((aligned(32)));
static bool have_last;
void rdc_forget_header(void) { have_last = false; }

static int pot(int n) { int p = 16; while (p < n) p <<= 1; return p; }

/* where a texture's RGBA rows come from: an image in memory, or a callback filling a few rows at a time */
typedef struct {
    const uint32_t *px; int pitch;
    RTexRows rows; void *ud; int w, h;
    uint32_t *buf; int y0, n, cap;
} Src;

static const uint32_t *src_row(Src *s, int y)
{
    if (s->px) return (const uint32_t *)((const uint8_t *)s->px + (size_t)y * s->pitch);
    if (y < s->y0 || y >= s->y0 + s->n) {
        s->y0 = y; s->n = s->h - y < s->cap ? s->h - y : s->cap;
        s->rows(s->ud, s->y0, s->n, s->buf);
    }
    return s->buf + (size_t)(y - s->y0) * s->w;
}

static uint32_t detect_format(Src *s)
{
    bool cut = false;
    for (int y = 0; y < s->h; y++) {
        const uint32_t *row = src_row(s, y);
        for (int x = 0; x < s->w; x++) {
            uint32_t a = row[x] >> 24;
            if (a == 255) continue;
            if (a != 0) return PVR_TXRFMT_ARGB4444;
            cut = true;
        }
    }
    return cut ? PVR_TXRFMT_ARGB1555 : PVR_TXRFMT_RGB565;
}

static inline uint16_t conv(uint32_t v, uint32_t fmt)
{
    uint32_t r = v & 0xff, g = (v >> 8) & 0xff, b = (v >> 16) & 0xff, a = v >> 24;
    if (fmt == PVR_TXRFMT_RGB565) return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
    if (fmt == PVR_TXRFMT_ARGB1555) return (uint16_t)((a >= 128) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
    return (uint16_t)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4);
}

/* convert and store a page, 32 rows at a time (no image-sized temporary) */
static void upload_page(RTex *t, Page *pg, Src *src)
{
    enum { CHUNK = 32 };
    static uint16_t buf[PAGE_MAX * CHUNK] __attribute__((aligned(32)));
    int rows = (int)(pg->bytes / ((size_t)pg->tw * 2));
    for (int y0 = 0; y0 < rows; y0 += CHUNK) {
        int n = rows - y0 < CHUNK ? rows - y0 : CHUNK;
        for (int y = 0; y < n; y++) {
            int sy = pg->y0 + (y0 + y < pg->h ? y0 + y : pg->h - 1);   /* the spare row repeats the last one */
            const uint32_t *row = src_row(src, sy) + pg->x0;
            uint16_t *dst = buf + (size_t)y * pg->tw;
            int x = 0;
            for (; x < pg->w; x++) dst[x] = conv(row[x], t->fmt);
            for (; x < pg->tw; x++) dst[x] = dst[pg->w - 1];            /* spare columns repeat the edge (linear filter) */
        }
        pvr_txr_load(buf, (uint8_t *)pg->mem + (size_t)y0 * pg->tw * 2, (size_t)n * pg->tw * 2);
    }
}

static void free_pages(RTex *t)
{
    for (int i = 0; i < t->npx * t->npy; i++) vram_free(t->pages[i].mem, t->pages[i].bytes);
    free(t->pages); t->pages = NULL;
}

static RTex *create(int w, int h, bool streaming, Src *src)
{
    if (w <= 0 || h <= 0) return NULL;
    RTex *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->w = w; t->h = h; t->streaming = streaming;
    t->r = t->g = t->b = t->a = 255; t->blend = R_BLEND_BLEND; t->scale = R_SCALE_NEAREST;
    t->fmt = src ? detect_format(src) : PVR_TXRFMT_RGB565;
    t->npx = (w + PAGE_MAX - 1) / PAGE_MAX; t->npy = (h + PAGE_MAX - 1) / PAGE_MAX;
    t->pages = memalign(32, sizeof(Page) * t->npx * t->npy);
    if (!t->pages) { free(t); return NULL; }
    memset(t->pages, 0, sizeof(Page) * t->npx * t->npy);
    for (int py = 0; py < t->npy; py++) for (int pxi = 0; pxi < t->npx; pxi++) {
        Page *pg = &t->pages[py * t->npx + pxi];
        pg->x0 = pxi * PAGE_MAX; pg->y0 = py * PAGE_MAX;
        pg->w = w - pg->x0 < PAGE_MAX ? w - pg->x0 : PAGE_MAX;
        pg->h = h - pg->y0 < PAGE_MAX ? h - pg->y0 : PAGE_MAX;
        pg->tw = pot(pg->w); pg->th = pot(pg->h);
        int rows = pg->h < pg->th ? pg->h + 1 : pg->h;     /* one spare row for the bilinear filter */
        pg->bytes = ((size_t)pg->tw * rows * 2 + 31) & ~(size_t)31;
        pg->mem = rdc_vram_alloc(pg->bytes);
        if (!pg->mem) { free_pages(t); free(t); return NULL; }
    }
    if (src) for (int i = 0; i < t->npx * t->npy; i++) upload_page(t, &t->pages[i], src);
    return t;
}

RTex *rtex_create(Ren *r, int w, int h, RTexAccess access, const uint32_t *px)
{
    (void)r;
    Src src = { .px = px, .pitch = w * 4, .w = w, .h = h };
    return create(w, h, access == R_TEX_STREAMING, px ? &src : NULL);
}

RTex *rtex_create_rows(Ren *r, int w, int h, RTexRows rows, void *ud)
{
    (void)r;
    int cap = 32768 / (w > 0 ? w : 1); if (cap < 1) cap = 1; if (cap > h) cap = h;
    Src src = { .rows = rows, .ud = ud, .w = w, .h = h, .buf = malloc((size_t)w * cap * 4), .y0 = -1, .n = 0, .cap = cap };
    if (!src.buf) return NULL;
    RTex *t = create(w, h, false, &src);
    free(src.buf);
    return t;
}

void rtex_update(RTex *t, const uint32_t *px, int pitch)
{
    if (!t || !px) return;
    Src src = { .px = px, .pitch = pitch, .w = t->w, .h = t->h };
    for (int i = 0; i < t->npx * t->npy; i++) upload_page(t, &t->pages[i], &src);
}
void rtex_destroy(RTex *t) { if (!t) return; free_pages(t); free(t); }
void rtex_size(const RTex *t, int *w, int *h) { *w = t ? t->w : 0; *h = t ? t->h : 0; }
void rtex_set_color_mod(RTex *t, uint8_t r, uint8_t g, uint8_t b) { if (t) { t->r = r; t->g = g; t->b = b; } }
void rtex_set_alpha_mod(RTex *t, uint8_t a) { if (t) t->a = a; }
void rtex_set_blend(RTex *t, RBlend b) { if (t) t->blend = b; }
void rtex_set_scale(RTex *t, RScale s) { if (t) t->scale = s; }
void rtex_set_tag(RTex *t, uint32_t tag) { if (t) t->tag = tag; }
Ren *rtex_renderer(const RTex *t) { (void)t; return &the_ren; }
size_t rdc_vram_used(void) { return vram_used; }

/* ------------------------------------------------------------------ headers */
void rdc_compile(pvr_poly_hdr_t *h, pvr_ptr_t base, uint32_t fmt, int tw, int th, RBlend blend, bool linear, bool repeat, bool offset)
{
    pvr_poly_cxt_t cxt;
    if (base) {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, fmt, tw, th, base, linear ? PVR_FILTER_BILINEAR : PVR_FILTER_NEAREST);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = repeat ? PVR_UVCLAMP_NONE : PVR_UVCLAMP_UV;
    } else pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.specular = offset;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = false;
    switch (blend) {
    case R_BLEND_NONE: cxt.blend.src = PVR_BLEND_ONE; cxt.blend.dst = PVR_BLEND_ZERO; break;
    case R_BLEND_ADD: cxt.blend.src = PVR_BLEND_SRCALPHA; cxt.blend.dst = PVR_BLEND_ONE; break;
    default: cxt.blend.src = PVR_BLEND_SRCALPHA; cxt.blend.dst = PVR_BLEND_INVSRCALPHA; break;
    }
    pvr_poly_compile(h, &cxt);
}

static const pvr_poly_hdr_t *page_hdr(RTex *t, Page *pg)
{
    int b = t->blend, l = t->scale == R_SCALE_LINEAR;
    if (!(pg->have & (1 << (b * 2 + l)))) {
        rdc_compile(&pg->hdr[b][l], pg->mem, t->fmt | PVR_TXRFMT_NONTWIDDLED, pg->tw, pg->th, (RBlend)b, l, false, false);
        pg->have |= (uint8_t)(1 << (b * 2 + l));
    }
    return &pg->hdr[b][l];
}

static pvr_poly_hdr_t col_hdr[3] __attribute__((aligned(32)));

/* ------------------------------------------------------------------ frame, state, clipping */
static bool in_frame;
static int prims, hdr_sent, hdr_asked;
static struct { uint8_t r, g, b, a; RBlend blend; bool clip_on; RRect clip; bool vp_on; RRect vp; } st = { 255, 255, 255, 255, R_BLEND_NONE, false, { 0 }, false, { 0 } };
static float clx0, cly0, clx1, cly1;   /* the effective clip in screen space */

static void update_clip(void)
{
    float x0 = pvr_view.ox, y0 = pvr_view.oy, x1 = pvr_view.ox + pvr_view.lw * pvr_view.sx, y1 = pvr_view.oy + pvr_view.lh * pvr_view.sy;
    float vx = st.vp_on ? (float)st.vp.x : 0, vy = st.vp_on ? (float)st.vp.y : 0;
    if (st.vp_on) {
        float a = pvr_view.ox + st.vp.x * pvr_view.sx, b = pvr_view.oy + st.vp.y * pvr_view.sy;
        float c = a + st.vp.w * pvr_view.sx, d = b + st.vp.h * pvr_view.sy;
        if (a > x0) x0 = a; if (b > y0) y0 = b; if (c < x1) x1 = c; if (d < y1) y1 = d;
    }
    if (st.clip_on) {
        float a = pvr_view.ox + (st.clip.x + vx) * pvr_view.sx, b = pvr_view.oy + (st.clip.y + vy) * pvr_view.sy;
        float c = a + st.clip.w * pvr_view.sx, d = b + st.clip.h * pvr_view.sy;
        if (a > x0) x0 = a; if (b > y0) y0 = b; if (c < x1) x1 = c; if (d < y1) y1 = d;
    }
    clx0 = x0; cly0 = y0; clx1 = x1; cly1 = y1;
}
void rdc_view_changed(void) { update_clip(); }
void rdc_clip_screen(float *x0, float *y0, float *x1, float *y1) { *x0 = clx0; *y0 = cly0; *x1 = clx1; *y1 = cly1; }

void rdc_init(void)
{
    for (int b = 0; b < 3; b++) rdc_compile(&col_hdr[b], NULL, 0, 0, 0, (RBlend)b, false, false, false);
    update_clip();
}

void rdc_frame_begin(void)
{
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_TR_POLY);
    in_frame = true; prims = hdr_sent = hdr_asked = 0; have_last = false;
    st.clip_on = st.vp_on = false; update_clip();
}

void rdc_frame_end(void)
{
    if (!in_frame) return;
    in_frame = false;
    pvr_list_finish();
    pvr_scene_finish();
}
bool rdc_in_frame(void) { return in_frame; }
int rdc_prims(void) { return prims; }
void rdc_header_stats(int *sent, int *asked) { *sent = hdr_sent; *asked = hdr_asked; }

/* ------------------------------------------------------------------ submission */
void rdc_header(const pvr_poly_hdr_t *h)
{
    if (!in_frame) return;
    hdr_asked++;
    if (have_last && !memcmp(&last_hdr, h, sizeof *h)) return;
    last_hdr = *h; have_last = true; hdr_sent++;
    void *dst = pvr_dr_target();
    memcpy(dst, h, sizeof *h);
    pvr_dr_commit(dst);
}

static inline void put(uint32_t flags, const RdcVert *v)
{
    pvr_vertex_t *d = pvr_dr_target();
    d->flags = flags; d->x = v->x; d->y = v->y; d->z = v->z;
    d->u = v->u; d->v = v->v; d->argb = v->argb; d->oargb = v->oargb;
    pvr_dr_commit(d);
}

void rdc_strip(const RdcVert *v, int n)
{
    if (!in_frame || n < 3) return;
    for (int i = 0; i < n - 1; i++) put(PVR_CMD_VERTEX, &v[i]);
    put(PVR_CMD_VERTEX_EOL, &v[n - 1]);
    prims++;
}

static inline uint32_t lerp_argb(uint32_t a, uint32_t b, float t)
{
    if (a == b) return a;
    uint32_t o = 0;
    for (int s = 0; s < 32; s += 8) {
        float ca = (float)((a >> s) & 0xff), cb = (float)((b >> s) & 0xff);
        int c = (int)(ca + (cb - ca) * t + 0.5f); c = c < 0 ? 0 : c > 255 ? 255 : c;
        o |= (uint32_t)c << s;
    }
    return o;
}
static inline RdcVert lerp_v(const RdcVert *a, const RdcVert *b, float t)
{
    RdcVert r;
    r.x = a->x + (b->x - a->x) * t; r.y = a->y + (b->y - a->y) * t; r.z = a->z + (b->z - a->z) * t;
    /* perspective-correct attributes: z is 1/w */
    float za = a->z, zb = b->z, zr = r.z;
    if (za != zb && zr != 0) {
        float ua = a->u * za, ub = b->u * zb, va = a->v * za, vb = b->v * zb;
        r.u = (ua + (ub - ua) * t) / zr; r.v = (va + (vb - va) * t) / zr;
    } else { r.u = a->u + (b->u - a->u) * t; r.v = a->v + (b->v - a->v) * t; }
    r.argb = lerp_argb(a->argb, b->argb, t); r.oargb = lerp_argb(a->oargb, b->oargb, t);
    return r;
}

/* Sutherland-Hodgman against one axis-aligned edge; side: 0 x>=k, 1 x<=k, 2 y>=k, 3 y<=k */
static int clip_edge(const RdcVert *in, int n, RdcVert *out, int side, float k)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const RdcVert *a = &in[i], *b = &in[(i + 1) % n];
        float da = side < 2 ? a->x : a->y, db = side < 2 ? b->x : b->y;
        bool ia = side & 1 ? da <= k : da >= k, ib = side & 1 ? db <= k : db >= k;
        if (ia) out[m++] = *a;
        if (ia != ib) out[m++] = lerp_v(a, b, (k - da) / (db - da));
    }
    return m;
}

void rdc_poly(const RdcVert *v, int n)
{
    if (!in_frame || n < 3 || n > 8) return;
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (int i = 1; i < n; i++) {
        if (v[i].x < x0) x0 = v[i].x; if (v[i].x > x1) x1 = v[i].x;
        if (v[i].y < y0) y0 = v[i].y; if (v[i].y > y1) y1 = v[i].y;
    }
    if (x1 <= clx0 || x0 >= clx1 || y1 <= cly0 || y0 >= cly1) return;   /* outside */
    RdcVert a[16], b[16]; const RdcVert *p = v; int m = n;
    if (x0 < clx0 || x1 > clx1 || y0 < cly0 || y1 > cly1) {
        memcpy(a, v, sizeof *v * n);
        m = clip_edge(a, m, b, 0, clx0); if (m < 3) return;
        m = clip_edge(b, m, a, 1, clx1); if (m < 3) return;
        m = clip_edge(a, m, b, 2, cly0); if (m < 3) return;
        m = clip_edge(b, m, a, 3, cly1); if (m < 3) return;
        p = a;
    }
    /* fan order -> strip order: 0, 1, m-1, 2, m-2, ... */
    RdcVert s[16]; int lo = 1, hi = m - 1, k = 0;
    s[k++] = p[0];
    while (lo <= hi) { s[k++] = p[lo++]; if (lo <= hi) s[k++] = p[hi--]; }
    rdc_strip(s, k);
}

/* ------------------------------------------------------------------ draw state */
void r_set_draw_color(Ren *r, uint8_t R, uint8_t G, uint8_t B, uint8_t A) { (void)r; st.r = R; st.g = G; st.b = B; st.a = A; }
void r_set_draw_blend(Ren *r, RBlend b) { (void)r; st.blend = b; }
void r_set_clip(Ren *r, const RRect *c) { (void)r; st.clip_on = c != NULL; if (c) st.clip = *c; update_clip(); }
void r_set_viewport(Ren *r, const RRect *vp) { (void)r; st.vp_on = vp != NULL; if (vp) st.vp = *vp; update_clip(); }
bool r_rect_intersect(const RRect *a, const RRect *b, RRect *out)
{
    int x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
    int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w, y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    out->x = x0; out->y = y0; out->w = x1 > x0 ? x1 - x0 : 0; out->h = y1 > y0 ? y1 - y0 : 0;
    return out->w > 0 && out->h > 0;
}

static inline float SX(float x) { return pvr_view.ox + (x + (st.vp_on ? st.vp.x : 0)) * pvr_view.sx; }
static inline float SY(float y) { return pvr_view.oy + (y + (st.vp_on ? st.vp.y : 0)) * pvr_view.sy; }
static inline uint32_t draw_argb(void) { return (uint32_t)st.a << 24 | (uint32_t)st.r << 16 | (uint32_t)st.g << 8 | st.b; }

static void fill_screen_quad(float x0, float y0, float x1, float y1)
{
    uint32_t c = draw_argb();
    RdcVert v[4] = { { x0, y0, 1, 0, 0, c, 0 }, { x1, y0, 1, 0, 0, c, 0 }, { x1, y1, 1, 0, 0, c, 0 }, { x0, y1, 1, 0, 0, c, 0 } };
    rdc_header(&col_hdr[st.blend]);
    rdc_poly(v, 4);
}

void r_clear(Ren *r)
{
    (void)r;
    if (prims == 0) pvr_set_bg_color(st.r / 255.0f, st.g / 255.0f, st.b / 255.0f);
    else { RBlend b = st.blend; st.blend = R_BLEND_NONE; fill_screen_quad(0, 0, 640, 480); st.blend = b; }
}

void r_fill_rect(Ren *r, const RFRect *q)
{
    (void)r;
    if (!q) {
        float w = st.vp_on ? st.vp.w : pvr_view.lw, h = st.vp_on ? st.vp.h : pvr_view.lh;
        fill_screen_quad(SX(0), SY(0), SX(w), SY(h));
        return;
    }
    if (q->w <= 0 || q->h <= 0) return;
    fill_screen_quad(SX(q->x), SY(q->y), SX(q->x + q->w), SY(q->y + q->h));
}
void r_fill_rects(Ren *r, const RFRect *q, int n) { for (int i = 0; i < n; i++) r_fill_rect(r, &q[i]); }
void r_rect(Ren *r, const RFRect *q)
{
    if (!q || q->w <= 0 || q->h <= 0) return;
    RFRect e[4] = { { q->x, q->y, q->w, 1 }, { q->x, q->y + q->h - 1, q->w, 1 }, { q->x, q->y + 1, 1, q->h - 2 }, { q->x + q->w - 1, q->y + 1, 1, q->h - 2 } };
    r_fill_rects(r, e, 4);
}
void r_point(Ren *r, float x, float y) { RFRect q = { floorf(x), floorf(y), 1, 1 }; r_fill_rect(r, &q); }

void r_line(Ren *r, float x0, float y0, float x1, float y1)
{
    (void)r;
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) { r_point(r, x0, y0); return; }
    dx /= len; dy /= len;
    /* through the pixel centres, half a pixel past each end, one pixel wide */
    float ax = x0 + 0.5f - dx * 0.5f, ay = y0 + 0.5f - dy * 0.5f, bx = x1 + 0.5f + dx * 0.5f, by = y1 + 0.5f + dy * 0.5f;
    float nx = -dy * 0.5f, ny = dx * 0.5f;
    uint32_t c = draw_argb();
    RdcVert v[4] = { { SX(ax + nx), SY(ay + ny), 1, 0, 0, c, 0 }, { SX(bx + nx), SY(by + ny), 1, 0, 0, c, 0 },
                     { SX(bx - nx), SY(by - ny), 1, 0, 0, c, 0 }, { SX(ax - nx), SY(ay - ny), 1, 0, 0, c, 0 } };
    rdc_header(&col_hdr[st.blend]);
    rdc_poly(v, 4);
}

/* ------------------------------------------------------------------ textured quads */
static void draw_tex(RTex *t, const RFRect *src, const RFRect *dst, double angle, const RFPoint *center, RFlip flip)
{
    if (!in_frame || !t || !t->pages) return;
    RFRect s = src ? *src : (RFRect){ 0, 0, (float)t->w, (float)t->h };
    RFRect d = dst ? *dst : (RFRect){ 0, 0, (float)(st.vp_on ? st.vp.w : pvr_view.lw), (float)(st.vp_on ? st.vp.h : pvr_view.lh) };
    if (s.w <= 0 || s.h <= 0 || d.w == 0 || d.h == 0) return;
    float kx = d.w / s.w, ky = d.h / s.h;
    /* keep the source inside the texture (SDL clips it and shrinks the destination with it) */
    if (s.x < 0) { d.x -= s.x * kx; d.w += s.x * kx; s.w += s.x; s.x = 0; }
    if (s.y < 0) { d.y -= s.y * ky; d.h += s.y * ky; s.h += s.y; s.y = 0; }
    if (s.x + s.w > t->w) { float cut = s.x + s.w - t->w; s.w -= cut; d.w -= cut * kx; }
    if (s.y + s.h > t->h) { float cut = s.y + s.h - t->h; s.h -= cut; d.h -= cut * ky; }
    if (s.w <= 0 || s.h <= 0) return;
    uint32_t argb = (uint32_t)t->a << 24 | (uint32_t)t->r << 16 | (uint32_t)t->g << 8 | t->b;
    float cx = center ? center->x : d.w * 0.5f, cy = center ? center->y : d.h * 0.5f;
    float ca = 1, sa = 0;
    if (angle != 0) { shz_sincos_t sc = shz_sincosf((float)(angle * (3.14159265358979 / 180.0))); ca = sc.cos; sa = sc.sin; }
    bool fh = flip & R_FLIP_H, fv = flip & R_FLIP_V;
    for (int i = 0; i < t->npx * t->npy; i++) {
        Page *pg = &t->pages[i];
        float px0 = s.x > pg->x0 ? s.x : (float)pg->x0, py0 = s.y > pg->y0 ? s.y : (float)pg->y0;
        float px1 = s.x + s.w < pg->x0 + pg->w ? s.x + s.w : (float)(pg->x0 + pg->w);
        float py1 = s.y + s.h < pg->y0 + pg->h ? s.y + s.h : (float)(pg->y0 + pg->h);
        if (px1 <= px0 || py1 <= py0) continue;
        /* texture corners -> destination-local positions (flip mirrors inside the destination) */
        float lx0 = (px0 - s.x) * kx, lx1 = (px1 - s.x) * kx, ly0 = (py0 - s.y) * ky, ly1 = (py1 - s.y) * ky;
        if (fh) { float a = d.w - lx1, b = d.w - lx0; lx0 = b; lx1 = a; }   /* lx0 now belongs to px0 */
        if (fv) { float a = d.h - ly1, b = d.h - ly0; ly0 = b; ly1 = a; }
        float u0 = (px0 - pg->x0) / pg->tw, u1 = (px1 - pg->x0) / pg->tw, v0 = (py0 - pg->y0) / pg->th, v1 = (py1 - pg->y0) / pg->th;
        float lx[4] = { lx0, lx1, lx1, lx0 }, ly[4] = { ly0, ly0, ly1, ly1 }, uu[4] = { u0, u1, u1, u0 }, vv[4] = { v0, v0, v1, v1 };
        RdcVert v[4];
        for (int k = 0; k < 4; k++) {
            float x = lx[k], y = ly[k];
            if (angle != 0) { float rx = x - cx, ry = y - cy; x = cx + rx * ca - ry * sa; y = cy + rx * sa + ry * ca; }
            v[k] = (RdcVert){ SX(d.x + x), SY(d.y + y), 1, uu[k], vv[k], argb, 0 };
        }
        rdc_header(page_hdr(t, pg));
        rdc_poly(v, 4);
    }
}

void r_tex(Ren *r, RTex *t, const RFRect *src, const RFRect *dst) { (void)r; draw_tex(t, src, dst, 0, NULL, R_FLIP_NONE); }
void r_tex_rot(Ren *r, RTex *t, const RFRect *src, const RFRect *dst, double angle, const RFPoint *center, RFlip flip) { (void)r; draw_tex(t, src, dst, angle, center, flip); }

static inline uint32_t fcolor(const RFColor *c)
{
    int a = (int)(c->a * 255 + 0.5f), r = (int)(c->r * 255 + 0.5f), g = (int)(c->g * 255 + 0.5f), b = (int)(c->b * 255 + 0.5f);
    return (uint32_t)(a & 255) << 24 | (uint32_t)(r & 255) << 16 | (uint32_t)(g & 255) << 8 | (uint32_t)(b & 255);
}

void r_geometry(Ren *r, RTex *t, const RVertex *vx, int nv, const int *idx, int ni)
{
    (void)r;
    if (!in_frame) return;
    int n = idx ? ni : nv;
    Page *pg = t && t->pages ? &t->pages[0] : NULL;
    rdc_header(pg ? page_hdr(t, pg) : &col_hdr[st.blend]);
    for (int i = 0; i + 2 < n; i += 3) {
        RdcVert v[3];
        for (int k = 0; k < 3; k++) {
            const RVertex *s = &vx[idx ? idx[i + k] : i + k];
            float u = pg ? s->tex_coord.x * t->w / pg->tw : 0, vv = pg ? s->tex_coord.y * t->h / pg->th : 0;
            v[k] = (RdcVert){ SX(s->position.x), SY(s->position.y), 1, u, vv, fcolor(&s->color), 0 };
        }
        rdc_poly(v, 3);
    }
}

/* ------------------------------------------------------------------ helpers for floor_pvr.c */
void rdc_xform(float x, float y, float *X, float *Y) { *X = SX(x); *Y = SY(y); }
void rdc_floor_haze(float ya, float yb, uint32_t haze, float sw)
{
    uint32_t c = 0xff000000u | (haze & 0xff) << 16 | ((haze >> 8) & 0xff) << 8 | ((haze >> 16) & 0xff);
    RdcVert v[4] = { { SX(0), SY(ya), 1, 0, 0, c, 0 }, { SX(sw), SY(ya), 1, 0, 0, c, 0 }, { SX(sw), SY(yb), 1, 0, 0, c, 0 }, { SX(0), SY(yb), 1, 0, 0, c, 0 } };
    rdc_header(&col_hdr[R_BLEND_NONE]);
    rdc_poly(v, 4);
}
