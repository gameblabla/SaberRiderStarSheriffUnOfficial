#include "campaign_pce.h"
#include "overlay_pce.h"
#include "race_pce.h"
#include "video_pce.h"
#include "assets.h"
#define RACE_SIN_SECTION ".ram_bank119.rodata"
#include "race_math.h"
/* The race's entities in the camera's frame, for the sprites (race_draw_pce.c): every product here is made with a table of quarter squares
 * (a b = q(a + b) - q(|a - b|), q(n) = n^2 / 4: two table reads instead of the generic multiply's 16 shift-and-adds, which took most of the
 * time of a frame: ten cars cost 100,000 cycles that way) and the distance's reciprocal is a table too. The tables live in the platform
 * background's tile reference array (video_pce.c cache_refs, 1792 bytes in the always-mapped work bank), which the race does not use; the road
 * builder calls qtable_init when it starts the race (road_pce.c road_init). $77: code here must not call a loader (pce_config.h PCE_X2).
 *
 * Cars are drawn far to near from the camera's frame: its focal length is 421 dots, its horizon row 113: a ground point at distance f is on row
 * 113 + 10080 / f; its offset from the middle is l * rows / 26.6 dots. */
extern uint16_t cache_refs[];
#define QSQ cache_refs                                 /* 511 entries: q(0..510) */
#define BELOW ((uint8_t*)cache_refs+1022)             /* 10080 / f for f 40..560 */
PCE_X2 void qtable_init(void) {
    uint16_t m2=0;
    for(uint16_t m=0;m<=255;++m){QSQ[2*m]=m2;QSQ[2*m+1]=m2+m;m2+=2*m+1;}   /* q(2m) = m^2, q(2m+1) = m^2 + m */
    for(uint16_t f=40;f<=560;++f)BELOW[f-40]=10080u/f;
}
static inline __attribute__((always_inline)) uint16_t umul(uint8_t a,uint8_t b) {
    uint8_t d=a>b?a-b:b-a;
    return QSQ[(uint16_t)a+b]-QSQ[d];
}
/* a * b exactly, a 16-bit and b 8-bit, both signed */
PCE_X2 static int32_t mulw(int16_t a,int8_t b) {
    uint16_t am=a<0?-a:a;uint8_t bm=b<0?-b:b;
    uint32_t p=((uint32_t)umul(am>>8,bm)<<8)+umul(am&255,bm);
    return (a<0)!=(b<0)?-(int32_t)p:(int32_t)p;
}
/* (a * b) >> 7 for |a| < 4096 */
PCE_X2 static int16_t mulq(int16_t a,int8_t b) {
    uint16_t am=a<0?-a:a;uint8_t bm=b<0?-b:b;
    uint16_t p=(umul(am>>8,bm)<<1)+(umul(am&255,bm)>>7);
    return (a<0)!=(b<0)?-(int16_t)p:(int16_t)p;
}
Visible vis[18];uint8_t nvis;
int16_t bolt_sx[10],bolt_sy[10];uint16_t bolt_ok;uint8_t bolt_step[10];
/* the ground point in the camera's frame: its screen column in dots, row and distance */
PCE_X2 static bool project_point(int16_t wx,int16_t wy,int16_t *sx,int16_t *row,int16_t *depth,bool fine) {
    int16_t rx=wrapdiff(wx,pce_control.x),ry=wrapdiff(wy,pce_control.y);
    if(rx>1900||rx<-1900||ry>1900||ry<-1900)return false;
    int16_t f=mulq(rx,cam_c)+mulq(ry,cam_s);
    if(f<40||f>560)return false;   /* beyond 560 units (road_pce.c FAR_F) the road is lost in the haze: nothing is drawn there */
    /* the side offset in quarter units, from the camera's Q14 sine and cosine (whole units moved a car four dots at a time near the camera, out of
     * step with the road under it); shots are small and short-lived and take the Q7 camera and a coarser offset */
    int16_t l;
    if(fine)l=(int16_t)((((mulw(ry,cam_c)-mulw(rx,cam_s))<<7)+mulw(ry,cam_cl)-mulw(rx,cam_sl))>>12);
    else l=(mulq(ry,cam_c)-mulq(rx,cam_s))<<2;
    if(l>4*f||l<-4*f)return false;
    uint8_t below=BELOW[f-40];
    *row=113+below;*depth=f;
    uint8_t scale=umul(below,154)>>8;   /* below * 154 >> 8 */
    uint16_t lm=l<0?-l:l;
    int16_t shift=(int16_t)(((uint32_t)lm*scale)>>6);
    *sx=256+(l<0?-shift:shift);
    return true;
}
PCE_X2 static void add(uint8_t kind,int16_t wx,int16_t wy) {
    int16_t sx,row,f;
    if(nvis>=18||!project_point(wx,wy,&sx,&row,&f,true))return;
    if(sx<-60||sx>572)return;
    uint8_t at=nvis;
    while(at&&vis[at-1].f>(uint16_t)f){vis[at]=vis[at-1];--at;}
    vis[at]=(Visible){sx,row,(uint16_t)f,kind};++nvis;
}
/* Every car, mine and wreck on view (nearest last), and the shots' screen places */
PCE_X2 void project_entities(void) {
    nvis=0;
    for(uint8_t k=0;k<7;++k)if(rv[k].hp)add(rv[k].kind,rv[k].x,rv[k].y);
    for(uint8_t k=0;k<6;++k)if(mines[k].t)add(6,mines[k].x,mines[k].y);
    if(rphase>=P_PURSUIT) {
        for(uint8_t k=0;k<2;++k)if(escort[k].hp)add(1,escort[k].x,escort[k].y);
        if(boss.state<3)add(2,boss.x,boss.y);
    }
    for(uint8_t k=0;k<4;++k)if(blasts[k].t)add(8+((blasts[k].t-1)>>2),blasts[k].x,blasts[k].y);   /* wrecks: kind 8 + frame */
    if(rphase<P_PURSUIT) {   /* the line's flag poles: track point 0, 138 units out each way along the normal (race_field_pce.c track_point) */
        int16_t dx=wrapdiff(track[1].x,track[0].x),dy=wrapdiff(track[1].y,track[0].y);
        for(uint8_t side=0;side<2;++side) {
            int16_t lat=side?138:-138,nx=dy*lat,ny=dx*lat;
            add(side?13:7,(track[0].x-((nx>>6)+(nx>>8)))&8191,(track[0].y+((ny>>6)+(ny>>8)))&8191);
        }
    }
    bolt_ok=0;
    for(uint8_t k=0;k<10;++k)if(race_bolts[k].t) {
        int16_t sx,row,f;
        if(project_point(race_bolts[k].x,race_bolts[k].y,&sx,&row,&f,false)){bolt_sx[k]=sx;bolt_sy[k]=row;bolt_ok|=1<<k;bolt_step[k]=f<130?0:f<210?1:f<320?2:f<440?3:4;}
    }
}
/* While a dialogue is up the race stands still behind it: the car and everything on view are drawn from the last frame's camera (story_pce.c; the
 * box is a BG panel at the top, so nothing here is hidden by it). */
PCE_X2 void race_dialog_cars(void) {
    project_entities();
    video_sprite_optional(PCE_CAR_STEER+4,256,215,false,16);
    for(uint8_t k=0;k<nvis;++k) {
        uint16_t rows=vis[k].y-113,dots=rows+(rows>>3)+(rows>>5);
        uint8_t kind=vis[k].kind;
        if(kind>=8&&kind<13){video_sprite_optional(PCE_CAR_EXPL+(dots>70?0:5)+kind-8,vis[k].x,vis[k].y,false,16);continue;}
        if(kind==7||kind==13){uint16_t f=vis[k].f;video_sprite_optional(PCE_CAR_POLE+(f<125?5:f<165?4:f<225?3:f<310?2:f<430?1:0),vis[k].x,vis[k].y,kind==13,16);continue;}
        uint8_t i=0;
        while(i<PCE_CAR_STEPS-1&&(uint16_t)pce_car_widths[i]*2<dots)++i;
        video_sprite_optional(3+kind*PCE_CAR_STEPS+i,vis[k].x,vis[k].y,false,16);
    }
}
