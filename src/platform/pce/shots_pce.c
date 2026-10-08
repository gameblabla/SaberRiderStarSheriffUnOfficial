#include "play_internal.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "loader_pce.h"
/* Every shot, one step: Q8 motion, the kneelers' grenade parabola (the source's vy = 12.667 tc^2 - speed/64 px a step,
 * tc = seconds in flight, at most 1), hits on the hero and on the enemies (the source's hurt boxes plus the bullet's
 * 5 px: about 10 px either side of the body, 21 above its middle to 37 below; a kneeler's top is 7). */
#define SHOT_CODE __attribute__((noinline,section(".ram_bank112.text")))
extern int16_t cell_x,cell_y;
extern uint8_t cell_value;
void cell_call(void);
SHOT_CODE static int16_t absolute(int16_t n) {return n<0?-n:n;}
SHOT_CODE static void advance(int16_t *p,uint8_t *f,int16_t v) {
    int16_t sum=(int16_t)*f+v;
    *p+=sum>>8;*f=sum;
}
SHOT_CODE void shots_step(void) {
    for(Shot *s=shots;s<shots+NSHOTS;++s) {
        if(!s->active)continue;
        if(s->enemy==4){if(++s->t>=40)s->active=0;continue;}   /* a grenade's burst: it burns where it fell (drawn from enemy_base + 80) */
        if(s->enemy==2) {
            uint8_t tc=s->t<60?s->t:60;
            s->vy=((((uint16_t)tc*tc>>3)*115)>>4)-((absolute(s->vx)*15)>>4);
            if(s->t<255)++s->t;
        }
        advance(&s->x,&s->fx,s->vx);advance(&s->y,&s->fy,s->vy);
        int16_t sx=s->x-(int16_t)camera;
        if(sx<-16||sx>272||s->y<0||s->y>240){s->active=0;continue;}
        if(s->enemy!=3) {
            /* bullets stop at solid cells and ramps (bullets.c); a grenade also bursts */
            cell_x=s->x>>3;cell_y=s->y>>3;overlay_call(0x81,cell_call);
            if(cell_value==15||(cell_value&16)){if(s->enemy==2){s->enemy=4;s->t=0;s->vx=s->vy=0;audio_effect(4);}else s->active=0;continue;}   /* a grenade bursts into its explosion, with the burst's sound */
        }
        if(s->enemy) {
            int16_t dx=s->x-player.x,dy=s->y-player.y,r=s->enemy==2?16:8;
            if(!safe_timer&&dx>-r&&dx<r&&dy>-14&&dy<25) {
                if(s->enemy==2){s->enemy=4;s->t=0;s->vx=s->vy=0;audio_effect(4);}else s->active=0;   /* (a grenade bursts on him) */
                safe_timer=120;audio_effect(5);
                campaign_hurt();
                if(pce_campaign.diagnostic&&!pce_metrics.hp)pce_metrics.hp=3;
            }
        } else for(Actor *a=actors;a<actors+8;++a) {
            if(!a->active||a->dead||(a->type>=11&&a->type<=28))continue;
            int16_t dx=s->x-a->b.x,dy=s->y-(a->b.y-(a->type==31?5:0));   /* the tower sniper draws 5px higher, so its hitbox does too */
            if(dx>-10&&dx<10&&dy>(a->type==8||a->type==9?-7:-21)&&dy<37) {
                s->active=0;if(a->type>=30&&facing==a->flip)a->hp=1;   /* a shot in the back kills a shield sniper */
                if(a->type>=30&&!(a->mode&4)&&facing!=a->flip){   /* the shield takes it, then burns away (4 steps a cell) */
                    if(!--a->hp){a->mode|=4;a->aim=18;a->hp=1;}
                    audio_effect(7);
                } else if(!--a->hp)actor_kill(a);else audio_effect(7);
                break;
            }
        }
    }
}
