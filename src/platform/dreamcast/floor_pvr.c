/* The floor plane (render.h r_floor_*) on the PowerVR, drawn as perspective-correct textured strips.
 *
 * The screen rows below the horizon are cut into distance bands. Along a screen row the ground's world position is
 * linear in x, so one quad per band strip (z = 1/distance) is textured exactly like the software raster's rows,
 * and a few rows per strip keep the per-vertex fog (vertex colour x texture + offset colour = the haze) close
 * to the per-row fog of the PC version.
 *
 * Mode 7 (a material map): the ground is baked into clipmaps, one 512x512 8-bit paletted (twiddled) texture per
 * band, level L at 2^L world units per texel (mip L of the materials), each a toroidal window around the camera
 * that the PVR samples with U/V repeat. When the camera moves, only the 8x8-texel blocks entering a window are
 * rebuilt (64 contiguous bytes in twiddled order, written straight into VRAM). A band ends where its trapezoid
 * (at any heading) would leave the window, so band L covers distances D0*2^(L-1) .. D0*2^L.
 * One material (Ramrod's floor): each mip level is its own repeating texture, the bands follow the software's
 * mip thresholds exactly. */
#include "pvr_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>

#define WIN 512                 /* clipmap size in texels */
#define WB (WIN / 8)            /* ... in 8x8 blocks */
#define MARGIN 32               /* texels kept unused at the window edge (updates land there while a frame renders) */
#define MAX_LEV 8

void rdc_xform(float x, float y, float *X, float *Y);

typedef struct { pvr_ptr_t mem; size_t bytes; int bx0, by0; bool valid; int tw; pvr_poly_hdr_t hdr __attribute__((aligned(32))); } Level;

struct RFloor {
    RFloorDesc d; int tex_shift;
    bool single;
    int nlev;
    Level lev[MAX_LEV];
    uint8_t *idx[MAX_LEV * 16];        /* [m * MAX_LEV + L]: material m at mip L as palette indices (clipmap path) */
    int msz[MAX_LEV];                  /* size of mip L (>= 1) */
    uint16_t pal[256];
    uint8_t tw_lx[64], tw_ly[64];      /* twiddled byte k of a block -> texel (lx, ly) */
};

static uint32_t twid(uint32_t v) { uint32_t r = 0; for (int b = 0; b < 10; b++) r |= ((v >> b) & 1) << (2 * b); return r; }
static inline uint16_t rgb565(uint32_t c) { return (uint16_t)(((c & 0xff) >> 3) << 11 | (((c >> 8) & 0xff) >> 2) << 5 | ((c >> 16) & 0xff) >> 3); }

/* box-filtered mip L+1 of an n x n RGBA image (n >= 2) */
static uint32_t *box(const uint32_t *src, int n)
{
    int h = n / 2; uint32_t *dst = malloc((size_t)h * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < h; x++) {
        uint32_t c[4] = { src[2 * y * n + 2 * x], src[2 * y * n + 2 * x + 1], src[(2 * y + 1) * n + 2 * x], src[(2 * y + 1) * n + 2 * x + 1] };
        uint32_t r = 0, g = 0, b = 0;
        for (int k = 0; k < 4; k++) { r += c[k] & 0xff; g += (c[k] >> 8) & 0xff; b += (c[k] >> 16) & 0xff; }
        dst[y * h + x] = 0xff000000u | ((b + 2) / 4) << 16 | ((g + 2) / 4) << 8 | ((r + 2) / 4);
    }
    return dst;
}

/* ---- palette: exact when the materials use <= 256 colours, else a median cut ---- */
typedef struct { uint16_t c; uint32_t n; } CCount;
static int cmp_r(const void *a, const void *b) { return (int)(((const CCount *)a)->c >> 11) - (int)(((const CCount *)b)->c >> 11); }
static int cmp_g(const void *a, const void *b) { return (int)((((const CCount *)a)->c >> 5) & 63) - (int)((((const CCount *)b)->c >> 5) & 63); }
static int cmp_b(const void *a, const void *b) { return (int)(((const CCount *)a)->c & 31) - (int)(((const CCount *)b)->c & 31); }

static int build_palette(CCount *cc, int n, uint16_t *pal)
{
    if (n <= 256) { for (int i = 0; i < n; i++) pal[i] = cc[i].c; return n; }
    typedef struct { int lo, hi; } Box;
    Box boxes[256]; int nb = 1; boxes[0] = (Box){ 0, n };
    while (nb < 256) {
        int best = -1, bestr = 0, axis = 0;
        for (int i = 0; i < nb; i++) {
            if (boxes[i].hi - boxes[i].lo < 2) continue;
            int mn[3] = { 99, 99, 99 }, mx[3] = { 0, 0, 0 };
            for (int k = boxes[i].lo; k < boxes[i].hi; k++) {
                int c[3] = { cc[k].c >> 11, ((cc[k].c >> 5) & 63) >> 1, cc[k].c & 31 };
                for (int a = 0; a < 3; a++) { if (c[a] < mn[a]) mn[a] = c[a]; if (c[a] > mx[a]) mx[a] = c[a]; }
            }
            for (int a = 0; a < 3; a++) if (mx[a] - mn[a] > bestr) { bestr = mx[a] - mn[a]; best = i; axis = a; }
        }
        if (best < 0) break;
        Box *bx = &boxes[best];
        qsort(cc + bx->lo, bx->hi - bx->lo, sizeof *cc, axis == 0 ? cmp_r : axis == 1 ? cmp_g : cmp_b);
        uint64_t tot = 0, half = 0; for (int k = bx->lo; k < bx->hi; k++) tot += cc[k].n;
        int mid = bx->lo + 1;
        for (int k = bx->lo; k < bx->hi - 1; k++) { half += cc[k].n; if (half * 2 >= tot) { mid = k + 1; break; } }
        boxes[nb++] = (Box){ mid, bx->hi }; bx->hi = mid;
    }
    for (int i = 0; i < nb; i++) {
        uint64_t r = 0, g = 0, b = 0, w = 0;
        for (int k = boxes[i].lo; k < boxes[i].hi; k++) { r += (uint64_t)(cc[k].c >> 11) * cc[k].n; g += (uint64_t)((cc[k].c >> 5) & 63) * cc[k].n; b += (uint64_t)(cc[k].c & 31) * cc[k].n; w += cc[k].n; }
        if (!w) w = 1;
        pal[i] = (uint16_t)((r / w) << 11 | (g / w) << 5 | (b / w));
    }
    return nb;
}
static int nearest(const uint16_t *pal, int n, uint16_t c)
{
    int best = 0, bd = 1 << 30, r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    for (int i = 0; i < n; i++) {
        int dr = r - (pal[i] >> 11), dg = (g - ((pal[i] >> 5) & 63)) / 2, db = b - (pal[i] & 31), dd = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (dd < bd) { bd = dd; best = i; if (!dd) break; }
    }
    return best;
}

/* ---- creation ---- */
static bool create_single(RFloor *f)
{
    const RFloorDesc *d = &f->d;
    f->nlev = d->mips < MAX_LEV ? d->mips : MAX_LEV;
    for (int L = 0; L < f->nlev; L++) {
        int n = d->tex >> L; if (n < 8) { f->nlev = L; break; }
        Level *lv = &f->lev[L];
        lv->tw = n; lv->bytes = (size_t)n * n * 2;
        lv->mem = rdc_vram_alloc(lv->bytes);
        if (!lv->mem) return false;
        uint16_t *tmp = malloc(lv->bytes); if (!tmp) return false;
        const uint32_t *src = d->mat[L];
        for (int i = 0; i < n * n; i++) tmp[i] = rgb565(src[i]);
        pvr_txr_load_ex(tmp, lv->mem, n, n, PVR_TXRLOAD_16BPP);   /* twiddles */
        free(tmp);
        rdc_compile(&lv->hdr, lv->mem, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, n, n, R_BLEND_NONE, false, true, true);
    }
    return f->nlev > 0;
}

static bool create_clipmap(RFloor *f)
{
    const RFloorDesc *d = &f->d;
    /* every material at every level down to 1x1, as RGBA, then one palette over all of them */
    int levels = f->tex_shift + 1; if (levels > MAX_LEV) levels = MAX_LEV;
    uint32_t *rgba[16 * MAX_LEV] = { 0 };
    int nm = d->nmat < 16 ? d->nmat : 16;
    for (int m = 0; m < nm; m++) for (int L = 0; L < levels; L++) {
        int n = d->tex >> L;
        if (L < d->mips) { rgba[m * MAX_LEV + L] = malloc((size_t)n * n * 4); memcpy(rgba[m * MAX_LEV + L], d->mat[m * d->mips + L], (size_t)n * n * 4); }
        else rgba[m * MAX_LEV + L] = box(rgba[m * MAX_LEV + L - 1], n * 2);
    }
    for (int L = 0; L < MAX_LEV; L++) { int n = d->tex >> (L < levels ? L : levels - 1); f->msz[L] = n < 1 ? 1 : n; }
    /* colour census */
    CCount *cc = malloc(sizeof(CCount) * 65536); int ncc = 0;
    static uint32_t count[65536]; memset(count, 0, sizeof count);
    for (int m = 0; m < nm; m++) for (int L = 0; L < levels; L++) { int n = d->tex >> L; for (int i = 0; i < n * n; i++) count[rgb565(rgba[m * MAX_LEV + L][i])]++; }
    for (int c = 0; c < 65536; c++) if (count[c]) cc[ncc++] = (CCount){ (uint16_t)c, count[c] };
    int np = build_palette(cc, ncc, f->pal);
    free(cc);
    static uint8_t map[65536];
    for (int c = 0; c < 65536; c++) if (count[c]) map[c] = (uint8_t)nearest(f->pal, np, (uint16_t)c);
    for (int m = 0; m < nm; m++) for (int L = 0; L < MAX_LEV; L++) {
        int LL = L < levels ? L : levels - 1, n = d->tex >> LL;
        uint8_t *ix = malloc((size_t)n * n);
        for (int i = 0; i < n * n; i++) ix[i] = map[rgb565(rgba[m * MAX_LEV + LL][i])];
        f->idx[m * MAX_LEV + L] = ix;
    }
    for (int i = 0; i < 16 * MAX_LEV; i++) free(rgba[i]);
    pvr_set_pal_format(PVR_PAL_RGB565);
    for (int i = 0; i < np; i++) pvr_set_pal_entry(i, f->pal[i]);
    for (int k = 0; k < 64; k++) {   /* twiddled order inside an 8x8 block: bit 0 = y0, bit 1 = x0, ... */
        int lx = 0, ly = 0;
        for (int b = 0; b < 3; b++) { ly |= ((k >> (2 * b)) & 1) << b; lx |= ((k >> (2 * b + 1)) & 1) << b; }
        f->tw_lx[k] = (uint8_t)lx; f->tw_ly[k] = (uint8_t)ly;
    }
    f->nlev = 0;   /* the textures are made when a view needs them (their count depends on the view) */
    return true;
}

RFloor *r_floor_create(Ren *r, const RFloorDesc *d)
{
    (void)r;
    RFloor *f = memalign(32, sizeof(RFloor));
    if (!f) return NULL;
    memset(f, 0, sizeof *f);
    f->d = *d;
    while ((1 << f->tex_shift) < d->tex) f->tex_shift++;
    f->single = d->mapn == 1 && d->nmat == 1;
    if (!(f->single ? create_single(f) : create_clipmap(f))) { r_floor_destroy(f); return NULL; }
    return f;
}

void r_floor_cells_changed(RFloor *f) { if (f) for (int L = 0; L < MAX_LEV; L++) f->lev[L].valid = false; }

void r_floor_destroy(RFloor *f)
{
    if (!f) return;
    rdc_forget_header();
    for (int L = 0; L < MAX_LEV; L++) if (f->lev[L].mem) rdc_vram_free(f->lev[L].mem, f->lev[L].bytes);
    for (int i = 0; i < MAX_LEV * 16; i++) free(f->idx[i]);
    free(f);
}

/* ---- clipmap upkeep ---- */
static void build_block(RFloor *f, int L, int bx, int by)
{
    const RFloorDesc *d = &f->d;
    uint32_t mask = (uint32_t)d->mapn - 1; int cs = d->cell_shift, msz = f->msz[L], mm = msz - 1;
    uint32_t words[16];
    uint8_t *b = (uint8_t *)words;
    for (int k = 0; k < 64; k++) {
        int tx = bx * 8 + f->tw_lx[k], ty = by * 8 + f->tw_ly[k];
        uint32_t wx = (uint32_t)tx << L, wy = (uint32_t)ty << L;
        uint8_t m = d->cells[((wy >> cs) & mask) * d->mapn + ((wx >> cs) & mask)];
        if (m >= 16) m = 0;
        b[k] = f->idx[m * MAX_LEV + L][(ty & mm) * msz + (tx & mm)];
    }
    uint32_t off = twid((uint32_t)(by & (WB - 1)) * 8) | twid((uint32_t)(bx & (WB - 1)) * 8) << 1;
    volatile uint32_t *dst = (volatile uint32_t *)((uint8_t *)f->lev[L].mem + off);
    for (int i = 0; i < 16; i++) dst[i] = words[i];
}

static void keep_window(RFloor *f, int L, float cam_x, float cam_y)
{
    Level *lv = &f->lev[L];
    if (!lv->mem) {
        lv->bytes = WIN * WIN; lv->tw = WIN;
        lv->mem = rdc_vram_alloc(lv->bytes);
        if (!lv->mem) return;
        rdc_compile(&lv->hdr, lv->mem, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(0) | PVR_TXRFMT_TWIDDLED, WIN, WIN, R_BLEND_NONE, false, true, true);
        lv->valid = false;
    }
    float scale = 1.0f / (float)(1 << L);
    int bx0 = (int)floorf(cam_x * scale / 8) - WB / 2, by0 = (int)floorf(cam_y * scale / 8) - WB / 2;
    if (!lv->valid || abs(bx0 - lv->bx0) >= WB || abs(by0 - lv->by0) >= WB) {
        for (int by = by0; by < by0 + WB; by++) for (int bx = bx0; bx < bx0 + WB; bx++) build_block(f, L, bx, by);
    } else {
        /* columns entering on the side we moved to, over the new rows; then rows entering, over all columns */
        int ox = lv->bx0, oy = lv->by0;
        if (bx0 > ox) { for (int bx = ox + WB; bx < bx0 + WB; bx++) for (int by = by0; by < by0 + WB; by++) build_block(f, L, bx, by); }
        else if (bx0 < ox) { for (int bx = bx0; bx < ox; bx++) for (int by = by0; by < by0 + WB; by++) build_block(f, L, bx, by); }
        int cx0 = bx0 > ox ? bx0 : ox, cx1 = (bx0 > ox ? ox : bx0) + WB;   /* columns already handled above are skipped */
        if (by0 > oy) { for (int by = oy + WB; by < by0 + WB; by++) for (int bx = bx0; bx < bx0 + WB; bx++) if (bx >= cx0 && bx < cx1) build_block(f, L, bx, by); }
        else if (by0 < oy) { for (int by = by0; by < oy; by++) for (int bx = bx0; bx < bx0 + WB; bx++) if (bx >= cx0 && bx < cx1) build_block(f, L, bx, by); }
    }
    lv->bx0 = bx0; lv->by0 = by0; lv->valid = true;
}

/* ---- drawing ---- */
typedef struct { const RFloorView *v; float rx, ry, half; } Ctx;

static float row_dist(const RFloorView *v, float y) { return v->cam_h * v->focal / (y + v->row_off - v->horizon); }
static float dist_row(const RFloorView *v, float dd) { return v->horizon + v->cam_h * v->focal / dd - v->row_off; }

static void fog_cols(const RFloorView *v, float dd, uint32_t *argb, uint32_t *oargb)
{
    float fog = (dd - v->fog0) / (v->fog1 - v->fog0); fog = fog < 0 ? 0 : fog > 1 ? 1 : fog;
    int fa = (int)(fog * v->fog_max); if (fa > 256) fa = 256;
    int keep = 256 - fa; if (keep > 255) keep = 255;
    uint32_t h = v->haze;
    uint32_t R = ((h & 0xff) * fa) >> 8, G = (((h >> 8) & 0xff) * fa) >> 8, B = (((h >> 16) & 0xff) * fa) >> 8;
    *argb = 0xff000000u | (uint32_t)keep << 16 | (uint32_t)keep << 8 | (uint32_t)keep;
    *oargb = 0xff000000u | R << 16 | G << 8 | B;
}

/* one band: rows [ya, yb) sampled with texture header h, u/v = world / period (texels repeat every period units) */
static void band(const Ctx *c, const pvr_poly_hdr_t *h, float ya, float yb, float period, float ubase, float vbase)
{
    const RFloorView *v = c->v;
    if (yb <= ya) return;
    rdc_header(h);
    float inv = 1.0f / period;
    int n = (int)ceilf((yb - ya) / 4.0f); if (n < 1) n = 1;
    for (int i = 0; i < n; i++) {
        float y0 = ya + (yb - ya) * i / n, y1 = ya + (yb - ya) * (i + 1) / n;
        RdcVert q[4]; float ys[2] = { y0, y1 };
        for (int e = 0; e < 2; e++) {
            float dd = row_dist(v, ys[e]), step = dd / v->focal;
            float cx = v->cam_x + v->fx * dd, cy = v->cam_y + v->fy * dd;
            float lx = cx - c->rx * step * c->half, ly = cy - c->ry * step * c->half;
            float rx = cx + c->rx * step * c->half, ry = cy + c->ry * step * c->half;
            uint32_t argb, oargb; fog_cols(v, dd, &argb, &oargb);
            float X0, Y0, X1, Y1;
            rdc_xform(0, ys[e], &X0, &Y0); rdc_xform((float)v->sw, ys[e], &X1, &Y1);
            float z = 1.0f / dd;
            RdcVert a = { X0, Y0, z, lx * inv - ubase, ly * inv - vbase, argb, oargb };
            RdcVert b = { X1, Y1, z, rx * inv - ubase, ry * inv - vbase, argb, oargb };
            if (e == 0) { q[0] = a; q[1] = b; } else { q[2] = b; q[3] = a; }
        }
        rdc_poly(q, 4);
    }
}

void rdc_floor_haze(float ya, float yb, uint32_t haze, float sw);

void r_floor_draw(Ren *r, RFloor *f, const RFloorView *v)
{
    (void)r;
    if (!f || !rdc_in_frame() || v->y1 <= v->y0) return;
    Ctx c = { v, -v->fy, v->fx, v->sw * 0.5f };
    float ytop = (float)v->y0, ybot = (float)v->y1;
    if (f->single) {
        /* bands at the software's mip switches: mip L from step >= mip_step * 2^(L-1) */
        float period = (float)f->d.tex;
        float ub = floorf(v->cam_x / period), vb = floorf(v->cam_y / period);
        float yprev = ybot;
        for (int L = 0; L < f->nlev; L++) {
            float dfar = L + 1 < f->nlev ? v->mip_step * (float)(1 << L) * v->focal : 1e30f;
            float yfar = L + 1 < f->nlev ? dist_row(v, dfar) : ytop;
            if (yfar < ytop) yfar = ytop;
            if (yfar < yprev) band(&c, &f->lev[L].hdr, yfar, yprev, period, ub, vb);
            yprev = yfar;
            if (yprev <= ytop) break;
        }
        return;
    }
    /* clipmaps: band L (L >= 1) from D0 * 2^(L-1) to D0 * 2^L */
    float w = v->sw * 0.5f / v->focal;
    float d0 = (WIN / 2 - MARGIN) / sqrtf(1 + w * w);
    float dmax = v->fog_max >= 256 ? v->fog1 : 1e30f;   /* past full fog there is only haze */
    float yhaze = dist_row(v, dmax); if (yhaze < ytop) yhaze = ytop; if (yhaze > ybot) yhaze = ybot;
    float yprev = ybot;
    for (int L = 0; L < MAX_LEV && yprev > yhaze; L++) {
        float dfar = d0 * (float)(1 << L);
        float yfar = L == MAX_LEV - 1 ? yhaze : dist_row(v, dfar);
        if (yfar < yhaze) yfar = yhaze;
        if (yfar < yprev) {
            keep_window(f, L, v->cam_x, v->cam_y);
            if (!f->lev[L].mem) return;
            float period = (float)(WIN << L);
            band(&c, &f->lev[L].hdr, yfar, yprev, period, floorf(v->cam_x / period), floorf(v->cam_y / period));
        }
        yprev = yfar;
    }
    if (yhaze > ytop) rdc_floor_haze(ytop, yhaze, v->haze, (float)v->sw);
}
