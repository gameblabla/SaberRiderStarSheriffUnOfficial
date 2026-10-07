/* V28.1 dual-SH2 + SCU A24 decoder, SCSP ADX audio, true RGB24 NBG0.
 * Use the game's bounded/retry-capable CDFS reader; no independent CD owner.
 * Finite clips never seek into a next lap while their final packets play. */
#include "../../video.h"
#include "sat_internal.h"
#include "video_vdp2_sat.h"
#include <yaul.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "svm.h"
#include "sv24_frame_v03.h"
#include "sv24_slice.h"
#include "sv24_dual.h"
#include "sv24_scu.h"
#include "sv24_audio.h"
#define QUEUE_COUNT 3u
#define STREAM_CACHE_BYTES (32u*1024u)
#define RAM_CLIP_MAX (512u*1024u)
const cdfs_filelist_entry_t *cd_sat_entry(const char *name);
void rsat_video_draw(const RFRect *dst);
struct Video {
    svm_header_t header;
    FILE *stream;
    uint8_t *ram,*queue,*dirty,*cache;
    uint32_t *index,*cells;
    uint32_t ram_size,ram_pos,packet_max,next,queued,epoch,groups_base,timeline_samples,cell_capacity,cache_pos,cache_size,cache_capacity,file_left;
    int w,h,logical_w,logical_h,pitch;
    bool done,surface,started,keep_bufs;
};
static Video *prepared_video;
static char prepared_name[16];
static uint32_t *spare_cells;
static uint8_t *spare_queue;
static uint32_t spare_capacity,spare_cells_capacity;
volatile uint32_t sat_movie_frames,sat_movie_drops,sat_movie_errors;
volatile uint32_t sat_movie_decode_us,sat_movie_present_us,sat_movie_read_us;
static void spares_release(void)
{
    free(spare_cells);spare_cells=NULL;spare_cells_capacity=0;free(spare_queue);spare_queue=NULL;spare_capacity=0;
}
static void release_buffers(Video *v)
{
    if(v->keep_bufs&&!spare_cells&&!spare_queue) {
        spare_cells=v->cells;spare_cells_capacity=v->cell_capacity;spare_queue=v->queue;spare_capacity=v->packet_max;
    } else {free(v->cells);free(v->queue);}
    v->cells=NULL;v->queue=NULL;
    free(v->index);v->index=NULL;free(v->dirty);v->dirty=NULL;
    free(v->ram);v->ram=NULL;free(v->cache);v->cache=NULL;
}
static void finish_video(Video *v)
{
    if(v->done)return;
    v->done=true;
    if(v->surface)rsat_video_hold();
    if(v->started)sv24_audio_stop();
    if(v->stream){fclose(v->stream);v->stream=NULL;cd_sat_stream_stop();}
    if(v->surface)aud_movie_end();
    release_buffers(v);
}
static void video_free(Video *v)
{
    if(v->stream)fclose(v->stream);
    release_buffers(v);free(v);
}
static bool read_bytes(Video *v,void *dst,uint32_t n)
{
    if(v->ram) {
        if(v->ram_pos>v->ram_size||n>v->ram_size-v->ram_pos)return false;
        memcpy(dst,v->ram+v->ram_pos,n);v->ram_pos+=n;return true;
    }
    /* Sector-aligned large reads amortize Get/Delete/DataEnd/EHST across
     * several packets. Reading one variable-size packet at a time costs
     * multiple firmware transactions and loses the optical drive's lead. */
    uint8_t *out=dst;
    while(n) {
        if(v->cache_pos==v->cache_size) {
            uint32_t take=v->file_left<v->cache_capacity?v->file_left:v->cache_capacity;
            if(!take||fread(v->cache,1,take,v->stream)!=take)return false;
            v->file_left-=take;v->cache_pos=0;v->cache_size=take;
        }
        uint32_t take=v->cache_size-v->cache_pos;if(take>n)take=n;
        memcpy(out,v->cache+v->cache_pos,take);v->cache_pos+=take;out+=take;n-=take;
    }
    return true;
}
static uint32_t frame_size(const Video *v,uint32_t i) {return v->index[2*i+1];}
static uint8_t *slot(Video *v,uint32_t i) {return v->queue+(i%QUEUE_COUNT)*v->packet_max;}
static uint32_t packet_samples(const svm_packet_t *p)
{
    uint32_t head=p->audio_size>=20u&&svm_be16(p->audio)==0x8000u?svm_be16(p->audio+2)+4u:0u;
    if(head>p->audio_size||(p->audio_size-head)%36u)return 0;
    return (p->audio_size-head)/36u*32u;
}
static uint32_t sample_us(const Video *v,uint32_t samples) {return (uint32_t)((uint64_t)samples*1000000u/v->header.rate);}
static bool read_packet(Video *v,uint32_t i)
{
    svm_packet_t p;
    return read_bytes(v,slot(v,i),frame_size(v,i))&&
           !svm_packet_parse(slot(v,i),frame_size(v,i),&p)&&p.frame_no==i&&p.pts==i&&
           packet_samples(&p)>0u&&packet_samples(&p)<=(8192u-64u)/(QUEUE_COUNT+1u);
}
static bool feed(Video *v,uint32_t i)
{
    svm_packet_t p;
    if(svm_packet_parse(slot(v,i),frame_size(v,i),&p))return false;
    return sv24_audio_feed_adx(p.audio,p.audio_size)==0;
}
static void mark_dirty(Video *v,const sv24_v03_frame_t *fr)
{
    for(unsigned s=0;s<fr->slice_count;s++) {
        const sv24_v03_slice_t *sl=&fr->slice[s];
        unsigned tc=svm_be16(sl->data);
        unsigned pitch=(unsigned)v->pitch;
        for(unsigned t=0;t<tc;t++)if(t%44u<pitch&&(sl->data[4+(t>>3)]&(0x80u>>(t&7))))
            v->dirty[(sl->tile_y0+t/44u)*pitch+t%44u]=1;
    }
}
static void hook(void *ud,volatile uint16_t *unused,int pitch)
{
    (void)unused;(void)pitch;Video *v=ud;
    unsigned catchup=0;
again:
    if(v->done)return;
    uint32_t elapsed=sat_timer_us()-v->epoch;
    if(v->started&&v->next==v->header.frames) {
        if(elapsed>=sample_us(v,v->timeline_samples))finish_video(v);
        return;
    }
    uint32_t i=v->next;
    uint32_t due=sample_us(v,v->timeline_samples);
    uint32_t refresh=vdp2_tvmd_tv_standard_get()==VDP2_TVMD_TV_STANDARD_PAL?20000u:16684u;
    if(v->started&&elapsed+3u*refresh<due)return;
    /* Decode at most two frames when catching up. Never produce enough
     * residual samples to overwrite the SCSP's unread 8192-sample ring. */
    uint32_t future=i+QUEUE_COUNT;
    if(v->started&&future<v->header.frames) {
        uint32_t produced=(sv24_audio_groups-v->groups_base)*32u;
        uint32_t played=(uint32_t)(((uint64_t)elapsed*v->header.rate)/1000000u);
        svm_packet_t queued;
        uint32_t add=svm_packet_parse(slot(v,i),frame_size(v,i),&queued)?2032u:packet_samples(&queued)+32u;
        if(produced>played&&produced-played+add>8192u-64u)return;
    }
    uint32_t tick=sat_timer_us();
    svm_packet_t pk;sv24_v03_frame_t fr;int r=svm_packet_parse(slot(v,i),frame_size(v,i),&pk);
    uint32_t frame_samples=r?0u:packet_samples(&pk);
    if(!r)r=sv24_v03_frame_parse(pk.video,pk.video_size,&fr);
    if(!r&&(fr.frame_no!=i||fr.pts!=i||!(fr.coding_flags&SV24_FRM3_MAP_DMA_SAFE)))r=-1;
    for(unsigned s=0;!r&&s<fr.slice_count;s++)
        if(fr.slice[s].tile_y0+fr.slice[s].tile_rows>(v->header.height+7u)/8u)r=-1;
    if(!r&&pk.dsp_size)r=sv24_scu_begin(pk.dsp,pk.dsp_size,v->cells);
    if(!r) {
        if(rsat_movie_dual_available()) {
            sv24_slice_dma_plan_t plans[SV24_V03_MAX_SLICES];
            sv24_dual_init();r=sv24_dual_decode_frame(&fr,v->cells,plans,8u);rsat_movie_decode_end();
        } else for(unsigned s=0;s<fr.slice_count&&!r;s++)r=sv24_decode_abs_slice_frame(fr.slice[s].data,fr.slice[s].size,fr.slice[s].tile_y0,fr.slice[s].tile_rows,v->cells);
        if(pk.dsp_size){int e=sv24_scu_end(v->cells);if(!r)r=e;}
    }
    if(r)goto error;
    sat_movie_decode_us=sat_timer_us()-tick;
    mark_dirty(v,&fr);
    /* Refill before the beam wait, so firmware latency uses presentation
     * slack rather than being added after the frame is already on screen.
     * This queue slot is safe to overwrite after both decoders have joined. */
    if(future<v->header.frames) {
        tick=sat_timer_us();
        if(!read_packet(v,future)||!feed(v,future))goto error;
        sat_movie_read_us=sat_timer_us()-tick;
    }
    elapsed=sat_timer_us()-v->epoch;
    v->timeline_samples+=frame_samples;
    bool late=v->started&&elapsed>sample_us(v,v->timeline_samples)&&i+1<v->header.frames;
    if(late)sat_movie_drops++;
    else {
        /* libyaul uses SCU level 0 and its completion callback for the
         * pending VDP1 list. Decode/CD may overlap drawing, but presentation
         * must not replace that callback while the list is still in flight. */
        vdp1_sync_wait();
        tick=sat_timer_us();
        r=movie_vdp2_present(v->cells,v->dirty,!v->started);if(r)goto error;
        sat_movie_present_us=sat_timer_us()-tick;
        if(!v->started) {sv24_audio_start(7u);v->epoch=sat_timer_us();v->started=true;}
    }
    v->next++;sat_movie_frames++;
    sv24_audio_tick();
    if(late&&++catchup<2u)goto again;
    return;
error:
    sat_movie_errors++;printf("video: SVM playback failed at frame %u (%d)\n",(unsigned)i,r);finish_video(v);
}
static bool activate_video(Video *v)
{
    if(v->surface)return true;
    sv24_output_tiles_x=(unsigned)v->pitch;
    sv24_output_tile_rows=(v->header.height+7u)/8u;
    if(!rsat_video_open(v->w,v->h,hook,v))return false;
    if(!movie_vdp2_open(v->w,v->h)){rsat_video_close();return false;}
    v->surface=true;aud_movie_begin(v->ram!=NULL);
    if(sv24_audio_init(NULL,0,v->header.rate,v->header.coef1,v->header.coef2)||sv24_scu_init())return false;
    v->groups_base=sv24_audio_groups;
    for(uint32_t i=0;i<v->queued;i++)if(!feed(v,i))return false;
    return true;
}
static Video *open_name(const char *name,bool activate)
{
    const cdfs_filelist_entry_t *entry=cd_sat_entry(name);
    FILE *stream=entry?fopen(name,"rb"):NULL;if(!stream){printf("video: %s missing\n",name);return NULL;}
    Video *v=lw_malloc(sizeof *v);if(!v){fclose(stream);return NULL;}memset(v,0,sizeof *v);v->stream=stream;
    const char *reason="header";
    uint8_t hdr[96];
    if(fread(hdr,1,96,stream)!=96||svm_header_parse(hdr,96,entry->size,&v->header))goto fail;
    reason="index";
    uint32_t n=v->header.frames;
    v->index=lw_malloc(n*8u);v->dirty=lw_malloc(SV24_FRAME_TILES);
    if(!v->index||!v->dirty||fseek(stream,v->header.index,SEEK_SET)||fread(v->index,8,n,stream)!=n)goto fail;
    uint32_t off=v->header.data;
    for(uint32_t i=0;i<n;i++) {
        uint8_t *p=(uint8_t *)v->index+i*8u;uint32_t at=svm_be32(p),sz=svm_be32(p+4);
        if(at!=off||sz<32u||sz>SVM_MAX_PACKET||at>entry->size||sz>entry->size-at)goto fail;
        v->index[2*i]=at;v->index[2*i+1]=sz;off+=sz;if(sz>v->packet_max)v->packet_max=sz;
    }
    if(off!=entry->size)goto fail;
    /* Preserve 4-byte alignment for every packet-slot start. */
    v->packet_max=(v->packet_max+3u)&~3u;
    reason="RGB24 staging / queue RAM";
    /* Full-height intros favor the fastest original 44-column decoder.
     * Smaller UI clips omit padding columns to fit beside resident stages. */
    v->pitch=v->header.height>=224u?44:(int)((v->header.width+7u)/8u);
    v->cell_capacity=((v->header.height+7u)/8u)*(unsigned)v->pitch*SV24_TILE_PIXELS*4u;
    if(spare_cells&&spare_cells_capacity>=v->cell_capacity)v->cells=spare_cells;
    else {free(spare_cells);v->cells=hw_memalign(32,v->cell_capacity);}
    spare_cells=NULL;spare_cells_capacity=0;
    if(spare_queue&&spare_capacity>=v->packet_max){v->queue=spare_queue;spare_queue=NULL;spare_capacity=0;}
    else {free(spare_queue);spare_queue=NULL;spare_capacity=0;v->queue=lw_memalign(32,QUEUE_COUNT*v->packet_max);}
    if(!v->cells||!v->queue)goto fail;
    memset(v->cells,0,v->cell_capacity);memset(v->dirty,0,SV24_FRAME_TILES);
    /* The briefing may stay in RAM and leave CD-DA playing. It is optional:
     * streamed playback has the same lifecycle and never requires a cartridge. */
    if(entry->size<=RAM_CLIP_MAX&&(v->ram=lw_malloc(entry->size))) {
        if(fseek(stream,0,SEEK_SET)||fread(v->ram,1,entry->size,stream)!=entry->size)goto fail;
        v->ram_size=entry->size;v->ram_pos=v->header.data;fclose(stream);v->stream=NULL;cd_sat_stream_stop();
    } else {
        reason="stream cache RAM / seek";
        v->cache_capacity=v->header.height>=224u?STREAM_CACHE_BYTES:STREAM_CACHE_BYTES/2u;
        v->cache=lw_memalign(32,v->cache_capacity);v->file_left=entry->size-v->header.data;
        if(!v->cache||fseek(stream,v->header.data,SEEK_SET))goto fail;
    }
    v->w=v->logical_w=v->header.width;v->h=v->logical_h=v->header.height;
    reason="packet prefill";
    v->queued=n<QUEUE_COUNT?n:QUEUE_COUNT;
    for(uint32_t i=0;i<v->queued;i++)if(!read_packet(v,i))goto fail;
    if(activate&&!activate_video(v)){video_close(v);return NULL;}
    printf("video: %s %dx%d RGB24, %u frames, ADX %u Hz%s\n",name,v->w,v->h,(unsigned)n,(unsigned)v->header.rate,activate?"":" (preloaded)");return v;
fail:
    printf("video: %s: failed %s\n",name,reason);video_free(v);return NULL;
}
static void movie_name(const char *path,char name[16])
{
    const char *b=strrchr(path,'/');b=b?b+1:path;int n=0;
    while(*b&&*b!='.'&&n<8)name[n++]=(char)toupper((unsigned char)*b++);strcpy(name+n,".SVM");
}
Video *video_open(Ren *r,uint32_t id)
{
    (void)r;char name[16];snprintf(name,sizeof name,"%08X.SVM",(unsigned)id);spares_release();Video *v=open_name(name,true);
    if(v&&id==0x2fe798c3u){v->logical_w=768;v->logical_h=312;}return v;
}
void video_preload_file(const char *path,real fps)
{
    (void)fps;char name[16]={0};if(path)movie_name(path,name);
    if(prepared_video&&path&&!strcmp(name,prepared_name))return;
    if(prepared_video){Video *old=prepared_video;prepared_video=NULL;prepared_name[0]=0;video_close(old);}
    if(!path){spares_release();return;}
    prepared_video=open_name(name,false);
    if(prepared_video){prepared_video->keep_bufs=true;prepared_video->logical_w=320;prepared_video->logical_h=224;snprintf(prepared_name,sizeof prepared_name,"%s",name);}
}
Video *video_open_file(Ren *r,const char *path,real fps)
{
    (void)r;(void)fps;if(!path)return NULL;char name[16];movie_name(path,name);
    if(prepared_video&&!strcmp(name,prepared_name)) {
        Video *v=prepared_video;prepared_video=NULL;prepared_name[0]=0;
        if(!activate_video(v)){video_close(v);return NULL;}return v;
    }
    if(prepared_video)video_preload_file(NULL,0);Video *v=open_name(name,true);if(v){v->keep_bufs=true;v->logical_w=320;v->logical_h=224;}return v;
}
bool video_update(Video *v,real dt) {(void)dt;return v&&!v->done;}
void video_draw_rect(Video *v,Ren *r,real x,real y,real w,real h)
{
    (void)r;if(!v||!v->surface)return;RFRect d={r_floorr(x),r_floorr(y),r_floorr(w),r_floorr(h)};rsat_video_draw(&d);
}
void video_draw(Video *v,Ren *r,int sw,int sh)
{
    if(!v)return;int w=v->logical_w,h=v->logical_h;
    if(w>sw||h>sh){if(w*sh>h*sw){h=h*sw/w;w=sw;}else{w=w*sh/h;h=sh;}}
    video_draw_rect(v,r,r_int((sw-w)/2),r_int((sh-h)/2),r_int(w),r_int(h));
}
void video_size(const Video *v,int *w,int *h){if(w)*w=v?v->logical_w:0;if(h)*h=v?v->logical_h:0;}
void video_close(Video *v)
{
    if(!v)return;finish_video(v);
    if(v->surface){rsat_video_close();spares_release();movie_vdp2_close();v->surface=false;}
    video_free(v);
}
