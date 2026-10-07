#include "sv24_vdp2_layout.h"
static uint16_t be16(const uint8_t*p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
uint32_t sv24_vdp2_tile_byte_offset(uint16_t id){
    unsigned y=id/SV24_TILES_X,x=id%SV24_TILES_X,band=y/SV24_BAND_TILE_ROWS,ly=y%SV24_BAND_TILE_ROWS;
    uint32_t base=band==0?SV24_VRAM_A0_OFFSET:band==1?SV24_VRAM_A1_OFFSET:SV24_VRAM_B0_OFFSET;
    return base+(ly*SV24_TILES_X+x)*SV24_CELL_BYTES;
}
uint16_t sv24_vdp2_tile_charno(uint16_t id){return (uint16_t)(sv24_vdp2_tile_byte_offset(id)>>5);}
void sv24_vdp2_build_pnd_page(uint16_t *d){
    unsigned y,x;for(y=0;y<SV24_PND_PAGE_TILES;y++)for(x=0;x<SV24_PND_PAGE_TILES;x++){
        unsigned e=(y*SV24_PND_PAGE_TILES+x)*2u;uint16_t cn=0;
        if(x<SV24_TILES_X&&y<SV24_TILES_Y)cn=sv24_vdp2_tile_charno((uint16_t)(y*SV24_TILES_X+x));
        d[e]=0;d[e+1]=cn;
    }
}
int sv24_vdp2_make_slice_dma_plan(const uint8_t *s,uint32_t sz,uint16_t y0,uint8_t rows,sv24_dma_plan_t *pl){
    unsigned tc,cc,mb,ti=0,ry,tx,run_open=0;const uint8_t*map;sv24_dma_run_t*r=0;
    if(!s||!pl||sz<4u||!rows||y0+rows>SV24_TILES_Y)return -1;
    tc=be16(s);cc=be16(s+2);if(tc!=(unsigned)rows*SV24_TILES_X||cc>tc)return -2;
    mb=(tc+7u)>>3;if(sz<4u+mb)return -3;map=s+4;pl->count=0;
    for(ry=0;ry<rows;ry++)for(tx=0;tx<SV24_TILES_X;tx++,ti++){
        unsigned ch=(map[ti>>3]>>(7u-(ti&7u)))&1u;
        uint16_t id=(uint16_t)((y0+ry)*SV24_TILES_X+tx);
        if(ch){uint32_t off=sv24_vdp2_tile_byte_offset(id);
            if(run_open && r->src_cell+r->cell_count==id && r->vram_byte_offset+(uint32_t)r->cell_count*SV24_CELL_BYTES==off)r->cell_count++;
            else{if(pl->count>=SV24_DMA_MAX_RUNS)return -4;r=&pl->run[pl->count++];r->src_cell=id;r->cell_count=1;r->vram_byte_offset=off;run_open=1;}
        }else run_open=0;
    }
    return 0;
}
