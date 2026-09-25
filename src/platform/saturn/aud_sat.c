/* Saturn game audio backend.
 *
 * One persistent PoneSound 68000 driver owns the SCSP.  Controls 0/1 play a
 * small 44.1 kHz stereo PCM ring which the master SH-2 fills every frame.
 * The ring mixes:
 *   - game SFX encoded with celeriyacon/scspadpcm's predictor/quantizer;
 *   - stereo 44.1 kHz CRI ADX music streamed from the data track.
 * Movie audio has independent controls/ring (film_snd.c), so starting a CPK
 * no longer clears/stops the game's music/SFX driver.
 *
 * The original scspadpcm proof-of-concept uses 16-bit SCSP playback lengths.
 * Our SADP wrapper has 32-bit block/sample counts and the SH-2 decoder walks
 * blocks directly, so long voice/effect samples are not capped at 64 KiB.
 */
#include "../aud.h"
#include "../../pack.h"
#include "../../assets.h"
#include "sat_internal.h"
#include "scsp_adpcm_sat.h"
#include "pcmsys.h"
#include <yaul.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MIX_RATE             44100u
#define MIX_RING_BYTES       16384u
#define MIX_RING_SAMPLES     (MIX_RING_BYTES / 2u)
#define MIX_LEFT_OFF         0x10000u
#define MIX_RIGHT_OFF        0x14000u
#define MOVIE_LEFT_OFF       0x18000u
/* SNDDRV.BIN/PoneSound owns fixed banks at 0x20000, 0x30000 and 0x50000.
 * Keep compressed resident SFX only in banks verified unused by the shared
 * driver, game PCM rings and movie ring.  0x7F000 is pcmsys' hard ceiling
 * (the 68k stack lives above it). */
#define RESIDENT_A0_BEGIN    0x28000u
#define RESIDENT_A0_END      0x30000u
#define RESIDENT_A1_BEGIN    0x40000u
#define RESIDENT_A1_END      0x50000u
#define RESIDENT_A2_BEGIN    0x60020u
#define RESIDENT_A2_END      0x7F000u
#define MIX_AHEAD_SAMPLES    2304u
#define MIX_CHUNK            256u
#define MAX_SAMPLES          192
#define MAX_VOICES           12

struct AudSample {
    uint32_t key;
    const uint8_t *data;
    uint32_t size, samples;
    bool missing, kept, resident;
    uint16_t users;
};

static AudSample samples[MAX_SAMPLES];
static int nsamples;
typedef struct { uint32_t next, end; } ResidentArena;
static ResidentArena resident_arena[] = {
    { RESIDENT_A0_BEGIN, RESIDENT_A0_END },
    { RESIDENT_A1_BEGIN, RESIDENT_A1_END },
    { RESIDENT_A2_BEGIN, RESIDENT_A2_END }
};
static uint32_t resident_bytes;

typedef struct {
    bool active, loop;
    AudSample *s;
    SatAdpDecoder dec;
    int32_t gain;                 /* 16.16 */
    uint16_t gen;
} Voice;
static Voice voices[MAX_VOICES];

static volatile int16_t *ring_l = (volatile int16_t *)(SNDRAM + MIX_LEFT_OFF);
static volatile int16_t *ring_r = (volatile int16_t *)(SNDRAM + MIX_RIGHT_OFF);
static uint32_t mix_start_us;
static uint64_t mix_written;
static unsigned mix_underruns;
/* sat_timer_us() is excellent for short intervals, but its phase must not be
 * allowed to free-run against the independent 44.1 kHz SCSP clock for
 * minutes.  Anchor it at each observed 4096-sample SCSP CA boundary. */
static bool mix_ca_seen, mix_sync_valid;
static uint8_t mix_ca_half;
static uint32_t mix_anchor_us;
static uint64_t mix_anchor_samples;

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint16_t rd16be(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static int16_t clip16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static int32_t gain_q16(real g) { int32_t v=(int32_t)g; return v<0?0:v>FX_ONE?FX_ONE:v; }

static bool sample_load(AudSample *s)
{
    if (!s || s->missing) return false;
    if (s->data) return true;
    const PackEntry *e = packs_find_type(s->key, RES_SAMPLE);
    if (!e || e->size < 32 || memcmp(e->data, "SADP", 4)) {
        printf("snd: sample %08lX missing/bad\n", (unsigned long)(s ? s->key : 0));
        if (s) s->missing = true;
        return false;
    }
    s->data = e->data; s->size = e->size; s->samples = sat_adp_samples(e->data, e->size);
    if (!s->samples) { s->missing=true; return false; }
    return true;
}

static bool resident_alloc(uint32_t n, uint32_t *at)
{
    /* Best-fit matters: the safe SCSP banks are fragmented.  Keeping each
     * SADP object inside one bank also means no decoder block can straddle a
     * bank owned by the sound driver. */
    int best = -1;
    uint32_t best_left = UINT32_MAX;
    for (unsigned i = 0; i < sizeof resident_arena / sizeof resident_arena[0]; i++) {
        uint32_t p = (resident_arena[i].next + 31u) & ~31u;
        if (p > resident_arena[i].end || n > resident_arena[i].end - p) continue;
        uint32_t left = resident_arena[i].end - (p + n);
        if (left < best_left) { best = (int)i; best_left = left; *at = p; }
    }
    if (best < 0) return false;
    resident_arena[best].next = *at + n;
    resident_bytes += n;
    return true;
}

static void sample_make_resident(AudSample *s)
{
    if (!sample_load(s) || s->resident) return;
    uint32_t n=(s->size+31u)&~31u, at;
    if (!resident_alloc(n, &at)) {
        printf("snd: resident pool full, %08lX stays in work RAM (%lu bytes)\n",
               (unsigned long)s->key, (unsigned long)s->size);
        return;
    }
    /* Saturn sound RAM is not byte-addressable from the SH-2.  Preserve the
     * SADP byte stream with aligned 16-bit writes; scsp_adpcm_sat.c mirrors
     * this with 16-bit reads when decoding resident samples. */
    volatile uint16_t *dst=(volatile uint16_t *)(SNDRAM+at);
    for(uint32_t i=0;i<n;i+=2){
        uint8_t a=i<s->size?s->data[i]:0, b=i+1<s->size?s->data[i+1]:0;
        dst[i>>1]=(uint16_t)(((uint16_t)a<<8)|b);
    }
    packs_release_type(s->key, RES_SAMPLE);
    s->data=(const uint8_t *)(SNDRAM+at); s->resident=true;
}

static AudSample *sample_find(uint32_t key)
{
    for (int i=0;i<nsamples;i++) if(samples[i].key==key) return samples[i].missing?NULL:&samples[i];
    if(nsamples>=MAX_SAMPLES) return NULL;
    AudSample *s=&samples[nsamples++]; memset(s,0,sizeof *s); s->key=key;
    return s;
}

AudSample *aud_sample_pack(uint32_t id) { return sample_find(id); }
AudSample *aud_sample_file(const char *path) { return path ? sample_find(asset_key(path)) : NULL; }
void aud_prefetch(AudSample *s) { (void)sample_load(s); }
void aud_keep(AudSample *s, bool loop)
{
    (void)loop; if(!s) return; s->kept=true;
    /* Core table samples are presented through aud_keep at boot. Copy what
     * fits into SCSP RAM compressed; this costs no SH-2 work RAM. */
    sample_make_resident(s);
}

static void voice_release(Voice *v)
{
    if(!v->active) return;
    AudSample *s=v->s; v->active=false; v->s=NULL;
    if(s && s->users) s->users--;
    if(s && !s->resident && !s->kept && s->users==0 && s->data) {
        packs_release_type(s->key, RES_SAMPLE); s->data=NULL; s->size=s->samples=0;
    }
}

int aud_play(AudSample *s, real gain, bool loop)
{
    if(!sample_load(s)) return -1;
    int best=-1;
    for(int i=0;i<MAX_VOICES;i++) if(!voices[i].active){best=i;break;}
    if(best<0){ for(int i=0;i<MAX_VOICES;i++) if(!voices[i].loop){best=i;break;} }
    if(best<0) best=0;
    voice_release(&voices[best]);
    Voice *v=&voices[best]; memset(&v->dec,0,sizeof v->dec);
    if(!sat_adp_init(&v->dec,s->data,s->size)) return -1;
    v->active=true; v->loop=loop; v->s=s; v->gain=gain_q16(gain); v->gen++; if(!v->gen) v->gen=1; s->users++;
    return best | ((int)v->gen<<8);
}
static Voice *voice_handle(int h){ int i=h&0xff; return i>=0&&i<MAX_VOICES&&voices[i].active&&((int)voices[i].gen<<8|i)==h?&voices[i]:NULL; }
void aud_set_gain(int h, real gain){ Voice *v=voice_handle(h); if(v) v->gain=gain_q16(gain); }
void aud_stop(int h){ Voice *v=voice_handle(h); if(v) voice_release(v); }
bool aud_playing(int h){ return voice_handle(h)!=NULL; }

/* ---- ADX music ---- */
typedef struct {
    FILE *f; uint32_t data_off, total_samples, pos; bool loop, paused;
    int32_t h1[2],h2[2],gain;
    int16_t pcm[2][32]; uint8_t ppos, pcount;
} Music;
static Music mus;
#define ADX_C1 14668
#define ADX_C2 (-6566)

static void adx_block(const uint8_t *src,int ch,int16_t *dst)
{
    int32_t scale=(src[0]<<8)|src[1], h1=mus.h1[ch],h2=mus.h2[ch];
    for(int i=2,o=0;i<18;i++){
        int n=src[i]>>4; if(n&8)n-=16; int32_t x=(n*scale*8192 + ADX_C1*h1 + ADX_C2*h2)>>13; x=clip16(x); dst[o++]=(int16_t)x; h2=h1;h1=x;
        n=src[i]&15; if(n&8)n-=16; x=(n*scale*8192 + ADX_C1*h1 + ADX_C2*h2)>>13; x=clip16(x); dst[o++]=(int16_t)x; h2=h1;h1=x;
    }
    mus.h1[ch]=h1; mus.h2[ch]=h2;
}
static bool music_rewind(void)
{
    if(!mus.f || fseek(mus.f,(off_t)mus.data_off,SEEK_SET)<0) return false;
    mus.pos=0; mus.ppos=mus.pcount=0; mus.h1[0]=mus.h1[1]=mus.h2[0]=mus.h2[1]=0; return true;
}
static bool music_fill_block(void)
{
    if(!mus.f) return false;
    if(mus.pos>=mus.total_samples){ if(!mus.loop||!music_rewind()) return false; }
    uint8_t b[36]; if(fread(b,1,sizeof b,mus.f)!=sizeof b){ if(!mus.loop||!music_rewind()||fread(b,1,sizeof b,mus.f)!=sizeof b) return false; }
    adx_block(b,0,mus.pcm[0]); adx_block(b+18,1,mus.pcm[1]);
    uint32_t left=mus.total_samples-mus.pos; mus.pcount=(uint8_t)(left<32?left:32); mus.ppos=0; return mus.pcount!=0;
}
static bool music_next(int16_t *l,int16_t *r)
{
    if(!mus.f || mus.paused){*l=*r=0;return false;}
    if(mus.ppos>=mus.pcount && !music_fill_block()){*l=*r=0;return false;}
    *l=mus.pcm[0][mus.ppos]; *r=mus.pcm[1][mus.ppos]; mus.ppos++; mus.pos++; return true;
}

bool aud_music_play(uint32_t id, bool loop)
{
    aud_music_stop();
    char name[16]; snprintf(name,sizeof name,"%08lX.ADX",(unsigned long)id); FILE *f=fopen(name,"rb"); if(!f){printf("music: %s missing\n",name);return false;}
    uint8_t h[36]; if(fread(h,1,sizeof h,f)!=sizeof h || rd16be(h)!=0x8000 || h[4]!=3 || h[5]!=18 || h[6]!=4 || h[7]!=2 || rd32be(h+8)!=MIX_RATE || rd16be(h+16)!=500){
        printf("music: %s unsupported ADX\n",name); fclose(f); return false;
    }
    memset(&mus,0,sizeof mus); mus.f=f; mus.data_off=(uint32_t)rd16be(h+2)+4u; mus.total_samples=rd32be(h+12); mus.loop=loop; mus.gain=FX_ONE;
    if(!music_rewind()){fclose(f); memset(&mus,0,sizeof mus); return false;}
    return true;
}
void aud_music_stop(void){ if(mus.f) fclose(mus.f); memset(&mus,0,sizeof mus); }
void aud_music_gain(real g){ mus.gain=gain_q16(g); }
void aud_music_pause(bool pause){ mus.paused=pause; }

static void mix_one(int16_t *ol,int16_t *or_)
{
    int32_t l=0,r=0; int16_t ml=0,mr=0;
    if(music_next(&ml,&mr)){ l+=(int32_t)(((int64_t)ml*mus.gain)>>16); r+=(int32_t)(((int64_t)mr*mus.gain)>>16); }
    for(int i=0;i<MAX_VOICES;i++){
        Voice *v=&voices[i]; if(!v->active) continue; int16_t x;
        if(!sat_adp_next(&v->dec,&x)){
            if(v->loop){ sat_adp_rewind(&v->dec); if(!sat_adp_next(&v->dec,&x)){voice_release(v);continue;} }
            else { voice_release(v); continue; }
        }
        int32_t y=(int32_t)(((int64_t)x*v->gain)>>16); l+=y; r+=y;
    }
    *ol=clip16(l); *or_=clip16(r);
}

static uint64_t nearest_ring_boundary(uint64_t ref, uint32_t ring_pos)
{
    uint64_t base = ref & ~((uint64_t)MIX_RING_SAMPLES - 1u);
    uint64_t best = base + ring_pos;
    uint64_t d_best = best > ref ? best - ref : ref - best;
    if (best >= MIX_RING_SAMPLES) {
        uint64_t c = best - MIX_RING_SAMPLES;
        uint64_t d = ref - c;
        if (d < d_best) { best = c; d_best = d; }
    }
    {
        uint64_t c = best + MIX_RING_SAMPLES;
        uint64_t d = c > ref ? c - ref : ref - c;
        if (d < d_best) best = c;
    }
    return best;
}

static uint64_t mix_played_now(void)
{
    uint32_t now = sat_timer_us();
    uint64_t free_run = ((uint64_t)(now - mix_start_us) * MIX_RATE) / 1000000u;
    uint64_t played = mix_sync_valid
        ? mix_anchor_samples + ((uint64_t)(now - mix_anchor_us) * MIX_RATE) / 1000000u
        : free_run;
    int ca = pcmsys_pcm_call_address(0);
    if (ca >= 0) {
        uint8_t half = (uint8_t)ca & 1u;
        if (!mix_ca_seen) {
            mix_ca_seen = true;
            mix_ca_half = half;
        } else if (half != mix_ca_half) {
            /* A CA transition is a hardware-observed 4096-sample boundary.
             * Snap the absolute estimate to the nearest congruent ring
             * position and restart the short-term FRT estimate there. */
            uint32_t pos = half ? (MIX_RING_SAMPLES / 2u) : 0u;
            mix_anchor_samples = nearest_ring_boundary(played, pos);
            mix_anchor_us = now;
            mix_sync_valid = true;
            mix_ca_half = half;
            played = mix_anchor_samples;
        }
    }
    return played;
}

void aud_update(void)
{
    uint64_t played=mix_played_now();
    if(played>mix_written){ mix_written=played; mix_underruns++; }
    uint64_t target=played+MIX_AHEAD_SAMPLES;
    while(mix_written<target){
        uint32_t n=(uint32_t)(target-mix_written); if(n>MIX_CHUNK)n=MIX_CHUNK;
        for(uint32_t j=0;j<n;j++){
            int16_t l,r; mix_one(&l,&r); uint32_t at=(uint32_t)(mix_written%MIX_RING_SAMPLES); ring_l[at]=l; ring_r[at]=r; mix_written++;
        }
    }
}

bool aud_init(void)
{
    if(!rsat_sound_driver_init()) return false;
    /* SCSP sound RAM must be accessed as words from the SH-2. */
    for (uint32_t i = 0; i < MIX_RING_SAMPLES; i++) {
        ring_l[i] = 0;
        ring_r[i] = 0;
    }
    pcmsys_pcm_configure(0,MIX_LEFT_OFF,MIX_RING_BYTES,MIX_RATE,16,PCM_FWD_LOOP,PCM_PAN_LEFT);
    pcmsys_pcm_configure(1,MIX_RIGHT_OFF,MIX_RING_BYTES,MIX_RATE,16,PCM_FWD_LOOP,PCM_PAN_RIGHT);
    pcmsys_pcm_start(0,PCM_MAX_VOLUME); pcmsys_pcm_start(1,PCM_MAX_VOLUME); sound_notify_driver();
    mix_start_us=sat_timer_us(); mix_written=MIX_AHEAD_SAMPLES;
    mix_ca_seen=false; mix_sync_valid=false; mix_ca_half=0; mix_anchor_us=mix_start_us; mix_anchor_samples=0;
    printf("snd: Saturn mixer 44.1k stereo, fragmented SCSP-resident SADP pool\n");
    return true;
}
void aud_shutdown(void)
{
    aud_music_stop(); for(int i=0;i<MAX_VOICES;i++) voice_release(&voices[i]); pcmsys_pcm_stop(0); pcmsys_pcm_stop(1); sound_notify_driver();
    for(int i=0;i<nsamples;i++) if(samples[i].data&&!samples[i].resident) packs_release_type(samples[i].key,RES_SAMPLE);
    printf("snd: mixer underruns %u, resident SADP %lu KB\n",mix_underruns,(unsigned long)(resident_bytes/1024));
}
