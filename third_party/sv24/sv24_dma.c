#include <yaul.h>
#include <stdint.h>
#include <stddef.h>
#include "sv24_dma.h"
#include "sv24_vdp2_layout.h"

#define VRAM_BASE 0x25E00000u
#define BAND_MAX_XFERS 256u

static scu_dma_xfer_t xfers0[BAND_MAX_XFERS] __aligned(4096);
static scu_dma_xfer_t xfers1[BAND_MAX_XFERS] __aligned(4096);
static scu_dma_xfer_t xfers2[BAND_MAX_XFERS] __aligned(4096);
static scu_dma_xfer_t * const tables[3] = { xfers0, xfers1, xfers2 };
static scu_dma_handle_t band_handle[3];
static uint16_t band_count[3];
static uint32_t band_bytes[3];
static const uint32_t *prepared_staging;

volatile uint32_t sv24_dma_submissions = 0;
volatile uint32_t sv24_dma_runs = 0;
volatile uint32_t sv24_dma_bytes = 0;
volatile uint32_t sv24_dma_fallback_bands = 0;
volatile uint32_t sv24_dma_prepare_ticks_total = 0;
volatile uint32_t sv24_dma_prepare_ticks_max = 0;
volatile uint32_t sv24_dma_prepare_calls = 0;
volatile uint32_t sv24_dma_upload_ticks_total = 0;
volatile uint32_t sv24_dma_upload_ticks_max = 0;
volatile uint32_t sv24_dma_upload_calls = 0;
volatile uint32_t sv24_dma_last_runs[3] = {0,0,0};
volatile uint32_t sv24_dma_last_bytes[3] = {0,0,0};
volatile uint32_t sv24_dma_max_runs[3] = {0,0,0};
volatile uint32_t sv24_dma_max_bytes[3] = {0,0,0};

static void handle_init(unsigned band)
{
    const scu_dma_level_cfg_t cfg = {
        .space = SCU_DMA_SPACE_BUS_B,
        .mode = SCU_DMA_MODE_INDIRECT,
        .stride = SCU_DMA_STRIDE_2_BYTES,
        .update = SCU_DMA_UPDATE_NONE,
        .xfer.indirect = tables[band]
    };
    scu_dma_config_buffer(&band_handle[band], &cfg);
}

void sv24_dma_init(void)
{
    unsigned b;
    for (b=0;b<3u;b++){band_count[b]=0;band_bytes[b]=0;handle_init(b);} prepared_staging=NULL;
}

static void prepare_full_band(const uint32_t *staging, unsigned band, int fallback)
{
    scu_dma_xfer_t *t=tables[band];
    uint32_t first=band*SV24_BAND_TILES, bytes=SV24_BAND_TILES*SV24_CELL_BYTES;
    t[0].len=bytes; t[0].dst=VRAM_BASE+band*SV24_VRAM_BANK_BYTES;
    t[0].src=(CPU_CACHE_THROUGH|(uint32_t)(uintptr_t)&staging[first*SV24_TILE_PIXELS])|SCU_DMA_INDIRECT_TABLE_END;
    band_count[band]=1; band_bytes[band]=bytes; if(fallback)sv24_dma_fallback_bands++;
}

typedef struct {uint32_t n,bytes,prev_src_end,prev_dst_end;uint8_t overflow;} build_t;
static void append_run(unsigned band, build_t *b, const uint32_t *staging,
                       uint32_t src, uint32_t dst, uint32_t len, uint32_t gapmax)
{
    scu_dma_xfer_t *t=tables[band];
    if(b->overflow)return;
    if(b->n && src>=b->prev_src_end && dst>=b->prev_dst_end){
        uint32_t sg=src-b->prev_src_end,dg=dst-b->prev_dst_end;
        if(sg==dg && sg<=gapmax){t[b->n-1u].len+=sg+len;b->bytes+=sg+len;b->prev_src_end=src+len;b->prev_dst_end=dst+len;return;}
    }
    if(b->n>=BAND_MAX_XFERS){b->overflow=1;return;}
    t[b->n].len=len;t[b->n].dst=VRAM_BASE+dst;t[b->n].src=CPU_CACHE_THROUGH|(uint32_t)(uintptr_t)((const uint8_t*)staging+src);
    b->bytes+=len;b->prev_src_end=src+len;b->prev_dst_end=dst+len;b->n++;
}

/* For an 8-cell bitmap byte (bit 0 = leftmost), pack count plus up to
 * four {start,len} runs.  This replaces 44 branchy bit tests per row. */
static const uint32_t run8_lut[256] = {
    0x00000000u, 0x00000010u, 0x00000011u, 0x00000020u, 0x00000012u, 0x00001210u, 0x00000021u, 0x00000030u,
    0x00000013u, 0x00001310u, 0x00001311u, 0x00001320u, 0x00000022u, 0x00002210u, 0x00000031u, 0x00000040u,
    0x00000014u, 0x00001410u, 0x00001411u, 0x00001420u, 0x00001412u, 0x00141210u, 0x00001421u, 0x00001430u,
    0x00000023u, 0x00002310u, 0x00002311u, 0x00002320u, 0x00000032u, 0x00003210u, 0x00000041u, 0x00000050u,
    0x00000015u, 0x00001510u, 0x00001511u, 0x00001520u, 0x00001512u, 0x00151210u, 0x00001521u, 0x00001530u,
    0x00001513u, 0x00151310u, 0x00151311u, 0x00151320u, 0x00001522u, 0x00152210u, 0x00001531u, 0x00001540u,
    0x00000024u, 0x00002410u, 0x00002411u, 0x00002420u, 0x00002412u, 0x00241210u, 0x00002421u, 0x00002430u,
    0x00000033u, 0x00003310u, 0x00003311u, 0x00003320u, 0x00000042u, 0x00004210u, 0x00000051u, 0x00000060u,
    0x00000016u, 0x00001610u, 0x00001611u, 0x00001620u, 0x00001612u, 0x00161210u, 0x00001621u, 0x00001630u,
    0x00001613u, 0x00161310u, 0x00161311u, 0x00161320u, 0x00001622u, 0x00162210u, 0x00001631u, 0x00001640u,
    0x00001614u, 0x00161410u, 0x00161411u, 0x00161420u, 0x00161412u, 0x16141210u, 0x00161421u, 0x00161430u,
    0x00001623u, 0x00162310u, 0x00162311u, 0x00162320u, 0x00001632u, 0x00163210u, 0x00001641u, 0x00001650u,
    0x00000025u, 0x00002510u, 0x00002511u, 0x00002520u, 0x00002512u, 0x00251210u, 0x00002521u, 0x00002530u,
    0x00002513u, 0x00251310u, 0x00251311u, 0x00251320u, 0x00002522u, 0x00252210u, 0x00002531u, 0x00002540u,
    0x00000034u, 0x00003410u, 0x00003411u, 0x00003420u, 0x00003412u, 0x00341210u, 0x00003421u, 0x00003430u,
    0x00000043u, 0x00004310u, 0x00004311u, 0x00004320u, 0x00000052u, 0x00005210u, 0x00000061u, 0x00000070u,
    0x00000017u, 0x00001710u, 0x00001711u, 0x00001720u, 0x00001712u, 0x00171210u, 0x00001721u, 0x00001730u,
    0x00001713u, 0x00171310u, 0x00171311u, 0x00171320u, 0x00001722u, 0x00172210u, 0x00001731u, 0x00001740u,
    0x00001714u, 0x00171410u, 0x00171411u, 0x00171420u, 0x00171412u, 0x17141210u, 0x00171421u, 0x00171430u,
    0x00001723u, 0x00172310u, 0x00172311u, 0x00172320u, 0x00001732u, 0x00173210u, 0x00001741u, 0x00001750u,
    0x00001715u, 0x00171510u, 0x00171511u, 0x00171520u, 0x00171512u, 0x17151210u, 0x00171521u, 0x00171530u,
    0x00171513u, 0x17151310u, 0x17151311u, 0x17151320u, 0x00171522u, 0x17152210u, 0x00171531u, 0x00171540u,
    0x00001724u, 0x00172410u, 0x00172411u, 0x00172420u, 0x00172412u, 0x17241210u, 0x00172421u, 0x00172430u,
    0x00001733u, 0x00173310u, 0x00173311u, 0x00173320u, 0x00001742u, 0x00174210u, 0x00001751u, 0x00001760u,
    0x00000026u, 0x00002610u, 0x00002611u, 0x00002620u, 0x00002612u, 0x00261210u, 0x00002621u, 0x00002630u,
    0x00002613u, 0x00261310u, 0x00261311u, 0x00261320u, 0x00002622u, 0x00262210u, 0x00002631u, 0x00002640u,
    0x00002614u, 0x00261410u, 0x00261411u, 0x00261420u, 0x00261412u, 0x26141210u, 0x00261421u, 0x00261430u,
    0x00002623u, 0x00262310u, 0x00262311u, 0x00262320u, 0x00002632u, 0x00263210u, 0x00002641u, 0x00002650u,
    0x00000035u, 0x00003510u, 0x00003511u, 0x00003520u, 0x00003512u, 0x00351210u, 0x00003521u, 0x00003530u,
    0x00003513u, 0x00351310u, 0x00351311u, 0x00351320u, 0x00003522u, 0x00352210u, 0x00003531u, 0x00003540u,
    0x00000044u, 0x00004410u, 0x00004411u, 0x00004420u, 0x00004412u, 0x00441210u, 0x00004421u, 0x00004430u,
    0x00000053u, 0x00005310u, 0x00005311u, 0x00005320u, 0x00000062u, 0x00006210u, 0x00000071u, 0x00000080u,
};

/* Changed-map bytes are MSB-first.  Each entry packs up to four
 * {start,len} runs, one byte per run, exactly like run8_lut but without
 * bit-reversing the compressed map at runtime. */
static const uint32_t run8_msb_lut[256] = {
    0x00000000u, 0x00000017u, 0x00000016u, 0x00000026u, 0x00000015u, 0x00001715u, 0x00000025u, 0x00000035u,
    0x00000014u, 0x00001714u, 0x00001614u, 0x00002614u, 0x00000024u, 0x00001724u, 0x00000034u, 0x00000044u,
    0x00000013u, 0x00001713u, 0x00001613u, 0x00002613u, 0x00001513u, 0x00171513u, 0x00002513u, 0x00003513u,
    0x00000023u, 0x00001723u, 0x00001623u, 0x00002623u, 0x00000033u, 0x00001733u, 0x00000043u, 0x00000053u,
    0x00000012u, 0x00001712u, 0x00001612u, 0x00002612u, 0x00001512u, 0x00171512u, 0x00002512u, 0x00003512u,
    0x00001412u, 0x00171412u, 0x00161412u, 0x00261412u, 0x00002412u, 0x00172412u, 0x00003412u, 0x00004412u,
    0x00000022u, 0x00001722u, 0x00001622u, 0x00002622u, 0x00001522u, 0x00171522u, 0x00002522u, 0x00003522u,
    0x00000032u, 0x00001732u, 0x00001632u, 0x00002632u, 0x00000042u, 0x00001742u, 0x00000052u, 0x00000062u,
    0x00000011u, 0x00001711u, 0x00001611u, 0x00002611u, 0x00001511u, 0x00171511u, 0x00002511u, 0x00003511u,
    0x00001411u, 0x00171411u, 0x00161411u, 0x00261411u, 0x00002411u, 0x00172411u, 0x00003411u, 0x00004411u,
    0x00001311u, 0x00171311u, 0x00161311u, 0x00261311u, 0x00151311u, 0x17151311u, 0x00251311u, 0x00351311u,
    0x00002311u, 0x00172311u, 0x00162311u, 0x00262311u, 0x00003311u, 0x00173311u, 0x00004311u, 0x00005311u,
    0x00000021u, 0x00001721u, 0x00001621u, 0x00002621u, 0x00001521u, 0x00171521u, 0x00002521u, 0x00003521u,
    0x00001421u, 0x00171421u, 0x00161421u, 0x00261421u, 0x00002421u, 0x00172421u, 0x00003421u, 0x00004421u,
    0x00000031u, 0x00001731u, 0x00001631u, 0x00002631u, 0x00001531u, 0x00171531u, 0x00002531u, 0x00003531u,
    0x00000041u, 0x00001741u, 0x00001641u, 0x00002641u, 0x00000051u, 0x00001751u, 0x00000061u, 0x00000071u,
    0x00000010u, 0x00001710u, 0x00001610u, 0x00002610u, 0x00001510u, 0x00171510u, 0x00002510u, 0x00003510u,
    0x00001410u, 0x00171410u, 0x00161410u, 0x00261410u, 0x00002410u, 0x00172410u, 0x00003410u, 0x00004410u,
    0x00001310u, 0x00171310u, 0x00161310u, 0x00261310u, 0x00151310u, 0x17151310u, 0x00251310u, 0x00351310u,
    0x00002310u, 0x00172310u, 0x00162310u, 0x00262310u, 0x00003310u, 0x00173310u, 0x00004310u, 0x00005310u,
    0x00001210u, 0x00171210u, 0x00161210u, 0x00261210u, 0x00151210u, 0x17151210u, 0x00251210u, 0x00351210u,
    0x00141210u, 0x17141210u, 0x16141210u, 0x26141210u, 0x00241210u, 0x17241210u, 0x00341210u, 0x00441210u,
    0x00002210u, 0x00172210u, 0x00162210u, 0x00262210u, 0x00152210u, 0x17152210u, 0x00252210u, 0x00352210u,
    0x00003210u, 0x00173210u, 0x00163210u, 0x00263210u, 0x00004210u, 0x00174210u, 0x00005210u, 0x00006210u,
    0x00000020u, 0x00001720u, 0x00001620u, 0x00002620u, 0x00001520u, 0x00171520u, 0x00002520u, 0x00003520u,
    0x00001420u, 0x00171420u, 0x00161420u, 0x00261420u, 0x00002420u, 0x00172420u, 0x00003420u, 0x00004420u,
    0x00001320u, 0x00171320u, 0x00161320u, 0x00261320u, 0x00151320u, 0x17151320u, 0x00251320u, 0x00351320u,
    0x00002320u, 0x00172320u, 0x00162320u, 0x00262320u, 0x00003320u, 0x00173320u, 0x00004320u, 0x00005320u,
    0x00000030u, 0x00001730u, 0x00001630u, 0x00002630u, 0x00001530u, 0x00171530u, 0x00002530u, 0x00003530u,
    0x00001430u, 0x00171430u, 0x00161430u, 0x00261430u, 0x00002430u, 0x00172430u, 0x00003430u, 0x00004430u,
    0x00000040u, 0x00001740u, 0x00001640u, 0x00002640u, 0x00001540u, 0x00171540u, 0x00002540u, 0x00003540u,
    0x00000050u, 0x00001750u, 0x00001650u, 0x00002650u, 0x00000060u, 0x00001760u, 0x00000070u, 0x00000080u,
};

int sv24_dma_prepare_plans(const uint32_t *staging,
                           const sv24_slice_dma_plan_t *plans,
                           unsigned plan_count,
                           unsigned max_gap_pixels)
{
    uint16_t t0 = cpu_frt_count_get();
    build_t b[3]={{0}};
    unsigned i,k;
    uint32_t gap=max_gap_pixels*4u;
    if(!staging||!plans||plan_count>SV24_V03_MAX_SLICES)return -31;

    for(i=0;i<plan_count;i++){
        const sv24_slice_dma_plan_t *p=&plans[i];
        unsigned ry;
        if(!p->rows || p->rows>3u || p->y0+p->rows>SV24_TILES_Y) return -33;
        for(ry=0u;ry<p->rows;ry++){
            unsigned y=p->y0+ry;
            unsigned band=y/10u;
            unsigned ly=y-band*10u;
            uint32_t src_row=y*SV24_TILES_X*SV24_CELL_BYTES;
            uint32_t dst_row=band*SV24_VRAM_BANK_BYTES+ly*SV24_TILES_X*SV24_CELL_BYTES;
            unsigned ch;
            /* SH-2 has no cheap variable shift.  The old q extraction made
               GCC call ___lshrsi3_r0 six times per row.  The target is
               big-endian, so walk each uint32_t from its least-significant
               byte in memory (address +3) toward the most significant. */
            const uint8_t *bp=(const uint8_t *)&p->row_lo[ry] + 3u;
            for (ch=0u; ch<6u; ch++) {
                uint8_t q; uint32_t z; unsigned base=ch<<3;
                if (ch==4u) bp=(const uint8_t *)&p->row_hi[ry] + 3u;
                q=*bp--;
                if (!q) continue;
                z=run8_lut[q];
                while(z) {
                    unsigned d=(unsigned)(z&255u);
                    unsigned st=d&7u;
                    unsigned ln=d>>4;
                    unsigned x0=base+st;
                    if (x0 < SV24_TILES_X) {
                        if (x0+ln > SV24_TILES_X) ln=SV24_TILES_X-x0;
                        append_run(band,&b[band],staging,
                                   src_row+x0*SV24_CELL_BYTES,
                                   dst_row+x0*SV24_CELL_BYTES,
                                   ln*SV24_CELL_BYTES,gap);
                    }
                    z>>=8;
                }
            }
        }
    }
    for(k=0;k<3u;k++){
        scu_dma_xfer_t *t=tables[k];
        if(b[k].overflow) prepare_full_band(staging,k,1);
        else { if(b[k].n)t[b[k].n-1u].src|=SCU_DMA_INDIRECT_TABLE_END; band_count[k]=(uint16_t)b[k].n; band_bytes[k]=b[k].bytes; }
        sv24_dma_last_runs[k]=band_count[k];sv24_dma_last_bytes[k]=band_bytes[k];
        if(band_count[k]>sv24_dma_max_runs[k])sv24_dma_max_runs[k]=band_count[k];
        if(band_bytes[k]>sv24_dma_max_bytes[k])sv24_dma_max_bytes[k]=band_bytes[k];
    }
    prepared_staging=staging;
    { uint16_t dt=(uint16_t)(cpu_frt_count_get()-t0); sv24_dma_prepare_ticks_total+=dt; if(dt>sv24_dma_prepare_ticks_max)sv24_dma_prepare_ticks_max=dt; sv24_dma_prepare_calls++; }
    return 0;
}


int sv24_dma_prepare_maps(const uint32_t *staging, const sv24_v03_frame_t *fr,
                           unsigned max_gap_pixels)
{
    uint16_t t0=cpu_frt_count_get();
    build_t b[3]={{0}};
    unsigned si,k;
    uint32_t gap=max_gap_pixels*4u;
    if(!staging||!fr)return -31;
    for(si=0;si<fr->slice_count;si++){
        const sv24_v03_slice_t *sl=&fr->slice[si];
        const uint8_t *map;
        unsigned tc,mb,ry;
        if(sl->size<4u)return -33;
        tc=((unsigned)sl->data[0]<<8)|sl->data[1];
        mb=(tc+7u)>>3;
        if(tc!=(unsigned)sl->tile_rows*SV24_TILES_X||sl->size<4u+mb)return -33;
        map=sl->data+4u;
        for(ry=0;ry<sl->tile_rows;ry++){
            unsigned y=(unsigned)sl->tile_y0+ry;
            unsigned band=(y<10u)?0u:((y<20u)?1u:2u);
            unsigned ly=y-band*10u;
            uint32_t src_row=y*SV24_TILES_X*SV24_CELL_BYTES;
            uint32_t dst_row=band*SV24_VRAM_BANK_BYTES+ly*SV24_TILES_X*SV24_CELL_BYTES;
            const uint8_t *rp = map + (ry==0u ? 0u : (ry==1u ? 5u : 11u));
            unsigned ch;
            for(ch=0u;ch<6u;ch++){
                uint8_t q;
                uint32_t z;
                unsigned base=ch<<3;
                if(ry==1u) q=(uint8_t)((rp[ch]<<4)|(rp[ch+1u]>>4));
                else q=rp[ch];
                if(ch==5u) q &= 0xf0u; /* only four cells remain in a 44-cell row */
                if(!q) continue;
                z=run8_msb_lut[q];
                while(z){
                    unsigned d=(unsigned)(z&255u);
                    unsigned st=d&7u, ln=d>>4;
                    unsigned x0=base+st;
                    if(x0<SV24_TILES_X){
                        if(x0+ln>SV24_TILES_X) ln=SV24_TILES_X-x0;
                        append_run(band,&b[band],staging,
                                   src_row+x0*SV24_CELL_BYTES,
                                   dst_row+x0*SV24_CELL_BYTES,
                                   ln*SV24_CELL_BYTES,gap);
                    }
                    z>>=8;
                }
            }
        }
    }
    for(k=0;k<3u;k++){
        scu_dma_xfer_t *t=tables[k];
        if(b[k].overflow)prepare_full_band(staging,k,1);
        else{if(b[k].n)t[b[k].n-1u].src|=SCU_DMA_INDIRECT_TABLE_END;band_count[k]=(uint16_t)b[k].n;band_bytes[k]=b[k].bytes;}
        sv24_dma_last_runs[k]=band_count[k];sv24_dma_last_bytes[k]=band_bytes[k];
        if(band_count[k]>sv24_dma_max_runs[k])sv24_dma_max_runs[k]=band_count[k];
        if(band_bytes[k]>sv24_dma_max_bytes[k])sv24_dma_max_bytes[k]=band_bytes[k];
    }
    prepared_staging=staging;
    { uint16_t dt=(uint16_t)(cpu_frt_count_get()-t0); sv24_dma_prepare_ticks_total+=dt; if(dt>sv24_dma_prepare_ticks_max)sv24_dma_prepare_ticks_max=dt; sv24_dma_prepare_calls++; }
    return 0;
}


int sv24_dma_prepare_dirty_cells(const uint32_t *staging, const uint8_t *dirty,
                                  unsigned max_gap_pixels)
{
    uint16_t t0=cpu_frt_count_get();
    build_t b[3]={{0}};
    unsigned y,k;
    uint32_t gap=max_gap_pixels*4u;
    if(!staging||!dirty)return -31;

    /* Recovery-only path.  A byte per cell is intentionally simple here:
       dropped frames are rare, and avoiding another compressed-map parser
       makes recovery state independent of the frame whose deadline was
       missed.  Adjacent dirty cells are emitted as one run, then append_run
       applies the normal small-gap coalescing policy. */
    for(y=0u;y<SV24_TILES_Y;y++){
        unsigned band=y/10u;
        unsigned ly=y-band*10u;
        unsigned x=0u;
        uint32_t src_row=y*SV24_TILES_X*SV24_CELL_BYTES;
        uint32_t dst_row=band*SV24_VRAM_BANK_BYTES+ly*SV24_TILES_X*SV24_CELL_BYTES;
        const uint8_t *row=dirty+y*SV24_TILES_X;
        while(x<SV24_TILES_X){
            unsigned x0;
            while(x<SV24_TILES_X && !row[x])x++;
            if(x>=SV24_TILES_X)break;
            x0=x;
            while(x<SV24_TILES_X && row[x])x++;
            append_run(band,&b[band],staging,
                       src_row+x0*SV24_CELL_BYTES,
                       dst_row+x0*SV24_CELL_BYTES,
                       (x-x0)*SV24_CELL_BYTES,gap);
        }
    }
    for(k=0;k<3u;k++){
        scu_dma_xfer_t *t=tables[k];
        if(b[k].overflow)prepare_full_band(staging,k,1);
        else{if(b[k].n)t[b[k].n-1u].src|=SCU_DMA_INDIRECT_TABLE_END;band_count[k]=(uint16_t)b[k].n;band_bytes[k]=b[k].bytes;}
        sv24_dma_last_runs[k]=band_count[k];sv24_dma_last_bytes[k]=band_bytes[k];
        if(band_count[k]>sv24_dma_max_runs[k])sv24_dma_max_runs[k]=band_count[k];
        if(band_bytes[k]>sv24_dma_max_bytes[k])sv24_dma_max_bytes[k]=band_bytes[k];
    }
    prepared_staging=staging;
    { uint16_t dt=(uint16_t)(cpu_frt_count_get()-t0); sv24_dma_prepare_ticks_total+=dt; if(dt>sv24_dma_prepare_ticks_max)sv24_dma_prepare_ticks_max=dt; sv24_dma_prepare_calls++; }
    return 0;
}

int sv24_dma_upload_prepared_band(unsigned band)
{
    uint16_t t0; uint32_t n,bytes;if(band>2u||!prepared_staging)return -32;n=band_count[band];bytes=band_bytes[band];if(!n)return 0;
    t0=cpu_frt_count_get();
    scu_dma_level_wait(0);scu_dma_config_set(0,SCU_DMA_START_FACTOR_ENABLE,&band_handle[band],NULL);scu_dma_level_fast_start(0);scu_dma_level_wait(0);
    { uint16_t dt=(uint16_t)(cpu_frt_count_get()-t0); sv24_dma_upload_ticks_total+=dt; if(dt>sv24_dma_upload_ticks_max)sv24_dma_upload_ticks_max=dt; sv24_dma_upload_calls++; }
    sv24_dma_submissions++;sv24_dma_runs+=n;sv24_dma_bytes+=bytes;return 0;
}
int sv24_dma_upload_band_full(const uint32_t *staging,unsigned band){if(!staging||band>2u)return -22;prepare_full_band(staging,band,0);prepared_staging=staging;return sv24_dma_upload_prepared_band(band);}
