#pragma once
/* Stage 4 — the forest ("Red Palm Jungle").
 *
 * Its art was never shipped: it is rebuilt from two 2018 screenshots of the forest level and the jungle clip
 * behind Colt (../forest: a de-JPEG net trained on the level-1 art, front / back segmentation, exemplar
 * inpainting and quilting) and laid out by ../forest/compose.py into assets/forest/forest.lvl + tile sheets.
 * The file takes over level-1 layer slots (SkyBG = the sky, FarMountains / Mountains = hazy far and mid trees,
 * NearMountains = the trees, MidBG / Cars MidBG = two rows of the fern hedge, Playfield, ForegroundStuff = tower
 * railings, ForegroundStuf2 = foreground ferns; any other tile layer is hidden), replaces the collision grid
 * (ramp ground, one-way tower decks) and carries the enemy triggers in level-1 LEVL object form. exit_x is unused
 * (the stage ends with the clearing's finale, below).
 *
 * forest.lvl (little endian): "FRST" u32 version, u32 width, f32 start_x, start_y, exit_x, u32 nlayers,
 *   nlayers x { char slot[16], char png[16], f32 parallax, u32 w, h, tile, u32 cells[w*h] (tile+1, 0 empty) },
 *   u32 cols, rows, u8 collision[cols*rows], u32 ntriggers,
 *   ntriggers x { u32 type, f32 x, zone_w, u32 nwp, f32 wp[3][2], i32 interval_ms, loops, rand_n, delay_ms } */
#include <stdbool.h>
#include <stdint.h>
#include "level.h"
#include "enemies.h"
#include "night.h"

#define FOREST_MAX_LAYERS 12
#define FOREST_MAX_TRIGGERS 64
#define FINALE_WARPS 8

/* The end of the stage (no exit): once the camera stops on the clearing it stays there and the Outriders come at the
 * hero - running in from both edges and materialising out of the Vapor Zone on the ground - for FINALE_ONSLAUGHT_T
 * seconds; then Hyperjumper comes back (stage 3's fight) while fewer of them keep coming. Once it is down nobody
 * new arrives, and the stage is won when the last Outrider on the field is gone. */
enum { FF_WAIT, FF_ONSLAUGHT, FF_BOSS, FF_MOPUP, FF_WON };
typedef struct {
    int state; float t;
    float arena_x; int sw;
    int tr0, ntr_onslaught, ntr;          /* its edge streams in Enemies.tr: the onslaught's, then the boss round's */
    float warp_t;                         /* time to the next warp-in */
    bool scene_ambush;                    /* set when the finale starts: the game opens FOREST_SCRIPT_AMBUSH */
    struct { bool on; float x, t; int type; } warp[FINALE_WARPS];
} ForestFinale;

typedef struct {
    TileMap maps[FOREST_MAX_LAYERS];
    uint32_t *cells[FOREST_MAX_LAYERS];
    uint8_t *collision;
    float width, start_x, start_y, exit_x;
    LevelObject triggers[FOREST_MAX_TRIGGERS]; int ntriggers;
    ForestFinale fin;
} Forest;

/* Rebuilds L in place from assets/forest/forest.lvl; on failure nothing in L is changed. */
bool forest_init(Forest *f, Level *L);
void forest_dispose(Forest *f);
/* the stage's enemy triggers (fed to enemies_add_trigger), put on the player's sprite layer */
int forest_triggers(const Forest *f, LevelObject *out, int max, int player_layer);
/* feet at (x, feet_y) up on a tower deck (at or above its one-way floor cells): the hero is drawn behind the rail
 * there, in the cabin like the gunmen; below a deck (jumping up through it) he stays in front of the planks */
bool forest_on_deck(const Level *L, float x, float feet_y);
/* radio scenes (dialog_open_script format, written for Fireball like stage 3's): the ambush springing, the win */
extern const char *const FOREST_SCRIPT_AMBUSH, *const FOREST_SCRIPT_OUTRO;

/* the finale, every live step after enemies_update; true once it is won. hj = Hyperjumper (night_boss_load'ed,
 * manual). While it runs the camera stays put (fin.state != FF_WAIT). */
bool forest_finale_update(Forest *f, Enemies *E, Night *hj, Effects *fx, const Level *L, const PhysicsWorld *W,
                          const Player *pl, float cam_x, int sw, int sh, int layer, float dt);
/* the objective top right once Hyperjumper is down: the Outriders left (the ambush itself shows no timer) */
void forest_finale_draw_hud(const Forest *f, const Enemies *E, const Night *hj, SDL_Renderer *ren, int sw);
