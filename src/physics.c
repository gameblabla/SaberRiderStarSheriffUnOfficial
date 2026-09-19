#include "physics.h"
#include <math.h>

#define EPS 0.1f

static bool overlap(float cx, float cy, float hx, float hy, int col, int row, int cw, int ch)
{
    float tx = col * cw + cw * 0.5f, ty = row * ch + ch * 0.5f;
    return fabsf(cx - tx) <= hx + cw * 0.5f && fabsf(cy - ty) <= hy + ch * 0.5f;
}

void physics_step(const PhysicsWorld *w, const Level *L, Body *b, float dt)
{
    const int cw = L->cellw, ch = L->cellh;
    float vx = b->vx, vy = b->vy;
    if (!(b->flags & PHYS_NO_GRAVITY)) { vx += w->gx * dt; vy += w->gy * dt; }
    float nx = b->x + b->ox + vx * dt;
    float ny = b->y + b->oy + vy * dt;
    const float hx = b->hx, hy = b->hy;
    uint8_t coll = 0, gtile = 0;

    /* right */
    if (vx > 0.0f && !(b->flags & PHYS_IGNORE_RIGHT)) {
        if (w->world_max_x < nx + hx) { nx = w->world_max_x - hx; coll |= COLL_RIGHT; if (vx >= 0) vx = 0; }
        int col = (int)floorf((nx + hx) / cw);
        int r0 = (int)floorf((ny - hy) / ch), r1 = (int)floorf((ny + hy) / ch);
        for (int r = r0; r < r1; r++) {
            if ((level_cell(L, col, r) & 1) && overlap(nx, ny, hx, hy, col, r, cw, ch)) {
                coll |= COLL_RIGHT; if (vx >= 0) vx = 0;
                nx = col * cw - hx;   /* tile center - cw/2 - hx */
                break;
            }
        }
    }
    /* left */
    if (vx < 0.0f && !(b->flags & PHYS_IGNORE_LEFT)) {
        if (nx - hx - EPS < w->world_min_x) { nx = w->world_min_x + hx; coll |= COLL_LEFT; if (vx <= 0) vx = 0; }
        int col = (int)floorf((nx - hx) / cw);
        int r0 = (int)floorf((ny - hy) / ch), r1 = (int)floorf((ny + hy) / ch);
        for (int r = r0; r < r1; r++) {
            if ((level_cell(L, col, r) & 2) && overlap(nx, ny, hx, hy, col, r, cw, ch)) {
                nx = (col + 1) * cw + hx; if (vx <= 0) vx = 0; coll |= COLL_LEFT;
                break;
            }
        }
    }
    /* up */
    if (vy < 0.0f && !(b->flags & PHYS_IGNORE_UP)) {
        int c0 = (int)floorf((nx + (EPS - hx)) / cw), c1 = (int)floorf((hx - EPS + nx) / cw);
        int row = (int)floorf((ny - hy) / ch);
        int last = c1 - (c0 < c1 ? 1 : 0);
        for (int c = c0; c <= last; c++) {
            if ((level_cell(L, c, row) & 8) && overlap(nx, ny, hx, hy, c, row, cw, ch)) {
                ny = (row + 1) * ch + hy; coll |= COLL_UP; if (vy <= 0) vy = 0;
                break;
            }
        }
    }
    /* down */
    if (vy > 0.0f && !(b->flags & PHYS_IGNORE_DOWN)) {
        int c0 = (int)floorf((nx + (EPS - hx)) / cw), c1 = (int)floorf((hx - EPS + nx) / cw);
        int row = (int)floorf((ny + hy) / ch);
        int last = c1 - (c0 < c1 ? 1 : 0);
        for (int c = c0; c <= last; c++) {
            uint8_t v = level_cell(L, c, row);
            if ((v & 4) && overlap(nx, ny, hx, hy, c, row, cw, ch)) {
                coll |= COLL_DOWN; if (vy >= 0) vy = 0;
                ny = row * ch + (1.0f - hy);     /* rests 1px into the floor, like the original */
                gtile = v;
                break;
            }
        }
    }
    b->x = nx - b->ox; b->y = ny - b->oy;
    b->vx = vx; b->vy = vy; b->coll = coll; b->ground_tile = gtile;
}
