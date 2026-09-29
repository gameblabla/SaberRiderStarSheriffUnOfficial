/* platform/render.h as a software rasteriser over a 426x240 RGBA8888 framebuffer, which the page blits to a
 * canvas with one putImageData a frame.
 *
 * Why software: the frame is small (426x240 = 102,240 pixels), the geometry is simple - a scaled, flipped,
 * colour-modulated rectangle out of a texture, a solid rectangle, a line, a point, a triangle, and the Mode 7
 * floor - and the alternatives were worse. Canvas 2D's drawImage would mean uploading every texture as its own
 * canvas and re-implementing the blend modes, rotation and colour modulation it does not have (a modulated
 * sprite is drawImage into an offscreen tinted copy, or a per-pixel pass anyway); WebGL would need a shader and
 * a texture upload path, and the user asked for no WebGL. So every draw is a loop over the destination pixels,
 * which also means the output is pixel-identical to the other ports' software path rather than to a GPU's.
 *
 * The framebuffer is the whole internal resolution whatever the game's logical width is (render.h's sw is 426
 * wide, or 320 in the 4:3 ratio, and the page scales the canvas), so the 4:3 modes just leave the right-hand
 * columns black. The floor plane is the shared software rasteriser (platform/common/floor_soft.c), the same one
 * the PC build uses: it renders the Mode 7 rows into a streaming texture and this file's r_tex blits them.
 *
 * Performance notes, in the order the game hits them:
 *   - the tile layers: one r_tex_batch of a thousand 16 px tiles, each an unscaled opaque blit with no modulation.
 *     That is a memcpy per tile row and it is the bulk of a stage frame;
 *   - sprites: scaled with a colour and an alpha modulation, blended. The inner loop is integer, with the
 *     modulate-and-blend factors hoisted out of it;
 *   - the floor: floor_soft.c's own per-pixel pass, unchanged;
 *   - rotation: only ever small sprites and a boss, so the inverse-mapped nearest-neighbour path is fine;
 *   - the scanline and fill passes are runs, not per-pixel tests.
 */
#include "../render.h"
#include "../common/floor_soft.h"
#include "wasm_internal.h"
#include <stdlib.h>
#include <string.h>

struct Ren {
    uint32_t *fb;          /* WASM_SCREEN_W * WASM_SCREEN_H, RGBA8888 (R first, 0xAABBGGRR as an integer) */
    int w, h;
    RRect viewport;        /* draws are offset by it and clipped to it (r_set_viewport) */
    RRect clip;            /* r_set_clip, in viewport coordinates */
    uint8_t cr, cg, cb, ca;/* the draw colour (r_set_draw_color) */
    RBlend blend;          /* the draw blend mode, for the solid primitives */
    int prims;             /* draws this frame, for the status panel */
    int drawn_w;           /* the game's logical width this frame */
    bool presented;
};

struct RTex {
    Ren     *r;
    int      w, h;
    uint32_t *px;          /* w * h, RGBA8888 */
    uint8_t  mr, mg, mb, ma;
    RBlend   blend;
    RScale   scale;
    uint32_t tag;
    bool     streamed;     /* rtex_update writes over it (the floor's rows) */
};

static Ren the_ren;
static uint32_t floor_us_acc;

/* ------------------------------------------------------------------ the framebuffer */
uint32_t *wasm_framebuffer(void) { return the_ren.fb; }
int wasm_frame_width(void) { return the_ren.w; }
int wasm_frame_height(void) { return the_ren.h; }
int prims_this_frame(void) { return the_ren.prims; }
int rwasm_drawn_width(void) { return the_ren.drawn_w; }
void rwasm_present(void) {
    the_ren.presented = true;
}
uint32_t floor_us_total(void) { return floor_us_acc; }
void floor_us_reset(void) { floor_us_acc = 0; }

Ren *rwasm_renderer(void)
{
    if (!the_ren.fb) {
        the_ren.fb = calloc((size_t)WASM_SCREEN_W * WASM_SCREEN_H, 4);
        if (!the_ren.fb) return NULL;
        the_ren.w = WASM_SCREEN_W;
        the_ren.h = WASM_SCREEN_H;
        the_ren.viewport.x = the_ren.viewport.y = 0;
        the_ren.viewport.w = WASM_SCREEN_W;
        the_ren.viewport.h = WASM_SCREEN_H;
        the_ren.cr = the_ren.cg = the_ren.cb = 0;
        the_ren.ca = 255;
        the_ren.blend = R_BLEND_BLEND;
        the_ren.drawn_w = WASM_SCREEN_W;
    }
    return &the_ren;
}

/* the draw state the geometry helpers read: the viewport's origin, and the clip rectangle in framebuffer
 * coordinates. render.h keeps the clip in viewport coordinates (SDL_SetRenderClipRect takes them that way too),
 * so the framebuffer clip is the viewport origin plus the clip, intersected with the viewport itself (SDL clips
 * every draw to the viewport as well). A NULL clip is no clip beyond the viewport. */
static int vp_x(Ren *r) { return r->viewport.x; }
static int vp_y(Ren *r) { return r->viewport.y; }
static void clip_fb(Ren *r, int *x0, int *y0, int *x1, int *y1)
{
    int vx0 = r->viewport.x, vy0 = r->viewport.y;
    int vx1 = vx0 + r->viewport.w, vy1 = vy0 + r->viewport.h;
    if (vx0 < 0) vx0 = 0;
    if (vy0 < 0) vy0 = 0;
    if (vx1 > r->w) vx1 = r->w;
    if (vy1 > r->h) vy1 = r->h;
    if (r->clip.w > 0) {
        int cx0 = vx0 + r->clip.x, cy0 = vy0 + r->clip.y;
        int cx1 = cx0 + r->clip.w, cy1 = cy0 + r->clip.h;
        if (cx0 > vx0) vx0 = cx0;
        if (cy0 > vy0) vy0 = cy0;
        if (cx1 < vx1) vx1 = cx1;
        if (cy1 < vy1) vy1 = cy1;
    }
    *x0 = vx0; *y0 = vy0; *x1 = vx1; *y1 = vy1;
}
static int clip_x0(Ren *r) { int x0, y0, x1, y1; clip_fb(r, &x0, &y0, &x1, &y1); return x0; }
static int clip_y0(Ren *r) { int x0, y0, x1, y1; clip_fb(r, &x0, &y0, &x1, &y1); return y0; }
static int clip_x1(Ren *r) { int x0, y0, x1, y1; clip_fb(r, &x0, &y0, &x1, &y1); return x1; }
static int clip_y1(Ren *r) { int x0, y0, x1, y1; clip_fb(r, &x0, &y0, &x1, &y1); return y1; }

/* ------------------------------------------------------------------ textures */
static RTex *tex_new(Ren *r, int w, int h, uint32_t *px, bool streamed)
{
    if (w <= 0 || h <= 0) return NULL;
    RTex *t = malloc(sizeof *t);
    if (!t) return NULL;
    t->px = px;
    if (!t->px) {
        t->px = calloc((size_t)w * h, 4);
        if (!t->px) { free(t); return NULL; }
    }
    t->r = r;
    t->w = w;
    t->h = h;
    t->mr = t->mg = t->mb = t->ma = 255;
    t->blend = R_BLEND_BLEND;
    t->scale = R_SCALE_NEAREST;
    t->tag = 0;
    t->streamed = streamed;
    return t;
}

RTex *rtex_create(Ren *r, int w, int h, RTexAccess access, const uint32_t *px)
{
    if (!r) r = rwasm_renderer();
    if (!r) return NULL;
    uint32_t *dst = NULL;
    if (px) {
        dst = malloc((size_t)w * h * 4);
        if (!dst) return NULL;
        memcpy(dst, px, (size_t)w * h * 4);
    }
    return tex_new(r, w, h, dst, access == R_TEX_STREAMING);
}
void rtex_update(RTex *t, const uint32_t *px, int pitch)
{
    if (!t || !px || !t->px) return;
    int w = t->w, h = t->h;
    if (pitch > 0 && pitch != w * 4) {   /* a source with its own stride: row by row */
        for (int y = 0; y < h; y++) memcpy(t->px + (size_t)y * w, px + (size_t)y * (pitch / 4), (size_t)w * 4);
    } else memcpy(t->px, px, (size_t)w * h * 4);
}
RTex *rtex_create_rows(Ren *r, int w, int h, RTexRows rows, void *ud)
{
    if (!r) r = rwasm_renderer();
    if (!r || !rows) return NULL;
    uint32_t *px = calloc((size_t)w * h, 4);
    if (!px) return NULL;
    rows(ud, 0, h, px);   /* the callback may run twice per row, so it has to be idempotent per row */
    RTex *t = tex_new(r, w, h, px, false);
    if (!t) free(px);
    return t;
}
RTex *rtex_create_baked(Ren *r, uint8_t *block, size_t size) { (void)r; (void)block; (void)size; return NULL; }
void rtex_destroy(RTex *t) { if (t) { free(t->px); free(t); } }
void rtex_size(const RTex *t, int *w, int *h) { if (w) *w = t ? t->w : 0; if (h) *h = t ? t->h : 0; }
void rtex_set_color_mod(RTex *t, uint8_t r, uint8_t g, uint8_t b) { if (t) { t->mr = r; t->mg = g; t->mb = b; } }
void rtex_set_alpha_mod(RTex *t, uint8_t a) { if (t) t->ma = a; }
void rtex_set_blend(RTex *t, RBlend b) { if (t) t->blend = b; }
void rtex_set_scale(RTex *t, RScale s) { if (t) t->scale = s; }
void rtex_set_tag(RTex *t, uint32_t tag) { if (t) t->tag = tag; }
Ren  *rtex_renderer(const RTex *t) { return t ? t->r : &the_ren; }
/* memory pressure: the game's own cache drops the texture drawn longest ago and the allocation is retried
 * (gfx.c's evict_one), which is what this hook is for */
static bool (*evict_hook)(void);
void r_set_evict_hook(bool (*hook)(void)) { evict_hook = hook; set_heap_hook(hook); }

/* ------------------------------------------------------------------ draw state */
void r_set_draw_color(Ren *r, uint8_t R, uint8_t G, uint8_t B, uint8_t A) { r->cr = R; r->cg = G; r->cb = B; r->ca = A; }
void r_set_draw_blend(Ren *r, RBlend b) { r->blend = b; }
void r_set_clip(Ren *r, const RRect *c) { if (c) r->clip = *c; else r->clip.w = 0; }
void r_set_viewport(Ren *r, const RRect *vp)
{
    if (vp) {
        r->viewport = *vp;
        if (r->viewport.w <= 0) r->viewport.w = r->w;
        if (r->viewport.h <= 0) r->viewport.h = r->h;
    } else {
        r->viewport.x = r->viewport.y = 0;
        r->viewport.w = r->w;
        r->viewport.h = r->h;
    }
}
bool r_rect_intersect(const RRect *a, const RRect *b, RRect *out)
{
    int x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
    int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w;
    int y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    if (x1 <= x0 || y1 <= y0) { if (out) *out = (RRect){ 0, 0, 0, 0 }; return false; }
    if (out) *out = (RRect){ x0, y0, x1 - x0, y1 - y0 };
    return true;
}

/* ------------------------------------------------------------------ the pixel writers
 * Pixels are 0xAABBGGRR (R in the low byte, the layout render.h asks for). A texture's colour/alpha mods are
 * 0..255 (SDL_SetTextureColorMod / AlphaMod), multiplied with the texel: modulated colour = texel * mod / 255,
 * and the effective source alpha for a BLEND/ADD draw is texel_alpha * alpha_mod / 255. A BLEND_NONE draw is an
 * opaque write of the modulated colour (SDL_BLENDMODE_NONE ignores the texture alpha). */
typedef struct { int mr, mg, mb, ma; } Mod;

static Mod mod_of(const RTex *t, uint8_t extra_alpha)
{
    Mod m;
    m.mr = t->mr;
    m.mg = t->mg;
    m.mb = t->mb;
    m.ma = (int)t->ma * (int)extra_alpha / 255;
    return m;
}

/* the modulated texel colour, opaque */
static inline uint32_t mod_px(uint32_t src, Mod m)
{
    int R = (int)(src & 0xff), G = (int)(src >> 8 & 0xff), B = (int)(src >> 16 & 0xff);
    R = (R * m.mr + 127) / 255; G = (G * m.mg + 127) / 255; B = (B * m.mb + 127) / 255;
    return 0xff000000u | (uint32_t)B << 16 | (uint32_t)G << 8 | (uint32_t)R;
}
/* src-over: the texel's own alpha (times the alpha mod) mixes the modulated colour with the destination */
static inline uint32_t blend_px(uint32_t dst, uint32_t src, Mod m)
{
    int sa = (int)(src >> 24 & 0xff);
    int a = (sa * m.ma + 127) / 255;
    if (a <= 0) return dst;
    int R = (int)(src & 0xff), G = (int)(src >> 8 & 0xff), B = (int)(src >> 16 & 0xff);
    R = (R * m.mr + 127) / 255; G = (G * m.mg + 127) / 255; B = (B * m.mb + 127) / 255;
    if (a >= 255) return 0xff000000u | (uint32_t)B << 16 | (uint32_t)G << 8 | (uint32_t)R;
    int ia = 255 - a;
    int dR = (int)(dst & 0xff), dG = (int)(dst >> 8 & 0xff), dB = (int)(dst >> 16 & 0xff);
    return 0xff000000u
         | (uint32_t)((B * a + dB * ia + 127) / 255) << 16
         | (uint32_t)((G * a + dG * ia + 127) / 255) << 8
         | (uint32_t)((R * a + dR * ia + 127) / 255);
}
/* a normalised 0..1 RFColor component as a byte, the way every other colour in this file is carried */
static inline int col_byte(float c)
{
    int v = (int)(c * 255.0f + 0.5f);
    return v < 0 ? 0 : v > 255 ? 255 : v;
}
/* additive: the modulated colour, scaled by the effective alpha, is added to the destination */
static inline uint32_t add_px(uint32_t dst, uint32_t src, Mod m)
{
    int sa = (int)(src >> 24 & 0xff);
    int a = (sa * m.ma + 127) / 255;
    int R = (int)(src & 0xff), G = (int)(src >> 8 & 0xff), B = (int)(src >> 16 & 0xff);
    R = (R * m.mr * a + 127 * 255) / (255 * 255);
    G = (G * m.mg * a + 127 * 255) / (255 * 255);
    B = (B * m.mb * a + 127 * 255) / (255 * 255);
    int dR = (int)(dst & 0xff) + R, dG = (int)(dst >> 8 & 0xff) + G, dB = (int)(dst >> 16 & 0xff) + B;
    return 0xff000000u
         | (uint32_t)(dB > 255 ? 255 : dB) << 16
         | (uint32_t)(dG > 255 ? 255 : dG) << 8
         | (uint32_t)(dR > 255 ? 255 : dR);
}

/* ------------------------------------------------------------------ solid primitives */
/* app.c's r_clear is the first draw of every frame (app_draw), which is where the frame's draw count starts */
void r_clear(Ren *r)
{
    r->prims = 0;
    uint32_t c = 0xff000000u | (uint32_t)r->cb << 16 | (uint32_t)r->cg << 8 | r->cr;
    /* the whole framebuffer, viewport included: SDL's RenderClear ignores the viewport and clip, and app.c's
     * r_clear at the top of every frame wants the whole screen */
    for (int i = 0, n = r->w * r->h; i < n; i++) r->fb[i] = c;
    r->prims++;
}

/* the destination rectangle of a solid fill, in framebuffer coordinates, clipped; false when nothing is left.
 * q is in viewport coordinates (NULL = the whole viewport); the viewport origin is added here. */

static bool fill_span(Ren *r, const RFRect *q, int *x0, int *y0, int *x1, int *y1)
{
    float fx, fy, fw, fh;
    if (q) { fx = q->x; fy = q->y; fw = q->w; fh = q->h; }
    else { fx = 0; fy = 0; fw = (float)r->viewport.w; fh = (float)r->viewport.h; }
    int rx = (int)floorf(fx), ry = (int)floorf(fy);
    int rw = (int)ceilf(fx + fw) - rx;
    int rh = (int)ceilf(fy + fh) - ry;
    if (rw <= 0 || rh <= 0) return false;
    rx += vp_x(r);
    ry += vp_y(r);
    int cx0 = rx, cy0 = ry, cx1 = rx + rw, cy1 = ry + rh;
    if (cx0 < clip_x0(r)) cx0 = clip_x0(r);
    if (cy0 < clip_y0(r)) cy0 = clip_y0(r);
    if (cx1 > clip_x1(r)) cx1 = clip_x1(r);
    if (cy1 > clip_y1(r)) cy1 = clip_y1(r);
    if (cx0 >= cx1 || cy0 >= cy1) return false;
    *x0 = cx0; *y0 = cy0; *x1 = cx1; *y1 = cy1;
    return true;
}

void r_fill_rect(Ren *r, const RFRect *q)
{
    int x0, y0, x1, y1;
    if (!fill_span(r, q, &x0, &y0, &x1, &y1)) return;
    uint32_t c = r->ca >= 255 ? (0xff000000u | (uint32_t)r->cb << 16 | (uint32_t)r->cg << 8 | r->cr)
                               : (0xff000000u | (uint32_t)((r->cb * r->ca) >> 8) << 16 | (uint32_t)((r->cg * r->ca) >> 8) << 8
                                 | (uint32_t)((r->cr * r->ca) >> 8));
    for (int y = y0; y < y1; y++) {
        uint32_t *row = r->fb + (size_t)y * r->w;
        if (r->blend == R_BLEND_NONE || r->ca >= 255) {
            for (int x = x0; x < x1; x++) row[x] = c;
        } else {
            Mod m = { 255, 255, 255, r->ca };
            for (int x = x0; x < x1; x++) row[x] = blend_px(row[x], 0xff000000u | (uint32_t)r->cb << 16 | (uint32_t)r->cg << 8 | r->cr, m);
        }
    }
    r->prims++;
}
void r_fill_rects(Ren *r, const RFRect *q, int n) { for (int i = 0; i < n; i++) r_fill_rect(r, &q[i]); }
/* 1 px outline, in framebuffer coordinates directly: decomposing into r_fill_rect calls would add the
 * viewport origin a second time (fill_span already does), so the four borders are filled here. */
void r_rect(Ren *r, const RFRect *q)
{
    int x0, y0, x1, y1;
    if (!q || !fill_span(r, q, &x0, &y0, &x1, &y1)) return;
    uint32_t c = 0xff000000u | (uint32_t)r->cb << 16 | (uint32_t)r->cg << 8 | r->cr;
    int cx0 = clip_x0(r), cy0 = clip_y0(r), cx1 = clip_x1(r), cy1 = clip_y1(r);
    if (r->blend == R_BLEND_NONE || r->ca >= 255) {
        for (int x = x0; x < x1; x++) {
            if (y0 >= cy0 && y0 < cy1 && x >= cx0 && x < cx1) r->fb[(size_t)y0 * r->w + x] = c;
            if (y1 - 1 != y0 && y1 - 1 >= cy0 && y1 - 1 < cy1 && x >= cx0 && x < cx1)
                r->fb[(size_t)(y1 - 1) * r->w + x] = c;
        }
        for (int y = y0 + 1; y < y1 - 1; y++) {
            if (y < cy0 || y >= cy1) continue;
            if (x0 >= cx0 && x0 < cx1) r->fb[(size_t)y * r->w + x0] = c;
            if (x1 - 1 != x0 && x1 - 1 >= cx0 && x1 - 1 < cx1) r->fb[(size_t)y * r->w + x1 - 1] = c;
        }
    } else {
        Mod m = { 255, 255, 255, r->ca };
        uint32_t src = 0xff000000u | (uint32_t)r->cb << 16 | (uint32_t)r->cg << 8 | r->cr;
        for (int x = x0; x < x1; x++) {
            if (y0 >= cy0 && y0 < cy1 && x >= cx0 && x < cx1)
                r->fb[(size_t)y0 * r->w + x] = blend_px(r->fb[(size_t)y0 * r->w + x], src, m);
            if (y1 - 1 != y0 && y1 - 1 >= cy0 && y1 - 1 < cy1 && x >= cx0 && x < cx1)
                r->fb[(size_t)(y1 - 1) * r->w + x] = blend_px(r->fb[(size_t)(y1 - 1) * r->w + x], src, m);
        }
        for (int y = y0 + 1; y < y1 - 1; y++) {
            if (y < cy0 || y >= cy1) continue;
            if (x0 >= cx0 && x0 < cx1)
                r->fb[(size_t)y * r->w + x0] = blend_px(r->fb[(size_t)y * r->w + x0], src, m);
            if (x1 - 1 != x0 && x1 - 1 >= cx0 && x1 - 1 < cx1)
                r->fb[(size_t)y * r->w + x1 - 1] = blend_px(r->fb[(size_t)y * r->w + x1 - 1], src, m);
        }
    }
    r->prims++;
}
void r_line(Ren *r, real x0, real y0, real x1, real y1)
{
    int ax = (int)x0 + vp_x(r), ay = (int)y0 + vp_y(r);
    int bx = (int)x1 + vp_x(r), by = (int)y1 + vp_y(r);
    int dx = bx > ax ? bx - ax : ax - bx, dy = by > ay ? by - ay : ay - by;
    int sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1;
    uint32_t c = 0xff000000u | (uint32_t)r->cb << 16 | (uint32_t)r->cg << 8 | r->cr;
    int cx0 = clip_x0(r), cy0 = clip_y0(r), cx1 = clip_x1(r), cy1 = clip_y1(r);
    if (dx >= dy) {
        int err = dx / 2;
        for (int i = 0; i <= dx; i++) {
            if (ax >= cx0 && ax < cx1 && ay >= cy0 && ay < cy1) r->fb[(size_t)ay * r->w + ax] = c;
            ax += sx;
            err -= dy;
            if (err < 0) { ay += sy; err += dx; }
        }
    } else {
        int err = dy / 2;
        for (int i = 0; i <= dy; i++) {
            if (ax >= cx0 && ax < cx1 && ay >= cy0 && ay < cy1) r->fb[(size_t)ay * r->w + ax] = c;
            ay += sy;
            err -= dx;
            if (err < 0) { ax += sx; err += dy; }
        }
    }
    r->prims++;
}
void r_point(Ren *r, real x, real y)
{
    int px = (int)x + vp_x(r), py = (int)y + vp_y(r);
    if (px < clip_x0(r) || px >= clip_x1(r) || py < clip_y0(r) || py >= clip_y1(r)) return;
    r->fb[(size_t)py * r->w + px] = 0xff000000u | (uint32_t)r->cb << 16 | (uint32_t)r->cg << 8 | r->cr;
    r->prims++;
}

/* ------------------------------------------------------------------ textured rectangles
 * A draw is a source rectangle in a texture (texels) and a destination rectangle in viewport coordinates.
 * Both may be fractional: the destination is snapped to the framebuffer pixels it covers, and each pixel reads
 * the texel under its centre (nearest-neighbour), so a 1:1 draw is an exact copy and a scaled one steps evenly.
 * A NULL source is the whole texture, a NULL destination the whole viewport. The per-pixel alpha of the texel
 * folds with the texture's alpha mod (SDL_BLENDMODE_BLEND); BLEND_NONE writes the modulated colour opaque, and
 * ADD adds it scaled by the effective alpha. */
typedef struct {
    float sx, sy, sw, sh;    /* the source, texels (already clamped to >0 size) */
    float dx, dy, dw, dh;    /* the destination, viewport coordinates */
    int flip_h, flip_v;
    uint32_t *px;
    int tw, th;
    Mod m;
    RBlend blend;
} Blit;

static bool blit_setup(Ren *r, RTex *t, const RFRect *src, const RFRect *dst, int flip_h, int flip_v, Blit *b)
{
    if (!t || !t->px) return false;
    int tw = t->w, th = t->h;
    float sx = 0, sy = 0, sw = (float)tw, sh = (float)th;
    if (src) { sx = src->x; sy = src->y; sw = src->w; sh = src->h; }
    float dx = 0, dy = 0, dw = (float)r->viewport.w, dh = (float)r->viewport.h;
    if (dst) { dx = dst->x; dy = dst->y; dw = dst->w; dh = dst->h; }
    /* a negative width mirrors in place (r_tex_batch's convention): the left edge stays where it was, and the
     * sampling flips. render_sdl.c draws the same rect with SDL_FLIP_HORIZONTAL for these. */
    if (dw < 0) { dw = -dw; flip_h = !flip_h; }
    if (dh < 0) { dh = -dh; flip_v = !flip_v; }
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;
    /* a source rectangle outside the texture clamps to what is there (a padded sprite frame, the floor's rows
     * past the horizon); the destination keeps its mapping, so the edge texel stretches rather than nothing
     * drawing at all. Sampling clamps per pixel below. */
    b->sx = sx; b->sy = sy; b->sw = sw; b->sh = sh;
    b->dx = dx; b->dy = dy; b->dw = dw; b->dh = dh;
    b->flip_h = flip_h; b->flip_v = flip_v;
    b->tw = tw; b->th = th;
    b->px = t->px;
    b->blend = t->blend;
    b->m = mod_of(t, 255);
    (void)r;
    return true;
}

/* the texel under a destination pixel: u/v in [0,1) across the draw, flipped, into the source rectangle */
static inline int blit_texel(float pos, float origin, float size, float sorigin, float ssize, int tsize, int flip)
{
    float u = (pos - origin) / size;
    if (u < 0.0f || u >= 1.0f) return -1;
    if (flip) u = 1.0f - u;
    float f = sorigin + u * ssize;
    int ti = (int)floorf(f);
    if (ti < 0) ti = 0;
    else if (ti >= tsize) ti = tsize - 1;
    return ti;
}

/* the loop this draw wants: an opaque write (modulated colour, no blending), or the general blend/add path.
 * Only the NONE blend mode with a fully opaque texture can skip the per-pixel alpha: anything else may have a
 * translucent texel in it. */
static void blit_run(Blit *b, Ren *r)
{
    int vpx = vp_x(r), vpy = vp_y(r);
    int cx0 = clip_x0(r), cy0 = clip_y0(r), cx1 = clip_x1(r), cy1 = clip_y1(r);
    int x0 = (int)floorf(b->dx + vpx), y0 = (int)floorf(b->dy + vpy);
    int x1 = (int)ceilf(b->dx + b->dw + vpx), y1 = (int)ceilf(b->dy + b->dh + vpy);
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 >= x1 || y0 >= y1) return;
    Mod m = b->m;
    bool opaque = b->blend == R_BLEND_NONE;
    bool add = b->blend == R_BLEND_ADD;
    uint32_t *px = b->px;
    int tw = b->tw, th = b->th;
    float vdx = b->dx + vpx, vdy = b->dy + vpy;
    for (int y = y0; y < y1; y++) {
        int ty = blit_texel((float)y + 0.5f, vdy, b->dh, b->sy, b->sh, th, b->flip_v);
        if (ty < 0) continue;
        const uint32_t *srow = px + (size_t)ty * tw;
        uint32_t *drow = r->fb + (size_t)y * r->w;
        for (int x = x0; x < x1; x++) {
            int tx = blit_texel((float)x + 0.5f, vdx, b->dw, b->sx, b->sw, tw, b->flip_h);
            if (tx < 0) continue;
            uint32_t tex = srow[tx];
            drow[x] = opaque ? mod_px(tex, m) : add ? add_px(drow[x], tex, m) : blend_px(drow[x], tex, m);
        }
    }
}

void r_tex(Ren *r, RTex *t, const RFRect *src, const RFRect *dst)
{
    Blit b;
    if (!blit_setup(r, t, src, dst, false, false, &b)) return;
    blit_run(&b, r);
    r->prims++;
}

void r_tex_batch(Ren *r, RTex *t, const RFRect *src, const RFRect *dst, int n)
{
    if (!t) return;
    for (int i = 0; i < n; i++) {
        /* a negative width mirrors that one horizontally (render.h): blit_setup reads that off dst->w itself, so
         * the flag must start false here or the two cancel and the tile is drawn unmirrored */
        Blit b;
        if (!blit_setup(r, t, &src[i], &dst[i], false, false, &b)) continue;
        blit_run(&b, r);
    }
    r->prims += n;
}

/* rotation: the inverse-mapped nearest path over the destination rectangle. Only ever small sprites and a boss,
 * so this is not worth a proper edge-function affine blit. The destination and centre are in viewport coordinates
 * (the centre relative to the destination, NULL = its middle); the loop runs in framebuffer coordinates. */
void r_tex_rot(Ren *r, RTex *t, const RFRect *src, const RFRect *dst, rdeg angle, const RFPoint *center, RFlip flip)
{
    RFRect d = dst ? *dst : (RFRect){ 0, 0, (float)r->viewport.w, (float)r->viewport.h };
    if (!t || !t->px) return;
    if (d.w < 0) { d.w = -d.w; flip ^= R_FLIP_H; }
    if (d.h < 0) { d.h = -d.h; flip ^= R_FLIP_V; }
    if (d.w <= 0 || d.h <= 0) return;
    float sw = t->w, sh = t->h, sx = 0, sy = 0;
    if (src) { sx = src->x; sy = src->y; sw = src->w; sh = src->h; }
    if (sw <= 0 || sh <= 0) return;

    float ccx = center ? d.x + center->x : d.x + d.w * 0.5f;
    float ccy = center ? d.y + center->y : d.y + d.h * 0.5f;
    /* clockwise degrees, as render.h says: the screen's y grows downwards */
    double rad = (double)angle * 3.14159265358979323846 / 180.0;
    double cs = cos(rad), sn = sin(rad);
    Mod m = mod_of(t, 255);
    bool add = t->blend == R_BLEND_ADD;
    int vpx = vp_x(r), vpy = vp_y(r);
    int cx0 = clip_x0(r), cy0 = clip_y0(r), cx1 = clip_x1(r), cy1 = clip_y1(r);

    int x0 = (int)floorf(d.x + vpx), y0 = (int)floorf(d.y + vpy);
    int x1 = (int)ceilf(d.x + d.w + vpx), y1 = (int)ceilf(d.y + d.h + vpy);
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            /* the pixel (viewport coordinates) relative to the rotation centre, turned back by -angle */
            double rx = (x - vpx + 0.5) - ccx, ry = (y - vpy + 0.5) - ccy;
            double ux = rx * cs + ry * sn, uy = -rx * sn + ry * cs;
            float u = (float)(ux + (ccx - d.x)) / d.w, v = (float)(uy + (ccy - d.y)) / d.h;
            if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) continue;
            if (flip & R_FLIP_H) u = 1.0f - u;
            if (flip & R_FLIP_V) v = 1.0f - v;
            int tx = (int)(sx + u * sw), ty = (int)(sy + v * sh);
            if (tx < 0) tx = 0; else if (tx >= t->w) tx = t->w - 1;
            if (ty < 0) ty = 0; else if (ty >= t->h) ty = t->h - 1;
            uint32_t c = t->px[(size_t)ty * t->w + tx];
            uint32_t *dp = &r->fb[(size_t)y * r->w + x];
            *dp = t->blend == R_BLEND_NONE ? mod_px(c, m) : add ? add_px(*dp, c, m) : blend_px(*dp, c, m);
        }
    }
    r->prims++;
}

/* triangles: a scanline fill over the three edges with the vertex colour interpolated along the two free axes.
 * The core only ever asks for the untextured form (ramrod.c's cockpit ellipses, space.c's circles), so that is
 * what is filled; a texture is ignored, as the software floor's callers do. */
void r_geometry(Ren *r, RTex *t, const RVertex *v, int nv, const int *idx, int ni)
{
    int n = idx ? ni : nv;
    int tris = n / 3;
    int cx0 = clip_x0(r), cy0 = clip_y0(r), cx1 = clip_x1(r), cy1 = clip_y1(r);
    float ox = (float)vp_x(r), oy = (float)vp_y(r);   /* vertices arrive in viewport coordinates */
    for (int k = 0; k < tris; k++) {
        const RVertex *a = &v[idx ? idx[k * 3] : k * 3];
        const RVertex *b = &v[idx ? idx[k * 3 + 1] : k * 3 + 1];
        const RVertex *c = &v[idx ? idx[k * 3 + 2] : k * 3 + 2];
        float ax = a->position.x + ox, ay = a->position.y + oy;
        float bx = b->position.x + ox, by = b->position.y + oy;
        float cx = c->position.x + ox, cy = c->position.y + oy;
        float miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
        float maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
        int y0 = (int)miny, y1 = (int)(maxy + 1);
        if (y0 < cy0) y0 = cy0;
        if (y1 > cy1) y1 = cy1;
        float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
        if (area == 0.0f) continue;
        for (int y = y0; y < y1; y++) {
            float py = y + 0.5f;
            /* where each edge crosses this scanline */
            float xs[3];
            float ex[3] = { ax, bx, cx }, ey[3] = { ay, by, cy };
            for (int e = 0; e < 3; e++) {
                float dyy = ey[(e + 1) % 3] - ey[e];
                xs[e] = dyy == 0.0f ? 0.0f : (py - ey[e]) * (ex[(e + 1) % 3] - ex[e]) / dyy + ex[e];
            }
            /* the two crossings that are not the horizontal edge: sort the three x values */
            int order[3] = { 0, 1, 2 };
            for (int i = 0; i < 3; i++)
                for (int j = i + 1; j < 3; j++)
                    if (xs[order[j]] < xs[order[i]]) { int tmp = order[i]; order[i] = order[j]; order[j] = tmp; }
            float xa = xs[order[0]], xb = xs[order[2]];
            if (xa == xb) continue;
            if (xa < (float)cx0) xa = (float)cx0;
            if (xb > (float)cx1) xb = (float)cx1;
            for (int x = (int)xa; x < (int)(xb + 0.5f); x++) {
                float px = x + 0.5f;
                /* barycentric weights */
                float w0 = ((bx - ax) * (py - ay) - (px - ax) * (by - ay)) / area;
                float w1 = ((px - ax) * (cy - ay) - (cx - ax) * (py - ay)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                float R = a->color.r * w0 + b->color.r * w1 + c->color.r * w2;
                float G = a->color.g * w0 + b->color.g * w1 + c->color.g * w2;
                float B = a->color.b * w0 + b->color.b * w1 + c->color.b * w2;
                float A = a->color.a * w0 + b->color.a * w1 + c->color.a * w2;
                /* RFColor's components are real, and the core passes them normalised 0..1 (render.h: it is
                 * SDL_FColour's range, so SDL scales them for us) - the byte conversion is this file's job */
                int ir = col_byte(R), ig = col_byte(G), ib = col_byte(B), ia = col_byte(A);
                uint32_t *dp = &r->fb[(size_t)y * r->w + x];
                if (ia >= 255) *dp = 0xff000000u | (uint32_t)ib << 16 | (uint32_t)ig << 8 | (uint32_t)ir;
                else {
                    int ka = 255 - ia;
                    int dR = (int)(*dp & 0xff), dG = (int)(*dp >> 8 & 0xff), dB = (int)(*dp >> 16 & 0xff);
                    *dp = 0xff000000u
                        | (uint32_t)((ib * ia + dB * ka + 127) / 255) << 16
                        | (uint32_t)((ig * ia + dG * ka + 127) / 255) << 8
                        | (uint32_t)((ir * ia + dR * ka + 127) / 255);
                }
            }
        }
    }
    r->prims += tris;
    (void)t;
}

/* ------------------------------------------------------------------ the Mode 7 floor
 * The shared software rasteriser (the same one the PC build uses, so the floor is pixel-identical): it renders
 * the visible rows into a streaming texture and asks for one textured draw of it. */
RFloor *r_floor_create(Ren *r, const RFloorDesc *d) { return floor_soft_create(r, d); }
void    r_floor_cells_changed(RFloor *f) { (void)f; }   /* the raster reads the cells every frame */
void    r_floor_destroy(RFloor *f) { floor_soft_destroy(f); }

void r_floor_draw(Ren *r, RFloor *f, const RFloorView *v)
{
    double t0 = js_now_ms();
    floor_soft_draw(r, f, v);
    floor_us_acc += (uint32_t)((js_now_ms() - t0) * 1000.0);
    r->prims++;
}

/* no hardware scroll planes: the core draws the tile layers itself */
bool r_layer(Ren *r, uint32_t level, int layer, real cam_x, real cam_y) { (void)r; (void)level; (void)layer; (void)cam_x; (void)cam_y; return false; }
bool r_layer_held(Ren *r, uint32_t level, int layer) { (void)r; (void)level; (void)layer; return false; }
void r_set_depth(Ren *r, int layer) { (void)r; (void)layer; }
