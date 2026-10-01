/* Actual CRHC/PNG regression, run with make -f Makefile.headless check-heroes
 * (also FIXED=1). Stdout is a pose manifest for tools/check_hero_poses.py. */
#include "app.h"
#include "heroes.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

Ren *rnull_renderer(void);
void rnull_frame_end(unsigned frame);

int main(int argc, char **argv)
{
    assert(app_init(rnull_renderer(), argc > 1 ? argv[1] : "SaberRider/data", 1));
    hero_quiet(true);
    static const uint32_t ids[] = {0x8403195A, 0x79260A58};
    static const uint32_t shoot_ids[] = {0x8403195A, 0x79260A58, 0x9C8F9A9E, 0x26818B85};
    /* Standing diagonal changes must emit from the new gun on the same step,
     * including turning from a horizontal shot to the opposite diagonal. */
    Game *g = app_game();
    g->state = 10; g->title_on = false; g->player.locked = false;
    for (int h = 0; h < 4; h++) for (int aim = 1; aim < 8; aim += 2) {
        player_spawn(&g->player, shoot_ids[h], R(1700), R(177));
        g->cam_x = R(1487);
        g->player.ch.body.coll = COLL_DOWN;
        player_resolve(&g->player.ch, R_DT);
        g->player_bullets.n = 0;
        for (int b = 0; b < BTN_COUNT; b++) { g->in.state[b] = 1; g->in.raw[b] = false; }
        bool left = aim == AIM_UL || aim == AIM_DL;
        bool up = aim == AIM_UL || aim == AIM_UR;
        g->in.raw[BTN_AIM] = g->in.raw[BTN_SHOOT] = true;
        g->in.raw[left ? BTN_LEFT : BTN_RIGHT] = true;
        g->in.raw[up ? BTN_UP : BTN_DOWN] = true;
        game_update(g, R_DT);
        Character *c = &g->player.ch;
        assert(c->aim == aim && c->facing == !left);
        assert(g->player_bullets.n == 1);
        Bullet *b = &g->player_bullets.b[0];
        real step = r_mul_dt(R(350), R_DT);
        assert(b->dir == aim);
        assert(r_abs(b->x + (left ? step : -step) - c->body.x - c->muzzle_x) < R(0.01f));
        assert(r_abs(b->y + (up ? step : -step) - c->body.y - c->muzzle_y) < R(0.01f));
    }
    unsigned draw_frame = 0;
    for (int h = 0; h < 2; h++) {
        Character c;
        assert(character_init(&c, ids[h], false));
        for (int side = 0; side < 2; side++) {
            /* A roof drop must lose the crouch pose and acquire both halves of
             * the falling sprite, without retaining a run bob. */
            c.state = CS_CROUCH; c.facing = side; c.coll = COLL_DOWN;
            c.flags = CF_DROP_REQ | CF_ON_ONEWAY;
            c.body.y = R(100); c.body.coll = COLL_DOWN;
            character_jump(&c); assert(c.state == CS_DROP);
            player_resolve(&c, R_DT);
            assert(c.body.flags & PHYS_IGNORE_DOWN);
            c.body.coll = 0; character_sync_ground(&c);
            c.aim = side ? AIM_R : AIM_L;
            player_resolve(&c, R_DT);
            assert(c.anim == (side ? 49 : 48));
            assert(c.overlay == (side ? 23 : 20) && !c.walk_bob);
            for (int pose = 0; pose < 3; pose++) {
                for (int aim = 0; aim < 8; aim++) {
                    if ((aim == AIM_L || aim == AIM_UL || aim == AIM_DL) && side) continue;
                    if ((aim == AIM_R || aim == AIM_UR || aim == AIM_DR) && !side) continue;
                    if (pose == 1 && (aim == AIM_U || aim == AIM_D)) continue;
                    for (int shoot = 0; shoot < 2; shoot++) {
                        character_reset(&c, false);
                        c.body.x = c.body.y = 0;
                        c.facing = side; c.aim = aim;
                        c.flags = shoot ? CF_SHOOT : 0;
                        c.state = pose == 0 ? CS_AIM : pose == 1 ? CS_WALK : CS_FALL;
                        c.anim = 255; c.overlay = 0;
                        player_resolve(&c, R_DT);
                        for (int tick = 0; tick < 42; tick++) {
                            character_animate(&c, R_DT);
                            character_draw(&c, 0, 0);
                            rnull_frame_end(draw_frame++);
                            int k = c.frame - c.def->anims[c.anim].first;
                            real bob = c.walk_bob && k >= 0 && k < 8 ? r_int(c.torso_bob[k]) : 0;
                            printf("%d %d %d %d %d %d %d %d %d %d %d %d\n", h, side, pose, aim, shoot,
                                   c.frame, c.overlay ? c.ov_frame : -1,
                                   r_round(c.base_ox), r_round(c.base_oy + bob),
                                   r_round(c.muzzle_x + c.origin_x), r_round(c.muzzle_y + c.origin_y), tick);
                        }
                    }
                }
            }
        }
    }
    app_shutdown();
    fprintf(stderr, "hero poses: roof drop and same-step diagonal firing passed\n");
    return 0;
}
