#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "loader_pce.h"
#include "save_pce.h"
#include "audio_pcm.h"
#include "road_pce.h"
#include "scenery_pce.h"
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
 * the car (its ground point is the road's nearest scanline); horizon row 113, focal 421 dots (road_pce.c). */
#define RACE_CODE PCE_RACE
#define N_RIVALS 7
#define MAX_OFF 0x1800
#define LAT_LIMIT 170   /* units from the road's middle: 50 beyond the kerb. The picture's windows wrap round it (road_pce.c) and the upper rows tolerate about 200 */
TrackPoint track[256] PCE_STAGE;
Rival rv[N_RIVALS];
Mine mines[6];
Blast blasts[4] PCE_STAGE;
/* Shots: 0-5 the car's own, 6-9 the enemies' (the car's firing used to fill the pool, so the enemy never got a slot). */
Bolt race_bolts[10] PCE_STAGE;
uint8_t boost_locked PCE_STAGE;
uint8_t lap_banner PCE_STAGE;
Leader boss,escort[2];
uint16_t px,py,hd,cam_hd,phase_t,race_time,gap_dist,race_rng=0xB5AD,ps;
int16_t speed,tilt,arg_x,arg_y,arg_dist,arg_radius,arg_speed;
uint8_t rphase,car_hp,car_max,boost,boost_on,hurt,shake,finish_rank,spin,ram_cd,arg_damage,arg_life;
int8_t lapp,cam_c,cam_s,cam_cl,cam_sl;
uint8_t road_idx;
__attribute__((noinline)) int16_t muls(int16_t a,int8_t b) {
    uint16_t m=(uint16_t)(absolute(a)>>2)*(uint8_t)(b<0?-b:b)>>5;
    return (a<0)!=(b<0)?-(int16_t)m:(int16_t)m;
}
static uint8_t pfx,pfy,fire_cd;
static uint16_t spawn_t;

RACE_CODE static uint8_t rnd(void) {
    uint8_t carry=race_rng&1;race_rng>>=1;if(carry)race_rng^=0xB400;
    return (uint8_t)(race_rng^(race_rng>>8));
}
/* The ground byte under a world position (arg_x, arg_y): colour (low nibble) and class (high: 0 sand, 1 kerb, 2 road); bank $78: $79 is full. */
PCE_SCENERY void ground_call(void) {
    uint8_t v;
    uint16_t x=arg_x,y=arg_y;
    if(rphase>=P_PURSUIT)arcade_read(1,PCE_RACE_PURSUIT_ROW+((x&8191)>>3),&v,1);
    else arcade_read(1,PCE_RACE_RACE_MAP+((uint32_t)((y&8191)>>3)<<10)+((x&8191)>>3),&v,1);
    arg_life=v>>4;
}
RACE_CODE static uint8_t ground(uint16_t x,uint16_t y) {arg_x=x;arg_y=y;overlay_call(0x78,ground_call);return arg_life;}
RACE_CODE static void track_point(uint16_t s,int16_t lat,int16_t *x,int16_t *y) {
    arg_speed=(int16_t)s;arg_dist=lat;overlay_call(0x6d,field_point_call);*x=arg_x;*y=arg_y;
}
RACE_CODE static void update_field(bool grid,int16_t plat) {arg_life=grid;arg_x=plat;overlay_call(0x6d,field_update_call);}
RACE_CODE static void standings(void) {overlay_call(0x6d,field_standings_call);}
/* a * b for magnitudes up to 255 by a table of quarter squares (race_proj.c): the generic 16-bit multiply took 600 cycles a product */
extern uint16_t cache_refs[];
static inline __attribute__((always_inline)) uint16_t umul(uint8_t a,uint8_t b) {
    uint8_t d=a>b?a-b:b-a;
    return cache_refs[(uint16_t)a+b]-cache_refs[d];
}
static inline __attribute__((always_inline)) int16_t smul(int16_t a,int16_t b) {   /* |a|, |b| <= 255 */
    uint16_t p=umul(a<0?-a:a,b<0?-b:b);
    return (a<0)!=(b<0)?-(int16_t)p:(int16_t)p;
}
static uint8_t seg_index=255;static uint16_t seg_inv;
/* The nearest sample of the circuit to the car (searched round the last one): its progress, and the lateral offset. */
RACE_CODE static int16_t project(void) {
    int16_t best=32000;uint8_t bi=road_idx;
    for(int8_t d=-2;d<=3;++d) {
        uint8_t i=road_idx+d;
        int16_t q=absolute(wrapdiff(px,track[i].x))+absolute(wrapdiff(py,track[i].y));
        if(q<best){best=q;bi=i;}
    }
    road_idx=bi;
    uint8_t n=bi+1;
    int16_t sx=wrapdiff(track[n].x,track[bi].x),sy=wrapdiff(track[n].y,track[bi].y);
    int16_t rx=wrapdiff(px,track[bi].x),ry=wrapdiff(py,track[bi].y);
    if(rx>240)rx=240;else if(rx<-240)rx=-240;if(ry>240)ry=240;else if(ry<-240)ry=-240;   /* keeps the products in 16 bits */
    /* where the car is along its segment, exactly (Q8): the segments are 36 to 66 units long, and a fixed length made the progress
     * (the stripes) and the road's direction jump each time the nearest sample changed. The division by the segment's squared length is a
     * multiplication by its reciprocal (2^24 / length^2), made when the nearest sample changes: a 32-bit division took 3,000 cycles a step. */
    if(seg_index!=bi){seg_index=bi;seg_inv=(uint16_t)(16777216UL/(uint16_t)(sx*sx+sy*sy));}
    int16_t dot=smul(rx,sx)+smul(ry,sy);
    uint16_t dm=dot<0?-dot:dot,magnitude=(uint16_t)umul(dm>>8,seg_inv>>8)+((uint16_t)(umul(dm>>8,seg_inv&255)+umul(dm&255,seg_inv>>8))>>8);   /* |dot| * inv >> 16 */
    int16_t along=dot<0?-(int16_t)magnitude:(int16_t)magnitude;
    if(along<-256)along=-256;if(along>511)along=511;
    uint16_t s=((uint16_t)bi<<8)+along;
    if(ps>0xc000&&s<0x4000)++lapp;else if(ps<0x4000&&s>0xc000)--lapp;
    ps=s;
    int16_t cross=smul(ry,sx)-smul(rx,sy);
    uint16_t cm=cross<0?-cross:cross;cm=((cm>>2)*5)>>6;   /* cross * 5 / 256 */
    int16_t lat=cross<0?-(int16_t)cm:(int16_t)cm;
    if(lat>LAT_LIMIT||lat<-LAT_LIMIT) {   /* the edge of the sand: the car slides along it */
        int16_t over=lat>0?lat-LAT_LIMIT:lat+LAT_LIMIT;
        if(over>60)over=60;else if(over<-60)over=-60;
        px=(px+((sy*over*5)>>8))&8191;py=(py-((sx*over*5)>>8))&8191;   /* back along the segment's normal (x5/256 is 1/51 of a unit) */
        speed-=speed>>4;
        lat=lat>0?LAT_LIMIT:-LAT_LIMIT;
    }
    return lat;
}
PCE_HUD static void hurt_car(uint8_t damage) {   /* bank $7c: $79 is full */
    if(hurt||(rphase!=P_RACE&&rphase!=P_PURSUIT&&rphase!=P_BOSS))return;
    audio_effect(5);hurt=42;shake=24;
    if(car_hp>damage){car_hp-=damage;pce_metrics.hp=(uint8_t)(((uint16_t)car_hp*8+car_max-1)/car_max);}
    else{car_hp=0;pce_metrics.hp=1;campaign_hurt();}
}
PCE_HUD void hurt_call(void) {hurt_car(arg_damage);}
/* The camera looks along the road, as Chase H.Q.'s does: the road's direction at the car (the circuit's tangent, blended between its
 * samples), not the car's own heading. Steering then slides the road sideways instead of swinging all of it round the car, which is
 * what kept the far road from settling. */
RACE_CODE static uint16_t road_heading(void) {
    if(rphase>=P_PURSUIT)return 0xc000;
    uint16_t q=ps-128;   /* a segment's direction holds at its middle */
    uint8_t i=q>>8;
    int8_t step=(int8_t)(track[(uint8_t)(i+1)].heading-track[i].heading);
    return ((uint16_t)track[i].heading<<8)+(int16_t)step*(uint8_t)q;
}
/* The camera's sine in Q14, between the table's 256 steps a turn (a step of 1.4 degrees swung the far road by ten dots at once). */
RACE_CODE static int16_t sine14(uint16_t angle) {
    int8_t low=SIN[(uint8_t)(angle>>8)],high=SIN[(uint8_t)((angle>>8)+1)];
    return (int16_t)low*128+(((int16_t)(int8_t)(high-low)*(uint8_t)angle)>>1);
}
/* ---- the car --------------------------------------------------------------------------------------------- */
RACE_CODE static void drive(uint8_t keys) {
    uint8_t turbo_was_on=boost_on;
    uint8_t cls=ground(px,py);
    int16_t vmax=cls==2?470:cls==1?380:250;
    bool playing=rphase==P_RACE||rphase>=P_PURSUIT;
    /* mode7.c: the meter drains 0.33 a second under turbo, refills 0.08 a second otherwise; a drained meter locks the turbo out until
     * it is half full again. (It was timed off race_time, which stands still in the pursuit: the meter never refilled there.) */
    if(boost_locked&&boost>=128)boost_locked=0;
    bool turbo=(keys&KEY_2)&&!boost_locked&&boost>13&&!spin&&playing;
    if(turbo){
        vmax=vmax*27/20;boost-=(phase_t&1)?1:2;boost_on=1;
        if(boost<=13){boost=0;boost_locked=1;boost_on=0;}
    } else {
        boost_on=0;
        if(boost<255&&phase_t%3==0)++boost;
        if(boost_locked&&boost>=128)boost_locked=0;
    }
    if(boost_on&&!turbo_was_on){audio_pcm_turbo_start();audio_pcm_turbo_loop(true);}
    else if(!boost_on&&turbo_was_on)audio_pcm_turbo_loop(false);
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
    {   /* The road is seen head-on (its picture is a straight road in perspective): the car keeps within 34 degrees of the road's direction. */
        uint16_t along=road_heading();
        int16_t off=(int16_t)(hd-along);
        if(off>MAX_OFF)hd=along+MAX_OFF;else if(off<-MAX_OFF)hd=along-MAX_OFF;
    }
    tilt+=(steer*112-tilt)>>3;                 /* Q4: whole numbers made (7 - 0) / 8 = 0, so the car never leaned */
    cam_hd+=(int16_t)(road_heading()-cam_hd)>>1;
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
RACE_CODE static void begin_pursuit(void);
RACE_CODE void race_start(void) {
    audio_pcm_turbo_loop(false);
    arcade_read(1,pce_scenes[1].track,track,sizeof track);
    car_max=pce_options.difficulty==0?16:pce_options.difficulty==1?12:8;car_hp=car_max;
    pce_metrics.hp=8;pce_campaign.lap=1;lap_banner=0;pce_campaign.rank=8;pce_campaign.boss_kind=0;pce_campaign.boss_hp=0;
    boost=255;boost_on=boost_locked=0;hurt=spin=shake=fire_cd=ram_cd=0;speed=0;tilt=0;race_time=0;phase_t=0;
    overlay_call(0x6d,field_start_call);
    memset(blasts,0,sizeof blasts);
    uint16_t s=63005u;
    int16_t x,y;track_point(s,48,&x,&y);
    px=x;py=y;ps=s;road_idx=s>>8;lapp=-1;
    hd=(uint16_t)track[road_idx].heading<<8;cam_hd=hd;
    pfx=pfy=0;
    rphase=P_COUNT;
    if(pce_control.phase)begin_pursuit();   /* resuming at the pursuit */
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
        if(phase_t%60==1)audio_effect(20);   /* 3, 2, 1: a pip each */
        if(phase_t>=180){rphase=P_RACE;phase_t=0;audio_effect(21);}   /* GO */
        break;
    case P_RACE: {
        ++race_time;
        drive(keys);int16_t lat=project();
        update_field(false,lat);
        if(arg_dist){pce_metrics.hp=1;campaign_hurt();break;}   /* every rival has finished the race: a life, and the race again (game over with no life left) */
        overlay_call(0x7a,foes_shots);standings();
        {uint8_t lap=lapp<0?1:lapp>=2?3:lapp+1;if(lap>pce_campaign.lap)lap_banner=150;pce_campaign.lap=lap;}   /* a new lap: its banner for 2.5 s */
        if(lap_banner)--lap_banner;
        if(lapp>=3) {
            overlay_call(0x6d,field_rank_call);rphase=P_FINISH;phase_t=0;finish_rank=pce_campaign.rank;lap_banner=0;
            memset(mines,0,sizeof mines);memset(race_bolts,0,sizeof race_bolts);audio_stop();
        }
        break; }
    case P_FINISH:
        drive(coast_keys());project();update_field(false,0);overlay_call(0x7a,foes_shots);
        if(phase_t>=192) {
            if(finish_rank<=3) {
                pce_campaign.story=1;pce_campaign.event=1;
                save_store(2,pce_control.hero,1);audio_music(14);   /* the pursuit's own track (mode7.c PH_PURSUIT) */
                begin_pursuit();
            } else {pce_metrics.hp=1;campaign_hurt();}   /* must finish 3rd or better: a life, and the race again */
        }
        break;
    case P_PURSUIT:
    case P_BOSS:
        drive(keys);
        {int16_t l=wrapdiff(px,4096);if(l>LAT_LIMIT||l<-LAT_LIMIT){px=l>0?4096+LAT_LIMIT:4096-LAT_LIMIT;speed-=speed>>4;}}   /* the pursuit's road is straight along x 4096 */
        overlay_call(0x7a,foes_shots);overlay_call(0x7a,foes_escorts);overlay_call(0x7a,foes_leader);
        gap_dist=boss.state<2?(uint16_t)hypot16(wrapdiff(px,boss.x),wrapdiff(py,boss.y)):0;
        if(rphase==P_PURSUIT) {
            if(spawn_t)--spawn_t;
            else if(gap_dist>700){spawn_t=(uint16_t)(180+rnd()%120);overlay_call(0x72,foes_spawn_escort);}
            if(gap_dist<260&&wrapdiff(boss.y,py)<0&&boss.state==0) {   /* on his tail: he stops running, the fight is on */
                speed=speed/2;
                pce_campaign.story=2;pce_campaign.event=1;
                boss.state=1;boss.t=0;boss.t2=90;boss.t3=250;boss.boost=0;boss.since=0;
                memset(escort,0,sizeof escort);memset(mines,0,sizeof mines);
                rphase=P_BOSS;pce_campaign.boss_kind=4;audio_music(17);   /* mode7.c PH_BOSS */
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
    /* the road is drawn from the camera: 92 units behind the car along the camera's heading */
    int16_t q=sine14(cam_hd+0x4000);cam_c=(q+64)>>7;cam_cl=q-cam_c*128;   /* each as a Q7 byte and the remainder */
    q=sine14(cam_hd);cam_s=(q+64)>>7;cam_sl=q-cam_s*128;
    pce_control.x=px-(cam_c*92>>7);
    pce_control.y=py-(cam_s*92>>7);
    pce_control.heading=(uint8_t)(cam_hd>>9)&127;pce_control.phase=rphase>=P_PURSUIT;
    overlay_call(0x6d,road_draw);
    overlay_call(0x7c,race_draw);
    pce_metrics.phase=rphase>=P_PURSUIT;pce_metrics.player_x=px;pce_metrics.player_y=py;
}
