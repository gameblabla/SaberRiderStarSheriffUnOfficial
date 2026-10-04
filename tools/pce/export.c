/* Host-only: export the active stage from current gameplay sources. The null
 * platform is linked with its entry point renamed. No cached Saturn dumps. */
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
Ren *rnull_renderer(void);
static int value(real x) { return (int)lroundf(x); }
/* The voice sample a dialogue script names on its first line (played when the box opens). */
static unsigned sfx_of(unsigned text) {
    Dialog d;
    return text&&dialog_open(&d,text)?d.pending_sfx:0;
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    int stage = atoi(argv[2]);
    if (!app_init(rnull_renderer(), argv[1], stage)) return 1;
    Game *g = app_game();
    char path[1024];
    snprintf(path, sizeof path, "%s/stage%d.layers", argv[3], stage);
    level_dump_layers(&g->level, path);
    snprintf(path, sizeof path, "%s/stage%d.json", argv[3], stage);
    FILE *f = fopen(path, "w");
    if (!f) return 1;
    Level *l = &g->level;
    Character *c = &g->player.ch;
    fprintf(f, "{\"stage\":%d,\"width\":%d,\"height\":%d,\"cols\":%d,\"rows\":%d,"
            "\"cellw\":%d,\"cellh\":%d,\"start\":[%d,%d],\"speed\":%d,\"jump\":%d,"
            "\"origin\":[%d,%d],\"box\":[%d,%d,%d,%d],\"objects\":[",
            stage, value(l->width), value(l->height), l->cols, l->rows, l->cellw, l->cellh,
            value(c->body.x), value(c->body.y), value(c->speed), value(c->jump_vel),
            value(c->origin_x), value(c->origin_y), value(c->box_ox), value(c->box_oy),
            value(c->box_hx), value(c->box_hy));
    for (int i = 0; i < l->nobjs; ++i) {
        LevelObject *o = &l->objs[i];
        fprintf(f, "%s{\"type\":%u,\"x\":%d,\"y\":%d,\"zone\":[%d,%d],\"layer\":%u,"
                "\"interval\":%u,\"delay\":%u,\"loops\":%d,\"waypoints\":[",
                i ? "," : "", o->type, value(o->x), value(o->y), value(o->spawn_x), value(o->spawn_y),
                o->layer, o->a, o->b, o->loops);
        for (int k = 0; k < o->n_wp && k < 3; ++k)
            fprintf(f, "%s[%d,%d]", k ? "," : "", value(o->wp[k][0]), value(o->wp[k][1]));
        fprintf(f, "]}");
    }
    fprintf(f, "],\"triggers\":[");
    for (int i=0; i<g->enemies.ntr; ++i) {
        Trigger *t=&g->enemies.tr[i];
        fprintf(f, "%s{\"type\":%d,\"layer\":%d,\"rand\":%u,\"zone\":[%d,%d,%d,%d],\"interval\":%u,\"delay\":%d,\"loops\":%d,\"waypoints\":[",
                i?",":"",t->type,t->layer,(unsigned)t->rand_n,value(t->cx),value(t->cy),value(t->hx),value(t->hy),t->interval_ms,value(t->timer*60),t->remaining);
        for (int k=0;k<t->nwp;++k) fprintf(f,"%s[%d,%d]",k?",":"",value(t->wp[k][0]),value(t->wp[k][1]));
        fprintf(f,"]}");
    }
    fprintf(f, "],\"dialogs\":[");
    for(int k=0;k<4;++k) {
        fprintf(f,"%s{\"id\":%u,\"zone\":[%d,%d,%d,%d],\"focus\":[%d,%d],\"hold\":[%d,%d],\"sfx\":%u}",k?",":"",g->dialogs[k].text,
                value(g->dialogs[k].cx),value(g->dialogs[k].cy),value(g->dialogs[k].hx),value(g->dialogs[k].hy),
                value(g->dialogs[k].focus_x),value(g->dialogs[k].focus_y),value(g->dialogs[k].t_in*1000),value(g->dialogs[k].t_out*1000),sfx_of(g->dialogs[k].text));
        if(g->dialogs[k].text) {
            Dialog d;
            if(dialog_open(&d,g->dialogs[k].text)) {
                char scriptpath[1024];snprintf(scriptpath,sizeof scriptpath,"%s/dialog%d.txt",argv[3],k);
                FILE *df=fopen(scriptpath,"w");if(!df)return 1;
                for(int j=0;j<d.npages;++j)fprintf(df,"%s<avatar:%08X>%s",j?"\n<<>>\n":"",d.pages[j].avatar_id,d.pages[j].text);
                fclose(df);
            }
        }
    }
    fprintf(f, "],\"stops\":[");
    for(int k=0;k<g->nstops;++k)fprintf(f,"%s[%d,%d,%d,%d]",k?",":"",
        value(g->stops[k].cx),value(g->stops[k].cy),abs(value(g->stops[k].hx)),abs(value(g->stops[k].hy)));
    fprintf(f, "],\"deathzones\":[");
    for(int k=0;k<g->ndeath;++k)fprintf(f,"%s[%d,%d,%d,%d,%d,%d]",k?",":"",
        value(g->deathzones[k].cx),value(g->deathzones[k].cy),value(g->deathzones[k].hx),value(g->deathzones[k].hy),
        value(g->deathzones[k].rx),value(g->deathzones[k].ry));
    fprintf(f, "]}"); fclose(f);
    snprintf(path, sizeof path, "%s/stage%d.collision", argv[3], stage);
    f = fopen(path, "wb");
    if (!f || fwrite(l->collision, 1, (size_t)l->cols * l->rows, f) != (size_t)l->cols * l->rows) return 1;
    fclose(f);
    app_shutdown();
    return 0;
}
