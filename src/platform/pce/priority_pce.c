#include "play_internal.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
/* Sprite priority on the platform stages. The SAT holds 64 entries and a scanline 16 width units, and admission is first come,
 * first served, so what is drawn first survives. The order is: hero, galloping herd, boss, enemies, enemy bullets and grenades,
 * the hero's own bullets, muzzle flash, then the foreground. When there is not enough room the hero's bullets go first, then the
 * enemies' bullets, and enemies last.
 * Refusals are tracked: an enemy that could not be drawn pauses the spawning of filler enemies for a while, and an enemy bullet
 * that could not be drawn stops further enemy fire for a while (a bullet nobody can see must not hurt), so the crowd settles at
 * what the hardware can show instead of flickering at its edge. */
uint8_t enemy_pressure PCE_WORK,shot_pressure PCE_WORK;

/* pce_control.phase: 0 the bosses' lasers (before the enemies; only called while a boss fights), 1 enemy bullets and grenades,
 * then the hero's bullets. Walks the pool by pointer: this runs every frame and the herd scenes have no cycles to spare. */
static __attribute__((noinline)) PCE_HUD bool draw_shot(const Shot *s,uint16_t id,bool flip) {
#ifdef PCE_SGX
    extern uint16_t sprite_emit_id;
    extern int16_t sprite_emit_x,sprite_emit_y;
    extern uint8_t sprite_emit_flip;
    void pce_sgx_projectile_body(void);
    sprite_emit_id=id;sprite_emit_x=s->x-camera;sprite_emit_y=s->y-16;sprite_emit_flip=flip;
    overlay_call(0x77,pce_sgx_projectile_body);
    return pce_control.ok;
#else
    return video_sprite_optional(id,s->x-camera,s->y-16,flip,16);
#endif
}
PCE_HUD void shots_draw_pass(void) {
    Shot *s=shots;
    if(pce_control.phase==0) {
        for(uint8_t n=NSHOTS;n;--n,++s)if(s->active&&s->enemy==3) {
            uint16_t id=45+(s->vy?(s->vx?1:2):0);
            if(!draw_shot(s,id,s->vx<0)){s->active=0;shot_pressure=30;}
        }
        return;
    }
    uint16_t grenade=pce_enemy_base[pce_metrics.stage-1]+60;
    for(uint8_t n=NSHOTS;n;--n,++s)if(s->active&&s->enemy&&s->enemy!=3) {
        if(!draw_shot(s,s->enemy==4?grenade+20+(s->t>>3):s->enemy==2?grenade+((s->t>>3)&7):37,false)) {
            int16_t x=s->x-camera;if(x>-8&&x<264){shot_pressure=30;s->active=0;}
        }
    }
    s=shots;
    for(uint8_t n=NSHOTS;n;--n,++s)if(s->active&&!s->enemy)draw_shot(s,36,false);
    if(enemy_pressure)--enemy_pressure;
    if(shot_pressure)--shot_pressure;
}

/* May a trigger bring in another humanoid? pce_control.y: the trigger is a placed one (finite loops), answer in pce_control.x.
 * A placed enemy (the level's own snipers, kneelers, shields) always comes when the hero crosses its zone, as in the source;
 * the endless streams of walkers and grunts fill what room is left: three at most, none while enemies are being refused. */
PCE_X1 void spawn_room(void) {
    uint8_t live=0;
    for(uint8_t k=0;k<8;++k)
        if(actors[k].active&&!actors[k].dead&&(actors[k].type<11||actors[k].type>28))++live;
    pce_control.x=pce_control.y?live<6:live<3&&!enemy_pressure;
}
