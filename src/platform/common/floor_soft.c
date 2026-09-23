#include "floor_soft.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct RFloor { RFloorDesc d; int tex_shift; RTex *tex; uint32_t *px; int tw, th; };

RFloor *floor_soft_create(Ren *r, const RFloorDesc *d)
{
    (void)r;
    RFloor *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->d = *d;
    while ((1 << f->tex_shift) < d->tex) f->tex_shift++;
    return f;
}

void floor_soft_destroy(RFloor *f)
{
    if (!f) return;
    if (f->tex) rtex_destroy(f->tex);
    free(f->px); free(f);
}

void floor_soft_draw(Ren *r, RFloor *f, const RFloorView *v)
{
    int sw = v->sw, rows = v->y1 - v->y0;
    if (sw <= 0 || rows <= 0) return;
    if (!f->tex || f->tw != sw || f->th < rows) {
        if (f->tex) rtex_destroy(f->tex);
        free(f->px);
        f->tw = sw; f->th = rows;
        f->px = calloc((size_t)sw * rows, 4);
        f->tex = rtex_create(r, sw, rows, R_TEX_STREAMING, NULL);
        if (!f->tex || !f->px) return;
        rtex_set_scale(f->tex, R_SCALE_NEAREST);
        rtex_set_blend(f->tex, R_BLEND_NONE);
    }
    const RFloorDesc *d = &f->d;
    float fx = v->fx, fy = v->fy, rx = -fy, ry = fx;
    uint32_t haze = v->haze, mapmask = (uint32_t)d->mapn - 1;
    int cs = d->cell_shift;
    for (int row = 0; row < rows; row++) {
        int y = v->y0 + row;
        float dd = v->cam_h * v->focal / (y + v->row_off - v->horizon);
        float step = dd / v->focal;   /* world units per screen pixel across this row */
        float fog = (dd - v->fog0) / (v->fog1 - v->fog0); fog = fog < 0 ? 0 : fog > 1 ? 1 : fog;
        int fa = (int)(fog * v->fog_max);
        int mip = 0; float thr = v->mip_step;
        while (mip < d->mips - 1 && step >= thr) { mip++; thr *= 2; }
        int msz = d->tex >> mip, mmask = msz - 1, msh = f->tex_shift - mip;
        float wx = v->cam_x + fx * dd - rx * step * (sw * 0.5f), wy = v->cam_y + fy * dd - ry * step * (sw * 0.5f);
        uint32_t *out = f->px + (size_t)row * sw;
        for (int x = 0; x < sw; x++) {
            int ix = (int)floorf(wx), iy = (int)floorf(wy);
            uint8_t t = d->cells[(((uint32_t)iy >> cs) & mapmask) * d->mapn + (((uint32_t)ix >> cs) & mapmask)];
            uint32_t c = d->mat[t * d->mips + mip][(((iy >> mip) & mmask) << msh) + ((ix >> mip) & mmask)];
            if (fa) {
                uint32_t R = ((c & 0xff) * (256 - fa) + (haze & 0xff) * fa) >> 8;
                uint32_t G = (((c >> 8) & 0xff) * (256 - fa) + ((haze >> 8) & 0xff) * fa) >> 8;
                uint32_t B = (((c >> 16) & 0xff) * (256 - fa) + ((haze >> 16) & 0xff) * fa) >> 8;
                c = 0xff000000u | B << 16 | G << 8 | R;
            }
            out[x] = c;
            wx += rx * step; wy += ry * step;
        }
    }
    rtex_update(f->tex, f->px, sw * 4);   /* rows past `rows` (a taller texture after a horizon bob) are not drawn */
    RFRect src = { 0, 0, (float)sw, (float)rows }, dst = { 0, (float)v->y0, (float)sw, (float)rows };
    r_tex(r, f->tex, &src, &dst);
}
