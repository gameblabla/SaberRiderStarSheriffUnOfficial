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
    EC_PROP = 10 /* ..21 */, EC_STAMPEDE = 22 /* ..25 */, EC_CUTSCENE = 26, EC_END = 27,
    EC_SHIELD = 28,  /* ours: stage 4's Outrider behind a riot shield (type 30; 31 = dormant until woken) */
    EC_STALKER = 29  /* ours: stage 4's finale, a blue Outrider (type 32) that runs in and takes a firing line on the hero */
};

#define SHIELD_SNIPER_SPRITE 0x534E5052u   /* assets/forest/sniper.png, registered by forest_init */

typedef struct {
    uint16_t cls;         /* 0 free */
    bool dying; float death_t;
    Character ch;
    int type, layer, variant;
    uint8_t dir;          /* boss sweep direction; for humanoids the walk direction is ch.facing itself (slot+0x1283d = character+9),
                             so walking into a wall turns them around */
    float spawn_t;        /* invulnerable drop-in timer (+0x1284c) */
    float t0, t1; int st; /* class-specific */
    float knock;          /* pending vx knockback */
    bool gun_alive; float gun_cd;   /* emitter (+0xb20) */
    int hp;                         /* slot+2 */
    int link;                       /* companion slot index or -1 */
    float ft;                       /* misc timer (+0x12840) */
    float flash;                    /* shield sniper: hit flash */
    bool on_deck;                   /* shield sniper: stands in a tower cabin (the shield's foot is clipped at the floor) */
    bool dormant;                   /* shield sniper type 31: holds his fire, shots pass through, until shield_wake */
    bool aim_down;                  /* shield sniper: rifle 45 degrees down at a hero on the ground ahead (from a tower) */
    float dbg_x; int dbg_still;   /* debug: stuck detection */
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
    int boss_phase;                  /* +0x40028 */
    bool boss_done;                  /* boss slot expired -> level complete (state 0xe) */
    int frame, tick;
    int difficulty;                  /* stage 4's shield: shots it takes */
    bool shield_wake;                /* stage 4's finale: the dormant shield snipers join in */
    bool aim_decks;                  /* stage 4: a sniper aims 45 degrees up at a hero on a tower deck (72 px up: 48 once
                                        quantised, which level 1's "more than 48" leaves flat) */
    int front_layer;                 /* stage 4: the cabin wall layer; a tower sniper's shots and aimed-down rifle are
                                        drawn with it (enemies_draw_front), in front of the planks; -1 = none */
    Trigger tr[MAX_TRIGGERS]; int ntr;
    /* per-frame cache (FUN_0041f240 preamble) */
    float px, py, phx, phy, phcx, phcy, death_floor, dt;
} Enemies;

void enemies_reset(Enemies *E);
void enemies_add_trigger(Enemies *E, const LevelObject *o);
void enemies_update(Enemies *E, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, Bullets *eb, Effects *fx,
                    float cam_x, int sw, int sh, float dt);
void enemies_draw(const Enemies *E, int layer, float cam_x, float cam_y);
void enemies_draw_front(const Enemies *E, float cam_x, float cam_y);
bool player_damage(Player *pl, int hit_dir, int dmg);
void player_check_enemy_bullets(Player *pl, Bullets *eb, Effects *fx, float cam_x, int sw, int sh);
Enemy *enemy_spawn(Enemies *E, const Trigger *t, float x, float y, const Level *L, const PhysicsWorld *W, float cam_x, int sw, int sh);
