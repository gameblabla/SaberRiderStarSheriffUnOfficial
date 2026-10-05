#include "campaign_pce.h"
#include "overlay_pce.h"
#include "race_pce.h"
#define RACE_SIN_SECTION ".ram_bank109.rodata"
#include "race_math.h"
#include <string.h>
/* The field of the Grand Prix (mode7.c's rivals): the circuit's rails, the rubber band, the Black Hornets' mines and
 * shots, and the bump of two cars. It lives in the floor's bank ($6d), which has room: the race core calls it with
 * overlay_call, passing its arguments in the arg_* globals. */
#define FIELD_CODE PCE_FLOOR
#define N_RIVALS 7
FIELD_CODE static uint8_t rnd(void) {
    uint8_t carry=race_rng&1;race_rng>>=1;if(carry)race_rng^=0xB400;
    return (uint8_t)(race_rng^(race_rng>>8));
}
/* x / 52 */
static inline int16_t d52(int16_t v) {return (v>>6)+(v>>8);}
/* A circuit position (progress s, lateral offset lat) in world units. */
FIELD_CODE static void track_point(uint16_t s,int16_t lat,int16_t *x,int16_t *y) {
    uint8_t i=s>>8,n=i+1,fr=s;
    int16_t dx=wrapdiff(track[n].x,track[i].x),dy=wrapdiff(track[n].y,track[i].y);
    *x=(track[i].x+(dx*fr>>8)-d52(dy*lat))&8191;
    *y=(track[i].y+(dy*fr>>8)+d52(dx*lat))&8191;
}
FIELD_CODE static void bump(int16_t x,int16_t y,int16_t dist,int16_t r) {
    int16_t nx=wrapdiff(px,x),ny=wrapdiff(py,y);
    if(dist<1){dist=1;nx=1;ny=0;}
    int16_t push=(r-dist)*3/5;
    px=(px+nx*push/dist)&8191;py=(py+ny*push/dist)&8191;
}
FIELD_CODE void bump_call(void) {bump(arg_x,arg_y,arg_dist,arg_radius);}
/* ---- the field --------------------------------------------------------------------------------------------- */
static const uint16_t FIELD_MAX[N_RIVALS]={575,555,540,525,530,510,500};
static const uint8_t FIELD_KIND[N_RIVALS]={3,1,1,4,1,5,4};   /* sprite types: 0 buggy 1 hornet 2 leader 3 firenza 4 blue 5 purple 6 mine */
FIELD_CODE static void start_field(void) {
    for(uint8_t k=0;k<N_RIVALS;++k) {
        Rival *r=&rv[k];
        *r=(Rival){(uint16_t)(65090u-298u*k),-1,(k&1)?48:-48,(k&1)?48:-48,0,FIELD_MAX[k],FIELD_KIND[k],FIELD_KIND[k]==1,
            (uint8_t)((FIELD_KIND[k]==1?10:7)+2*pce_options.difficulty),0,(uint8_t)(45+rnd()%45),30,0,0,0};
        track_point(r->s,r->lat,&r->x,&r->y);
    }
    memset(mines,0,sizeof mines);memset(race_bolts,0,sizeof race_bolts);
}
/* How far ahead of the car a rival is, in units (negative: behind), in 16 bits: the lap difference only matters once it
 * puts the rival more than half a lap away, where the value saturates as the source's does. */
FIELD_CODE static int16_t gap_units(const Rival *r) {
    int8_t laps=r->lap-lapp;
    bool below=r->s<ps;uint16_t m=below?ps-r->s:r->s-ps;   /* the in-lap difference: below ? -m : m */
    uint16_t mag;bool negative;
    if(laps==0){mag=m;negative=below;}
    else if(laps>0&&below&&laps==1){mag=-m;negative=false;}       /* 65536 - m */
    else if(laps<0&&!below&&laps==-1){mag=-m;negative=true;}
    else {mag=32767;negative=laps<0;}
    if(mag>32767)mag=32767;
    int16_t q=(int16_t)((mag>>3)+(mag>>4)+(mag>>7)+(mag>>8));   /* x 13205 / 65536 */
    return negative?-q:q;
}
FIELD_CODE static void update_field(bool grid,int16_t plat) {
    for(uint8_t k=0;k<N_RIVALS;++k) {
        Rival *r=&rv[k];if(!r->hp)continue;
        int16_t gap=r->gap=gap_units(r),target=r->max;
        if(gap>2600)target-=target>>2;else if(gap>1200)target-=(target>>3)+(target>>6);else if(gap<-900)target+=target>>3;   /* x3/4, x0.86, x1.12 */
        if(r->knock){r->knock=r->knock>4?r->knock-4:0;target>>=1;}
        if(grid)target=0;
        else if(rphase==P_FINISH)target=r->hornet?760:target-(target>>3)-(target>>4);
        int16_t d=target-r->speed;
        r->speed+=r->speed<target?((d>>5)+(d>>6)+(d>>7))*2:((d>>4)+(d>>6)+(d>>7))*2;   /* x7/256, x11/256 a step: four simulation steps */
        if(!grid&&rphase!=P_FINISH) {
            if(r->t<=2){r->t=(uint8_t)(45+rnd()%90);r->lat_t=(int16_t)(rnd()%150)-75;}else r->t-=2;
            if(gap>30&&gap<320&&rphase==P_RACE)r->lat_t+=2*((plat-r->lat_t)>>(r->hornet?4:5));
            {int16_t e=r->lat_t-r->lat;r->lat+=2*((e>>5)+(e>>7));}   /* x5/256 a step: four simulation steps */
        } else if(rphase==P_FINISH&&r->hornet)r->lat+=2*((420-r->lat)>>4);
        uint16_t old=r->s;
        {uint16_t v=r->speed>0?r->speed:0;r->s+=2*((v>>3)+(v>>5)+(v>>7)+(v>>9));}   /* x85/1024 a step: four simulation steps */
        if(r->s<old)++r->lap;
        track_point(r->s,r->lat,&r->x,&r->y);
        if(r->hornet&&rphase==P_RACE&&race_time>480&&r->speed>200) {
            if(r->t2>2)r->t2-=2;
            else if(gap>60&&gap<900){r->t2=(uint8_t)(54+rnd()%45);arg_x=r->x;arg_y=r->y;arg_life=200;overlay_call(0x7a,foes_drop_mine);}   /* a mine out the back when ahead of the car */
            else if(gap<-40&&gap>-800){r->t2=(uint8_t)(36+rnd()%30);arg_x=r->x;arg_y=r->y;arg_speed=2773;arg_life=130;overlay_call(0x7a,foes_aimed_bolt);}   /* shots at the car when behind it */
            else r->t2=15;
        }
        int16_t dist=hypot16(wrapdiff(px,r->x),wrapdiff(py,r->y));
        if(dist<44){bump(r->x,r->y,dist,44);r->speed=r->speed*9/10;speed=speed*9/10;shake=9;}
    }
}
FIELD_CODE static void standings(void) {
    uint8_t ahead=0;
    for(uint8_t k=0;k<N_RIVALS;++k)if(rv[k].hp&&rv[k].gap>0)++ahead;
    pce_campaign.rank=ahead+1;
}

FIELD_CODE void field_start_call(void) {start_field();}
/* arg_x: the car's lateral offset on the circuit, arg_life: 1 on the grid */
/* arg_dist comes back 1 when every rival still running has finished the race (three laps) before the car: the race is lost */
FIELD_CODE void field_update_call(void) {
    uint8_t alive=0,done=0;
    if(!(phase_t&3))update_field(arg_life,arg_x);   /* every fourth step, each worth four */
    for(uint8_t k=0;k<N_RIVALS;++k)if(rv[k].hp){++alive;if(rv[k].lap>=3)++done;}
    arg_dist=alive&&done==alive;
}
FIELD_CODE void field_standings_call(void) {standings();}
/* arg_speed: progress (as unsigned), arg_dist: lateral offset; the point comes back in arg_x, arg_y */
FIELD_CODE void field_point_call(void) {track_point((uint16_t)arg_speed,arg_dist,&arg_x,&arg_y);}
