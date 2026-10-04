#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "loader_pce.h"
#include "save_pce.h"
#include "floor_pce.h"
#include "assets.h"
#define RACE_SIN_SECTION ".ram_bank121.rodata"
#include "race_math.h"
#include "race_pce.h"
#include <string.h>
/* The Grand Prix (mode7.c), in whole 1/60 s steps. The car is free: it steers by heading, its top speed follows the
 * ground under it (road 470 u/s, kerb 380, sand 250; turbo x1.35), the field runs on the circuit's rails with the source's
 * speeds, rubber band and Black Hornet mines and shots, and the circuit is raced for three laps with a top-three finish
 * needed; the pursuit follows (race_foes_pce.c). Positions are world units (8192 wrap); angles 16 bits (65536 = a turn);
 * speeds units a second; progress along the circuit in 1/256ths of its 256 sample points. The camera sits 92 units behind
 * the car (its ground point is the floor's nearest row); horizon row 113, focal 421 dots (the floor tables' own). */
#define RACE_CODE PCE_RACE
#define N_RIVALS 7
TrackPoint track[256] PCE_STAGE;
Rival rv[N_RIVALS];
Mine mines[6];
Bolt race_bolts[6];
Leader boss,escort[2];
uint16_t px,py,hd,cam_hd,phase_t,race_time,gap_dist,race_rng=0xB5AD,ps;
int16_t speed,tilt,arg_x,arg_y,arg_dist,arg_radius,arg_speed;
uint8_t rphase,car_hp,car_max,boost,boost_on,hurt,shake,finish_rank,spin,ram_cd,arg_damage,arg_life;
int8_t lapp,cam_c,cam_s;
static uint8_t pfx,pfy,near_idx,fire_cd;
static uint16_t spawn_t;

RACE_CODE static uint8_t rnd(void) {
    uint8_t carry=race_rng&1;race_rng>>=1;if(carry)race_rng^=0xB400;
    return (uint8_t)(race_rng^(race_rng>>8));
}
/* The ground byte under a world position: colour (low nibble) and class (high: 0 sand, 1 kerb, 2 road). */
RACE_CODE static uint8_t ground(uint16_t x,uint16_t y) {
    uint8_t v;
    if(rphase>=P_PURSUIT)arcade_read(1,PCE_RACE_PURSUIT_ROW+((x&8191)>>3),&v,1);
    else arcade_read(1,PCE_RACE_RACE_MAP+((uint32_t)((y&8191)>>3)<<10)+((x&8191)>>3),&v,1);
    return v>>4;
}
RACE_CODE static void track_point(uint16_t s,int16_t lat,int16_t *x,int16_t *y) {
    arg_speed=(int16_t)s;arg_dist=lat;overlay_call(0x6d,field_point_call);*x=arg_x;*y=arg_y;
}
RACE_CODE static void update_field(bool grid,int16_t plat) {arg_life=grid;arg_x=plat;overlay_call(0x6d,field_update_call);}
RACE_CODE static void standings(void) {overlay_call(0x6d,field_standings_call);}
/* The nearest sample of the circuit to the car (searched round the last one): its progress, and the lateral offset. */
RACE_CODE static int16_t project(void) {
    int16_t best=32000;uint8_t bi=near_idx;
    for(int8_t d=-2;d<=3;++d) {
        uint8_t i=near_idx+d;
        int16_t q=absolute(wrapdiff(px,track[i].x))+absolute(wrapdiff(py,track[i].y));
        if(q<best){best=q;bi=i;}
    }
    near_idx=bi;
    uint8_t n=bi+1;
    int16_t sx=wrapdiff(track[n].x,track[bi].x),sy=wrapdiff(track[n].y,track[bi].y);
    int16_t rx=wrapdiff(px,track[bi].x),ry=wrapdiff(py,track[bi].y);
    /* a segment is about 52 units long: dot / 52^2 * 256 and cross / 52 */
    int16_t along=(rx*sx+ry*sy)*3>>5;
    if(along<-256)along=-256;if(along>511)along=511;
    uint16_t s=((uint16_t)bi<<8)+along;
    if(ps>0xc000&&s<0x4000)++lapp;else if(ps<0x4000&&s>0xc000)--lapp;
    ps=s;
    return ((rx*-sy+ry*sx)>>1)*5>>7;
}
RACE_CODE static void hurt_car(uint8_t damage) {
    if(hurt||(rphase!=P_RACE&&rphase!=P_PURSUIT&&rphase!=P_BOSS))return;
    audio_effect(5);hurt=42;shake=24;
    if(car_hp>damage){car_hp-=damage;pce_metrics.hp=(uint8_t)(((uint16_t)car_hp*8+car_max-1)/car_max);}
    else{car_hp=0;pce_metrics.hp=1;campaign_hurt();}
}
RACE_CODE void hurt_call(void) {hurt_car(arg_damage);}
/* ---- the car --------------------------------------------------------------------------------------------- */
RACE_CODE static void drive(uint8_t keys) {
    uint8_t cls=ground(px,py);
    int16_t vmax=cls==2?470:cls==1?380:250;
    bool playing=rphase==P_RACE||rphase>=P_PURSUIT;
    bool turbo=(keys&KEY_2)&&boost>5&&!spin&&playing;
    if(turbo){vmax=vmax*27/20;boost-=boost>2?2:boost;boost_on=boost>5;}
    else {boost_on=0;if(boost<255&&!(race_time&3))++boost;}
    bool accel=(keys&(KEY_1|KEY_UP))!=0;
    if(spin){accel=false;--spin;}
    if(accel)speed+=turbo?9:5;else speed-=2;
    if(keys&KEY_DOWN)speed-=8;
    if(speed>vmax)speed-=(speed-vmax)>>(cls==2?4:3);
    if(speed<0)speed=((keys&KEY_DOWN)&&speed>-80)?speed:0;
    int8_t steer=spin?0:(keys&KEY_RIGHT?1:0)-(keys&KEY_LEFT?1:0);
    int16_t sp=absolute(speed);if(sp>345)sp=345;
    int16_t turn=sp+(sp>>3);                  /* 1.9 rad/s at 300 u/s: 330 units a step */
    if(cls==0)turn-=turn>>3;
    hd+=steer*turn;
    tilt+=(steer*7-tilt)/8;
    cam_hd+=((int16_t)(hd-cam_hd)>>3)+((int16_t)(hd-cam_hd)>>5);
    /* the step in Q8 units: speed / 60 * 256 = speed * 4.27 */
    int16_t mx=muls(speed,cosine(hd)),my=muls(speed,sine(hd));
    int16_t tx=pfx+mx*4+(mx>>2),ty=pfy+my*4+(my>>2);
    px=(px+(tx>>8))&8191;py=(py+(ty>>8))&8191;pfx=tx;pfy=ty;
    if(fire_cd)--fire_cd;
    if((keys&KEY_1)&&!fire_cd&&!spin&&playing) {
        for(uint8_t k=0;k<6;++k)if(!race_bolts[k].t) {
            int16_t v=(1500+absolute(speed))>>1;   /* 750 + half the speed: u/s, then x4.27 for the Q8 step */
            int16_t vx=muls(v,cosine(hd)),vy=muls(v,sine(hd));
            race_bolts[k]=(Bolt){px+(cosine(hd)>>2),py+(sine(hd)>>2),vx*8+(vx>>1),vy*8+(vy>>1),66,1};
            break;
        }
        fire_cd=10;audio_effect(1);
    }
}
/* ---- the phases ---------------------------------------------------------------------------------------------- */
RACE_CODE void race_start(void) {
    arcade_read(1,pce_scenes[1].track,track,sizeof track);
    car_max=pce_options.difficulty==0?16:pce_options.difficulty==1?12:8;car_hp=car_max;
    pce_metrics.hp=8;pce_campaign.lap=1;pce_campaign.rank=8;pce_campaign.boss_kind=0;pce_campaign.boss_hp=0;
    boost=255;boost_on=0;hurt=spin=shake=fire_cd=ram_cd=0;speed=0;tilt=0;race_time=0;phase_t=0;
    overlay_call(0x6d,field_start_call);
    uint16_t s=63005u;
    int16_t x,y;track_point(s,48,&x,&y);
    px=x;py=y;ps=s;near_idx=s>>8;lapp=-1;
    hd=(uint16_t)track[near_idx].heading<<11;cam_hd=hd;
    pfx=pfy=0;
    rphase=P_COUNT;
    if(pce_control.phase) {   /* resuming at the pursuit */
        px=py=4096;hd=cam_hd=0xc000;speed=200;rphase=P_PURSUIT;
        overlay_call(0x7a,foes_leader_start);spawn_t=150;
    }
}
RACE_CODE static void begin_pursuit(void) {
    rphase=P_PURSUIT;px=py=4096;pfx=pfy=0;hd=cam_hd=0xc000;speed=200;hurt=0;spin=0;car_hp=car_max;pce_metrics.hp=8;
    overlay_call(0x7a,foes_leader_start);spawn_t=150;phase_t=0;
}
RACE_CODE static uint8_t coast_keys(void) {
    /* hands off the wheel: steer to the circuit's point 240 units ahead, throttle up for the first 1.2 s */
    int16_t x,y;track_point(ps+1191,0,&x,&y);
    int16_t dx=wrapdiff(x,px),dy=wrapdiff(y,py);
    int16_t cross=muls(dy,cosine(hd))-muls(dx,sine(hd));   /* > 0: the point is to the right */
    uint8_t keys=phase_t<72?KEY_UP:0;
    if(cross>3)keys|=KEY_RIGHT;else if(cross<-3)keys|=KEY_LEFT;
    return keys;
}
RACE_CODE static void race_tick(uint8_t keys) {
    ++phase_t;
    if(hurt)--hurt;if(shake)--shake;if(ram_cd)--ram_cd;
    switch(rphase) {
    case P_COUNT:
        update_field(true,0);standings();
        if(phase_t>=180){rphase=P_RACE;phase_t=0;audio_effect(1);}
        break;
    case P_RACE: {
        ++race_time;
        drive(keys);int16_t lat=project();
        update_field(false,lat);overlay_call(0x7a,foes_shots);standings();
        pce_campaign.lap=lapp<0?1:lapp>=2?3:lapp+1;
        if(lapp>=3) {
            rphase=P_FINISH;phase_t=0;finish_rank=pce_campaign.rank;
            memset(mines,0,sizeof mines);memset(race_bolts,0,sizeof race_bolts);
        }
        break; }
    case P_FINISH:
        drive(coast_keys());project();update_field(false,0);overlay_call(0x7a,foes_shots);
        if(phase_t>=192) {
            if(finish_rank<=3) {
                pce_campaign.story=1;pce_campaign.event=1;
                save_store(2,pce_control.hero,1);audio_music(9);
                begin_pursuit();
            } else {pce_metrics.hp=1;campaign_hurt();}   /* must finish 3rd or better: a life, and the race again */
        }
        break;
    case P_PURSUIT:
    case P_BOSS:
        drive(keys);overlay_call(0x7a,foes_shots);overlay_call(0x7a,foes_escorts);overlay_call(0x7a,foes_leader);
        gap_dist=boss.state<2?(uint16_t)hypot16(wrapdiff(px,boss.x),wrapdiff(py,boss.y)):0;
        if(rphase==P_PURSUIT) {
            if(spawn_t)--spawn_t;
            else if(gap_dist>700){spawn_t=(uint16_t)(180+rnd()%120);overlay_call(0x7a,foes_spawn_escort);}
            if(gap_dist<260&&wrapdiff(boss.y,py)<0&&boss.state==0) {   /* on his tail: he stops running, the fight is on */
                speed=speed/2;
                pce_campaign.story=2;pce_campaign.event=1;
                boss.state=1;boss.t=0;boss.t2=90;boss.t3=250;boss.boost=0;boss.since=0;
                memset(escort,0,sizeof escort);memset(mines,0,sizeof mines);
                rphase=P_BOSS;pce_campaign.boss_kind=4;audio_music(8);
            }
        }
        if(rphase==P_BOSS)pce_campaign.boss_hp=boss.hp;
        break;
    case P_VICTORY:
        drive(KEY_DOWN);overlay_call(0x7a,foes_shots);
        if(phase_t==120){pce_campaign.result=1;pce_campaign.story=3;pce_campaign.event=1;}
        break;
    }
}
RACE_CODE void race_frame(void) {
    uint8_t keys=pce_control.keys;
    for(uint8_t i=0;i<pce_control.elapsed&&!pce_campaign.event&&!pce_campaign.result&&pce_campaign.state==CAM_PLAY;++i)race_tick(keys);
    /* the floor is drawn from the camera: 92 units behind the car along the camera's heading */
    cam_c=cosine(cam_hd);cam_s=sine(cam_hd);
    pce_control.x=px-(cam_c*92>>7);
    pce_control.y=py-(cam_s*92>>7);
    pce_control.heading=(uint8_t)(cam_hd>>9)&127;pce_control.phase=rphase>=P_PURSUIT;
    overlay_call(0x6d,floor_draw);
    overlay_call(0x7c,race_draw);
    overlay_call(0x6d,floor_present);
    pce_metrics.phase=rphase>=P_PURSUIT;pce_metrics.player_x=px;pce_metrics.player_y=py;
}
