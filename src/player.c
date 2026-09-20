#include "player.h"
#include "audio.h"

void player_spawn(Player *p, uint32_t crhc_id, float x, float y)
{
    memset(p, 0, sizeof *p);
    character_init(&p->ch, crhc_id, false);
    p->ch.body.x = x; p->ch.body.y = y;
    p->hp = 2; p->lives = 2;   /* lives are overridden from the options (DAT_00aab788), 2 hearts */
    p->safe_x = p->respawn_x = x; p->safe_y = p->respawn_y = y;
}

void player_control(Player *p, const Input *in, float dt)
{
    Character *c = &p->ch;
    if (p->locked) { character_idle_aim(c); return; }
    bool L = btn_down(in, BTN_LEFT), R = btn_down(in, BTN_RIGHT), U = btn_down(in, BTN_UP), D = btn_down(in, BTN_DOWN);
    if (L && R) L = R = false;
    if (U && D) U = D = false;

    p->want_fire = false;
    if (btn_down(in, BTN_SHOOT)) {
        if (character_request_shoot(c)) p->want_fire = true;
    }
    if (btn_down(in, BTN_AIM) && !(c->flags & CF_IN_JUMP)) {
        /* aim mode: feet planted, 8-way aim */
        if (L && U) character_aim(c, AIM_UL);
        else if (R && U) character_aim(c, AIM_UR);
        else if (L && D) character_aim(c, AIM_DL);
        else if (R && D) character_aim(c, AIM_DR);
        else if (L) character_aim(c, AIM_L);
        else if (R) character_aim(c, AIM_R);
        else if (U) character_aim(c, AIM_U);
        else if (D) character_aim(c, AIM_D);
        else character_idle_aim(c);
        character_aim_stand(c);
    } else {
        if (L) character_move_left(c, U ? 2 : D ? 6 : 0);
        else if (R) character_move_right(c, U ? 2 : D ? 6 : 0);
        else if (U) { character_aim(c, AIM_U); character_aim_stand(c); }
        else if (D) character_down(c);
        else character_idle_aim(c);
    }
    if (btn_pressed(in, BTN_JUMP)) { uint8_t st = c->state; character_jump(c); if (c->state != st) sfx_play(2, 0); }
}

static const float AIM_ANGLE[8] = { 3.1415927f, 2.3561945f, 1.5707964f, 0.7853982f, 0, 5.4977871f, 4.712389f, 3.9269908f };

bool player_try_fire(Player *p, Bullets *bs, Effects *fx, int layer)
{
    Character *c = &p->ch;
    if (!p->want_fire || p->fire_cooldown > 0.0f) return false;
    float x = c->body.x + c->muzzle_x, y = c->body.y + c->muzzle_y;
    /* muzzle flash: strip 8623249C, frames 0..4 for diagonals, 4..8 straight, 15 ms/frame */
    AnimDef diag = { 0, 0, 4, 0, 0.015f, 0 }, straight = { 0, 4, 8, 4, 0.015f, 0 };
    Effect *e = effects_spawn(fx, 0x8623249C, layer, ((c->aim & 0xf9) == 1) ? &diag : &straight, x, y, 8, 8, AIM_ANGLE[c->aim & 7]);
    if (e) { e->follow_x = &c->body.x; e->follow_y = &c->body.y; e->fx0 = c->body.x; e->fy0 = c->body.y; }
    bullets_spawn(bs, BK_PLAYER, layer, x, y, c->aim & 7, 500.0f);
    p->fire_cooldown = 0.2f;
    sfx_play(1, 0);
    return true;
}

void player_frame_end(Player *p, float dt)
{
    /* FUN_0041ef20 + FUN_0041d8f0: the shoot flag (and pose) clears when the emitter cooldown runs out */
    if (p->fire_cooldown > 0) { p->fire_cooldown -= dt; if (p->fire_cooldown <= 0) { p->fire_cooldown = 0; p->ch.flags &= ~CF_SHOOT; } }
    else p->ch.flags &= ~CF_SHOOT;
}

bool player_death_update(Player *p, float dt, float level_h, float *cam_x, int sw)
{
    Character *c = &p->ch; Body *b = &c->body;
    if (c->state != CS_DEAD) {
        p->dead_t = 0;
        if (c->coll & COLL_DOWN) { p->safe_x = b->x; p->safe_y = b->y; }
        return false;
    }
    p->want_fire = false;
    if (c->coll & COLL_DOWN) {
        p->safe_x = b->x; p->safe_y = b->y;
        if (b->y - c->hurt[c->anim < CHAR_MAX_ANIMS ? c->anim : 0].oy < level_h) { p->respawn_x = b->x; p->respawn_y = b->y; }
    } else if (b->y < level_h) {
        p->dead_t -= dt;      /* the timer only runs once the body has settled or left the level */
    }
    p->dead_t += dt;
    if (p->dead_t < 1.0f) return false;
    /* respawn */
    character_reset(c, false);
    c->anim = 0xff; c->frame = 0;
    p->fire_cooldown = 0; p->dead_t = 0;
    b->x = p->respawn_x; b->y = p->respawn_y; b->vx = b->vy = 0;
    c->flags |= CF_HIT; c->hit_t = 170.0f;
    if (p->lives < 1) { p->game_over = true; c->flags |= CF_DEAD; return true; }
    p->lives--;
    p->hp = 2;
    float half = sw * 0.5f;
    float camc = *cam_x + half;
    if (p->respawn_x < 12.0f) p->respawn_x = b->x = 12.0f;
    while (camc - half > p->respawn_x - 12.0f && camc - half > 0) camc -= 4.0f;
    if (camc < half) camc = half;
    *cam_x = camc - half;
    return true;
}
