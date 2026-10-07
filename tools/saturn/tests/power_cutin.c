/* Saturn first-use movie and resident fallback regression. */
#include <assert.h>
#include "../../../src/power.c"
static unsigned opens, prepared, portraits, faces;
static uint32_t portrait_id;
static int face_frame;
static int dummy;
static Sprite portrait;
static CBlock face;
const char *asset_path(const char *s) { return s; }
const char *plat_getenv(const char *s) { return NULL; }
void music_set_duck(real gain) { }
void sfx_play_file(const char *s) { }
void sfx_preload_file(const char *s) { }
void sfx_play(int id,int delay) { }
void voice_stop(void) { }
void video_preload_file(const char *s,real fps) { prepared+=s!=NULL; }
Video *video_open_file(Ren *r,const char *s,real fps) { opens++;return (Video *)&dummy; }
void video_close(Video *v) { assert(v==(Video *)&dummy); }
Sprite *sprite_get(uint32_t id) { portrait_id=id;return &portrait; }
RTex *sprite_tex(const Sprite *s) { return (RTex *)&dummy; }
bool gfx_keep_sprite(const Sprite *s) { assert(s==&portrait);portraits++;return true; }
CBlock *cblock_get(uint32_t id) { assert(id==0x2DEF1664);return &face; }
bool gfx_keep_cblock_frame(const CBlock *c,int frame) { assert(c==&face);face_frame=frame;faces++;return true; }
int main(void)
{
    for(int hero=0;hero<4;hero++)for(int bomb=0;bomb<2;bomb++) {
        Power pw={0};unsigned o=opens,p=prepared;
        power_warm_cutin(hero,bomb);
        assert(portrait_id==PORTRAIT[hero]&&face_frame==PIECE[hero]);
        power_reset(&pw,hero,bomb);
        bool movie=!bomb&&hero<2;
        assert(prepared==p+movie);
        power_start(&pw,NULL);
        assert(opens==o+movie&&pw.items==1&&pw.movie_used);
        end_cutin(&pw);pw.phase=PW_IDLE;pw.cooldown=0;pw.boost_t=0;
        power_start(&pw,NULL);
        assert(opens==o+movie&&!pw.video&&pw.items==0);
        assert(portrait_id==PORTRAIT[hero]&&face_frame==PIECE[hero]);
        power_close(&pw);
    }
    assert(portraits==8&&faces>=8);
    puts("PASS: all heroes/bombs retain matching portraits and faces; Saturn movies open only on first use");
}
