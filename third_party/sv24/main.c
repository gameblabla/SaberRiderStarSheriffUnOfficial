#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <yaul.h>
#include "sv24_slice.h"
#include "sv24_frame_v03.h"
#include "sv24_vdp2_layout.h"
#include "sv24_dual.h"
#include "sv24_dma.h"
#include "sv24_audio.h"
#include "sv24_scu.h"

#ifndef SV24_SPARSE_GAP
#define SV24_SPARSE_GAP 8u
#endif

#ifndef SV24_LOOP
#define SV24_LOOP 0
#endif
#define MAX_FRAMES 2048u
#define MAX_FRAME_BYTES 65536u
#define MAX_PACKET_BYTES 70000u
#define PACKET_QUEUE_COUNT 10u
#define PACKET_QUEUE_BASE 0x40000u
#define VRAM_BASE 0x25E00000u
#define STREAM_CACHE_BYTES (256u * 1024u)
#define STREAM_RING_BYTES STREAM_CACHE_BYTES

volatile uint32_t sv24_status = 0x10000000u;
volatile uint32_t sv24_frame_no = 0;
volatile uint32_t sv24_frames_decoded = 0;
volatile uint32_t sv24_cd_errors = 0;
volatile uint32_t sv24_decode_errors = 0;
volatile uint32_t sv24_max_frame_bytes = 0;
volatile uint32_t sv24_deadline_misses = 0;
volatile uint32_t sv24_stage = 0;
volatile uint32_t sv24_vblanks = 0;
volatile uint32_t sv24_beam_success = 0;
volatile uint32_t sv24_miss_code = 0;
volatile uint32_t sv24_miss_vc = 0;
volatile uint32_t sv24_miss_delta = 0;
volatile uint32_t sv24_pace_waits = 0;
volatile uint32_t sv24_pace_late = 0;
volatile uint32_t sv24_pace_target = 0;
volatile uint32_t sv24_video_drops = 0;
volatile uint32_t sv24_full_resyncs = 0;
volatile uint32_t sv24_line_late = 0;
volatile uint32_t sv24_recovery_sparse = 0;
volatile uint32_t sv24_recovery_full = 0;
volatile uint32_t sv24_recovery_dirty_peak = 0;
volatile uint32_t sv24_cd_nonblock_batches = 0;
volatile uint32_t sv24_scu_slack_hist[8] = {0};

static uint32_t staging[SV24_FRAME_TILES * SV24_TILE_PIXELS] __aligned(32);
static sv24_slice_dma_plan_t dma_plans[SV24_V03_MAX_SLICES] __aligned(32);
static uint8_t recovery_dirty[SV24_FRAME_TILES] __aligned(32);
static uint8_t framebuf[MAX_FRAME_BYTES] __aligned(32);
static sector_buffer_t secbuf __aligned(32);
static uint8_t *const stream_cache = (uint8_t *)LWRAM(0);
static uint8_t *const packet_queue = (uint8_t *)LWRAM(PACKET_QUEUE_BASE);
static uint32_t stream_cache_off = 0;
static uint32_t stream_cache_len = 0;
volatile uint32_t sv24_cd_batches = 0;
volatile uint32_t sv24_cd_cache_hits = 0;
volatile uint32_t sv24_cd_stream_sectors = 0;
volatile uint32_t sv24_cd_stream_waits = 0;
static uint32_t stream_rpos = 0, stream_wpos = 0, stream_used = 0;
static uint32_t stream_loop_pad = 0;
static uint32_t stream_sector_count = 0;
static uint32_t stream_lap_sectors = 0;
static fad_t stream_start_fad = 0;
volatile uint32_t sv24_stream_loops = 0;
volatile uint32_t sv24_cd_rewinds = 0;
volatile uint32_t sv24_stream_pad_bytes = 0;
static uint32_t frame_off[MAX_FRAMES];
static uint32_t frame_size[MAX_FRAMES];
static cdfs_filelist_t filelist;
static uint32_t pace_origin,pace_samples,pace_frame_samples,pace_rate;
static uint32_t pace_clock,pace_field_clocks;

static uint16_t be16(const uint8_t *p){ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint32_t be32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

static void cycp_write_bank(unsigned bank, uint32_t raw){
    volatile vdp2_ioregs_t *io=(volatile vdp2_ioregs_t *)VDP2_IOREG_BASE;
    volatile uint16_t *r=&io->cyca0l + bank*2u;
    r[0]=(uint16_t)(raw>>16); r[1]=(uint16_t)raw;
}

/* 16.77M-color NBG0 needs all eight character-fetch slots in whichever
   VRAM bank is under the beam.  On real Saturn hardware CPU slots must match
   across both halves of a VRAM chip (A0/A1 together and B0/B1 together).
   B1 holds the immutable pattern-name page at T0, so it is CPU-accessible
   only when B0 is also CPU-accessible. */
static void cycle_all_cg(void){
    cycp_write_bank(0,0x44444444u); /* A0: NBG0 character fetch T0..T7 */
    cycp_write_bank(1,0x44444444u); /* A1: NBG0 character fetch T0..T7 */
    cycp_write_bank(2,0x44444444u); /* B0: NBG0 character fetch T0..T7 */
    cycp_write_bank(3,0x0FFFFFFFu); /* B1: PND at T0, no CPU mismatch */
}

/* Bottom 80 lines are being drawn from B0.  A0/A1 are both hidden and may
   be updated as one legal VRAM-A CPU window. */
static void cycle_b_visible_a_cpu(void){
    cycp_write_bank(0,0xFEEEEEEEu);
    cycp_write_bank(1,0xFEEEEEEEu);
    cycp_write_bank(2,0x44444444u);
    cycp_write_bank(3,0x0FFFFFFFu);
}

/* Top/middle 160 lines are being drawn from A0/A1.  B0 is hidden.  T0 on
   B0 is deliberately no-access because B1 must spend T0 on the PND read;
   T1..T7 are matched CPU slots on both halves of VRAM-B. */
static void cycle_a_visible_b_cpu(void){
    cycp_write_bank(0,0x44444444u);
    cycp_write_bank(1,0x44444444u);
    cycp_write_bank(2,0xFFEEEEEEu);
    cycp_write_bank(3,0x0FEEEEEEu);
}

/* Display-off setup: CPU writes may use all image banks.  Keep B1's T0 for
   the PND entry while it exists; during vertical blank this is conservative
   and also leaves the same legal B0/B1 CPU pairing used during playback. */
static void cycle_image_cpu(void){
    cycp_write_bank(0,0xFEEEEEEEu);
    cycp_write_bank(1,0xFEEEEEEEu);
    cycp_write_bank(2,0xFEEEEEEEu);
    cycp_write_bank(3,0xFEEEEEEEu);
}

static int hblank_after_line_wait(uint16_t line){
    volatile vdp2_ioregs_t *io=(volatile vdp2_ioregs_t *)VDP2_IOREG_BASE;
    uint16_t vc;

    /* If called from VBLANK, wait for the requested line in the coming
       active field.  If already in active display, never wrap silently to
       the next field: that would turn a missed deadline into a torn frame. */
    if(vdp2_tvmd_vblank_in()){
        vdp2_tvmd_vcount_wait(line);
    }else{
        vdp2_tvmd_extern_latch();
        vc=vdp2_tvmd_vcount_get();
        if(vc<line)vdp2_tvmd_vcount_wait(line);
        else if(vc>line)return 1;
    }

    while((io->tvstat & 0x0004u)==0u){
        vdp2_tvmd_extern_latch();
        vc=vdp2_tvmd_vcount_get();
        if(vc!=line)return 1;
    }
    return 0;
}


static int hblank_exit_wait(void){
    volatile vdp2_ioregs_t *io=(volatile vdp2_ioregs_t *)VDP2_IOREG_BASE;
    /* Do not rewrite cycle patterns while HBLANK is asserted.  Mednafen's
       scanline renderer can expose a one-line hole when a bank is removed
       from the fetch schedule inside that HBLANK.  Waiting for the next
       active phase costs only the horizontal blanking interval, not a full
       scanline, and is conservative for VDP2 prefetch timing as well. */
    while(io->tvstat & 0x0004u){}
    return 0;
}

static void display_on_direct(void){ volatile vdp2_ioregs_t *io=(volatile vdp2_ioregs_t *)VDP2_IOREG_BASE; io->tvmd |= 0x8000u; }
static void display_off_direct(void){ volatile vdp2_ioregs_t *io=(volatile vdp2_ioregs_t *)VDP2_IOREG_BASE; io->tvmd &= 0x7FFFu; }

static void vblank_count_cb(void *work){ (void)work; sv24_vblanks++; }

/* Presentation follows the sample count in each interleaved ADX packet.
 * Decode/CD work happens before the wait on the appropriate PAL/NTSC field. */
static void pace_set_target(void){
    uint64_t divisor=(uint64_t)pace_rate*pace_field_clocks;
    uint32_t fields=(uint32_t)(((uint64_t)pace_samples*pace_clock+divisor/2u)/divisor);
    sv24_pace_target=pace_origin+fields-1u;
}
static void pace_arm_after_first(void){
    bool pal=vdp2_tvmd_tv_standard_get()==VDP2_TVMD_TV_STANDARD_PAL;
    /* ST-013-R3: wide clock 28.4375 MHz PAL, 28.6364 MHz NTSC.
     * Use consumed ADX samples, including the released muxer's NTSC stretch,
     * rather than assuming that every video frame lasts four refreshes. */
    pace_clock=pal?28437500u:28636400u;
    pace_field_clocks=pal?455u*625u*2u:455u*525u*2u;
    pace_origin=sv24_vblanks;pace_samples=pace_frame_samples;pace_set_target();
}

/* Audio is the master clock. A decoded frame owns its ADX sample interval.  If decode/planning finishes after that slot has already begun, do
   not make the entire movie clock late by presenting it anyway: drop only
   its presentation, advance the timeline, and immediately decode the next
   frame.  The next frame that is actually presented is a full-band resync,
   because VDP2 still contains the last displayed frame. */
static int pace_is_late(void){
    int32_t d=(int32_t)(sv24_vblanks-sv24_pace_target);
    uint16_t vc;
    if(d<0)return 0;
    if(d>1)return 1;
    if(d==0)return 0; /* present_beamrace may use a one-refresh elastic slip */
    if(vdp2_tvmd_vblank_in())return 0;
    vdp2_tvmd_extern_latch();vc=vdp2_tvmd_vcount_get();
    return vc>167u;
}

static unsigned recovery_mark_frame(const sv24_v03_frame_t *fr)
{
    unsigned si,added=0u;
    if(!fr)return 0u;
    for(si=0u;si<fr->slice_count;si++){
        const sv24_v03_slice_t *sl=&fr->slice[si];
        unsigned tc=((unsigned)sl->data[0]<<8)|sl->data[1];
        unsigned mb=(tc+7u)>>3;
        const uint8_t *map=sl->data+4u;
        unsigned ti;
        if(sl->size<4u+mb)continue;
        for(ti=0u;ti<tc;ti++){
            if(map[ti>>3]&(uint8_t)(0x80u>>(ti&7u))){
                unsigned id=(unsigned)sl->tile_y0*SV24_TILES_X+ti;
                if(id<SV24_FRAME_TILES && !recovery_dirty[id]){recovery_dirty[id]=1u;added++;}
            }
        }
    }
    {
        unsigned i,n=0u;
        for(i=0u;i<SV24_FRAME_TILES;i++)n+=(recovery_dirty[i]!=0u);
        if(n>sv24_recovery_dirty_peak)sv24_recovery_dirty_peak=n;
    }
    return added;
}


static unsigned recovery_mark_plans(const sv24_slice_dma_plan_t *plans, unsigned plan_count)
{
    unsigned si, added = 0u;
    if (!plans) return 0u;
    for (si = 0u; si < plan_count; si++) {
        const sv24_slice_dma_plan_t *pl = &plans[si];
        unsigned ry;
        for (ry = 0u; ry < pl->rows && ry < 3u; ry++) {
            uint32_t lo = pl->row_lo[ry];
            uint32_t hi = pl->row_hi[ry] & 0x0fffu;
            unsigned x;
            for (x = 0u; x < 32u; x++) if (lo & (1u << x)) {
                unsigned id = ((unsigned)pl->y0 + ry) * SV24_TILES_X + x;
                if (id < SV24_FRAME_TILES && !recovery_dirty[id]) { recovery_dirty[id] = 1u; added++; }
            }
            for (x = 0u; x < 12u; x++) if (hi & (1u << x)) {
                unsigned id = ((unsigned)pl->y0 + ry) * SV24_TILES_X + 32u + x;
                if (id < SV24_FRAME_TILES && !recovery_dirty[id]) { recovery_dirty[id] = 1u; added++; }
            }
        }
    }
    {
        unsigned i, n = 0u;
        for (i = 0u; i < SV24_FRAME_TILES; i++) n += (recovery_dirty[i] != 0u);
        if (n > sv24_recovery_dirty_peak) sv24_recovery_dirty_peak = n;
    }
    return added;
}

static unsigned recovery_mark_current(const sv24_v03_frame_t *fr,
                                      const sv24_slice_dma_plan_t *plans)
{
    if (fr && (fr->coding_flags & SV24_FRM3_MAP_DMA_SAFE))
        return recovery_mark_frame(fr);
    return recovery_mark_plans(plans, fr ? fr->slice_count : 0u);
}

static void recovery_clear(void){memset(recovery_dirty,0,sizeof(recovery_dirty));}

static void pace_wait_target(void){
    while((int32_t)(sv24_vblanks-sv24_pace_target)<0){
        /* vblank_in_wait() returns immediately if we are already inside the
           same blanking interval.  Leave it first so each loop waits for one
           *new* VBLANK-IN instead of spinning thousands of times. */
        if(vdp2_tvmd_vblank_in())vdp2_tvmd_vblank_out_wait();
        vdp2_tvmd_vblank_in_wait();
        sv24_pace_waits++;
    }
}

static void pace_advance_slot(void){
    pace_samples+=pace_frame_samples;pace_set_target();
}

static void renderer_init(void){
    const vdp2_vram_ctl_t ctl={.coeff_table=VDP2_VRAM_CTL_COEFF_TABLE_VRAM,.vram_mode=VDP2_VRAM_CTL_MODE_PART_BANK_BOTH};
    const vdp2_vram_usage_t usage={.a0=VDP2_VRAM_USAGE_TYPE_CPD_BPD,.a1=VDP2_VRAM_USAGE_TYPE_CPD_BPD,.b0=VDP2_VRAM_USAGE_TYPE_CPD_BPD,.b1=VDP2_VRAM_USAGE_TYPE_PND};
    const vdp2_scrn_cell_format_t fmt={.scroll_screen=VDP2_SCRN_NBG0,.ccc=VDP2_SCRN_CCC_RGB_16770000,.char_size=VDP2_SCRN_CHAR_SIZE_1X1,.pnd_size=2,.aux_mode=VDP2_SCRN_AUX_MODE_0,.plane_size=VDP2_SCRN_PLANE_SIZE_1X1,.cpd_base=VDP2_VRAM_ADDR(0,0),.palette_base=0};
    const vdp2_scrn_normal_map_t map={.base_addr={VDP2_VRAM_ADDR(3,0),VDP2_VRAM_ADDR(3,0),VDP2_VRAM_ADDR(3,0),VDP2_VRAM_ADDR(3,0)}};
    vdp2_tvmd_display_clear();
    vdp2_vram_control_set(&ctl); vdp2_vram_usage_set(&usage);
    cycle_image_cpu();
    sv24_vdp2_build_pnd_page((uint16_t*)VDP2_VRAM_ADDR(3,0));
    vdp2_scrn_cell_format_set(&fmt,&map);
    vdp2_scrn_sf_set(VDP2_SCRN_NBG0,0,VDP2_SCRN_SF_TYPE_NONE,VDP2_SCRN_SF_CODE_A);
    vdp2_scrn_priority_set(VDP2_SCRN_NBG0,3);
    vdp2_scrn_scroll_x_set(VDP2_SCRN_NBG0,0); vdp2_scrn_scroll_y_set(VDP2_SCRN_NBG0,0);
    vdp2_scrn_display_set(VDP2_SCRN_DISPTP_NBG0);
    vdp2_tvmd_display_res_set(VDP2_TVMD_INTERLACE_NONE,VDP2_TVMD_HORZ_NORMAL_B,VDP2_TVMD_VERT_240);
    vdp2_sync();
    vdp2_sync_wait();
    vdp_sync_vblank_in_set(vblank_count_cb,NULL);
}

static cdfs_filelist_entry_t *find_file(const char *name){
    uint32_t i;
    for(i=0;i<filelist.entries_count;i++) if(strcmp(filelist.entries[i].name,name)==0) return &filelist.entries[i];
    return 0;
}

static int file_read_at(const cdfs_filelist_entry_t *e, uint32_t off, void *dstv, uint32_t len){
    uint32_t aligned, remain, rdlen;
    int r;
    if(!e || off > e->size || len > e->size-off) return -1;
    if(len==0u) return 0;
    if(stream_cache_len && off>=stream_cache_off && off+len<=stream_cache_off+stream_cache_len){
        memcpy(dstv,stream_cache+(off-stream_cache_off),len);
        sv24_cd_cache_hits++;
        return 0;
    }
    aligned=off&~2047u;
    remain=e->size-aligned;
    rdlen=remain>STREAM_CACHE_BYTES?STREAM_CACHE_BYTES:remain;
    rdlen=(rdlen+2047u)&~2047u;
    if(rdlen>STREAM_CACHE_BYTES)rdlen=STREAM_CACHE_BYTES;
    r=cd_block_sectors_read(e->starting_fad+(aligned>>11),stream_cache,rdlen);
    if(r!=0){sv24_cd_errors++; return -2;}
    sv24_cd_batches++;
    stream_cache_off=aligned;
    stream_cache_len=rdlen;
    if(off+len>stream_cache_off+stream_cache_len)return -3;
    memcpy(dstv,stream_cache+(off-stream_cache_off),len);
    return 0;
}


/* CDC 3.2-3.4: CMOK is command acceptance; ESEL/EHST finish later.
 * Read the status HIGH byte. FAD's low byte is not a drive status. */
#define CDREG(o) (*(volatile uint16_t *)(0x25890000u+(o)))
static int cd_wait(uint16_t flags)
{
    uint16_t last=cpu_frt_count_get();uint32_t ticks=0;
    while((CDREG(8)&flags)!=flags){uint16_t now=cpu_frt_count_get();ticks+=(uint16_t)(now-last);last=now;if(ticks>450000u)return -60;}
    return 0;
}
static int cd_command(uint16_t flags,uint16_t c1,uint16_t c2,uint16_t c3,uint16_t c4,uint16_t *response)
{
    uint16_t st[4];static uint16_t last_command;
    for(unsigned retry=0;retry<64u;retry++){
        if(cd_wait(1u))return -61;
        /* CDC 3.2: space writes by at least 300 us at /128, wide clock. */
        while((uint16_t)(cpu_frt_count_get()-last_command)<70u){}
        uint8_t sr=cpu_intc_mask_get();cpu_intc_mask_set(15);
        last_command=cpu_frt_count_get();
        CDREG(8)=(uint16_t)(0x3fffu & ~(1u|flags));
        CDREG(0x18)=c1;CDREG(0x1c)=c2;CDREG(0x20)=c3;CDREG(0x24)=c4;
        cpu_intc_mask_set(sr);
        if(cd_wait(1u))return -62;
        st[0]=CDREG(0x18);st[1]=CDREG(0x1c);st[2]=CDREG(0x20);st[3]=CDREG(0x24);
        uint8_t status=(uint8_t)(st[0]>>8);
        if(status==0xffu)return -63;
        if(status&0x80u){
            uint16_t wait=cpu_frt_count_get();
            while((uint16_t)(cpu_frt_count_get()-wait)<3800u){}
            continue;
        }
        unsigned state=status&15u;if(state==6u||state==7u||state==9u||state==10u)return -64;
        if(flags&&cd_wait(flags))return -65;
        if(response)memcpy(response,st,sizeof st);return 0;
    }
    return -66;
}
static int cd_transfer(uint8_t *dst,uint32_t sectors)
{
    /* Get+delete acknowledges before the data port is ready. Always finish
     * an accepted transfer before touching selector/partition state. */
    int r=cd_command(0,0x6300,0,0,(uint16_t)sectors,NULL);if(r)return r;
    r=cd_wait(2u);
    if(!r){uint32_t *p=(uint32_t *)dst;for(uint32_t i=0;i<sectors*512u;i++)*p++=*(volatile uint32_t *)0x25810000u;}
    uint16_t response[4];
    int end=cd_command(0,0x0600,0,0,0,response);if(!end)end=cd_wait(0x80u);
    if(!end && (((uint32_t)(response[0]&255u)<<16)|response[1])!=sectors*1024u)end=-69;
    return r?r:end;
}

static int movie_cd_rewind(void)
{
    int r;
    /* The software ring is deliberately NOT reset here.  It already contains
       the tail of the old lap, which buys the optical pickup time to seek. */
    r=cd_command(0x40u,0x3000,0,0xff00,0,NULL);if(r)return r;
    r=cd_command(0x40u,0x4800,0,0,0,NULL);if(r)return r;
    r=cd_command(0x40u,(uint16_t)(0x4000|(stream_start_fad>>16)),(uint16_t)stream_start_fad,(uint16_t)(stream_sector_count>>16),(uint16_t)stream_sector_count,NULL);if(r)return r;
    r=cd_command(0x40u,0x4440,0,0,0,NULL);if(r)return r;
    r=cd_command(0x40u,0x4603,0xff,0,0,NULL);if(r)return r;
    r=cd_command(0x40u,0x3000,0,0,0,NULL);if(r)return r;
    r=cd_command(0,(uint16_t)(0x1080|(stream_start_fad>>16)),(uint16_t)stream_start_fad,(uint16_t)(0x80|(stream_sector_count>>16)),(uint16_t)stream_sector_count,NULL);if(r)return r;
    stream_lap_sectors = 0;
    sv24_cd_rewinds++;
    return 0;
}

static int movie_stream_start(const cdfs_filelist_entry_t *e, uint32_t byte_off)
{
    uint32_t payload;
    int r;
    if (!e || (byte_off & 2047u) || byte_off >= e->size) return -30;
    payload = e->size - byte_off;
    stream_sector_count = (payload + 2047u) >> 11;
    stream_loop_pad = (stream_sector_count << 11) - payload;
    stream_start_fad = e->starting_fad + (byte_off >> 11);
    stream_rpos = stream_wpos = stream_used = 0;
    stream_lap_sectors = 0;
    r = cd_command(0,0x6000,0,0,0,NULL);if(r)return r;
    return movie_cd_rewind();
}

static int movie_stream_pump(uint32_t want_used, int wait)
{
    uint32_t idle_ticks=0;uint16_t last_tick=cpu_frt_count_get();
    while (stream_used < want_used) {
        if(stream_lap_sectors==stream_sector_count){
#if SV24_LOOP
            int rr=movie_cd_rewind();if(rr)return rr;
#else
            return !wait||stream_used>=want_used?0:-67;
#endif
        }
        uint16_t response[4];
        int ready=cd_command(0,0x5100,0,0,0,response);if(ready)return ready;ready=response[3];
        uint32_t free_bytes, free_secs, contig_secs, nsec;
        int r;
        if (ready <= 0) {
            if (!wait) return 0;
            sv24_cd_stream_waits++;
            uint16_t t=cpu_frt_count_get();idle_ticks+=(uint16_t)(t-last_tick);last_tick=t;
            if(idle_ticks>1120000u)return -68;
            continue;
        }
        free_bytes = STREAM_RING_BYTES - stream_used;
        free_secs = free_bytes >> 11;
        if (!free_secs) return 0;
        if (stream_wpos == STREAM_RING_BYTES) stream_wpos = 0;
        contig_secs = (STREAM_RING_BYTES - stream_wpos) >> 11;
        nsec = (uint32_t)ready;
        if (nsec > free_secs) nsec = free_secs;
        if (nsec > contig_secs) nsec = contig_secs;
        if (nsec > 32u) nsec = 32u;
        if(nsec>stream_sector_count-stream_lap_sectors)nsec=stream_sector_count-stream_lap_sectors;
        if (!nsec) return 0;
        r=cd_command(0,0x5400,0,0,0,response);if(r)return r;
        uint32_t fad=((uint32_t)(response[0]&255u)<<16)|response[1];
        if(fad!=stream_start_fad+stream_lap_sectors){
            r=cd_command(0x80u,0x6200,0,0,1,NULL);if(r)return r;continue;
        }
        CDREG(8)=(uint16_t)(0x3fffu & ~(2u|0x80u));
        r = cd_transfer(stream_cache+stream_wpos,nsec);
        if (r) { sv24_cd_errors++; return -35; }
        stream_wpos += nsec << 11;
        if (stream_wpos == STREAM_RING_BYTES) stream_wpos = 0;
        stream_used += nsec << 11;
        sv24_cd_stream_sectors += nsec;
        stream_lap_sectors += nsec;
        sv24_cd_batches++;
        /* Once every physical sector of this lap is safely in the software
           ring, start the pickup seeking back immediately.  Playback keeps
           consuming the buffered tail while the new beginning sectors arrive. */
        if (SV24_LOOP && stream_lap_sectors == stream_sector_count) {
            r = movie_cd_rewind();
            if (r) { sv24_cd_errors++; return -37; }
        }
        /* wait==0 means a time-bounded top-up, not "keep copying until the
           high-water mark is full".  One <=64 KiB CD-block batch per video
           frame is already several times the average stream consumption and
           prevents a burst of ready sectors from stealing the next deadline. */
        if(!wait){sv24_cd_nonblock_batches++;return 0;}
    }
    return 0;
}


static int movie_stream_skip(uint32_t len)
{
    int r;
    if (!len) return 0;
    r = movie_stream_pump(len, 1); if (r) return r;
    stream_rpos += len;
    while (stream_rpos >= STREAM_RING_BYTES) stream_rpos -= STREAM_RING_BYTES;
    stream_used -= len;
    sv24_stream_pad_bytes += len;
    return 0;
}

static int movie_stream_read(void *dstv, uint32_t len)
{
    uint8_t *dst = (uint8_t *)dstv;
    uint32_t first;
    int r;
    if (len > STREAM_RING_BYTES) return -36;
    r = movie_stream_pump(len, 1); if (r) return r;
    first = STREAM_RING_BYTES - stream_rpos;
    if (first > len) first = len;
    memcpy(dst, stream_cache + stream_rpos, first);
    if (first < len) memcpy(dst + first, stream_cache, len - first);
    stream_rpos += len;
    if (stream_rpos >= STREAM_RING_BYTES) stream_rpos -= STREAM_RING_BYTES;
    stream_used -= len;
    return 0;
}


typedef struct {
    const uint8_t *video;
    uint32_t video_size;
    const uint8_t *audio;
    uint32_t audio_size;
    const uint8_t *dsp;
    uint32_t dsp_size;
    uint32_t frame_no;
} svm_packet_t;

static int svm_packet_parse(const uint8_t *p, uint32_t size, svm_packet_t *o)
{
    uint32_t vs, as, ds;
    if (!p || !o || size < 32u || be32(p) != 0x504B5430u) return -40;
    if (be32(p + 4) != size) return -41;
    vs = be32(p + 16); as = be32(p + 20); ds = be32(p + 28);
    uint32_t left=size-32u;
    if(ds>left)return -42;left-=ds;
    if(vs>MAX_FRAME_BYTES || vs>left)return -42;left-=vs;
    if(as!=left)return -42;
    o->frame_no = be32(p + 8);
    o->dsp = ds ? p + 32 : 0;
    o->dsp_size = ds;
    o->video = p + 32 + ds;
    o->video_size = vs;
    o->audio = p + 32 + ds + vs;
    o->audio_size = as;
    return 0;
}

static uint8_t *packet_slot(unsigned i)
{
    return packet_queue + (i % PACKET_QUEUE_COUNT) * MAX_PACKET_BYTES;
}

static int upload_changed(const sv24_v03_frame_t *fr){
    (void)fr; return -90;
}

static int upload_changed_band(const sv24_v03_frame_t *fr, unsigned band){
    (void)fr; (void)band; return -90;
}

static int upload_full_band(unsigned band){
    return sv24_dma_upload_band_full(staging,band);
}

static int upload_coalesced_band(const sv24_v03_frame_t *fr, unsigned band){
    (void)fr;
    return sv24_dma_upload_prepared_band(band);
}

static int present_first(const sv24_v03_frame_t *fr){
    int r; (void)fr;
    sv24_stage=0x301u; vdp2_tvmd_vblank_in_wait(); sv24_stage=0x302u; display_off_direct(); cycle_image_cpu();
    sv24_stage=0x303u;
    r=upload_full_band(0); if(!r)r=upload_full_band(1); if(!r)r=upload_full_band(2);
    sv24_stage=0x304u; cycle_all_cg(); display_on_direct(); sv24_stage=0x305u; return r;
}

static int present_beamrace(const sv24_v03_frame_t *fr, int full){
    int r;
    uint16_t vc;
    uint32_t epoch;
    (void)fr;

    /* One 60-Hz refresh is used as a hardware-safe transfer window.

       Old refresh, lines 0..159: all three image banks are fetched normally.
       At HBLANK after line 159 the A chip is no longer needed, so A0+A1 are
       changed together to CPU slots and receive the next frame.

       VBLANK-IN: A is now coherent.  Switch A back to fetches and make B0/B1
       a matched CPU pair.  B0 is updated during VBLANK plus the next frame's
       top/middle 160 lines.

       New refresh, HBLANK after line 159: B0 is coherent, so restore the
       steady all-fetch pattern before the bottom 80 lines begin.  The entire
       refresh therefore displays one coherent 24-bit frame without CRAM. */
    sv24_stage=0x401u;
    if(pace_is_late())return 1;
    if(hblank_after_line_wait(167u)){
        /* Decode/planning occasionally lands in the bottom 80 lines of the
           nominal transition refresh.  Rather than drop a 15-Hz picture,
           slip this transition by one 60-Hz refresh; the following picture
           then naturally occupies three refreshes and catches the clock up. */
        if(sv24_vblanks==sv24_pace_target && !vdp2_tvmd_vblank_in()){
            vdp2_tvmd_vblank_in_wait();
            if(hblank_after_line_wait(167u)){
                sv24_miss_code=10u;sv24_miss_vc=vdp2_tvmd_vcount_get();return 1;
            }
            sv24_line_late++;
        }else{
            sv24_miss_code=10u;sv24_miss_vc=vdp2_tvmd_vcount_get();return 1;
        }
    }
    epoch=sv24_vblanks;
    if(epoch!=sv24_pace_target && epoch!=sv24_pace_target+1u){
        sv24_miss_code=10u;sv24_miss_vc=vdp2_tvmd_vcount_get();return 1;
    }
    if(epoch==sv24_pace_target+1u)sv24_line_late++;

    sv24_stage=0x402u;
    hblank_exit_wait();
    cycle_b_visible_a_cpu();
    r=full?upload_full_band(0):upload_coalesced_band(fr,0); if(r)return r;
    r=full?upload_full_band(1):upload_coalesced_band(fr,1); if(r)return r;
    sv24_stage=0x403u;

    /* A0+A1 must finish before the next active scan starts. */
    if(sv24_vblanks==epoch){
        vdp2_tvmd_vblank_in_wait();
    }else if(!(sv24_vblanks==epoch+1u && vdp2_tvmd_vblank_in())){
        sv24_miss_code=11u;sv24_miss_vc=vdp2_tvmd_vcount_get();
        sv24_miss_delta=sv24_vblanks-epoch;sv24_deadline_misses++;
        goto hard_resync;
    }
    if(sv24_vblanks!=epoch+1u || !vdp2_tvmd_vblank_in()){
        sv24_miss_code=12u;sv24_miss_vc=vdp2_tvmd_vcount_get();
        sv24_miss_delta=sv24_vblanks-epoch;sv24_deadline_misses++;
        goto hard_resync;
    }

    /* New frame begins with the already-updated A chip.  B0 is hidden until
       line 160 and can be updated through matched B0/B1 CPU slots. */
    sv24_stage=0x404u;
    cycle_a_visible_b_cpu();
    r=full?upload_full_band(2):upload_coalesced_band(fr,2); if(r)return r;
    sv24_stage=0x405u;

    /* Once B DMA is complete, restore its fetch slots immediately, rather
     * than toggling them beside the prefetch boundary at line 152. */
    vdp2_tvmd_extern_latch();vc=vdp2_tvmd_vcount_get();
    if(!vdp2_tvmd_vblank_in() && vc>151u){
        sv24_miss_code=13u;sv24_miss_vc=vc;
        sv24_deadline_misses++;goto hard_resync;
    }
    if(sv24_vblanks!=epoch+1u){
        sv24_miss_code=14u;sv24_miss_vc=vdp2_tvmd_vcount_get();
        sv24_miss_delta=sv24_vblanks-epoch;sv24_deadline_misses++;
        goto hard_resync;
    }

    /* B0 is now needed for the bottom band; remove all active-display CPU
       slots.  This steady state remains until the next 15-Hz transition. */
    hblank_exit_wait();
    cycle_all_cg();
    sv24_beam_success++;
    sv24_stage=0x40Eu;
    return 0;

hard_resync:
    /* A missed transfer deadline is rare, but real hardware correctness is
       more important than showing a half-updated chip.  Hide NBG0, finish a
       coherent true-color frame with display inactive, then resume using the
       legal steady fetch pattern. */
    sv24_stage=0x4F0u;
    display_off_direct();
    cycle_image_cpu();
    r=upload_full_band(0);if(!r)r=upload_full_band(1);if(!r)r=upload_full_band(2);
    cycle_all_cg();
    display_on_direct();
    sv24_stage=0x4FBu;
    return r;
}

void user_init(void){
    bios_clock_speed_chg(CPU_CLOCK_SPEED_28MHZ);
}

int main(void){
    cdfs_filelist_entry_t *ents,*mov,*snd;
    uint8_t hdr[96];
    uint32_t frames,ioff,doff,i,q,sequence,audio_rate;
    int16_t audio_coef1, audio_coef2;
    uint8_t recovery_mode=0u; /* 0 none, 1 sparse dirty-union, 2 full fallback */
    int r;

    sv24_status=0x11000000u;
    cpu_frt_init(CPU_FRT_CLOCK_DIV_128);
    cdfs_init();
    ents=cdfs_entries_alloc(64);
    cdfs_filelist_init(&filelist,ents,64);
    cdfs_filelist_root_read(&filelist);
    mov=find_file("MOVIE.SVM");
    snd=find_file("SNDDRV.BIN");
    if(!mov || !snd){sv24_status=0xE0010000u; goto halt;}

    /* Read the movie header before starting the 68K sound driver so the SCSP
       pitch follows the stream.  v26.7 intentionally supports several ADX
       rates: 32 kHz balanced, 28 kHz middle ground, and 24 kHz video-priority. */
    sv24_status=0x12000000u;
    if(file_read_at(mov,0,hdr,96)){sv24_status=0xE0020000u; goto halt;}
    audio_rate=be32(hdr+28);pace_rate=audio_rate;
    audio_coef1=(int16_t)be16(hdr+48);
    audio_coef2=(int16_t)be16(hdr+50);
    if(be32(hdr)!=0x53564D31u || be16(hdr+4)!=0x0100u || be16(hdr+8)!=352u ||
       be16(hdr+10)!=240u || be16(hdr+12)!=15u || be16(hdr+14)!=1u ||
       (audio_rate<12000u || audio_rate>44100u) ||
       be32(hdr+32)!=2u || be32(hdr+36)!=18u || be32(hdr+40)!=32u){
        sv24_status=0xE0030000u; goto halt;
    }
    stream_cache_len=0; /* next bootstrap read is from SNDDRV.BIN */

    /* Load Sega-style 68K sound driver before taking over the CD block with
       the continuous interleaved movie stream. */
    if(snd->size > MAX_FRAME_BYTES || file_read_at(snd,0,framebuf,snd->size)){
        sv24_status=0xE0090000u; goto halt;
    }
    r=sv24_audio_init(framebuf,snd->size,(uint16_t)audio_rate,audio_coef1,audio_coef2);
    if(r){sv24_status=0xE00A0000u|((uint32_t)(-r)&0xffffu); goto halt;}
    stream_cache_len=0; /* bootstrap cache is per-file; next read is MOVIE.SVM */
    frames=be32(hdr+16); ioff=be32(hdr+20); doff=be32(hdr+24);
    if(!frames || frames>MAX_FRAMES || doff>=mov->size || (doff&2047u)){
        sv24_status=0xE0040000u; goto halt;
    }
    if(file_read_at(mov,ioff,framebuf,frames*8u)){sv24_status=0xE0050000u; goto halt;}
    uint32_t expected=doff;
    for(i=0;i<frames;i++){
        frame_off[i]=be32(framebuf+i*8u); frame_size[i]=be32(framebuf+i*8u+4u);
        if(frame_size[i]>sv24_max_frame_bytes)sv24_max_frame_bytes=frame_size[i];
        if(frame_size[i]<32u || frame_size[i]>MAX_PACKET_BYTES || frame_off[i]!=expected || frame_off[i]>mov->size || frame_size[i]>mov->size-frame_off[i]){
            sv24_status=0xE0060000u+i; goto halt;
        }
        expected+=frame_size[i];
    }
    if(expected!=mov->size){sv24_status=0xE006ffffu;goto halt;}

    renderer_init();
    sv24_dual_init();
    sv24_dma_init();
    sv24_scu_init();

restart_stream:
    recovery_mode=0u; recovery_clear();
    /* Only the initial startup resets the SCSP/DSP stream.  Subsequent movie
       laps are continuous in both the CD selector and the sound ring. */
    sv24_audio_stop();
    sv24_audio_stream_reset();
    r=movie_stream_start(mov,frame_off[0]);
    if(r){sv24_status=0xE0070000u|((uint32_t)(-r)&0xffffu); goto halt;}
    r=movie_stream_pump(96u*1024u,1);
    if(r){sv24_status=0xE0080000u|((uint32_t)(-r)&0xffffu); goto halt;}

    /* Ten packets (~0.67 s of 44.1-kHz stereo ADX) are queued before
       key-on.  Thereafter every consumed queue slot is refilled ten frames
       ahead, including across the physical CD repeat boundary. */
    q=(frames<PACKET_QUEUE_COUNT)?frames:PACKET_QUEUE_COUNT;
    for(i=0;i<q;i++){
        svm_packet_t pk;
        uint8_t *slot=packet_slot(i);
        if(movie_stream_read(slot,frame_size[i])){sv24_status=0xE0100000u+i; goto halt;}
        r=svm_packet_parse(slot,frame_size[i],&pk);
        if(r || pk.frame_no!=i){sv24_status=0xE0110000u|i; goto halt;}
        r=sv24_audio_feed_adx(pk.audio,pk.audio_size);
        if(r){sv24_status=0xE0120000u|((uint32_t)(-r)&0xffffu); goto halt;}
    }
    sv24_status=0x20000000u;
    sequence=0u;

    for(;;){
      for(i=0;i<frames;i++,sequence++){
        sv24_v03_frame_t fr;
        svm_packet_t pk;
        uint8_t *slot=packet_slot(sequence);
        uint32_t future_abs=i+PACKET_QUEUE_COUNT;
        uint32_t future=(future_abs>=frames)?(future_abs-frames):future_abs;

        r=svm_packet_parse(slot,frame_size[i],&pk);
        if(r || pk.frame_no!=i){sv24_status=0xE1000000u|i; goto halt;}
        r=sv24_v03_frame_parse(pk.video,pk.video_size,&fr);
        if(r){sv24_decode_errors++; sv24_status=0xE2000000u|((uint32_t)(-r)&0xffffu); goto halt;}
        /* True SCU offload runs concurrently with the dual-SH2 slice pass.
           DSP-owned records are MODE_DSP_SKIP, so staging writes are disjoint.
           Synchronize only at the frame barrier before DMA planning/presentation. */
        /* Zero-sidecar frames are the codec's saturation escape hatch: avoid
           entering the SCU helper path at all when there is no DSP work. */
        if(pk.dsp_size){
            r=sv24_scu_begin(pk.dsp,pk.dsp_size,staging);
            if(r){sv24_decode_errors++; sv24_status=0xE3200000u|((uint32_t)(-r)&0xffffu); goto halt;}
        }
        r=sv24_dual_decode_frame(&fr,staging,dma_plans,SV24_SPARSE_GAP);
        if(r){
            if(pk.dsp_size)(void)sv24_scu_end(staging);
            sv24_decode_errors++; sv24_status=0xE3000000u|((uint32_t)(-r)&0xffffu); goto halt;
        }
        if(pk.dsp_size){
            r=sv24_scu_end(staging);
            if(r){sv24_decode_errors++; sv24_status=0xE3210000u|((uint32_t)(-r)&0xffffu); goto halt;}
        }


        uint32_t audio_head=(pk.audio_size>=20u && be16(pk.audio)==0x8000u)?be16(pk.audio+2)+4u:0u;
        if(audio_head>pk.audio_size || (pk.audio_size-audio_head)%36u){sv24_status=0xe3220000u;goto halt;}
        pace_frame_samples=(pk.audio_size-audio_head)/36u*32u;
        sv24_audio_tick();
        if (sv24_frames_decoded == 0u) {
            r=present_first(&fr);
            if(!r){
                sv24_audio_start(7u);
                pace_arm_after_first();
            }
        } else {
            /* If this decoded frame has already missed its nominal display
               slot, skip presentation and immediately continue.  MAP_DMA_SAFE
               frames are accumulated into a dirty-cell union, so recovery
               copies only cells touched by the skipped interval instead of
               forcing a 338 KiB whole-frame upload. */
            if (pace_is_late()) {
                sv24_pace_late++;
                sv24_video_drops++;
                if (recovery_mode != 2u) { recovery_mark_current(&fr,dma_plans); recovery_mode=1u; }
                pace_advance_slot();
                r=0;
            } else {
                if(recovery_mode==1u){
                    recovery_mark_current(&fr,dma_plans);
                    r=sv24_dma_prepare_dirty_cells(staging,recovery_dirty,SV24_SPARSE_GAP);
                }else if(recovery_mode==2u){
                    r=0;
                }else{
                    r=(fr.coding_flags & SV24_FRM3_MAP_DMA_SAFE) ? sv24_dma_prepare_maps(staging,&fr,SV24_SPARSE_GAP) : sv24_dma_prepare_plans(staging,dma_plans,fr.slice_count,SV24_SPARSE_GAP);
                }
                if(r){sv24_status=0xE3500000u|((uint32_t)(-r)&0xffffu); goto halt;}
                /* Planning itself can push a marginal frame past the slot. */
                if (pace_is_late()) {
                    sv24_pace_late++;
                    sv24_video_drops++;
                    if (recovery_mode != 2u) { recovery_mark_current(&fr,dma_plans); recovery_mode=1u; }
                    pace_advance_slot();
                    r=0;
                } else {
                    pace_wait_target();
                    r=present_beamrace(&fr,recovery_mode==2u);
                    if(r==1){
                        /* Transition window closed before any VRAM write.
                           Treat it exactly like a decode/planning deadline
                           drop and merge this frame into the recovery union. */
                        sv24_pace_late++;
                        sv24_video_drops++;
                        if(recovery_mode!=2u){recovery_mark_current(&fr,dma_plans);recovery_mode=1u;}
                        pace_advance_slot();
                        r=0;
                    }else if(!r){
                        if(recovery_mode==1u){sv24_recovery_sparse++;sv24_full_resyncs++;}
                        else if(recovery_mode==2u){sv24_recovery_full++;sv24_full_resyncs++;}
                        if(recovery_mode){recovery_mode=0u;recovery_clear();}
                        pace_advance_slot();
                    }
                }
            }
        }
        if(r){sv24_status=0xE4000000u|((uint32_t)(-r)&0xffffu); goto halt;}
        sv24_audio_tick();
        sv24_frame_no=i; sv24_frames_decoded++;
        sv24_status=0x40000000u|i;

        /* Refill the consumed packet slot only after the current frame has
           met (or intentionally skipped) its presentation deadline.  Audio
           is ten packets ahead, so this copy/feed work has ample lead and no
           longer steals time from the frame that was just decoded. */
        /* For the final ten frames the future index wraps to the next lap,
           so next-lap ADX is fed before the old lap ends and producer lead
           remains continuous.  Discard only final-sector zero padding. */
        {
            svm_packet_t fp;
            if(SV24_LOOP && future_abs>=frames && future==0u){
                r=movie_stream_skip(stream_loop_pad);
                if(r){sv24_status=0xE10F0000u|((uint32_t)(-r)&0xffffu); goto halt;}
                sv24_stream_loops++;
            }
            if(SV24_LOOP || future_abs<frames){
            if(movie_stream_read(slot,frame_size[future])){sv24_status=0xE1100000u|future; goto halt;}
            r=svm_packet_parse(slot,frame_size[future],&fp);
            if(r || fp.frame_no!=future){sv24_status=0xE1200000u|future; goto halt;}
            r=sv24_audio_feed_adx(fp.audio,fp.audio_size);
            if(r){sv24_status=0xE1300000u|((uint32_t)(-r)&0xffffu); goto halt;}
            }
        }
        r=movie_stream_pump(192u*1024u,0);
        if(r){sv24_cd_errors++;sv24_status=0xe1400000u|((uint32_t)(-r)&0xffffu);goto halt;}
      }
      if(!SV24_LOOP){
          /* Last presentation owns its full timeline slot. Keep its RGB24
           * surface visible at EOF rather than seeking or blanking it. */
          sv24_pace_target++;pace_wait_target();sv24_status=0x50000000u;break;
      }
    }

halt:
    sv24_audio_stop();
    cycle_all_cg(); display_on_direct();
    while(1){vdp2_sync();vdp2_sync_wait();}
    return 0;
}
