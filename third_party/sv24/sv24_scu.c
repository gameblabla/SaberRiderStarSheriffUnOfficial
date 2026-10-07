#include <yaul.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "sv24_scu.h"
#include "sv24_slice.h"

#define DSP_MAGIC 0x44535033u /* DSP3 */
#define DSP_HEADER 8u
#define SCU_MAX_JOBS 8u
#define SCU_MAX_PAYLOAD (24u*4u + 64u)
#define SCU_SIDECAR_MAX (DSP_HEADER + SCU_MAX_JOBS * (4u + SCU_MAX_PAYLOAD))

volatile uint32_t sv24_scu_frames = 0;
volatile uint32_t sv24_scu_jobs = 0;
volatile uint32_t sv24_scu_errors = 0;
volatile uint32_t sv24_scu_last_jobs = 0;

static uint8_t scu_sidecar_h[SCU_SIDECAR_MAX] __aligned(32);
static uint16_t active_tiles[SCU_MAX_JOBS];
#ifdef PLAT_SATURN
/* The SCU cannot DMA into low work RAM (SCU manual, DMA restrictions).
 * Decode its few owned tiles into high RAM, then copy at the SH-2 barrier. */
static uint32_t scu_pixels[SCU_MAX_JOBS][SV24_TILE_PIXELS] __aligned(32);
#endif
static uint16_t active_count;
static uint8_t active_running;
static const uint32_t scu_copy_program[] __aligned(4) = {
#include "sv24_scu_a16a24.inc"
};

static uint16_t be16(const uint8_t *p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static uint32_t be32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}

int sv24_scu_init(void)
{
    scu_dsp_program_stop();scu_dsp_program_clear();
    scu_dsp_program_load(scu_copy_program,(uint32_t)(sizeof(scu_copy_program)/sizeof(scu_copy_program[0])));
    active_count=0u;active_running=0u;
    sv24_scu_frames=sv24_scu_jobs=sv24_scu_errors=sv24_scu_last_jobs=0;
    return 0;
}

int sv24_scu_begin(const uint8_t *sidecar,uint32_t size,uint32_t *staging)
{
    uint16_t count;uint32_t j;const uint8_t *p,*end;
    uint32_t desc[1u+SCU_MAX_JOBS*4u] __aligned(4);
    if(active_running){sv24_scu_errors++;return -84;}
    active_count=0u;sv24_scu_last_jobs=0u;
    if(!size)return 0;
    if(!sidecar||!staging||size<DSP_HEADER||size>SCU_SIDECAR_MAX||(be32(sidecar)!=DSP_MAGIC && be32(sidecar)!=0x44535032u)){sv24_scu_errors++;return -80;}
    count=be16(sidecar+4);
    if(count==0u||count>SCU_MAX_JOBS){sv24_scu_errors++;return -81;}
    memcpy(scu_sidecar_h,sidecar,size);
    cpu_cache_area_purge(scu_sidecar_h,size);
    desc[0]=(uint32_t)(count-1u);p=scu_sidecar_h+DSP_HEADER;end=scu_sidecar_h+size;
    for(j=0;j<(uint32_t)count;j++){
        uint16_t tile;uint32_t colors,payload_bytes,total_words;
        if((uint32_t)(end-p)<4u){sv24_scu_errors++;return -82;}
        tile=be16(p);
        /* Released V28.1 mux writes DSP2 with a reserved zero word and
         * fixed A24 palettes. Later source snapshots use DSP3's color byte. */
        colors=be32(sidecar)==0x44535032u?24u:(uint32_t)p[2];
        if(colors!=16u && colors!=24u){sv24_scu_errors++;return -83;}
        payload_bytes=colors*4u+64u;total_words=colors+16u;
        if((uint32_t)(end-p)<4u+payload_bytes){sv24_scu_errors++;return -85;}
        if(tile>=SV24_FRAME_TILES||tile%SV24_TILES_X>=sv24_output_tiles_x||tile/SV24_TILES_X>=sv24_output_tile_rows){sv24_scu_errors++;return -86;}
        for (uint32_t k=0;k<j;k++) if(active_tiles[k]==tile)return -89;
        for (uint32_t k=0;k<64u;k++) if(p[4u+colors*4u+k]>=colors)return -90;
        active_tiles[j]=tile;
        desc[1u+j*4u]=(uint32_t)((uintptr_t)(p+4)>>2);
#ifdef PLAT_SATURN
        desc[1u+j*4u+1u]=(uint32_t)((uintptr_t)scu_pixels[j]>>2);
#else
        desc[1u+j*4u+1u]=(uint32_t)((uintptr_t)(staging+(uint32_t)tile*SV24_TILE_PIXELS)>>2);
#endif
        desc[1u+j*4u+2u]=colors;
        desc[1u+j*4u+3u]=total_words;
        p+=4u+payload_bytes;
    }
    if(p!=end){sv24_scu_errors++;return -87;}
    scu_dsp_program_stop();
    scu_dsp_data_write(DSP_RAM_PAGE_0,0,desc,1u+(uint32_t)count*4u);
    scu_dsp_program_pc_set(0);
    active_count=count;active_running=1u;
    scu_dsp_program_start();
    return 0;
}

int sv24_scu_end(uint32_t *staging)
{
    uint32_t j;
    if(!active_running){sv24_scu_frames++;return 0;}
    /* A corrupted descriptor must not turn playback failure into a hang.
     * /32 in the game, /128 standalone: 200000 ticks allows ample DMA time. */
    uint16_t last=cpu_frt_count_get();uint32_t ticks=0;
    while(!scu_dsp_program_end()) {
        uint16_t now=cpu_frt_count_get();ticks+=(uint16_t)(now-last);last=now;
        if(ticks>200000u) {
            scu_dsp_program_stop();active_count=0;active_running=0;
            sv24_scu_errors++;return -91;
        }
    }
    for(j=0;j<(uint32_t)active_count;j++){
        uint16_t tile=active_tiles[j];
#ifdef PLAT_SATURN
        tile=(uint16_t)((tile/SV24_TILES_X)*sv24_output_tiles_x+tile%SV24_TILES_X);
        memcpy((void *)(CPU_CACHE_THROUGH | (uintptr_t)(staging+(uint32_t)tile*SV24_TILE_PIXELS)),
               (const void *)(CPU_CACHE_THROUGH | (uintptr_t)scu_pixels[j]), sizeof scu_pixels[j]);
#endif
        cpu_cache_area_purge(staging+(uint32_t)tile*SV24_TILE_PIXELS,SV24_TILE_PIXELS*sizeof(uint32_t));
    }
    sv24_scu_jobs+=active_count;sv24_scu_last_jobs=active_count;sv24_scu_frames++;
    active_count=0u;active_running=0u;return 0;
}

int sv24_scu_apply(const uint8_t *sidecar,uint32_t size,uint32_t *staging,uint32_t budget_jobs)
{
    uint16_t count=size>=DSP_HEADER?be16(sidecar+4):0u;int r;
    if(size && count>budget_jobs){sv24_scu_errors++;return -88;}
    r=sv24_scu_begin(sidecar,size,staging);if(r)return r;return sv24_scu_end(staging);
}
