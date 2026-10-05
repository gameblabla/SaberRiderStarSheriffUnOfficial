#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "loader_pce.h"
#include <string.h>
#include "hud_pce.h"
#include "scenery_pce.h"
#define SPACE_CODE __attribute__((noinline,minsize,section(".ram_bank115.text")))
typedef struct __attribute__((packed)) {uint16_t time;uint8_t kind,n;int16_t y;uint16_t gap;uint8_t pattern,drop;} Event;
typedef struct {int16_t x,y;uint8_t kind,hp,pattern,drop;uint16_t clock;uint8_t charge;} Foe;
typedef struct {int16_t x,y;int8_t vx,vy;uint8_t on,enemy;} Bolt;
/* An explosion runs 30 steps through the source's frames (space.c FX_EXPL, half a second; five are visible); t 0 is a free slot. */
typedef struct {int16_t x,y;uint8_t t,small;} Expl;
static Foe foes[8];
static Bolt bolts[12];
static Expl expl[10] PCE_WORK;   /* the console RAM is full: this lives in the work bank */
static Event next_event;
static uint16_t flight_clock,spawn_clock,beam_clock,power_timer;
static uint8_t space_flash,hull_flash;
static uint8_t event_index,event_count,pending,gun_clock,hurt,power,bombs,boss_die,boss_gone,noise;
static int16_t ship_x,ship_y,pickup_x,pickup_y,space_boss_y;
static uint8_t pickup,port_hp[7],dead_t;
/* The cruiser's gun ports in dots from the hull's place (boss.txt at four fifths: x 76 + 0.82 x, y 0.8 y); the first five are open, the strut's two
 * open under two thirds. Plain tables: both the simulation's bank and the cruiser's use them. */
static const uint8_t port_x[7]={153,169,187,203,229,234,223},port_y[7]={40,40,39,38,60,52,67};
/* The cruiser (space.c boss_*): ph 0 not yet, 1 WARNING (the music stops, the screen fades), 2 flying in from the right, 3 its greeting is up,
 * 4 the fight, with the nose cannon (laser 0 charging up its wait, 1 gathering, 2 firing, 3 fading), the hangar's drone swarms and the mine rack. */
typedef struct {uint8_t ph,laser,beam,gather;uint16_t t;} SBoss;
static SBoss sb PCE_WORK;
__attribute__((noinline)) static int16_t dist(int16_t a,int16_t b) {int16_t n=a-b;return n<0?-n:n;}
SPACE_CODE static void event_load(void) {
    if(event_index<event_count)arcade_read(2,pce_scenes[6].track+1+(uint16_t)event_index*10,&next_event,10);
}
SPACE_CODE void space_start(void) {
    memset(foes,0,sizeof foes);memset(bolts,0,sizeof bolts);memset(expl,0,sizeof expl);memset(&sb,0,sizeof sb);space_hull_x=0;boss_die=boss_gone=0;
    flight_clock=spawn_clock=beam_clock=power_timer=0;space_flash=hull_flash=0;space_hull_ready=0;space_flashing=0;event_index=pending=gun_clock=hurt=pickup=0;
    pce_campaign.power_cd=pce_campaign.timer=0;
    power=1;bombs=3;ship_x=48;ship_y=112;space_boss_y=80;
    arcade_read(2,pce_scenes[6].track,&event_count,1);event_load();
    for(uint8_t k=0;k<7;++k)port_hp[k]=20;
    pce_campaign.boss_kind=0;pce_campaign.boss_hp=1200;pce_campaign.boss_round=0;pce_metrics.hp=4;pce_death=dead_t=0;
}
__attribute__((noinline)) static void bolt(int16_t x,int16_t y,int8_t vx,int8_t vy,bool enemy) {
    for(uint8_t k=0;k<12;++k)if(!bolts[k].on){bolts[k]=(Bolt){x,y,vx,vy,1,enemy};return;}
}
__attribute__((noinline)) static void blast(int16_t x,int16_t y,bool small) {
    for(uint8_t k=0;k<10;++k)if(!expl[k].t){expl[k]=(Expl){x,y,1,small};return;}
}
__attribute__((noinline)) static uint8_t random(void) {noise=noise*37+11;return noise>>2;}
/* A ship going up (space.c blast): a fireball and smaller ones round it, a bigger ship more of them. Fighters and gunships
 * yell and burst (the source's sfx 5 and 6, one CD ADPCM event); a mine only bursts. */
SPACE_CODE static void foe_kill(Foe *f) {
    int16_t x=f->x,y=f->y;
    blast(x,y,false);
    if(f->kind<=3)audio_effect(4);
    else {
        blast(x-10,y+(random()&7)-4,true);blast(x+10,y+(random()&7)-4,true);
        if(f->kind==5){blast(x-24,y-8,false);blast(x+22,y+8,false);}
        audio_effect(8);
    }
    f->kind=0;++pce_campaign.score;
    if(f->drop){pickup=f->drop;pickup_x=f->x;pickup_y=f->y;}
}
SPACE_CODE static void spawn(void) {
    for(uint8_t k=0;k<8;++k)if(!foes[k].kind) {
        uint8_t n=next_event.n-pending;
        foes[k]=(Foe){272,next_event.y+(next_event.kind==4?0:n*next_event.gap),
            next_event.kind,next_event.kind==5?22:3,next_event.pattern,next_event.drop,0,0};
        if(foes[k].y>208)foes[k].y=208;
        spawn_clock=next_event.kind==4?next_event.gap:20;
        if(!--pending){++event_index;event_load();}return;
    }
}
__attribute__((noinline,minsize,section(".ram_bank114.text"))) static void space_contact(void) {   /* bank $72: $78 is full */
    if(space_hull_hit(ship_x+12-space_hull_x,ship_y-space_boss_y)) {
        if(!hurt){hurt=90;campaign_hurt();}
        while(ship_x>16&&space_hull_hit(ship_x+12-space_hull_x,ship_y-space_boss_y))--ship_x;
    }
}
#define BOSS_STEP __attribute__((noinline,minsize,section(".ram_bank119.text")))   /* the cruiser's show runs in the CD buffer's second bank ($77, overlay_call) */
/* One shot out of a gun port, aimed at Ramrod: 3 dots a step across, the vertical part follows the aim (dy / 32 for the 100-odd dots between). */
BOSS_STEP static void port_shot(uint8_t k,int8_t spread) {
    int16_t y=space_boss_y+port_y[k]+3,vy=((ship_y-y)>>5)+spread;
    bolt(port_x[k]-4+space_hull_x,y,-3,vy>3?3:vy<-3?-3:vy,true);
}
BOSS_STEP static void foe_in(int16_t x,int16_t y,uint8_t kind,uint8_t hp,uint8_t pattern,uint8_t drop) {
    for(uint8_t k=0;k<8;++k)if(!foes[k].kind){foes[k]=(Foe){x,y,kind,hp,pattern,drop,0,0};return;}
}

BOSS_STEP static void boss_guns(void) {
    uint8_t round=pce_campaign.boss_round;
    /* the gun ports in turn, one aimed shot every half second: each of the five (seven) fires about every 2.7 s (the strut's three at once from the
     * second stage); they hold their fire under the beam */
    if(sb.laser!=2&&!(beam_clock&31)) {
        uint8_t k=(beam_clock>>5)%(round?7:5);
        if(port_hp[k]){if(k>=4&&round){port_shot(k,-1);port_shot(k,0);port_shot(k,1);}else port_shot(k,0);}
    }
    /* the hangar: a swarm of drones out of the bay (5 + the stage, one every 16 steps; the last carries a gift), then the mine rack from the second stage */
    uint16_t cycle=beam_clock%(round==0?600:round==1?520:460);
    if(cycle<16*(5+round)&&!(cycle&15)){foe_in(212+space_hull_x,space_boss_y+69,1,2,1,cycle==16*(4+round)?1+((beam_clock>>9)&1):0);audio_effect(13);}
    if(round&&cycle==300){foe_in(161+space_hull_x,space_boss_y+85,4,3,0,0);foe_in(161+space_hull_x,space_boss_y+97,4,3,0,0);}
}
static void boss_step(void);
/* The ship has been shot down (space.c update_world): it bursts, 2.2 s on a spare ship flies back in where the fight is (the flight does not
 * start again), shielded for three seconds, with the shield cells full and at least three torpedoes; with no spare the game is over. */
__attribute__((noinline,minsize,section(".ram_bank118.text"))) static void space_dead(void) {
    if(!dead_t)blast(ship_x,ship_y,false);
    if(++dead_t<132)return;
    pce_death=dead_t=0;
    if(!pce_campaign.lives){pce_campaign.state=CAM_OVER;pce_campaign.timer=0;return;}
    --pce_campaign.lives;pce_metrics.hp=4;if(bombs<3)bombs=3;
    ship_x=16;ship_y=112;hurt=180;
}
SPACE_CODE static void space_tick(void) {
    uint8_t keys=pce_control.keys;
    if(pce_death){keys=0;pce_control.pressed=0;overlay_call(0x76,space_dead);}
    if(power_timer) {
        pce_campaign.timer=--power_timer;
        if(!power_timer) {
            for(uint8_t k=0;k<8;++k)if(foes[k].kind)foe_kill(&foes[k]);
            for(uint8_t k=0;k<12;++k)if(bolts[k].enemy)bolts[k].on=0;
            if(sb.ph==4) {
                pce_campaign.boss_hp=pce_campaign.boss_hp>84?pce_campaign.boss_hp-84:0;
                for(uint8_t k=0;k<7;++k)if(k<5||pce_campaign.boss_round)port_hp[k]=0;
            }
            hurt=150;pce_campaign.power_cd=720;space_flash=8;hull_flash=12;audio_effect(8);audio_effect(4);
        }
        return;
    }
    if(pce_campaign.power_cd)--pce_campaign.power_cd;
    if((pce_control.pressed&KEY_SELECT)&&(keys&KEY_1)&&pce_campaign.powers&&!pce_campaign.power_cd) {
        --pce_campaign.powers;power_timer=114;pce_campaign.timer=114;return;
    }
    if(space_flash)--space_flash;if(hull_flash)--hull_flash;
    ++flight_clock;if(gun_clock)--gun_clock;if(hurt)--hurt;
    uint8_t velocity=keys&KEY_SELECT?1:2;
    if(keys&KEY_LEFT)ship_x-=velocity;if(keys&KEY_RIGHT)ship_x+=velocity;
    if(keys&KEY_UP)ship_y-=velocity;if(keys&KEY_DOWN)ship_y+=velocity;
    if(ship_x<16)ship_x=16;if(ship_x>240)ship_x=240;
    if(ship_y<16)ship_y=16;if(ship_y>208)ship_y=208;
    if(pce_campaign.boss_kind&&ship_x>112)ship_x=112;
    if(sb.ph==4&&space_hull_ready)overlay_call(0x72,space_contact);
    if((keys&KEY_1)&&!gun_clock) {bolt(ship_x+22,ship_y,6,0,false);gun_clock=8;audio_effect(1);}
    if((pce_control.pressed&KEY_2)&&bombs) {
        --bombs;space_flash=6;hull_flash=12;audio_effect(8);audio_effect(4);
        for(uint8_t k=0;k<8;++k)if(foes[k].kind) {
            Foe *f=&foes[k];f->hp=f->hp>14?f->hp-14:0;
            if(f->kind<=3||!f->hp)foe_kill(f);
        }
        for(uint8_t k=0;k<12;++k)if(bolts[k].enemy)bolts[k].on=0;
        if(sb.ph==4){pce_campaign.boss_hp=pce_campaign.boss_hp>28?pce_campaign.boss_hp-28:0;
            for(uint8_t k=0;k<7;++k)if(k<5||pce_campaign.boss_round)port_hp[k]=port_hp[k]>10?port_hp[k]-10:0;
        }
        hurt=60;
    }
    if(pending){if(spawn_clock)--spawn_clock;else spawn();}
    else if(event_index<event_count&&flight_clock>=next_event.time) {
        pending=next_event.n;spawn();
        /* Keep the current recipe until all its entities have spawned. */
    }
    for(uint8_t k=0;k<8;++k) {
        Foe *f=&foes[k];if(!f->kind)continue;++f->clock;
        f->x-=f->kind==4?2:1;
        if(f->pattern==1){if((f->clock&63)<32)++f->y;else --f->y;}
        if(f->pattern==2&&f->x>80)f->y+=f->y<ship_y?1:-1;
        if(f->kind>=4&&!(f->clock%90))bolt(f->x-12,f->y,-3,ship_y<f->y?-1:1,true);
        if(f->kind==2&&(f->charge||(f->clock>=144&&ship_x<f->x-30&&dist(ship_y,f->y)<34&&f->x<244))) {
            if(++f->charge>=39){bolt(f->x-22,f->y,-4,0,true);f->charge=0;f->clock=0;audio_effect(1);}
        }
        if(!hurt&&dist(ship_x,f->x)<(f->kind<=3?24:18)&&dist(ship_y,f->y)<(f->kind<=3?26:10)) {
            hurt=90;campaign_hurt();
        }
        if(f->x< -48)f->kind=0;
    }
    if(sb.ph||flight_clock>=6360)overlay_call(0x77,boss_step);   /* the cruiser's whole show */
    for(uint8_t k=0;k<10;++k)if(expl[k].t){expl[k].x-=1;if(++expl[k].t>30)expl[k].t=0;}
    for(uint8_t k=0;k<12;++k) {
        Bolt *b=&bolts[k];if(!b->on)continue;
        b->x+=b->vx;b->y+=b->vy;
        if(b->x<0||b->x>256||b->y<0||b->y>224){b->on=0;continue;}
        if(b->enemy) {
            if(!hurt&&dist(ship_x,b->x)<6&&dist(ship_y,b->y)<6){b->on=0;hurt=90;campaign_hurt();}
        } else {
            for(uint8_t j=0;j<8&&b->on;++j) {
                Foe *f=&foes[j];if(!f->kind||dist(b->x,f->x)>24||dist(b->y,f->y)>16)continue;
                b->on=0;
                /* Armoured mines deflect ordinary shots, as in space.c. */
                if(f->kind>=4){f->hp=f->hp>power?f->hp-power:0;if(!f->hp)foe_kill(f);}
            }
            if(b->on&&sb.ph==4&&space_hull_hit(b->x-space_hull_x,b->y-space_boss_y)) {
                b->on=0;hull_flash=4;
                uint16_t damage=power;
                for(uint8_t j=0;j<7;++j)if(port_hp[j]&&(j<5||pce_campaign.boss_round)&&dist(b->y,space_boss_y+port_y[j])<4) {
                    port_hp[j]=port_hp[j]>power?port_hp[j]-power:0;if(!port_hp[j])damage+=30;break;
                }
                pce_campaign.boss_hp=pce_campaign.boss_hp>damage?pce_campaign.boss_hp-damage:0;
            }
        }
    }
    if(pickup) {
        --pickup_x;
        if(dist(pickup_x,ship_x)<18&&dist(pickup_y,ship_y)<16) {
            if(pickup==1){if(power<3)++power;else if(bombs<5)++bombs;}
            else if(pickup==2){if(pce_metrics.hp<4)++pce_metrics.hp;}
            else if(bombs<5)++bombs;
            pickup=0;audio_effect(3);
        }
        if(pickup_x<0)pickup=0;
    }
}
/* The cruiser's show runs in the CD buffer's second bank ($77, overlay_call); it calls the scenery bank's hull code the same way. */
BOSS_STEP static void boss_step(void) {
    switch(sb.ph) {
    case 0:   /* the timeline's end (106 s), once the field is clear or six seconds on: no more events, the music stops, WARNING */
        if(flight_clock<6360)return;
        for(uint8_t k=0;k<8;++k)if(foes[k].kind&&flight_clock<6720)return;
        sb.ph=1;sb.t=0;event_index=event_count;pending=0;audio_stop();audio_effect(12);
        return;
    case 1:   /* the screen fades to black (space_hull_load), then the cruiser's cells wait off screen right */
        if(++sb.t<216)return;
        sb.ph=2;sb.t=0;pce_campaign.boss_kind=5;beam_clock=0;
        memset(foes,0,sizeof foes);space_boss_y=56;space_hull_x=180;
        overlay_call(0x78,space_hull_load);audio_music(11);audio_effect(12);
        return;
    case 2: {   /* flies in over five seconds, slowing (space.c boss_update: 1 - (1 - p)^3) */
        uint16_t u=300-sb.t;
        space_hull_x=((u>>1)*(u>>1))/125;   /* 180 (u / 300)^2: 180 at the start, 0 at the end */
        if(++sb.t>=300){sb.ph=3;space_hull_x=0;pce_campaign.story=1;pce_campaign.event=1;}   /* its greeting (SCRIPT_BOSS) */
        return; }
    case 3:   /* the greeting is up: space_frame puts the hull's cells back once it has closed */
        return;
    }
    if(boss_die) {   /* the cruiser breaks up (space.c PH_BOSS_DIE): its fire stops, fireballs burst over the hull to the big bang, then the ship is gone */
        if(!--boss_die){boss_gone=1;space_boss_y&=~7;blast(164,space_boss_y+48,false);audio_effect(17);pce_campaign.result=1;pce_campaign.story=0;pce_campaign.event=1;}
        else if(!(boss_die%5)){blast(84+random()%160,space_boss_y+random()%90,random()&1);if(random()%5<2)audio_effect(random()&1?7:8);}   /* (space.c: the bursts yell now and then) */
        return;
    }
    if(!pce_campaign.boss_hp) {
        boss_die=72;sb.t=0;
        for(uint8_t k=0;k<12;++k)bolts[k].on=bolts[k].on&&!bolts[k].enemy;
        return;
    }
    ++beam_clock;
    uint8_t round=pce_campaign.boss_round=pce_campaign.boss_hp<396?2:pce_campaign.boss_hp<792?1:0;
    /* the nose cannon's cycle in one counter: waits (8.5 / 7 / 5.5 s), gathers 1.3 s, fires 1.5 s (2.6 s in the last stage), fades 0.25 s. The sparks and the
     * beam's cells are drawn in space_frame from sb.laser (0 idle, 1 gathering, 2 firing, 3 fading). */
    uint16_t wait=round==0?510:round==1?420:330,fire=round>=2?156:90;
    if(++sb.t>=wait+93+fire)sb.t=0;
    sb.laser=sb.t<wait?0:sb.t<wait+78?1:sb.t<wait+78+fire?2:3;
    sb.gather=sb.laser==1?sb.t-wait:0;
    if(sb.t==wait)audio_effect(18);else if(sb.t==wait+78)audio_effect(19);   /* the PC's charge.wav and beam.wav */
    if(sb.laser==2&&!hurt&&ship_x<84+space_hull_x&&dist(ship_y,space_boss_y+48)<(round>=2?12:7)){hurt=90;campaign_hurt();}
    /* the cruiser swings about the whole height of the field (34-106, a lap every 4 s, a pixel a step) and rocks forward and back (the hull moves 0-20 dots to the
     * right of its place, a lap every 3 s); both stop while the cannon gathers and fires (the beam is cells of the hull), and in the last stage the beam chases Ramrod */
    if(sb.laser==2&&round==2)space_boss_y+=ship_y<space_boss_y+48?-1:1;
    else if(sb.laser==0||sb.laser==3) {
        uint8_t c=(beam_clock>>1)&127,tri=c<64?c:128-c;
        int16_t want=34+tri+(tri>>3);
        space_boss_y+=want>space_boss_y?1:want<space_boss_y?-1:0;
        uint8_t u=(beam_clock/3)&63,rock=u<32?u:64-u;
        int16_t forward=(rock*5)>>3;
        if(!(beam_clock&1))space_hull_x+=forward>space_hull_x?1:forward<space_hull_x?-1:0;
    }
    if(space_boss_y<30)space_boss_y=30;if(space_boss_y>120)space_boss_y=120;
    boss_guns();   /* the ports and the hangar */
}
/* Scenery and flight drawing share bank $78; simulation stays in $73. */
PCE_SCENERY void space_dialog_ship(void) {   /* the dialogues (story_pce.c draw) keep Ramrod's ship on view where it flies */
    if(!pce_death)video_sprite(3,ship_x,ship_y,false,16);
}
PCE_SCENERY void space_frame(void) {
    if(sb.ph==3&&!pce_campaign.event&&pce_campaign.state==CAM_PLAY){space_hull_bat();sb.ph=4;sb.t=270;}   /* the greeting has closed: the cells it covered come back, the fight begins */
    for(uint8_t i=0;i<pce_control.elapsed&&!pce_campaign.event&&!pce_campaign.result&&pce_campaign.state==CAM_PLAY;++i) {
        overlay_call(0x73,space_tick);pce_control.pressed=0;
    }
    if(pce_campaign.boss_kind)space_hull_draw(space_boss_y,hull_flash,boss_gone);
    else video_background(flight_clock>>3);
    video_sat_begin();
    /* the HUD first: the first sprites of the table are in front of the rest and are admitted first when a scanline is full, so nothing hides it */
    hud7=(Hud7){pce_metrics.hp,pce_campaign.lives,power,bombs,pce_campaign.powers,pce_campaign.boss_kind!=0,(uint8_t)flight_clock,pce_campaign.boss_hp};
    overlay_call(0x7c,hud7_draw);
    if(!pce_death&&(!hurt||(flight_clock&4)))video_sprite(3,ship_x,ship_y,false,16);
    for(uint8_t k=0;k<12;++k)if(bolts[k].on)
        if(!video_sprite_optional(bolts[k].enemy?1:0,bolts[k].x,bolts[k].y,false,16))bolts[k].on=0;
    for(uint8_t k=0;k<8;++k)if(foes[k].kind) {
        Foe *f=&foes[k];
        if(!video_sprite_optional(f->kind<=3?8:f->kind==5?5:4,f->x,f->y,false,16)){f->kind=0;continue;}   /* the art faces left, the way they fly */
        if(f->charge>20&&(flight_clock&4))video_sprite_optional(1,f->x-20,f->y,false,16);
    }
    if(pickup)video_sprite_optional(8+pickup,pickup_x,pickup_y,false,16);
    for(uint8_t k=0;k<10;++k)if(expl[k].t)video_sprite_optional(12+(expl[k].small?5:0)+(expl[k].t-1)/6,expl[k].x,expl[k].y,false,16);
    /* the nose cannon: sparks drawn into its mouth while it gathers; the beam itself is cells of the playfield (space_beam), set when the state changes */
    if(sb.laser==1) {
        int16_t reach=(78-sb.gather)>>2,x=84+space_hull_x,y=space_boss_y+48;
        video_sprite_optional(1,x+reach,y,false,16);video_sprite_optional(1,x-reach,y,false,16);
        video_sprite_optional(1,x,y+reach,false,16);video_sprite_optional(1,x,y-reach,false,16);
    }
    {
        uint8_t beam=sb.laser==2?(pce_campaign.boss_round>=2?2:1):0;
        if(boss_die||boss_gone)beam=0;
        if(beam!=sb.beam&&space_hull_ready){space_beam(beam);sb.beam=beam;}
    }
    video_sat_end();
    space_screen_flash(space_flash);
    pce_metrics.player_x=ship_x;pce_metrics.player_y=ship_y;
}
