#include "physics.h"
#include "fx.h"
#include <math.h>

#define EPS 0.1f
#define EPS_FX FX(0.1)

/* The collision decisions (which cells a box touches, whether it overlaps one) run in 16.16 fixed point (fx.h): exact
 * integer tests instead of a dozen float divisions and floors per step, the same on every platform. The positions
 * themselves keep their float formulas (a correction snaps to the same values as before). */
static int cell_floor(fx v, int size, int shift)   /* floor(v / size) for a cell size in pixels */
{
    int p = fx_floor(v);
    if (shift >= 0) return p >> shift;
    return p >= 0 ? p / size : -((size - 1 - p) / size);
}
static int pow2_shift(int n) { int s = 0; while ((1 << s) < n) s++; return (1 << s) == n ? s : -1; }

static bool overlap(fx cx, fx cy, fx hx, fx hy, int col, int row, int cw, int ch)
{
    fx tx = fx_from_int(col * cw) + cw * FX_HALF, ty = fx_from_int(row * ch) + ch * FX_HALF;
    return fx_abs(cx - tx) <= hx + cw * FX_HALF && fx_abs(cy - ty) <= hy + ch * FX_HALF;
}

void physics_step(const PhysicsWorld *w, const Level *L, Body *b, float dt)
{
    const int cw = L->cellw, ch = L->cellh, sw = pow2_shift(cw), sh = pow2_shift(ch);
    float vx = b->vx, vy = b->vy;
    if (!(b->flags & PHYS_NO_GRAVITY)) { vx += w->gx * dt; vy += w->gy * dt; }
    float nx = b->x + b->ox + vx * dt;
    float ny = b->y + b->oy + vy * dt;
    const float hx = b->hx, hy = b->hy;
    fx X = fx_from_float(nx), Y = fx_from_float(ny);
    const fx HX = fx_from_float(hx), HY = fx_from_float(hy);
    uint8_t coll = 0, gtile = 0;

    /* right */
    if (vx > 0.0f && !(b->flags & PHYS_IGNORE_RIGHT)) {
        if (w->world_max_x < nx + hx) { nx = w->world_max_x - hx; X = fx_from_float(nx); coll |= COLL_RIGHT; if (vx >= 0) vx = 0; }
        int col = cell_floor(X + HX, cw, sw);
        int r0 = cell_floor(Y - HY, ch, sh), r1 = cell_floor(Y + HY, ch, sh);
        for (int r = r0; r < r1; r++) {
            if ((level_cell(L, col, r) & 1) && overlap(X, Y, HX, HY, col, r, cw, ch)) {
                coll |= COLL_RIGHT; if (vx >= 0) vx = 0;
                nx = col * cw - hx;   /* tile center - cw/2 - hx */
                X = fx_from_int(col * cw) - HX;
                break;
            }
        }
    }
    /* left */
    if (vx < 0.0f && !(b->flags & PHYS_IGNORE_LEFT)) {
        if (nx - hx - EPS < w->world_min_x) { nx = w->world_min_x + hx; X = fx_from_float(nx); coll |= COLL_LEFT; if (vx <= 0) vx = 0; }
        int col = cell_floor(X - HX, cw, sw);
        int r0 = cell_floor(Y - HY, ch, sh), r1 = cell_floor(Y + HY, ch, sh);
        for (int r = r0; r < r1; r++) {
            if ((level_cell(L, col, r) & 2) && overlap(X, Y, HX, HY, col, r, cw, ch)) {
                nx = (col + 1) * cw + hx; X = fx_from_int((col + 1) * cw) + HX; if (vx <= 0) vx = 0; coll |= COLL_LEFT;
                break;
            }
        }
    }
    /* up */
    if (vy < 0.0f && !(b->flags & PHYS_IGNORE_UP)) {
        int c0 = cell_floor(X + (EPS_FX - HX), cw, sw), c1 = cell_floor(HX - EPS_FX + X, cw, sw);
        int row = cell_floor(Y - HY, ch, sh);
        int last = c1 - (c0 < c1 ? 1 : 0);
        for (int c = c0; c <= last; c++) {
            if ((level_cell(L, c, row) & 8) && overlap(X, Y, HX, HY, c, row, cw, ch)) {
                ny = (row + 1) * ch + hy; Y = fx_from_int((row + 1) * ch) + HY; coll |= COLL_UP; if (vy <= 0) vy = 0;
                break;
            }
        }
    }
    /* down */
    if (vy > 0.0f && !(b->flags & PHYS_IGNORE_DOWN)) {
        int c0 = cell_floor(X + (EPS_FX - HX), cw, sw), c1 = cell_floor(HX - EPS_FX + X, cw, sw);
        int row = cell_floor(Y + HY, ch, sh);
        int last = c1 - (c0 < c1 ? 1 : 0);
        for (int c = c0; c <= last; c++) {
            uint8_t v = level_cell(L, c, row);
            if ((v & 4) && overlap(X, Y, HX, HY, c, row, cw, ch)) {
                /* ramp ground (bit 0x10, stage 4's slopes; never set in the pack data): walking into a
                 * higher ramp column climbs onto its top (up to 2 cells) instead of sinking into it */
                if (v & COLL_RAMP) {
                    int top = row;
                    for (int cc = c0; cc <= last; cc++)
                        for (int k = 1; k <= 2 && (level_cell(L, cc, row - k) & (COLL_RAMP | 4)) == (COLL_RAMP | 4); k++)
                            if (row - k < top) { top = row - k; v = level_cell(L, cc, top); }
                    row = top;
                }
                coll |= COLL_DOWN; if (vy >= 0) vy = 0;
                ny = row * ch + (1.0f - hy);     /* rests 1px into the floor, like the original */
                gtile = v;
                break;
            }
        }
        /* walking down a ramp: stay on it rather than falling a step at a time */
        if (!(coll & COLL_DOWN) && (b->coll & COLL_DOWN) && (b->ground_tile & COLL_RAMP))
            for (int c = c0; c <= last; c++)
                if ((level_cell(L, c, row + 1) & (COLL_RAMP | 4)) == (COLL_RAMP | 4)) {
                    coll |= COLL_DOWN; vy = 0; ny = (row + 1) * ch + (1.0f - hy); gtile = level_cell(L, c, row + 1);
                    break;
                }
    }
    b->x = nx - b->ox; b->y = ny - b->oy;
    b->vx = vx; b->vy = vy; b->coll = coll; b->ground_tile = gtile;
}
