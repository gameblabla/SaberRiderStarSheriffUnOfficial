#pragma once
/* Stage 5's second boss: Dark April. Once level 1's boss is down in the cave lab's hall a shadow of April forms in
 * front of the hero (whoever that is - April included) and fights with April's own moveset, Dark Link style: she is
 * April's body (her CRHC and sheet, drawn as a violet shadow) run through the very same player_control /
 * player_resolve as the hero, and her "buttons" come from a small AI that mostly mirrors the player's own buttons a
 * moment later (left and right swapped: walk at her and she walks at you, shoot and she shoots back, crouch and she
 * crouches), dodges shots by jumping or crouching, and breaks off into her own attacks - aimed volleys, slides, leaps
 * over the hero. Her shots are the hero's in violet, slower. The game opens the story scenes she asks for. */
#include "platform/render.h"
#include "platform/plat.h"
#include "player.h"
#include "level.h"
#include "bullets.h"
#include "effects.h"
#include "input.h"

enum { DA_OFF, DA_WAIT, DA_CALL, DA_APPEAR, DA_TALK, DA_READY, DA_FIGHT, DA_DYING, DA_GONE, DA_DONE };

#define DARK_MOTES 64
#define DARK_GHOSTS 4
#define DARK_HIST 32

typedef struct { real x, y, vx, vy, life, max; } DarkMote;

typedef struct {
    int state; real t;
    Player p;                        /* her body: April's CRHC through the player's own controls */
    Input in; bool want[BTN_COUNT];  /* the buttons the AI holds this step (edges made by dark_update) */
    int hp, hp_max, difficulty;
    real flash, iframes, alpha;
    real arena_x; int sw;
    int mode; real mode_t, dodge_cd, dodge_t; int dodge;
    real counter_t;                 /* >0: ducking the hero's level fire and answering low; <0: time since */
    real pref;                      /* ATTACK: the distance she keeps */
    bool crouch_shoot, settled;
    real player_idle, voice_t;
    uint8_t hist[DARK_HIST]; int hist_i;   /* the player's buttons, one byte a step (mirror delay) */
    Character ghost[DARK_GHOSTS]; int nghost, ghost_i; real ghost_t;
    DarkMote motes[DARK_MOTES];
    bool scene_call, scene_meet, scene_outro;   /* requests to the game: open that story scene now */
} DarkApril;

/* level 1's boss just burnt out: the arena is the locked screen at arena_x */
void dark_begin(DarkApril *d, real arena_x, int sw, int difficulty);
/* every live step of stage 5 (live: no dialogue is open). pb = the hero's shots, eb = enemy shots (hers go there). */
void dark_update(DarkApril *d, Player *pl, const Input *pin, const Level *L, const PhysicsWorld *W, Bullets *pb,
                 Bullets *eb, Effects *fx, int layer, real dt, bool live);
void dark_animate(DarkApril *d, real dt);   /* while a dialogue holds the world: she keeps breathing */
void dark_draw(const DarkApril *d, real cam_x, real cam_y);
void dark_draw_hud(const DarkApril *d, Ren *ren, int sw);
bool dark_holds_arena(const DarkApril *d);
void dark_power_hit(DarkApril *d, real frac);   /* a hero's power attack (power.c) */   /* from her arrival to her end: no waves, the camera stays */

/* the story scenes (dialog_open_script format). _APRIL ones are for April as the hero and are opened without the
 * hero swap (dialog_set_hero(HERO_FIREBALL) around the open); the others are written for Fireball like every stage */
extern const char *const DARK_SCRIPT_CALL, *const DARK_SCRIPT_MEET, *const DARK_SCRIPT_OUTRO;
extern const char *const DARK_SCRIPT_CALL_APRIL, *const DARK_SCRIPT_MEET_APRIL, *const DARK_SCRIPT_OUTRO_APRIL;
extern const char *const LAB_SCRIPT_INTRO;
