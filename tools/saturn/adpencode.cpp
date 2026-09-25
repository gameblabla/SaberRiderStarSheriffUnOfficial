/* Saturn SFX encoder adapted from celeriyacon/scspadpcm (2021).
 * The predictor/quantizer is the original 4-bit codec.  Saber Rider stores
 * blocks interleaved (header + 8 data bytes) with 32-bit counts, removing the
 * demo driver's 16-bit SCSP-address/length ceiling while keeping the codec. */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const int filter_tab[16][3] = {
 {1920,0,0},{0,0,0},{3680,-1664,0},{3136,-1760,0},{3904,-1920,0},{3136,-2368,1248},{2976,-1472,544},{3680,-2880,1280},
 {3168,-2592,1440},{1728,-832,896},{1856,288,-128},{2144,-288,-512},{2976,-992,256},{2144,-480,256},{4095,-2048,-32},{3616,-3584,1792}
};
static const int scale_tab[16]={1,2,3,5,9,16,28,48,84,147,256,446,776,1351,2352,4095};
struct Ctx { int bits=4, per_byte=2; int32_t s[3]{}; };
static int32_t sx(int bits,int32_t v){ uint32_t u=(uint32_t)v << (32-bits); return (int32_t)u >> (32-bits); }
static int32_t predicted(const Ctx& c,int f){ int32_t p=0; for(int j=0;j<3;j++){ p += (int32_t)(((int64_t)c.s[j]*filter_tab[f][j])>>12); p=sx(26,p); } return p; }
static int32_t dec(const Ctx& c,int sh,int32_t p,unsigned n){
    int q; if(c.bits==1){ static const int t[2]={3,-3}; q=t[n&1]; }
    else if(c.bits==2){ static const int t[4]={3,7,-7,-3}; q=t[n&3]; }
    else q=sx(4,(int32_t)n);
    int32_t r=p + (int32_t)(((int64_t)q*524288*scale_tab[sh])>>12); return sx(26,r)<<1; }
static void push(Ctx& c,int32_t v){c.s[2]=c.s[1];c.s[1]=c.s[0];c.s[0]=v;}
static void encblock(Ctx& c,const int16_t *in,uint8_t *out){
    int64_t best=INT64_MAX; int bf=0,bs=0; uint8_t bn[16]{}; Ctx bc{};
    for(int f=0;f<16;f++) for(int sh=0;sh<16;sh++){
        Ctx t=c; int64_t err=0; uint8_t nn[16]{};
        for(int i=0;i<16;i++){
            int32_t p=predicted(t,f), be=INT32_MAX,bv=0; unsigned bi=0;
            for(unsigned n=0;n<(1u<<c.bits);n++){ int32_t v=dec(t,sh,p,n), e=std::abs((int)in[i]-(int)(v>>8)); if(e<be){be=e;bv=v;bi=n;} }
            nn[i]=(uint8_t)bi; push(t,bv); err += (int64_t)be*(16 + std::abs(i*2-15));
        }
        if(err<best){best=err;bf=f;bs=sh;memcpy(bn,nn,16);bc=t;}
    }
    c=bc; out[0]=(uint8_t)((bf<<4)|bs); int db=16/c.per_byte; memset(out+1,0,(size_t)db); for(int i=0;i<16;i++) out[1+i/c.per_byte]|=(uint8_t)(bn[i]<<((i%c.per_byte)*c.bits));
}
static void wr32(FILE *f,uint32_t v){ uint8_t b[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; fwrite(b,1,4,f); }
int main(int ac,char **av){
    if(ac!=5){fprintf(stderr,"usage: %s FORMAT(0=4bit,1=2bit,2=1bit) RATE input.s16le output.sadp\n",av[0]);return 2;}
    int format=atoi(av[1]); if(format<0||format>2){fprintf(stderr,"bad format\n");return 2;}
    int rate=atoi(av[2]); FILE *in=fopen(av[3],"rb"); if(!in){perror(av[2]);return 1;} fseek(in,0,SEEK_END); long bytes=ftell(in); fseek(in,0,SEEK_SET);
    if(bytes<0 || (bytes&1)){fprintf(stderr,"bad raw PCM size\n");return 1;} uint32_t samples=(uint32_t)(bytes/2), blocks=(samples+15)/16;
    std::vector<int16_t> pcm(samples? samples:1); if(samples && fread(pcm.data(),2,samples,in)!=samples){perror("read");return 1;} fclose(in);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    for(auto &v:pcm) v=(int16_t)__builtin_bswap16((uint16_t)v);
#endif
    FILE *out=fopen(av[4],"wb"); if(!out){perror(av[4]);return 1;}
    int bits=4>>format, per_byte=8/bits, block_bytes=1+16/per_byte;
    fwrite("SADP",1,4,out); wr32(out,1); wr32(out,(uint32_t)rate); wr32(out,samples); wr32(out,blocks); wr32(out,blocks*(uint32_t)block_bytes); fputc(format,out); fputc(block_bytes,out); fputc(0,out); fputc(0,out); wr32(out,0);
    Ctx c; c.bits=bits; c.per_byte=per_byte; int16_t ib[16]{}; uint8_t ob[9]; uint32_t pos=0;
    for(uint32_t b=0;b<blocks;b++) { for(int i=0;i<16;i++){ if(pos<samples) ib[i]=pcm[pos++]; else ib[i]=pos?pcm[samples-1]:0; } encblock(c,ib,ob); fwrite(ob,1,(size_t)block_bytes,out); }
    fclose(out); return 0;
}
