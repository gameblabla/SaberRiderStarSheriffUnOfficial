#include "play_internal.h"
#include "presentation_pce.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "assets.h"
#include <string.h>

extern uint8_t sat_count,sat_page;
extern vdc_sprite_t sat[2][64];
extern volatile uint16_t pce_scroll_y;

/* Positions retain whole-world range. Fractions and velocities are separate
 * Q8 values; a 16-bit fixed-point world coordinate would overflow after 127px. */
Body player;
Actor actors[8] PCE_WORK;
Shot shots[NSHOTS] PCE_WORK;
Body *phys_body;int16_t cell_x,cell_y;uint8_t cell_value;
static uint8_t collision[32][32] PCE_WORK;
static uint16_t column_tags[32];
const PceScene *play_scene;
#define scene play_scene
uint16_t camera,frame;
uint8_t hero,facing,safe_timer;
uint8_t slide_time,pce_panel_restore;
static uint8_t fire_timer,crouch,death_time,jumping,jump_time,flash_time,flash_diag;
static int8_t flash_dx,flash_dy;
int16_t safe_x,safe_y;
static int16_t drop_y;

PCE_CODE static uint8_t cell(int16_t x,int16_t y) {
    if(x<0||(uint16_t)x>=scene->ccols||y<0||(uint16_t)y>=scene->crows) return 0;
    uint8_t slot=x&31;
    if(column_tags[slot]!=(uint16_t)x) {
        arcade_read(1,scene->collision+(uint32_t)(uint16_t)x*scene->crows,collision[slot],scene->crows);
        column_tags[slot]=x;
    }
    return collision[slot][y];
}
/* Spawn probe (the source's face_and_probe): an enemy that appears at the screen edge walks 64 px ahead in its
 * mind; if a wall is in the way (a crashed car) its spawn point is raised 8 px and the walk lengthened, until the
 * body clears, so it drops onto the roof instead of being born inside the car and jittering there. */
int16_t probe_x,probe_y;uint8_t probe_left;
PCE_CODE void spawn_clear(void) {
    int16_t y=probe_y,n=64;
    while(y>8) {
        int16_t r0=(y+9-23)>>3,r1=(y+9+23)>>3,x=probe_x+4,c0=probe_left?(x-8-n)>>3:(x+8)>>3,c1=probe_left?(x-8)>>3:(x+8+n)>>3;
        bool hit=false;
        for(int16_t c=c0;c<=c1&&!hit;++c)for(int16_t r=r0;r<r1;++r)if(cell(c,r)&(probe_left?2:1)){hit=true;break;}
        if(!hit)break;
        y-=8;n+=8;
    }
    probe_y=y;
}
PCE_CODE static void advance(int16_t *p,uint8_t *fraction,int16_t velocity) {
    int16_t sum=(int16_t)*fraction+velocity;
    *p+=sum>>8;*fraction=sum;
}
PCE_CODE static void physics(Body *b) {
    uint8_t prior=b->coll,ground=b->ground;
    b->vy+=34;if(b->vy>2560) b->vy=2560;
    advance(&b->x,&b->fx,b->vx);advance(&b->y,&b->fy,b->vy);
    int16_t x=b->x+4,y=b->y+9;
    uint8_t hit=0,tile=0;
    int16_t r0=(y-23)>>3,r1=(y+23)>>3;
    if(b->vx>0) {
        int16_t col=(x+8)>>3;
        if(x+8>scene->width) { x=scene->width-8;hit|=1;b->vx=0; }
        else for(int16_t r=r0;r<r1;++r) if(cell(col,r)&1) {x=col*8-8;b->vx=0;hit|=1;break;}
    } else if(b->vx<0) {
        int16_t col=(x-8)>>3;
        if(x-8<0) {x=8;b->vx=0;hit|=2;}
        else for(int16_t r=r0;r<r1;++r) if(cell(col,r)&2) {x=(col+1)*8+8;b->vx=0;hit|=2;break;}
    }
    int16_t c0=(x-7)>>3,c1=(x+7)>>3,last=c1-(c0<c1),row;
    if(b->vy<0) {
        row=(y-23)>>3;
        for(int16_t c=c0;c<=last;++c) if(cell(c,row)&8) {y=(row+1)*8+23;b->vy=0;hit|=8;break;}
    } else if(b->vy>0&&!(b==&player&&b->y<drop_y)) {
        row=(y+23)>>3;
        for(int16_t c=c0;c<=last;++c) {
            uint8_t v=cell(c,row);
            if(v&4) {
                if(v&16) for(uint8_t k=0;k<2;++k) if((cell(c,row-1)&20)==20) {--row;v=cell(c,row);}
                y=row*8+1-23;b->vy=0;hit|=4;tile=v;break;
            }
        }
        if(!(hit&4)&&(prior&4)&&(ground&16)) for(int16_t c=c0;c<=last;++c)
            if((cell(c,row+1)&20)==20) {y=(row+1)*8+1-23;b->vy=0;hit|=4;tile=cell(c,row+1);break;}
    }
    b->x=x-4;b->y=y-9;b->coll=hit;b->ground=tile;
}
PCE_CODE static void shoot(int16_t x,int16_t y,int16_t vx,int16_t vy) {
    uint8_t active=0;
    for(uint8_t k=0;k<NSHOTS;++k)if(shots[k].active&&!shots[k].enemy)++active;
    if(active>=4)return;
    for(uint8_t k=0;k<NSHOTS;++k) if(!shots[k].active) {
        shots[k]=(Shot){x,y,vx<<8,vy<<8,1,0,0,0,0};return;
    }
}
/* Enemy shots (called from enemy_pce.c). Q8 px a step: 166 px/s (shield 200), 0.7 x that along a diagonal. */
uint16_t rng=0xACE1;
const Actor *fire_actor;uint8_t fire_aim;int8_t fire_mx,fire_my;bool fire_fast;
PCE_CODE static Shot *new_shot(int16_t x,int16_t y,uint8_t enemy) {
    uint8_t live=0;
    for(uint8_t k=0;k<NSHOTS;++k)if(shots[k].active&&shots[k].enemy)++live;
    if(live>=(pce_campaign.boss_kind?3:5))return 0;
    for(uint8_t k=0;k<NSHOTS;++k)if(!shots[k].active) {
        shots[k]=(Shot){x,y,0,0,1,enemy,0,0,0};return &shots[k];
    }
    return 0;
}
PCE_CODE void fire_call(void) {
    static const int8_t dir_x[8]={-1,-1,0,1,1,1,0,-1},dir_y[8]={0,-1,-1,-1,0,1,1,1};
    uint8_t aim=fire_aim;
    Shot *s=new_shot(fire_actor->b.x+fire_mx,fire_actor->b.y+fire_my,1);if(!s)return;
    int16_t v=aim&1?(fire_fast?597:496):(fire_fast?853:708);
    s->vx=dir_x[aim]>0?v:dir_x[aim]<0?-v:0;
    s->vy=dir_y[aim]>0?v:dir_y[aim]<0?-v:0;
}
PCE_CODE static uint8_t distance8(int16_t a,int16_t b) {
    int16_t n=a-b;if(n<0)n=-n;
    return n>255?255:n;
}
PCE_CODE void grenade_call(void) {
    /* aimed ahead of a hero walking towards the thrower (enemies.c update_kneeler); facing: the hero looks left */
    const Actor *a=fire_actor;
    int16_t py=player.y+8,bx=a->b.x,by=a->b.y;
    uint8_t dx=distance8((bx<player.x?60:0)-bx,facing?60-player.x:-6-player.x);
    if(dx<80)dx=80;if(dx>200)dx=200;
    int16_t dy=distance8(py,by)>>2;if(py<by)dy=-((dy<<3)+(dy<<1));
    uint8_t r;{uint8_t carry=rng&1;rng>>=1;if(carry)rng^=0xB400;r=(uint8_t)(rng^(rng>>8));}
    int16_t spd=(int16_t)(r%6)+(((dx-dy)*7)>>3)-3;
    if(spd<60)spd=60;if(spd>160)spd=160;
    Shot *s=new_shot(bx+(a->aim?14:-15),by-4,2);if(!s)return;
    s->vx=(spd<<2)+(spd>>2);if(!a->aim)s->vx=-s->vx;   /* Q8 px a step = speed/60*256; aim 0 = left, 4 = right */
}
PCE_CODE void phys_call(void) {physics(phys_body);}
PCE_CODE void cell_call(void) {cell_value=cell(cell_x,cell_y);}
PCE_CODE void play_init(uint8_t stage,uint8_t selected) {
    scene=&pce_scenes[stage-1];hero=selected;hero_sprite=selected*9;camera=frame=0;
    facing=fire_timer=safe_timer=crouch=slide_time=death_time=jumping=jump_time=pce_death=0;drop_y=-32767;
    player=(Body){.x=scene->sx,.y=scene->sy};safe_x=player.x;safe_y=player.y;
    memset(actors,0,sizeof actors);memset(shots,0,sizeof shots);
    memset(column_tags,0xff,sizeof column_tags);
    overlay_call(0x70,encounter_init);
    pce_metrics.hp=campaign_hearts();
    overlay_call(0x70,combat_start);
}
PCE_CODE void play_tick(uint8_t keys,uint8_t pressed) {
    ++frame;
    if(!pce_campaign.diagnostic){overlay_call(0x70,combat_tick);
        if(pce_campaign.state!=CAM_PLAY||pce_campaign.event)return;}
    /* The story camera pans with the world frozen. */
    if(cut_phase==2||cut_phase==6){pce_metrics.camera_x=camera;return;}
    if(cut_phase)keys=pressed=0;
    if(safe_timer)--safe_timer;if(fire_timer)--fire_timer;if(flash_time)--flash_time;
    if(pce_death) {
        /* Like the main game: the death animation plays where the hero fell,
         * then it stands up again at the last safe spot - no stage reload. */
        keys=pressed=0;slide_time=0;jumping=0;
        if(!death_time){player.vx=0;safe_timer=255;}
        if(++death_time>=80) {
            death_time=0;pce_death=0;
            if(!pce_campaign.lives){pce_campaign.state=CAM_OVER;pce_campaign.timer=0;return;}
            --pce_campaign.lives;pce_metrics.hp=campaign_hearts();
            player=(Body){.x=safe_x,.y=safe_y};safe_timer=120;
        }
    }
    /* Down alone crouches; down with a direction keeps running while aiming down-diagonally. */
    player.vx=0;crouch=(keys&KEY_DOWN)&&(player.coll&4)&&!(keys&(KEY_LEFT|KEY_RIGHT));
    if(!(keys&KEY_SELECT)&&!crouch) {
        if(keys&KEY_LEFT){player.vx=pce_campaign.boost&&hero==2?-726:-427;facing=1;}
        else if(keys&KEY_RIGHT){player.vx=pce_campaign.boost&&hero==2?726:427;facing=0;}
    }
    if((pressed&KEY_2)&&(player.coll&4)) {
        if(crouch&&player.ground==4)drop_y=player.y+20;
        else if(crouch)slide_time=24;
        else {player.vy=-1237;audio_effect(2);jumping=1;jump_time=0;}
    }
    if(keys&KEY_SELECT) {
        if(keys&KEY_LEFT)facing=1;
        if(keys&KEY_RIGHT)facing=0;
    }
    if(slide_time){crouch=1;player.vx=(int16_t)slide_time*43;if(facing)player.vx=-player.vx;--slide_time;}
    physics(&player);
    if(player.coll&4)jumping=0;else if(jumping&&jump_time<255)++jump_time;
    /* The screen only scrolls forwards: the left edge is a wall. */
    if(!cut_phase&&player.x<(int16_t)camera+8){player.x=camera+8;if(player.vx<0)player.vx=0;}
    if((pce_campaign.boss_kind||herd_locked)&&player.x>(int16_t)camera+248)player.x=camera+248;   /* boss arena / herd: the screen is locked */
    if((player.coll&4)&&!pce_death){safe_x=player.x;safe_y=player.y;}
    if(player.y>272&&!pce_death) {
        campaign_hurt();audio_effect(11);   /* the fall voice replaces the hurt yell */
        if(!pce_death){player=(Body){.x=safe_x,.y=safe_y};safe_timer=120;}
    }
    if((keys&KEY_1)&&!fire_timer) {
        /* Straight shots fly 8 px a step, diagonals 6 on each axis (the source's 0.7 x). The shot leaves the barrel
         * of the pose drawn: pce_muzzle holds the source game's own muzzle offset per hero and pose (level, level
         * running, crouch, up, down, then up / down diagonals standing and running); the art is mirrored for left. */
        bool side=keys&(KEY_LEFT|KEY_RIGHT),run=(player.coll&4)&&player.vx;
        bool up=keys&KEY_UP,down=(keys&KEY_DOWN)&&(!crouch||(keys&(KEY_SELECT|KEY_LEFT|KEY_RIGHT)));
        int16_t vx=facing?-8:8,vy=0;uint8_t pose=crouch?2:run;
        if(up||down) {
            vy=up?-8:8;
            if(side){vx=facing?-6:6;vy=up?-6:6;pose=(up?5:6)+(run?2:0);}
            else{vx=0;pose=up?3:4;}
        }
        const int8_t *muzzle=pce_muzzle[hero][pose];
        int8_t air[2];
        if(!(player.coll&4)) {
            /* In the air the somersault (or the frozen run frame) is drawn whatever is aimed; the shot leaves a
             * 14 px ring around the ball's centre (0,16): the source's own air muzzles (16,18) (11,7) (0,2). */
            air[0]=up||down?(side?11:0):16;air[1]=up?(side?7:2):down?(side?25:30):18;muzzle=air;
        }
        shoot(player.x+(facing?-muzzle[0]:muzzle[0]),player.y+muzzle[1],vx,vy);
        flash_time=4;flash_diag=side&&(up||down);flash_dx=facing?-muzzle[0]:muzzle[0];flash_dy=muzzle[1];   /* the source's 4-frame flash at the barrel */fire_timer=pce_campaign.boost&&hero==3?4:12;audio_effect(1);
    }
    if(!(frame&3)&&!(pce_metrics.stage==4&&pce_campaign.boss_round==2))overlay_call(0x6f,encounters);
    overlay_call(0x73,world_update);
    if(pce_campaign.boss_kind)camera=scene->width-256;
    else if(!cut_phase&&!herd_locked&&player.x>120&&(uint16_t)(player.x-120)>camera) {
        uint16_t gap=player.x-120-camera;camera+=gap>4?4:gap;   /* catches up at most 4 px a step (after a lock) */
    }
    if(camera>(uint16_t)(scene->width-256))camera=scene->width-256;
    pce_metrics.player_x=player.x;pce_metrics.player_y=player.y;pce_metrics.camera_x=camera;pce_metrics.hero=hero;
}
uint16_t hero_sprite;
PCE_BOSS void play_draw(void) {
    /* The hardware scroll stays put until this frame's SAT is uploaded (see irq.S). */
    pce_scroll_hold=1;
    video_background(camera);video_sat_begin();foreground_prepare();presentation_draw();
    uint8_t world_first=sat_count;
    uint8_t keys=pce_control.keys,stage=pce_metrics.stage-1;
    bool grounded=player.coll&4,side=keys&(KEY_LEFT|KEY_RIGHT);
    uint8_t pose=crouch?8:!grounded?7:player.vx?1+(frame/6)%6:0;
    uint16_t id=hero*9+pose,mb=pce_motion_base[stage];
    const uint8_t *ap=pce_hero_pose[hero];
    /* Extra strips after the motion rows: death, slide, standing shot, idle breathing, somersault jump. */
    if(pce_death){uint8_t d=death_time/6;id=mb+ap[4]+(d<ap[5]?d:ap[5]-1);}
    else if(slide_time)id=mb+hero*3+2;
    else if(grounded&&!crouch&&!player.vx&&fire_timer)id=mb+hero*3+(fire_timer>6);
    else if(!pose)id=mb+ap[0]+(uint8_t)(frame/10)%ap[1];
    else if(jumping&&!grounded)id=mb+ap[2]+((jump_time/3)&3);
    /* Aim poses: up / down diagonals (running when moving), straight up, and straight down in the air. */
    if(!slide_time&&!pce_death&&grounded&&((keys&KEY_UP)||((keys&KEY_DOWN)&&(side||(keys&KEY_SELECT))))) {
        uint8_t direction=keys&KEY_UP?0:1;
        id=pce_present_base[stage][2]+hero*16;
        if(!side)id+=14+direction;
        else {
            /* Row layout: 0/1 standing up/down, 2-7 running up, 8-13 running down, 14/15 straight up/down. */
            if(player.vx)id+=2+direction*6+(frame/6)%6;
            else id+=direction;
        }
        if(keys&KEY_LEFT)facing=1;
        if(keys&KEY_RIGHT)facing=0;
    }
    hero_sprite=id;   /* what the dialogue overlay draws while the world is frozen */
    if(!safe_timer||(frame&4)||pce_death)video_sprite(id,player.x-camera,player.y-16,facing,16);
    /* The galloping herd comes right after the hero, ahead of shots and enemies: where the SAT or a scanline is full it
     * is what the optional sprites give way to, so the horses never flicker. */
    if(herd_on)overlay_call(0x6f,herd_draw);
    /* Essential projectiles precede optional distant enemies (while the herd runs they give way to the horses). */
    bool (*draw)(uint16_t,int16_t,int16_t,bool,uint8_t)=herd_on?video_sprite_optional:video_sprite;
    if(!pce_campaign.diagnostic)overlay_call(0x70,combat_draw);
    if(flash_time)draw(pce_flash_base[stage]+(flash_diag?0:4)+4-flash_time,player.x+flash_dx-camera,player.y+flash_dy-16,false,16);
    overlay_call(0x70,shots_draw);
    overlay_call(0x74,actors_draw);
    /* While the herd is on screen the SAT has no room for the foreground pieces as well: they would come and go with
     * every horse's piece count, so the foreground layer is left out until it has passed. */
    bool herd=false;
    for(uint8_t k=0;k<8;++k)if(actors[k].active&&actors[k].type==11)herd=true;
    if(!herd)foreground_draw();
    /* A closing dialogue's cells return in the frame its sprites leave: the new SAT takes effect at the next VBlank, so
     * read the cells now, queue the SAT, and write them right after that VBlank, before the beam reaches the panel. */
    if(pce_panel_restore)video_panel_restore_prepare(pce_panel_restore);
    /* Move the world together; HUD entries precede video_front_mark and stay fixed.
     * Apply after admission so a changing shake never splits horse columns. */
    uint8_t shake_y=herd_on?((frame*13^(frame>>2))&3):0;
    if(shake_y)for(uint8_t i=world_first;i<sat_count;++i)sat[sat_page][i].y-=shake_y;
    video_sat_end();
    pce_scroll_y=shake_y;
    if(pce_panel_restore) {
        video_wait();
        overlay_call(0x6f,story_graphics_restore);
        video_panel_restore_apply();pce_panel_restore=0;
    }
}
