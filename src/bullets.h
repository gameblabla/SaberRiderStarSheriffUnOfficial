#pragma once
/* saber_game::Bullets (one instance for player shots, one for enemy shots). */
#include <stdint.h>
#include <stdbool.h>
#include "level.h"
#include "effects.h"

#define MAX_BULLETS 1024
enum { BK_PLAYER = 0, BK_ENEMY = 1, BK_GRENADE = 2, BK_LASER = 3 };

typedef struct {
    float x, y;
    uint8_t dir;      /* 0..7 aim dir; 8..15 = ballistic variants */
    float speed, t, angle;
    uint8_t kind;
    int layer;
    Sprite *spr;
    bool dark;        /* stage 5: Dark April's shots (the player's shot sprite in violet) */
} Bullet;

typedef struct { Bullet b[MAX_BULLETS]; int n; } Bullets;

void bullets_spawn(Bullets *bs, int kind, int layer, float x, float y, int dir, float speed);
void bullets_update(Bullets *bs, const Level *L, Effects *fx, float dt, float cam_x, float cam_y, int sw, int sh);
void bullets_draw(const Bullets *bs, int layer, float cam_x, float cam_y);
