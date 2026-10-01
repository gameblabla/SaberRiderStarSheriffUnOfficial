/* Host regression for shared character code and emitted shadow geometry.
 * make -f Makefile.headless check-shadows (also FIXED=1)
 * To exercise the Saturn configuration explicitly:
 * cc -std=gnu11 -O2 -ffunction-sections -fdata-sections -DPLAT_SATURN -DFX_NO_FLOAT
 *    -Isrc tools/saturn/tests/hero_shadows.c src/character.c src/heroes.c src/fx.c
 *    -Wl,--gc-sections -o /tmp/hero_shadows && /tmp/hero_shadows
 */
#include "character.h"
#include "heroes.h"
#include "assets.h"
#include "audio.h"
#include "platform/render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static RFRect bands[3];
static int nbands;
const char *asset_path(const char *name) { return name; }
const char *plat_getenv(const char *name) { (void)name; return NULL; }
CBlock *cblock_from_png(uint32_t id, const char *path, int tw, int th)
{ static CBlock cb; (void)id; (void)path; (void)tw; (void)th; return &cb; }
void sfx_clear_overrides(void) { }
void sfx_set_override(int id, const char *const *paths, int n) { (void)id; (void)paths; (void)n; }
Ren *gfx_renderer(void) { return NULL; }
void r_set_draw_blend(Ren *r, RBlend b) { (void)r; (void)b; }
void r_set_draw_color(Ren *r, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{ (void)r; (void)a; (void)b; (void)c; (void)d; }
void r_fill_rect(Ren *r, const RFRect *q) { (void)r; assert(nbands < 3); bands[nbands++] = *q; }
uint8_t level_cell(const Level *l, int x, int y)
{ return x >= 0 && x < l->cols && y == 4 ? 4 : 0; }

static void check(uint32_t id, real crouch_l, real crouch_r)
{
    Character c = {0}; CharDef def = {0};
    c.crhc_id = id; c.def = &def; c.origin_x = R(32);
    for (int a = 1; a < CHAR_MAX_ANIMS; a++)
        def.anims[a] = (AnimDef){0, 0, 5, 0, R(0.1f), 0};
    hero_quiet(true); assert(hero_apply(&c));
    Level l = {0}; l.cols = 100; l.rows = 8; l.cellw = l.cellh = 16;
    static const uint8_t collision = 4; l.collision = &collision;
    c.body.y = R(64); c.body.x = R(500);
    const real steps[] = {R_DT, R(1.0f / 30), R(1.0f / 144)};
    for (int rate = 0; rate < 3; rate++)
    for (int side = 0; side < 2; side++) {
        c.facing = side;
        for (int pose = 0; pose < 6; pose++) {
            static const int states[] = {CS_WALK, CS_CROUCH, CS_CROUCH, CS_IDLE, CS_JUMP, CS_FALL};
            static const int anims[] = {36, 40, 42, 1, 46, 48};
            c.state = states[pose]; c.anim = anims[pose] + side;
            real expected = pose == 0 ? c.run_shadow_x[side] :
                            pose < 3 ? (side ? crouch_r : crouch_l) :
                            pose == 3 ? c.idle_shadow_x[side] :
                            pose == 4 ? c.jump_shadow_x[side] : c.fall_shadow_x[side];
            c.shadow_x = R(100); /* transition from any preceding pose */
            character_animate(&c, steps[rate]); assert(c.shadow_x == expected);
            real relative[3] = {0}, width[3] = {0};
            for (int frame = 0; frame < 120; frame++) {
                /* Sweep independent fractional world/camera positions across
                 * repeated animation cycles. */
                c.body.x = R(500) + frame * R(0.37f);
                /* Include negative screen coordinates near the left edge. */
                real cam = R(400) + pose * R(20) + frame * R(0.23f);
                nbands = 0; character_animate(&c, steps[rate]);
                assert(c.shadow_x == expected);
                character_draw_shadow(&c, &l, cam, 0, 352, 224);
                assert(nbands == 3);
                real sprite = r_floorr(c.body.x - c.origin_x - cam);
                for (int b = 0; b < 3; b++) {
                    real offset = bands[b].x - sprite;
                    if (!frame) { relative[b] = offset; width[b] = bands[b].w; }
                    assert(offset == relative[b]); assert(bands[b].w == width[b]);
                }
                assert(bands[1].x + bands[1].w / 2 == sprite + c.origin_x + r_floorr(expected));
            }
        }
    }
}
int main(void)
{
    check(0x8403195A, R(2), 0);
    check(0x79260A58, R(2), R(-1));
    puts("Hero shadows: running, crouching, idle, jumping and falling stay anchored in both directions at 30/60/144 Hz");
    return 0;
}
