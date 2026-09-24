/* platform/render.h on SDL3's renderer: Ren is the SDL_Renderer, RTex an SDL_Texture, and the rect / point /
 * vertex types share SDL's layout, so this is a thin pass-through. The floor plane uses the software raster. */
#include "../render.h"
#include "../common/floor_soft.h"
#include <SDL3/SDL.h>
#include <stddef.h>

#define SR(r) ((SDL_Renderer *)(r))
#define ST(t) ((SDL_Texture *)(t))
_Static_assert(sizeof(RFRect) == sizeof(SDL_FRect) && sizeof(RRect) == sizeof(SDL_Rect), "rect layout");
_Static_assert(sizeof(RVertex) == sizeof(SDL_Vertex) && offsetof(RVertex, tex_coord) == offsetof(SDL_Vertex, tex_coord), "vertex layout");

static SDL_BlendMode blend(RBlend b) { return b == R_BLEND_ADD ? SDL_BLENDMODE_ADD : b == R_BLEND_NONE ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND; }

RTex *rtex_create(Ren *r, int w, int h, RTexAccess access, const uint32_t *px)
{
    SDL_Texture *t = SDL_CreateTexture(SR(r), SDL_PIXELFORMAT_ABGR8888,
                                       access == R_TEX_STREAMING ? SDL_TEXTUREACCESS_STREAMING : SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) { SDL_Log("texture %dx%d: %s", w, h, SDL_GetError()); return NULL; }
    if (px) SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    return (RTex *)t;
}
void rtex_update(RTex *t, const uint32_t *px, int pitch) { SDL_UpdateTexture(ST(t), NULL, px, pitch); }
RTex *rtex_create_rows(Ren *r, int w, int h, RTexRows rows, void *ud)
{
    uint32_t *px = SDL_malloc((size_t)w * h * 4);
    if (!px) return NULL;
    rows(ud, 0, h, px);
    RTex *t = rtex_create(r, w, h, R_TEX_STATIC, px);
    SDL_free(px);
    return t;
}
RTex *rtex_create_baked(Ren *r, uint8_t *block, size_t size) { (void)r; (void)block; (void)size; return NULL; }   /* PC: no baked textures */
void rtex_destroy(RTex *t) { if (t) SDL_DestroyTexture(ST(t)); }
void rtex_size(const RTex *t, int *w, int *h) { float fw = 0, fh = 0; SDL_GetTextureSize((SDL_Texture *)t, &fw, &fh); *w = (int)fw; *h = (int)fh; }
void rtex_set_color_mod(RTex *t, uint8_t r, uint8_t g, uint8_t b) { if (t) SDL_SetTextureColorMod(ST(t), r, g, b); }
void rtex_set_alpha_mod(RTex *t, uint8_t a) { if (t) SDL_SetTextureAlphaMod(ST(t), a); }
void rtex_set_blend(RTex *t, RBlend b) { if (t) SDL_SetTextureBlendMode(ST(t), blend(b)); }
void rtex_set_scale(RTex *t, RScale s) { if (t) SDL_SetTextureScaleMode(ST(t), s == R_SCALE_LINEAR ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST); }
void rtex_set_tag(RTex *t, uint32_t tag) { (void)t; (void)tag; }
void r_set_evict_hook(bool (*hook)(void)) { (void)hook; }
Ren *rtex_renderer(const RTex *t) { return t ? (Ren *)SDL_GetRendererFromTexture((SDL_Texture *)t) : NULL; }

void r_set_draw_color(Ren *r, uint8_t R, uint8_t G, uint8_t B, uint8_t A) { SDL_SetRenderDrawColor(SR(r), R, G, B, A); }
void r_set_draw_blend(Ren *r, RBlend b) { SDL_SetRenderDrawBlendMode(SR(r), blend(b)); }
void r_set_clip(Ren *r, const RRect *c) { SDL_SetRenderClipRect(SR(r), (const SDL_Rect *)c); }
void r_set_viewport(Ren *r, const RRect *vp) { SDL_SetRenderViewport(SR(r), (const SDL_Rect *)vp); }
bool r_rect_intersect(const RRect *a, const RRect *b, RRect *out) { return SDL_GetRectIntersection((const SDL_Rect *)a, (const SDL_Rect *)b, (SDL_Rect *)out); }

void r_clear(Ren *r) { SDL_RenderClear(SR(r)); }
void r_fill_rect(Ren *r, const RFRect *q) { SDL_RenderFillRect(SR(r), (const SDL_FRect *)q); }
void r_fill_rects(Ren *r, const RFRect *q, int n) { SDL_RenderFillRects(SR(r), (const SDL_FRect *)q, n); }
void r_rect(Ren *r, const RFRect *q) { SDL_RenderRect(SR(r), (const SDL_FRect *)q); }
void r_line(Ren *r, float x0, float y0, float x1, float y1) { SDL_RenderLine(SR(r), x0, y0, x1, y1); }
void r_point(Ren *r, float x, float y) { SDL_RenderPoint(SR(r), x, y); }
void r_tex(Ren *r, RTex *t, const RFRect *src, const RFRect *dst)
{
    if (t) SDL_RenderTexture(SR(r), ST(t), (const SDL_FRect *)src, (const SDL_FRect *)dst);
}
void r_tex_rot(Ren *r, RTex *t, const RFRect *src, const RFRect *dst, double angle, const RFPoint *center, RFlip flip)
{
    if (t) SDL_RenderTextureRotated(SR(r), ST(t), (const SDL_FRect *)src, (const SDL_FRect *)dst, angle, (const SDL_FPoint *)center,
                                    (flip & R_FLIP_H ? SDL_FLIP_HORIZONTAL : 0) | (flip & R_FLIP_V ? SDL_FLIP_VERTICAL : 0));
}
void r_tex_batch(Ren *r, RTex *t, const RFRect *src, const RFRect *dst, int n)
{
    if (!t) return;
    for (int i = 0; i < n; i++) {   /* SDL merges consecutive copies of one texture into a single draw */
        if (dst[i].w >= 0) SDL_RenderTexture(SR(r), ST(t), (const SDL_FRect *)&src[i], (const SDL_FRect *)&dst[i]);
        else {
            SDL_FRect d = { dst[i].x, dst[i].y, -dst[i].w, dst[i].h };
            SDL_RenderTextureRotated(SR(r), ST(t), (const SDL_FRect *)&src[i], &d, 0, NULL, SDL_FLIP_HORIZONTAL);
        }
    }
}
void r_geometry(Ren *r, RTex *t, const RVertex *v, int nv, const int *idx, int ni)
{
    SDL_RenderGeometry(SR(r), ST(t), (const SDL_Vertex *)v, nv, idx, ni);
}

RFloor *r_floor_create(Ren *r, const RFloorDesc *d) { return floor_soft_create(r, d); }
void    r_floor_cells_changed(RFloor *f) { (void)f; }   /* the raster reads the cells every frame */
void    r_floor_draw(Ren *r, RFloor *f, const RFloorView *v) { floor_soft_draw(r, f, v); }
void    r_floor_destroy(RFloor *f) { floor_soft_destroy(f); }
bool    r_layer(Ren *r, uint32_t level, int layer, float cam_x, float cam_y) { (void)r; (void)level; (void)layer; (void)cam_x; (void)cam_y; return false; }
void    r_set_depth(Ren *r, int layer) { (void)r; (void)layer; }
