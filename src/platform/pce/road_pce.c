#include "road_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "assets.h"
#include "overlay_pce.h"
#include "campaign_pce.h"
#define RACE_SIN_SECTION ".ram_bank111.rodata"
#include "race_math.h"
#include "race_pce.h"

/* The race's road, the classic way (see docs/PCE_CLASSIC_ROAD_20261005.md). The picture of a straight road in perspective is
 * static in the BAT (build_assets.py road_assets); each scanline below the horizon (113) is scrolled sideways (BXR) so that
 * the road's centre lands where the circuit puts it, and reads one of two copies of the picture (BYR) for the stripes. This
 * file builds the per-scanline tables the hblank handler (irq.S) reads, for the camera of the frame:
 *   1. the circuit's points ahead of the camera are put in the camera's frame (f ahead, sd to the right);
 *   2. a point at distance f is on scanline 113 + 10080 / f and sd * d / 26.6 dots right of the middle (the picture's kerb, 120 units, is 4.5 dots a line)
 *      with d the lines below the horizon; between two points the centre is a straight line of the scanline;
 *   3. the stripe of a scanline follows the distance along the circuit (progress + 10080 / d), 32 units to a band.
 * Tables: three pages of 128 bytes (low and high byte of BXR, the copy a line reads) for each of two buffers, in the
 * platform background's cache array (unused in the race); the buffer on display is the one the VBlank handler latched. */
#define ROAD_CODE PCE_FLOOR     /* bank $6d: the loop that fills the tables */
#define ROAD_MATH PCE_MISSION   /* bank $6f: the camera-frame arithmetic and its tables ($6d holds the field and the sky loader) */
#define DMAX 110          /* the nearest scanline, 223 */
#define NEAR_F 88         /* the camera-frame distance of scanline DMAX + 1 */
#define POINTS 13
#define FAR_F 560         /* the farthest distance drawn: scanline 113 + 18, where the road is lost in the haze */
#define SIDE_LIMIT 1200   /* 300 units in Q2: farther than that a point's offset is held there (keeps the Q4 dots, and the slope between two points, in 16 bits) */
extern uint16_t columns[33][30];
extern volatile uint8_t pce_floor_pending;
extern volatile uint16_t pce_sky_far,pce_sky_near;
/* road_fill.S: arguments of its primitives */
uint8_t *road_fill_base,*road_sel_base;
uint8_t road_fill_b0,road_fill_b1,road_fill_b2,road_fill_s0,road_fill_s1,road_fill_s2,road_fill_m,road_fill_n,road_div_n,road_along;
uint16_t road_div_q;
void road_fill(void),road_divide(void),road_stripes(void);
void road_advance(int8_t dx,int8_t dy);
/* the knots of the frame (scanline and offset, Q4 dots), after the tables in the platform background cache array */
#define kn_d ((int16_t*)((uint8_t*)columns+1024))
#define kn_x ((int16_t*)((uint8_t*)columns+1056))
static uint8_t kn_n,pursuit;
static uint16_t camera_x,camera_y;
int32_t road_k[2];   /* road_fill.S: the knot being walked in the camera's frame, forward and to the right, in 1/16384 units */

/* a * b for int16 a and int8 b, in 32 bits: the high and the low byte of a apart */
ROAD_MATH static int32_t mulw(int16_t a,int8_t b) {
    int8_t high=a>>8;uint8_t low=a;
    return ((int32_t)(int16_t)(high*b)<<8)+(int16_t)((int16_t)low*b);
}
/* a * (128 m + ml): the camera's sine or cosine is the byte m (Q7) and a small remainder ml, which together keep the road still
 * while the camera turns (a Q7 value alone moves the far road three dots at each step of it) */
ROAD_MATH static int32_t rotate(int16_t a,int8_t m,int8_t ml) {return (mulw(a,m)<<7)+mulw(a,ml);}
/* Scanline of a distance (lines below the horizon, d = 10080 / f). */
ROAD_MATH static int16_t line_of(int16_t f) {return f>=766?pce_road_d[383]:pce_road_d[f>>1];}
/* The dots (Q4) to the right of the middle of a side offset (Q2: quarter units) at d lines below the horizon: side * d * 0.0376 * 16 / 4
 * (the picture's kerb, 120 units, is 4.5 dots a line). A scanline's window is 512 dots of the 1024-dot picture and wraps round it:
 * the lower rows (d >= 64) have wrap copies whose wrapped half is sand, so they take any offset; the upper rows, whose kerbs are
 * under 290 dots out, show sand for the first 224 dots of either edge, and a window may run that far past it (offset 480 dots). */
extern uint16_t cache_refs[];
ROAD_MATH static int16_t dots_q4(int16_t side,int16_t d) {
    /* side * d * 77 / 512 in 16 bits: the product by the table of quarter squares (race_proj.c) as A 256 + B, then 77 / 512 = 1/8 + 1/64 + 1/128 + 1/512 */
    uint16_t sm=side<0?-side:side;
    uint8_t da=sm>>8,db=sm,dd=d,m1=da>dd?da-dd:dd-da,m2=db>dd?db-dd:dd-db;uint16_t s1=(uint16_t)da+dd,s2=(uint16_t)db+dd;
    uint16_t A=cache_refs[s1]-cache_refs[m1],B=cache_refs[s2]-cache_refs[m2];
    uint16_t y=(((A<<6)+(A<<3)+(A<<2)+A)>>1)+(B>>3)+(B>>6)+(B>>7)+(B>>9);
    int16_t limit=d<64?7680:12800;
    int16_t x=y>(uint16_t)limit?limit:(int16_t)y;
    return side<0?-x:x;
}
/* The knots: the circuit's points ahead of the camera, a few before it, as scanline kn_d[i] and offset kn_x[i] (Q4 dots), nearest
 * first. The first point is rotated into the camera's frame; each next one is the last plus the rotated step between them (a step is
 * under 128 units: a byte times a byte, kept to 14 bits). The segment that crosses the near distance is cut there; the list ends where
 * the road turns away from the camera or at the far distance. */
ROAD_MATH static void knots_call(void) {
    int16_t previous_f=-32000,previous_side=0;
    uint8_t index=road_idx-3;
    bool have=false;
    kn_n=0;
    for(uint8_t k=0;k<POINTS;++k) {
        if(!k) {
            int16_t wx,wy;
            if(pursuit){wx=4096;wy=(camera_y&~63)+192;}
            else {const TrackPoint *t=&track[index];wx=t->x;wy=t->y;}
            int16_t rx=wrapdiff(wx,camera_x),ry=wrapdiff(wy,camera_y);
            road_k[0]=rotate(rx,cam_c,cam_cl)+rotate(ry,cam_s,cam_sl);road_k[1]=rotate(ry,cam_c,cam_cl)-rotate(rx,cam_s,cam_sl);
        } else {
            uint8_t step=k<=7?1:2,next=index+step;   /* every sample near, every second one far: a chord of the far curve is a dot off at most */
            int8_t dx,dy;
            if(pursuit){dx=0;dy=-64*step;}
            else {dx=(int8_t)(track[next].x-track[index].x);dy=(int8_t)(track[next].y-track[index].y);}
            index=next;
            road_advance(dx,dy);
        }
        int16_t f=(int16_t)(road_k[0]>>14);
        int32_t wide=road_k[1]>>12;
        int16_t side=wide>SIDE_LIMIT?SIDE_LIMIT:wide<-SIDE_LIMIT?-SIDE_LIMIT:(int16_t)wide;
        if(f<=previous_f)break;                  /* the road turns away from the camera: it ends here */
        if(f<NEAR_F){previous_f=f;previous_side=side;continue;}
        int16_t d=line_of(f),x=dots_q4(side,d);
        if(!have) {
            have=true;
            if(previous_f>-32000) {              /* the segment from the point behind, cut at the near distance */
                uint16_t t=((uint16_t)(NEAR_F-previous_f)<<7)/(uint16_t)(f-previous_f);
                int16_t s0=previous_side+(int16_t)((int32_t)(side-previous_side)*(t>127?127:t)>>7);
                kn_d[0]=line_of(NEAR_F);kn_x[0]=dots_q4(s0,kn_d[0]);kn_n=1;
            }
        }
        kn_d[kn_n]=d;kn_x[kn_n]=x;++kn_n;
        previous_f=f;previous_side=side;
        if(f>=FAR_F)break;
    }
}
/* Lines dcur down to d_end + 1 get the offset xq (Q4 dots) and then slope more on every line nearer the horizon. */
ROAD_CODE static void fill(int16_t dcur,int16_t d_end,int16_t xq,int16_t slope) {
    int16_t n=dcur-d_end;
    if(n<=0)return;
    int32_t b=65536L-((int32_t)xq<<4),s=-((int32_t)slope<<4);   /* BXR = 256 - x, 256 to a dot */
    road_fill_m=dcur+1;road_fill_n=n;
    road_fill_b0=b;road_fill_b1=b>>8;road_fill_b2=b>>16;road_fill_s0=s;road_fill_s1=s>>8;road_fill_s2=s>>16;
    road_fill();
}
ROAD_CODE static void road_update(void) {
    for(uint16_t guard=0;pce_floor_pending!=pce_floor_page&&guard<20000;++guard){}   /* a finished buffer is shown at the next VBlank; never write the one about to be */
    uint8_t back=pce_floor_page^1;
    road_fill_base=(uint8_t*)columns+(back?128:0);road_sel_base=road_fill_base+512;
    pursuit=rphase>=P_PURSUIT;camera_x=pce_control.x;camera_y=pce_control.y;
    overlay_call(0x6f,knots_call);
    int16_t dcur=DMAX;
    if(!kn_n)fill(dcur,-2,0,0);
    else {
        int16_t d_p=kn_d[0],x_p=kn_x[0];
        if(dcur>d_p){fill(dcur,d_p,x_p,0);dcur=d_p;}
        for(uint8_t i=1;i<kn_n&&dcur>=1;++i) {
            int16_t d_q=kn_d[i],x_q=kn_x[i],n=d_p-d_q;
            if(n<=0)continue;
            int16_t delta=x_q-x_p;
            road_div_q=delta<0?-delta:delta;road_div_n=n;road_divide();
            int16_t slope=delta<0?-(int16_t)road_div_q:(int16_t)road_div_q;
            fill(dcur,d_q,dcur<d_p?x_p+slope*(d_p-dcur):x_p,slope);
            dcur=d_q;d_p=d_q;x_p=x_q;
        }
        fill(dcur,-2,x_p,0);                     /* the far end: the road's last centre, up to the horizon */
    }
    road_along=pursuit?(uint8_t)(-camera_y):(uint8_t)((ps>>8)*52+(((ps&255)*52)>>8));
    road_stripes();
    pce_sky_far=(cam_hd>>7)&511;
    pce_sky_near=(cam_hd>>6)&511;   /* the mountains turn with the camera and do not slide as the car drives (they are far off) */
    pce_floor_pending=back;
    ++pce_metrics.floor_commits;
}
ROAD_CODE static bool road_init(void) {
    pce_floor_pending=pce_floor_page=0;
    video_display(false);video_race_init();video_display(false);
    if(!video_race_sky())return false;
    overlay_call(0x77,qtable_init);   /* the entities' multiplication and reciprocal tables (race_proj.c) */
    /* both buffers start as a straight road ahead (BXR 256: the road's centre in the middle of the screen) */
    for(uint16_t i=0;i<768;++i)((uint8_t*)columns)[i]=0;
    for(uint8_t m=0;m<=DMAX+1;++m){((uint8_t*)columns)[m]=0;((uint8_t*)columns)[m+256]=1;((uint8_t*)columns)[m+128]=0;((uint8_t*)columns)[m+384]=1;}
    return true;   /* the display stays off: the caller turns it on once the first frame is built (a stage fades in from black; a resume shows it at once) */
}
ROAD_CODE void road_start(void) {pce_control.ok=road_init();}
ROAD_CODE void road_draw(void) {road_update();}
