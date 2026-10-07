/* V28.1 RGB24 NBG0 surface. Staging lives in high work RAM for legal
 * direct SCU -> B-bus transfers within the hidden bank's scanline window.
 * Never change a displayed image bank's CPU slots while the beam uses it. */
#include <yaul.h>
#include <string.h>
#include "sat_internal.h"
#include "video_vdp2_sat.h"
#include "sv24_vdp2_layout.h"
#define IO ((volatile vdp2_ioregs_t *)VDP2_IOREG_BASE)
static vdp2_ioregs_t saved;
static int width,height,band_rows,tile_cols;
static bool owned,visible;
typedef struct { int x,y,w,h,cx,cy,cw,ch; } Geometry;
static Geometry pending __uncached;
static Geometry current;
static void cycles(uint32_t a,uint32_t b0,uint32_t b1)
{
    IO->cyca0l=(uint16_t)(a>>16);IO->cyca0u=(uint16_t)a;
    IO->cyca1l=(uint16_t)(a>>16);IO->cyca1u=(uint16_t)a;
    IO->cycb0l=(uint16_t)(b0>>16);IO->cycb0u=(uint16_t)b0;
    IO->cycb1l=(uint16_t)(b1>>16);IO->cycb1u=(uint16_t)b1;
}
static void fetch_all(void) { cycles(0x44444444u,0x44444444u,0x0fffffffu); }
static void all_cpu(void) { cycles(0xfeeeeeeeu,0xfeeeeeeeu,0xfeeeeeeeu); }
static uint16_t line(void) { vdp2_tvmd_extern_latch(); return vdp2_tvmd_vcount_get(); }
static uint32_t fields(void) { return sat_vblanks(); }
static void next_blank(void)
{
    if(vdp2_tvmd_vblank_in())vdp2_tvmd_vblank_out_wait();
    vdp2_tvmd_vblank_in_wait();
}
static void apply_geometry(void)
{
    Geometry g=pending;
    if(g.w<=0||g.h<=0)return;
    current=g;
    /* ST-058-R2 5.2: source = increment * screen + scroll.
     * Images are baked at their native UI size; enlarge without additional
     * PND reads. RGB24 horizontal reduction is deliberately not requested. */
    uint32_t zx=(uint32_t)(((uint64_t)width<<16)/(unsigned)g.w)&0x0007ff00u;
    uint32_t zy=(uint32_t)(((uint64_t)height<<16)/(unsigned)g.h)&0x0007ff00u;
    if(zx>0x10000u)zx=0x10000u;
    vdp2_ioregs_t *sh=vdp2_regs_get();
    sh->zm0.x=(fix16_t)zx;sh->zm0.y=(fix16_t)zy;
    sh->sc0.x=(fix16_t)(-(int64_t)g.x*zx);sh->sc0.y=(fix16_t)(-(int64_t)g.y*zy);
    sh->wpsx0=(uint16_t)(g.cx*2);sh->wpex0=(uint16_t)((g.cx+g.cw-1)*2);
    sh->wpsy0=(uint16_t)g.cy;sh->wpey0=(uint16_t)(g.cy+g.ch-1);
    sh->wctla=0x0003u; /* make pixels outside W0 transparent (manual 8.2) */
    IO->zm0=sh->zm0;IO->sc0=sh->sc0;
    IO->wpsx0=sh->wpsx0;IO->wpex0=sh->wpex0;IO->wpsy0=sh->wpsy0;IO->wpey0=sh->wpey0;IO->wctla=sh->wctla;
}
bool movie_vdp2_open(int w,int h)
{
    if(owned||w<1||w>352||h<1||h>240)return false;
    vdp2_sync_wait(); saved=*vdp2_regs_get();
    sat_planes_movie_begin();rsat_movie_vram_invalidate();
    width=w;height=h;tile_cols=(int)sv24_output_tiles_x;band_rows=((h+7)/8+2)/3;owned=true;visible=false;
    pending=(Geometry){0,0,w,h,0,0,w,h};current=pending;
    vdp2_scrn_display_set(VDP2_SCRN_DISP_NONE);IO->bgon=0;
    const vdp2_vram_ctl_t ctl={.coeff_table=VDP2_VRAM_CTL_COEFF_TABLE_VRAM,.vram_mode=VDP2_VRAM_CTL_MODE_PART_BANK_BOTH};
    const vdp2_vram_usage_t usage={.a0=VDP2_VRAM_USAGE_TYPE_CPD_BPD,.a1=VDP2_VRAM_USAGE_TYPE_CPD_BPD,.b0=VDP2_VRAM_USAGE_TYPE_CPD_BPD,.b1=VDP2_VRAM_USAGE_TYPE_PND};
    const vdp2_scrn_cell_format_t fmt={.scroll_screen=VDP2_SCRN_NBG0,.ccc=VDP2_SCRN_CCC_RGB_16770000,.char_size=VDP2_SCRN_CHAR_SIZE_1X1,.pnd_size=2,.aux_mode=VDP2_SCRN_AUX_MODE_0,.plane_size=VDP2_SCRN_PLANE_SIZE_1X1,.cpd_base=VDP2_VRAM_ADDR(0,0),.palette_base=0};
    const vdp2_scrn_normal_map_t map={.base_addr={VDP2_VRAM_ADDR(3,0),VDP2_VRAM_ADDR(3,0),VDP2_VRAM_ADDR(3,0),VDP2_VRAM_ADDR(3,0)}};
    vdp2_vram_control_set(&ctl);vdp2_vram_usage_set(&usage);
    all_cpu();
    uint16_t *pnd=(uint16_t *)VDP2_VRAM_ADDR(3,0);
    for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++) {
        unsigned id=(y*64u+x)*2u,offset=0;
        if(y<(unsigned)(h+7)/8u&&x<(unsigned)tile_cols)
            offset=(y/(unsigned)band_rows)*0x20000u+((y%(unsigned)band_rows)*(unsigned)tile_cols+x)*256u;
        pnd[id]=0;pnd[id+1]=(uint16_t)(offset>>5);
    }
    vdp2_scrn_cell_format_set(&fmt,&map);vdp2_scrn_priority_set(VDP2_SCRN_NBG0,4);
    vdp2_ioregs_t *sh=vdp2_regs_get();
    sh->scrctl=0;sh->zmctl=0;sh->ccctl=0;sh->lnclen=0;sh->sfprmd=0;sh->sfccmd=0;sh->clofen=0;
    sh->wctlb=sh->wctlc=sh->wctld=0;sh->lwta0u=sh->lwta0l=0;
    vdp2_vram_cycp_t c;
    c.pt[0].raw=c.pt[1].raw=c.pt[2].raw=0x44444444u;c.pt[3].raw=0x0fffffffu;vdp2_vram_cycp_set(&c);
    vdp2_sync();vdp2_sync_wait();fetch_all();
    return true;
}
void movie_vdp2_geometry(int x,int y,int w,int h,const RRect *clip)
{
    if(!owned)return;
    RRect image={x,y,w,h},cut;
    if(!r_rect_intersect(&image,clip,&cut))return;
    pending=(Geometry){x,y,w,h,cut.x,cut.y,cut.w,cut.h};
}
void movie_vdp2_geometry_tick(void)
{
    if(!owned||!visible)return;
    if(memcmp(&current,(const void *)&pending,sizeof current)) {
        vdp2_tvmd_vblank_in_wait();apply_geometry();
    }
}
/* Keep the indirect table within one 4 KB page (SCU DMA errata). */
#define MAX_RUNS 64u
static scu_dma_xfer_t transfers[MAX_RUNS] __aligned(1024);
static int upload(const uint32_t *cells,uint8_t *dirty,unsigned band,bool full)
{
    unsigned first=band*(unsigned)band_rows*(unsigned)tile_cols,end=(unsigned)(height+7)/8u*(unsigned)tile_cols;
    if(end>first+(unsigned)band_rows*(unsigned)tile_cols)end=first+(unsigned)band_rows*(unsigned)tile_cols;
    if(end<=first)return 0;
    unsigned count=0,last=first;
    for(unsigned at=first;at<end;) {
        if(!full&&!dirty[at]){at++;continue;}
        unsigned begin=at++;while(at<end&&(full||dirty[at]))at++;
        if(count&&begin-last<=2u)transfers[count-1u].len=(at-(last-transfers[count-1u].len/256u))*256u;
        else {
            if(count==MAX_RUNS){full=true;count=0;break;}
            transfers[count++]=(scu_dma_xfer_t){.len=(at-begin)*256u,
                .src=(uint32_t)(uintptr_t)(cells+begin*64u),
                .dst=0x25e00000u+band*0x20000u+(begin-first)*256u};
        }
        last=at;
    }
    if(full) {
        count=1;transfers[0]=(scu_dma_xfer_t){.len=(end-first)*256u,
            .src=(uint32_t)(uintptr_t)(cells+first*64u),.dst=0x25e00000u+band*0x20000u};
    }
    if(!count)return 0;
    transfers[count-1u].src|=SCU_DMA_INDIRECT_TABLE_END;
    cpu_cache_area_purge(transfers,count*sizeof *transfers);
    scu_dma_handle_t handle;
    const scu_dma_level_cfg_t cfg={.space=SCU_DMA_SPACE_BUS_B,.mode=SCU_DMA_MODE_INDIRECT,
        .stride=SCU_DMA_STRIDE_2_BYTES,.update=SCU_DMA_UPDATE_NONE,.xfer.indirect=transfers};
    scu_dma_config_buffer(&handle,&cfg);
    scu_dma_level_wait(0);scu_dma_config_set(0,SCU_DMA_START_FACTOR_ENABLE,&handle,NULL);
    scu_dma_level_fast_start(0);scu_dma_level_wait(0);
    return 0;
}

int movie_vdp2_present(const uint32_t *cells,uint8_t *dirty,bool first)
{
    if(!owned||!cells||!dirty)return -1;
    if(first||!visible) {
        vdp2_tvmd_vblank_in_wait();IO->bgon=0;all_cpu();
        for(unsigned b=0;b<3;b++)upload(cells,dirty,b,true);
        next_blank();apply_geometry();fetch_all();
        vdp2_scrn_display_set(VDP2_SCRN_DISPTP_NBG0);IO->bgon=vdp2_regs_get()->bgon;visible=true;
    } else {
        /* Leave an entire cell row of fetch margin on each side of the band
         * boundary. VCNT is explicitly re-latched on every read. Geometry's
         * clip limits the visible region; the source split follows the two VRAM-A bands. */
        Geometry g=current;
        int split=g.y+((band_rows*16)*g.h+height-1)/height;
        int last=g.cy+g.ch;
        if(last<split)split=last;
        int start=split+8;
        if(start>=SAT_SCREEN_H)start=SAT_SCREEN_H-1;
        /* If the window has already passed, use the next field. */
        if(!vdp2_tvmd_vblank_in()&&line()>start)next_blank();
        vdp2_tvmd_vcount_wait((uint16_t)start);
        uint32_t epoch=fields();
        while(IO->tvstat&4u){}
        cycles(0xfeeeeeeeu,0x44444444u,0x0fffffffu);
        upload(cells,dirty,0,false);upload(cells,dirty,1,false);
        /* If a transfer ran into active display, hide NBG0 and complete a
         * coherent resync instead of exposing a partially rewritten bank. */
        if(fields()!=epoch&&!vdp2_tvmd_vblank_in())goto resync;
        if(fields()==epoch)vdp2_tvmd_vblank_in_wait();
        cycles(0x44444444u,0xffeeeeeeu,0x0feeeeeeu);
        apply_geometry();upload(cells,dirty,2,false);
        int restore=current.y+((band_rows*16)*current.h)/height-8;
        if(current.cy+current.ch<=current.y+((band_rows*16)*current.h)/height)restore=current.cy-8;
        if(restore<0)restore=0;
        if(!vdp2_tvmd_vblank_in()&&line()>restore)goto resync;
        /* Restore B fetches immediately after DMA completes. Waiting until
         * line 151 wastes decode/CD time and needlessly changes the pattern
         * beside the hardware's prefetch boundary. */
        fetch_all();
    }
    memset(dirty,0,SV24_FRAME_TILES);return 0;
resync:
    IO->bgon=0;all_cpu();for(unsigned b=0;b<3;b++)upload(cells,dirty,b,true);
    next_blank();apply_geometry();fetch_all();IO->bgon=vdp2_regs_get()->bgon;
    memset(dirty,0,SV24_FRAME_TILES);return 0;
}
void movie_vdp2_close(void)
{
    if(!owned)return;
    vdp2_sync_wait();vdp2_tvmd_vblank_in_wait();IO->bgon=0;
    *vdp2_regs_get()=saved;
    if(!sat_planes_movie_end())printf("video: could not restore level VRAM\n");
    vdp2_sync();vdp2_sync_wait();owned=visible=false;
}
