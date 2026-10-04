#include "play_internal.h"
#include "presentation_pce.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "assets.h"
#include <string.h>

/* Positions retain whole-world range. Fractions and velocities are separate
 * Q8 values; a 16-bit fixed-point world coordinate would overflow after 127px. */
Body player;
Actor actors[8] PCE_WORK;
Shot shots[24] PCE_WORK;
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
PCE_CODE static void shoot(int16_t x,int16_t y,int16_t vx,int16_t vy,bool enemy) {
    uint8_t active=0;
    for(uint8_t k=0;k<24;++k)if(shots[k].active&&shots[k].enemy==enemy)++active;
    if(active>=(enemy?(pce_campaign.boss_kind?2:4):4))return;
    for(uint8_t k=0;k<24;++k) if(!shots[k].active) {
        shots[k]=(Shot){x,y,vx,vy,1,enemy};return;
    }
}
PCE_CODE void play_init(uint8_t stage,uint8_t selected) {
    scene=&pce_scenes[stage-1];hero=selected;camera=frame=0;
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
    if((pce_campaign.boss_kind||herd_on)&&player.x>(int16_t)camera+248)player.x=camera+248;   /* boss arena / herd: the screen is locked */
    if((player.coll&4)&&!pce_death){safe_x=player.x;safe_y=player.y;}
    if(player.y>272&&!pce_death) {
        campaign_hurt();
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
        shoot(player.x+(facing?-muzzle[0]:muzzle[0]),player.y+muzzle[1],vx,vy,false);
        flash_time=4;flash_diag=side&&(up||down);flash_dx=facing?-muzzle[0]:muzzle[0];flash_dy=muzzle[1];   /* the source's 4-frame flash at the barrel */fire_timer=pce_campaign.boost&&hero==3?4:12;audio_effect(1);
    }
    if(!(frame&3)&&!(pce_metrics.stage==4&&pce_campaign.boss_round==2))overlay_call(0x70,encounters);
    for(uint8_t k=0;k<8;++k) {
        Actor *a=&actors[k];if(!a->active)continue;
        if(((a->b.x<(int16_t)camera-80||(a->b.x>(int16_t)camera+384&&a->type!=11))&&a->type!=28)||a->b.y>272){a->active=0;continue;}
        if(a->type>=12&&a->type<=23)continue;
        if(a->type>=24&&a->type<=27) {
            const int16_t speeds[4]={2016,1142,2352,1344};
            advance(&a->b.x,&a->b.fx,a->flip?-speeds[a->type-24]:speeds[a->type-24]);continue;
        }
        if(a->type==11){advance(&a->b.x,&a->b.fx,a->flip?-512:512);continue;}   /* the herd gallops at the source's 120 px/s */
        if(a->type==28) {
            /* The cutscene outrider (hp 2 waiting, 1 alarmed, 0 running): once it is on screen it freezes for a second
             * facing the hero, then turns and runs off to the right. */
            if(a->hp==2){int16_t d=camera+128-a->b.x;if(d<0)d=-d;if(d<128){a->hp=1;a->timer=62;}}
            else if(a->hp==1){if(!--a->timer){a->hp=0;a->flip=0;}}
            a->b.vx=a->hp?0:512;physics(&a->b);
            if(!a->hp){++a->anim;if(a->b.x>(int16_t)camera+272)a->active=0;}
            continue;
        }
        if(a->dead){if(++a->dead>24)a->active=0;continue;}
        /* Walkers keep the heading they spawned with and turn round at walls and cars; the rest face the hero. */
        if(a->type>=6)a->flip=player.x<a->b.x;
        a->b.vx=a->type<6?(a->flip?-512:512):0;
        if(a->type>=2&&a->timer+24>(pce_options.difficulty==0?120:pce_options.difficulty==1?90:62))a->b.vx=0;   /* a shooter plants its feet for 24 steps, then runs on */
        physics(&a->b);
        if(a->b.coll&3)a->flip^=1;
        if(a->b.vx)++a->anim;
        if(a->timer)--a->timer;else if(a->type>=2&&(a->type!=31||pce_campaign.boss_kind)&&(a->flip==(player.x<a->b.x))) {
            shoot(a->b.x+(a->flip?-16:16),a->b.y-8,a->flip?-3:3,0,true);a->timer=pce_options.difficulty==0?120:pce_options.difficulty==1?90:62;
        }
    }
    for(uint8_t k=0;k<24;++k) {
        Shot *s=&shots[k];if(!s->active)continue;
        s->x+=s->vx;s->y+=s->vy;
        if(s->x<(int16_t)camera-16||s->x>(int16_t)camera+272||s->y<0||s->y>240){s->active=0;continue;}
        if(s->enemy) {
            if(!safe_timer&&s->x>player.x-8&&s->x<player.x+8&&s->y>player.y-14&&s->y<player.y+25) {
                s->active=0;safe_timer=120;audio_effect(5);
                campaign_hurt();
                if(pce_campaign.diagnostic&&!pce_metrics.hp)pce_metrics.hp=3;
            }
        } else for(uint8_t j=0;j<8;++j) {
            Actor *a=&actors[j];
            if(a->active&&!a->dead&&!(a->type>=11&&a->type<=28)&&s->x>a->b.x-10&&s->x<a->b.x+10&&s->y>a->b.y-18&&s->y<a->b.y+24) {
                s->active=0;if(a->type>=30&&facing==a->flip)a->hp=1;
                if(!--a->hp)actor_kill(a);else audio_effect(7);
                audio_effect(4);break;
            }
        }
    }
    if(pce_campaign.boss_kind)camera=scene->width-256;
    else if(!cut_phase&&!herd_on&&player.x>120&&(uint16_t)(player.x-120)>camera) {
        uint16_t gap=player.x-120-camera;camera+=gap>4?4:gap;   /* catches up at most 4 px a step (after a lock) */
    }
    if(camera>(uint16_t)(scene->width-256))camera=scene->width-256;
    pce_metrics.player_x=player.x;pce_metrics.player_y=player.y;pce_metrics.camera_x=camera;pce_metrics.hero=hero;
}
__attribute__((noinline)) void play_draw(void) {
    /* The hardware scroll stays put until this frame's SAT is uploaded (see irq.S). */
    pce_scroll_hold=1;
    video_background(camera);video_sat_begin();foreground_prepare();presentation_draw();
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
            id+=direction;
            if(grounded&&player.vx)id+=2+direction*6+(frame/6)%6;
        }
        if(keys&KEY_LEFT)facing=1;
        if(keys&KEY_RIGHT)facing=0;
    }
    if(!safe_timer||(frame&4)||pce_death)video_sprite(id,player.x-camera,player.y-16,facing,16);
    if(flash_time)video_sprite(pce_flash_base[stage]+(flash_diag?0:4)+4-flash_time,player.x+flash_dx-camera,player.y+flash_dy-16,false,16);
    /* Essential projectiles precede optional distant enemies. */
    for(uint8_t k=0;k<24;++k) if(shots[k].active)
        video_sprite(shots[k].enemy?37:36,shots[k].x-camera,shots[k].y-16,false,16);
    if(!pce_campaign.diagnostic)overlay_call(0x70,combat_draw);
    overlay_call(0x74,actors_draw);
    /* While the herd is on screen the SAT has no room for the foreground pieces as well: they would come and go with
     * every horse's piece count, so the foreground layer is left out until it has passed. */
    bool herd=false;
    for(uint8_t k=0;k<8;++k)if(actors[k].active&&actors[k].type==11)herd=true;
    if(!herd)foreground_draw();
    /* A closing dialogue's cells return in the frame its sprites leave: the new SAT takes effect at the next VBlank, so
     * read the cells now, queue the SAT, and write them right after that VBlank, before the beam reaches the panel. */
    if(pce_panel_restore)video_panel_restore_prepare(pce_panel_restore);
    video_sat_end();
    if(pce_panel_restore){video_wait();video_panel_restore_apply();pce_panel_restore=0;}
}
