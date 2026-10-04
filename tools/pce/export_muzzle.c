/* Host-only: the source game's barrel position (muzzle offset from the body) for each aim pose of the selected
 * hero (SABER_HERO), measured by running the real player code. Right-facing only: the PCE draws one facing and
 * mirrors it, so the offset is mirrored too. Output: nine "dx dy" lines in pce_muzzle order. */
#include "app.h"
#include "player.h"
#include <math.h>
#include <stdio.h>
Ren *rnull_renderer(void);
static void setin(Input *in, int L, int R, int U, int D, int S, int A) {
    for (int b = 0; b < BTN_COUNT; b++) { in->state[b] = 1; in->raw[b] = false; }
    if (L) in->state[BTN_LEFT] = 0;
    if (R) in->state[BTN_RIGHT] = 0;
    if (U) in->state[BTN_UP] = 0;
    if (D) in->state[BTN_DOWN] = 0;
    if (S) in->state[BTN_SHOOT] = 0;
    if (A) in->state[BTN_AIM] = 0;
}
int main(int argc, char **argv) {
    if (argc != 2 || !app_init(rnull_renderer(), argv[1], 1)) return 1;
    Game *g = app_game(); Player *p = &g->player; Character *c = &p->ch;
    /* level, level while running, crouch, up, down, up-diagonal standing / down-diagonal standing, and the same running */
    static const struct { int R, U, D, A; } cases[9] = {
        {0,0,0,0}, {1,0,0,0}, {0,0,1,0}, {0,1,0,0}, {0,0,1,1}, {1,1,0,1}, {1,0,1,1}, {1,1,0,0}, {1,0,1,0},
    };
    real dt = R(1.0f / 60);
    for (int i = 0; i < 9; i++) {
        Input in;
        character_reset(c, false); c->body.x = 100; c->body.y = 155; c->body.vx = c->body.vy = 0;
        for (int s = 0; s < 50; s++) {   /* land, facing right */
            setin(&in, 0, s > 40, 0, 0, 0, 0);
            character_sync_ground(c); player_control(p, &in, dt); player_resolve(c, dt);
            physics_step(&g->world, &g->level, &c->body, dt); character_animate(c, dt);
        }
        for (int s = 0; s < 24; s++) {
            setin(&in, 0, cases[i].R, cases[i].U, cases[i].D, 1, cases[i].A);
            character_sync_ground(c); player_control(p, &in, dt); player_resolve(c, dt);
            physics_step(&g->world, &g->level, &c->body, dt); character_animate(c, dt);
        }
        printf("%ld %ld\n", lroundf((float)c->muzzle_x), lroundf((float)c->muzzle_y));
    }
    return 0;
}
