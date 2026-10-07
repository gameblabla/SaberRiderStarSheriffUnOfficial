/* Host oracle uses the shipped SH-2 decoder plus a software SCU palette job. */
#include "svm.h"
#include "sv24_frame_v03.h"
#include "sv24_slice.h"
int decode_packet(const uint8_t *data,uint32_t size,uint32_t *cells)
{
    svm_packet_t pk;sv24_v03_frame_t fr;int r=svm_packet_parse(data,size,&pk);
    if(r)return r;
    if((r=sv24_v03_frame_parse(pk.video,pk.video_size,&fr)))return r;
    for(unsigned i=0;i<fr.slice_count;i++) {
        sv24_v03_slice_t *sl=&fr.slice[i];
        if((r=sv24_decode_abs_slice_frame(sl->data,sl->size,sl->tile_y0,sl->tile_rows,cells)))return r;
    }
    if(pk.dsp_size) {
        const uint8_t *d=pk.dsp,*end=d+pk.dsp_size;
        if(pk.dsp_size<8||svm_be32(d)!=0x44535033)return -80;
        unsigned n=svm_be16(d+4);d+=8;
        if(n>8)return -81;
        for(unsigned i=0;i<n;i++) {
            if(end-d<4)return -82;
            unsigned id=svm_be16(d),colors=d[2];d+=4;
            if(id>=SV24_FRAME_TILES||id%44u>=sv24_output_tiles_x||id/44u>=sv24_output_tile_rows||(colors!=16&&colors!=24)||end-d<colors*4+64)return -83;
            id=id/44u*sv24_output_tiles_x+id%44u;
            const uint8_t *sel=d+colors*4;
            for(unsigned j=0;j<64;j++) {
                /* DSP masks low byte then rotates left by 8 between pixels. */
                unsigned at=(j&~3u)+((j+3u)&3u),index=sel[at];
                if(index>=colors)return -84;
                cells[id*64+j]=svm_be32(d+index*4);
            }
            d=sel+64;
        }
        if(d!=end)return -85;
    }
    return 0;
}
