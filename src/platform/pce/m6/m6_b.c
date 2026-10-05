#pragma clang section text=".ram_bank129.text" rodata=".ram_bank129.rodata" data=".ram_bank129.data" bss=".ram_bank129.bss"
#include "m6_common.h"
#define M6_SECTION ".ram_bank129.rodata"
#define M6_RCP_ONLY
#include "m6.h"
/* Image B: the Renegades (ramrod.c mech_update, spawn_mech, fire_plasma): the source's states, speeds, volleys and waits. */
static const uint8_t AIM[3]={42,51,42},REST[3]={60,66,30};
/* The waves, thinned for this hardware: three green Renegades, two at a time at most; a red with two greens and then another pair; the Commander with one green
 * beside him (the PC's waves have 4, 6 and 8 and show up to three at once). */
static const uint8_t W_KIND[3][8]={{0,0,0},{1,0,0,1,0},{2,0,0,0}};
static const int16_t W_ANG[3][8]={{0,720,-800},{-2080,2080,160,4800,-3520},{0,-960,960,3840}};   /* the source's bearings in radians x 214 */
static inline uint16_t rcp_of(uint16_t dist_q2) {uint16_t i=dist_q2>>5;return m6_rcp[i<200?i:200];}
static uint8_t attackers(bool melee) {
    uint8_t n=0;
    for(uint8_t k=0;k<3;++k) {
        uint8_t st=A.mech[k].st;
        if(melee?(st==S_CHARGE||st==S_WINDUP||st==S_PUNCH):(st>=S_AIM&&st<=S_PUNCH))++n;
    }
    return n;
}
static void fire_plasma(Mech6 *m) {
    Shot6 *s=shot_new();if(!s)return;
    int16_t v=pce_options.difficulty==2?31:pce_options.difficulty==1?27:23;   /* 470 / 410 / 350 units a second, in 1/4 unit a step */
    int16_t z0=(int16_t)SCALE16[m->variant]*39;      /* chest height: 78 x scale, 1/8 unit */
    int16_t steps=m->dist/v;if(steps<1)steps=1;
    /* the veterans' volleys fan out (+-0.09 radian): a bearing off by 19 dots at the focal length */
    int16_t spread=m->volley>0&&VOLLEY[m->variant]>1?((int16_t)(m->volley%3)-1)*19:0;
    *s=(Shot6){m->ang+spread*16,z0,0,-v,(296-z0)/steps,A.lat,m->dist,(uint8_t)(steps+90>255?255:steps+90),1,SHOT_DMG[m->variant]};
    burst(m->ang,m->dist,z0,1,9);
    audio_effect(13);
}
/* a new Renegade of wave sp_w's sp_i-th */
static void spawn(void) {
    uint8_t w=A.sp_w,i=A.sp_i;
    Mech6 *m=0;for(uint8_t k=0;k<3;++k)if(A.mech[k].st==S_OFF){m=&A.mech[k];break;}
    if(!m)return;
    uint8_t kind=W_KIND[w][i];
    int16_t a=wrapq(A.aim+W_ANG[w][i]*16);
    uint8_t hpmul=pce_options.difficulty==0?3:pce_options.difficulty==1?4:5;
    *m=(Mech6){a,(int16_t)((HP[kind]*hpmul+2)/5),0,(uint16_t)(1100+rnd())*4,(uint16_t)(30+rnd()%60)*4,(uint16_t)(850+rnd()),S_ENTER,kind,0,0,0,0,0,rnd(),(rnd()&1)?-1:1,0};
}
static void step(Mech6 *m) {
    uint8_t diffq=pce_options.difficulty==0?3:pce_options.difficulty==1?4:5;   /* the difficulty's pace, quarters */
    uint16_t d=m->dist>>2;
    uint8_t spd=SPEED[m->variant],cap=2+pce_options.difficulty+(m->variant==2);
    int16_t mv=0;   /* the change of distance this step, 1/4 unit: negative closes */
    if(m->flash)--m->flash;
    if(m->st_t)--m->st_t;
    switch(m->st) {
    case S_ENTER:mv=-spd;if(d<950)m->st=S_APPROACH;break;
    case S_APPROACH:
        mv=-spd;
        if(m->fire_t>diffq)m->fire_t-=diffq;else m->fire_t=0;
        if(d<m->pref){m->st=S_CIRCLE;m->st_t=60+rnd()%90;}
        else if(!m->fire_t&&attackers(false)<cap){m->st=S_AIM;m->st_t=(uint8_t)(AIM[m->variant]*4/diffq);}
        break;
    case S_CIRCLE: {   /* strafe round Ramrod, holding the preferred range */
        int16_t rad=((int16_t)d-(int16_t)m->pref)*4/5;if(rad>76)rad=76;else if(rad<-76)rad=-76;
        mv=-(rad>>4);
        m->ang=wrapq(m->ang+m->strafe*(int16_t)(rcp_of(m->dist)>>4));   /* 0.8 of its speed across: a bearing of 1/d radian a step */
        if(m->fire_t>diffq)m->fire_t-=diffq;else m->fire_t=0;
        if(d<230&&!attackers(true)){m->st=S_WINDUP;m->st_t=(uint8_t)(156/diffq);m->lat0=A.lat;break;}   /* too close: it swings */
        if(!m->st_t) {
            m->st_t=36+rnd()%60;
            if(rnd()<77)m->strafe=-m->strafe;
            if(d<700&&rnd()<(uint8_t)(30*diffq/4)&&!attackers(true)&&attackers(false)<cap){m->st=S_CHARGE;m->st_t=180;}
            else if(!m->fire_t&&d<1700&&attackers(false)<cap){m->st=S_AIM;m->st_t=(uint8_t)(AIM[m->variant]*4/diffq);}
            else m->pref=800+(uint16_t)rnd()*500/256;   /* the Renegades hold their distance and shoot from it, as the PC's do */
        }
        break; }
    case S_AIM:if(!m->st_t){m->st=S_FIRE;m->volley=VOLLEY[m->variant];m->st_t=0;}break;
    case S_FIRE:
        if(!m->st_t){
            fire_plasma(m);--m->volley;m->st_t=14;
            if(!m->volley){m->st=S_CIRCLE;m->st_t=24+rnd()%30;m->fire_t=(uint16_t)REST[m->variant]*(256+rnd())/64;}
        }
        break;
    case S_CHARGE:
        mv=-(int16_t)(spd*2+spd/10);
        if(d<225){m->st=S_WINDUP;m->st_t=(uint8_t)(144/diffq);m->lat0=A.lat;}
        else if(!m->st_t){m->st=S_CIRCLE;m->st_t=60;}
        break;
    case S_WINDUP:
        if(d>260)mv=-spd;
        if(!m->st_t) {
            m->st=S_PUNCH;m->st_t=21;
            /* the swing was aimed where Ramrod stood: a side-step (more than 0.3 radian of it) makes it miss */
            if(d<(uint16_t)(((uint16_t)SCALE16[m->variant]*250)>>4)+30&&(uint16_t)abs16(A.lat-m->lat0)<((d*5)>>1)){hurt_player(PUNCH_DMG[m->variant],36);A.flash_white=15;}
        }
        break;
    case S_PUNCH:if(!m->st_t){m->st=S_CIRCLE;m->st_t=72;m->pref=900+rnd();if(m->fire_t<240)m->fire_t=240;}break;
    case S_STAGGER:if(!m->st_t){m->st=S_CIRCLE;m->st_t=36;m->pref=850+(uint16_t)rnd()*3/4;}break;
    case S_DYING: {   /* bursts all over it while it sinks, then the big one */
        ++m->dying;
        if(m->expl_t)--m->expl_t;
        if(!m->expl_t&&m->dying<78) {
            m->expl_t=7;
            burst(wrapq(m->ang+((int16_t)(rnd()&63)-32)*8),m->dist,(int16_t)(160+(rnd()%90)*8),1+(rnd()&1),30);
        }
        if(m->dying>=90) {
            burst(m->ang,m->dist,240,3,30);burst(wrapq(m->ang+160),m->dist,400,2,30);burst(wrapq(m->ang-120+ARC),m->dist,200,3,30);
            audio_effect(17);if(A.shake<20)A.shake=d<600?20:9;
            m->st=S_OFF;
        }
        return; }
    default:return;
    }
    if(m->kick){mv+=m->kick>>2;m->kick-=2;if(m->kick<0)m->kick=0;}   /* a punched mech is pushed back */
    m->dist=(uint16_t)((int16_t)m->dist+mv);
    if(m->dist<(uint16_t)(60<<2))m->dist=60<<2;   /* it will not walk through Ramrod */
    m->anim+=(uint8_t)(mv<0?-mv:mv)*3;
    /* keep clear of the others: a mech on top of another one's bearing and depth is pushed out */
    for(uint8_t j=0;j<3;++j) {
        Mech6 *o=&A.mech[j];if(o==m||o->st==S_OFF||o->st==S_DYING)continue;
        int16_t da=m->ang-o->ang;if(da>ARC/2)da-=ARC;else if(da<-ARC/2)da+=ARC;
        uint16_t dd=m->dist>o->dist?m->dist-o->dist:o->dist-m->dist;
        if(abs16(da)<640&&dd<320)m->ang=wrapq(m->ang+(da>=0?4:-4));
    }
}
void m6_step(void) {step(&A.mech[A.cur]);}
void m6_spawn(void) {spawn();}
