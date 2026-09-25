/* SH-2 streaming decoder for the 4-bit predictor used by celeriyacon/scspadpcm.
 * The original SCSP/DSP demo addresses headers/data with 16-bit playback lengths.
 * Our container keeps the same predictor and 16-sample blocks, but interleaves
 * blocks and uses 32-bit counts so long game samples are not limited to 64 KiB. */
#include "scsp_adpcm_sat.h"
#include <string.h>
#include <stdint.h>

#define SAT_SNDRAM_BASE 0x25A00000u
#define SAT_SNDRAM_END  0x25A80000u

static bool in_sound_ram(const void *p)
{
    uintptr_t a=(uintptr_t)p; return a>=SAT_SNDRAM_BASE && a<SAT_SNDRAM_END;
}
static uint8_t src8(const uint8_t *p, uint32_t i, bool sound_ram)
{
    if (!sound_ram) return p[i];
    uintptr_t a=(uintptr_t)p+i;
    uint16_t w=*(volatile const uint16_t *)(a & ~(uintptr_t)1);
    return (a & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}
static uint32_t src32le(const uint8_t *p,uint32_t off,bool sound_ram)
{
    return (uint32_t)src8(p,off,sound_ram) | ((uint32_t)src8(p,off+1,sound_ram)<<8) |
           ((uint32_t)src8(p,off+2,sound_ram)<<16) | ((uint32_t)src8(p,off+3,sound_ram)<<24);
}

static const int16_t filter_tab[16][3] = {
 {1920,0,0},{0,0,0},{3680,-1664,0},{3136,-1760,0},{3904,-1920,0},{3136,-2368,1248},{2976,-1472,544},{3680,-2880,1280},
 {3168,-2592,1440},{1728,-832,896},{1856,288,-128},{2144,-288,-512},{2976,-992,256},{2144,-480,256},{4095,-2048,-32},{3616,-3584,1792}
};
static const int16_t scale_tab[16]={1,2,3,5,9,16,28,48,84,147,256,446,776,1351,2352,4095};
static int32_t sx(int bits,int32_t v){ uint32_t u=(uint32_t)v << (32-bits); return (int32_t)u >> (32-bits); }
static int32_t prediction(SatAdpDecoder *d,unsigned f){ int32_t p=0; for(int j=0;j<3;j++){ p += (int32_t)(((int64_t)d->hist[j]*filter_tab[f][j])>>12); p=sx(26,p);} return p; }
static void decode_block(SatAdpDecoder *d){
    const uint8_t *b=d->blocks+d->block_index*d->block_bytes; uint8_t h=src8(b,0,d->sound_ram); unsigned sh=h&15, f=h>>4;
    for(int i=0;i<16;i++){
        unsigned n=(src8(b,1+i/d->per_byte,d->sound_ram)>>((i%d->per_byte)*d->bits))&((1u<<d->bits)-1); int32_t p=prediction(d,f);
        int q; if(d->bits==1){ static const int t[2]={3,-3}; q=t[n&1]; }
        else if(d->bits==2){ static const int t[4]={3,7,-7,-3}; q=t[n&3]; } else q=sx(4,(int32_t)n);
        int32_t v=p+(int32_t)(((int64_t)q*524288*scale_tab[sh])>>12); v=sx(26,v)<<1;
        d->hist[2]=d->hist[1]; d->hist[1]=d->hist[0]; d->hist[0]=v; d->decoded[i]=(int16_t)(v>>8);
    }
    d->block_index++; d->block_pos=0;
}
bool sat_adp_init(SatAdpDecoder *d,const void *data,size_t size){
    const uint8_t *p=data; if(!d||!p||size<32) return false; bool sr=in_sound_ram(p);
    if(src8(p,0,sr)!='S'||src8(p,1,sr)!='A'||src8(p,2,sr)!='D'||src8(p,3,sr)!='P'||src32le(p,4,sr)!=1) return false;
    uint32_t samples=src32le(p,12,sr), blocks=src32le(p,16,sr), bytes=src32le(p,20,sr); uint8_t format=src8(p,24,sr), block_bytes=src8(p,25,sr);
    if(format>2) return false; uint8_t bits=(uint8_t)(4u>>format), per_byte=(uint8_t)(8u/bits), expect=(uint8_t)(1u+16u/per_byte);
    if(block_bytes!=expect||bytes!=blocks*(uint32_t)block_bytes||32u+bytes>size) return false;
    memset(d,0,sizeof *d); d->blocks=p+32; d->sample_count=samples; d->block_count=blocks; d->format=format; d->bits=bits; d->per_byte=per_byte; d->block_bytes=block_bytes; d->sound_ram=sr; return true;
}
void sat_adp_rewind(SatAdpDecoder *d){ const uint8_t *b=d->blocks; uint32_t s=d->sample_count,n=d->block_count; uint8_t f=d->format,bits=d->bits,pb=d->per_byte,bb=d->block_bytes; bool sr=d->sound_ram; memset(d,0,sizeof *d); d->blocks=b; d->sample_count=s; d->block_count=n; d->format=f; d->bits=bits; d->per_byte=pb; d->block_bytes=bb; d->sound_ram=sr; }
bool sat_adp_next(SatAdpDecoder *d,int16_t *sample){
    if(!d||d->sample_pos>=d->sample_count) return false; if(d->block_pos==0){ if(d->block_index>=d->block_count) return false; decode_block(d); }
    *sample=d->decoded[d->block_pos++]; d->sample_pos++; if(d->block_pos==16) d->block_pos=0; return true;
}
uint32_t sat_adp_samples(const void *data,size_t size){ const uint8_t *p=data; if(!p||size<32)return 0; bool sr=in_sound_ram(p); return src8(p,0,sr)=='S'&&src8(p,1,sr)=='A'&&src8(p,2,sr)=='D'&&src8(p,3,sr)=='P'?src32le(p,12,sr):0; }
