#pragma once
/* A softly edged ellipse for ground shadows. The inner disc keeps the shadow's body solid; the
 * narrow outer ring fades to transparent so the edge does not turn into a blocky silhouette when
 * the logical screen is scaled up. */
#include "platform/render.h"

static inline void shadow_draw_ellipse(Ren *ren, real cx, real cy, real rx, real ry, RFColor color,
                                       real clip_left, real clip_right)
{
    enum { N = 24 };
    const real inner = R(0.8f);
    static real ux[N], uy[N];
    static bool unit_ready;
    if (!unit_ready) {
        for (int i = 0; i < N; i++) {
            real a = r_mul(r_int(i), r_div(R(6.283185307f), r_int(N)));
            ux[i] = r_cos(a); uy[i] = r_sin(a);
        }
        unit_ready = true;
    }

    if (rx <= 0 || ry <= 0 || color.a <= 0) return;
    RVertex v[1 + N * 2];
    int idx[N * 9];
    RFColor edge = color; edge.a = R(0);
    v[0].position = (RFPoint){ cx, cy }; v[0].color = color;
    for (int i = 0; i < N; i++) {
        int j = (i + 1) % N;
        real x = r_mul(rx, ux[i]), y = r_mul(ry, uy[i]);
        real ix = r_max(clip_left, r_min(clip_right, cx + r_mul(inner, x)));
        real iy = cy + r_mul(inner, y);
        real ox = r_max(clip_left, r_min(clip_right, cx + x));
        v[1 + i].position = (RFPoint){ ix, iy }; v[1 + i].color = color;
        v[1 + N + i].position = (RFPoint){ ox, cy + y }; v[1 + N + i].color = edge;

        /* Filled inner disc plus a fading ring, with no overlapping translucent layers. */
        int at = i * 9;
        idx[at] = 0; idx[at + 1] = 1 + i; idx[at + 2] = 1 + j;
        idx[at + 3] = 1 + i; idx[at + 4] = 1 + N + i; idx[at + 5] = 1 + N + j;
        idx[at + 6] = 1 + i; idx[at + 7] = 1 + N + j; idx[at + 8] = 1 + j;
    }
    r_geometry(ren, NULL, v, 1 + N * 2, idx, N * 9);
}
