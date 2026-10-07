/* Exercise the actual Saturn backend's cleanup without hardware or disc I/O. */
#include <assert.h>
#include "../../../src/platform/saturn/video_sat.c"
static unsigned stops, ends, holds, closes, surfaces, stream_stops;
static uint32_t mock_us;
void vdp1_sync_wait(void) { }
volatile uint32_t sv24_audio_groups;
uint32_t sat_timer_us(void) { return mock_us; }
vdp2_tvmd_tv_standard_t vdp2_tvmd_tv_standard_get(void) { return VDP2_TVMD_TV_STANDARD_NTSC; }
void sv24_audio_reprime(void) { }
void sv24_audio_start(uint8_t volume) { (void)volume; }
void sv24_audio_tick(void) { }
int sv24_audio_feed_adx(const uint8_t *p,uint32_t size)
{
    svm_packet_t pk={.audio=p,.audio_size=size};sv24_audio_groups+=packet_samples(&pk)/32u;return 0;
}
int sv24_v03_frame_parse(const uint8_t *p,size_t size,sv24_v03_frame_t *fr)
{
    if(size!=16u)return -1;memset(fr,0,sizeof *fr);
    fr->frame_no=svm_be32(p);fr->pts=svm_be32(p+4);fr->coding_flags=SV24_FRM3_MAP_DMA_SAFE;return 0;
}
bool rsat_movie_dual_available(void) { return false; }
void rsat_movie_decode_end(void) { }
void sv24_dual_init(void) { }
int sv24_dual_decode_frame(const sv24_v03_frame_t *f,uint32_t *c,sv24_slice_dma_plan_t *p,unsigned gap) { return 0; }
int sv24_decode_abs_slice_frame(const uint8_t *p,size_t size,uint16_t y,uint8_t rows,uint32_t *c) { return 0; }
int sv24_scu_begin(const uint8_t *p,uint32_t size,uint32_t *cells) { return 0; }
int sv24_scu_end(uint32_t *cells) { return 0; }
int movie_vdp2_present(const uint32_t *c,uint8_t *dirty,bool first) { mock_us+=2000u;return 0; }
static void (*pending_start)(void *);
void rsat_video_audio_start(void (*start)(void *)) { pending_start=start; }
int rsat_video_present(const uint32_t *c,int pitch) { mock_us+=2000u;return 0; }
void sv24_audio_stop(void) { stops++; }
void aud_movie_end(void) { ends++; }
void rsat_video_hold(void) { holds++; }
void rsat_video_close(void) { closes++; }
void movie_vdp2_close(void) { surfaces++; }
void cd_sat_stream_stop(void) { stream_stops++; }
void rsat_video_draw(const RFRect *dst) { (void)dst; }
static Video *clip(bool streamed,bool keep)
{
    Video *v=calloc(1,sizeof *v);assert(v);
    v->cells=malloc(1024);v->queue=malloc(1024);v->dirty=malloc(16);v->index=malloc(16);
    v->ram=streamed?NULL:malloc(128);v->stream=streamed?tmpfile():NULL;
    assert(v->cells&&v->queue&&v->dirty&&v->index&&(streamed?v->stream!=NULL:v->ram!=NULL));
    v->w=v->logical_w=256;v->h=v->logical_h=104;v->packet_max=256;
    v->surface=v->started=true;v->keep_bufs=keep;return v;
}
static void put32(uint8_t *p,uint32_t x) {p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x;}
static void audio_clock(bool stall,bool sprite)
{
    unsigned frame_base=sat_movie_frames,drop_base=sat_movie_drops;
    Video *v=calloc(1,sizeof *v);assert(v);v->sprite=sprite;v->header.frames=4;v->header.rate=24000;
    v->header.fps_num=15;v->header.fps_den=1;v->header.height=104;v->pitch=32;
    v->packet_max=1900;v->ram=calloc(1,7600);v->ram_size=7600;
    v->queue=calloc(3,1900);v->index=calloc(8,4);v->dirty=calloc(SV24_FRAME_TILES,1);v->cells=calloc(64,4);
    unsigned samples[4]={1568,1632,1600,1600},pos=0;
    for(unsigned i=0;i<4;i++) {
        unsigned head=i==0?40:0,snd=head+samples[i]/32*36,size=48+snd;uint8_t *p=v->ram+pos;
        put32(p,0x504b5430);put32(p+4,size);put32(p+8,i);put32(p+12,i);put32(p+16,16);put32(p+20,snd);
        put32(p+32,i);put32(p+36,i);
        if(head){p[48]=0x80;p[51]=36;}
        v->index[i*2]=pos;v->index[i*2+1]=size;pos+=size;
    }
    v->ram_size=pos;v->surface=true;v->groups_base=sv24_audio_groups;
    for(unsigned i=0;i<3;i++){assert(read_packet(v,i));assert(feed(v,i));}v->audio_next=3;
    mock_us=100000;hook(v,NULL,0);
    if(sprite) {
        assert(!v->started&&pending_start==start_playback);
        mock_us+=40000;pending_start(v);pending_start=NULL;
        assert(v->epoch==mock_us&&v->started);
    }
    /* Refill overwrites packet 0's slot with packet 3: its saved sample
     * duration must survive, including the ADX header's extra bytes. */
    assert(v->started&&v->next==1&&v->timeline_samples==1568);
    if(stall) {
        uint32_t underruns=sat_movie_audio_underruns;
        mock_us=v->epoch+400000;hook(v,NULL,0);
        assert(v->next==2&&v->audio_origin_samples==1568&&v->started);
        assert(sat_movie_audio_underruns==underruns+1);
        mock_us=v->epoch+70000;hook(v,NULL,0);
        assert(sat_movie_audio_underruns==underruns+1&&!v->done);
        video_close(v);puts("PASS: starvation re-primes remaining packets without repeating underruns");return;
    }
    mock_us=v->epoch+66000;hook(v,NULL,0);assert(v->next==2&&v->timeline_samples==3200);
    mock_us=v->epoch+210000;hook(v,NULL,0);assert(v->next==4&&v->timeline_samples==6400);
    assert(sat_movie_frames==frame_base+4&&sat_movie_drops==drop_base+1&&!sat_movie_errors);
    mock_us=v->epoch+266665;hook(v,NULL,0);assert(!v->done);
    mock_us=v->epoch+266666;hook(v,NULL,0);assert(v->done);video_close(v);
    puts("PASS: ADX sample clock, overwritten packet-slot duration, two-frame catch-up, audio drain at EOF");
}
int main(void)
{
    for(int streamed=0;streamed<2;streamed++)for(int keep=0;keep<2;keep++) {
        Video *v=clip(streamed,keep);uint32_t *cells=v->cells;uint8_t *queue=v->queue;
        unsigned a=stops,e=ends,h=holds,c=closes,s=stream_stops,u=surfaces;
        finish_video(v);
        assert(v->done&&v->surface&&!v->cells&&!v->queue&&!v->dirty&&!v->index&&!v->ram&&!v->stream);
        assert(stops==a+1&&ends==e+1&&holds==h+1&&closes==c&&surfaces==u&&stream_stops==s+streamed);
        assert(!video_update(v,R(1)));int w,hgt;video_size(v,&w,&hgt);assert(w==256&&hgt==104);
        video_draw_rect(v,NULL,0,0,R(256),R(104));
        if(keep)assert(spare_cells==cells&&spare_queue==queue&&spare_capacity==256);
        finish_video(v);video_close(v);
        assert(stops==a+1&&ends==e+1&&holds==h+1&&closes==c+1&&surfaces==u+1);
        spares_release();assert(!spare_cells&&!spare_queue&&!spare_capacity);
    }
    Video *v=clip(true,false);unsigned e=ends;video_close(v);assert(ends==e+1);
    v=clip(false,true);v->surface=v->started=false;e=ends;unsigned a=stops;
    video_close(v);assert(ends==e&&stops==a);spares_release();
    puts("PASS: end/error/skip cleanup, RAM/stream, retained surface, reusable buffers, close after end, unused preload");
    audio_clock(false,false);audio_clock(true,false);audio_clock(false,true);
    Video rate={0};rate.header.rate=24000;playback_clock(&rate);
    assert(sample_us(&rate,24000)==1000500u);
    assert(played_samples(&rate,1000000)==23987u);
    rate.audio_origin_samples=1568;assert(ring_played(&rate,sample_us(&rate,1568))==0);
    v=clip(false,false);v->sprite=true;unsigned u=surfaces;video_close(v);assert(surfaces==u);
    puts("PASS: quantized SCSP pitch clock and VDP1 surface closes without VDP2 restoration");
}
