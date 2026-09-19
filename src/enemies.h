#pragma once
/* saber_game::EnemySpawner + saber_game::Enemy */
#include "character.h"
#include "level.h"
#include "bullets.h"
#include "effects.h"
#include "player.h"

#define MAX_ENEMIES 64
#define MAX_TRIGGERS 100

enum EnemyClass {   /* dispatch ids used by FUN_0041f240 */
    EC_NONE = 0, EC_WALKER = 1, EC_GRUNT = 2, EC_GRUNT_B = 3, EC_SNIPER = 4, EC_SNIPER_B = 5,
    EC_KNEELER = 6, EC_KNEELER_B = 7, EC_HORSEBOSS = 8, EC_BUGGY = 9,
    EC_PROP = 10 /* ..21 */, EC_STAMPEDE = 22 /* ..25 */, EC_CUTSCENE = 26, EC_END = 27
};

typedef struct {
    uint16_t cls;         /* 0 free */
    bool dying; float death_t;
    Character ch;
    int type, layer, variant;
    uint8_t dir;          /* 0 left, 1 right, 2 stand (slot+0x15) */
    float spawn_t;        /* invulnerable drop-in timer (+0x1284c) */
    float t0, t1; int st; /* class-specific */
    float knock;          /* pending vx knockback */
    bool gun_alive; float gun_cd;   /* emitter (+0xb20) */
} Enemy;

typedef struct {
    float cx, cy, hx, hy;
    int type, layer;
    uint16_t interval_ms; uint8_t rand_n;
    float timer; int remaining, remaining_init, spawned;
    float wp[8][2]; int nwp;
} Trigger;

typedef struct {
    Enemy e[MAX_ENEMIES]; int count;
    bool spawner_enabled;
    bool cam_locked, cam_shake;      /* camera flags driven by the horse convoy (FUN_00414eb0) */
    bool release_request;            /* set when the last convoy horse is gone */
    Trigger tr[MAX_TRIGGERS]; int ntr;
    /* per-frame cache (FUN_0041f240 preamble) */
    float px, py, phx, phy, phcx, phcy, death_floor, dt;
} Enemies;

void enemies_reset(Enemies *E);
void enemies_add_trigger(Enemies *E, const LevelObject *o);
void enemies_update(Enemies *E, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, Bullets *eb, Effects *fx,
                    float cam_x, int sw, int sh, float dt);
void enemies_draw(const Enemies *E, int layer, float cam_x, float cam_y);
bool player_damage(Player *pl, int hit_dir, int dmg);
void player_check_enemy_bullets(Player *pl, Bullets *eb, Effects *fx, float cam_x, int sw, int sh);
Enemy *enemy_spawn(Enemies *E, const Trigger *t, float x, float y, const Level *L, const PhysicsWorld *W, float cam_x, int sw, int sh);
