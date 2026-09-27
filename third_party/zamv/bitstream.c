#include "zamv_internal.h"
#include <stdlib.h>
#include <string.h>

const zamv_se_lut_entry_t zamv_se_lut7[128] = {
    { 0,0}, { 0,0}, { 0,0}, { 0,0}, { 0,0}, { 0,0}, { 0,0}, { 0,0},
    { 4,7}, {-4,7}, { 5,7}, {-5,7}, { 6,7}, {-6,7}, { 7,7}, {-7,7},
    { 2,5}, { 2,5}, { 2,5}, { 2,5}, {-2,5}, {-2,5}, {-2,5}, {-2,5},
    { 3,5}, { 3,5}, { 3,5}, { 3,5}, {-3,5}, {-3,5}, {-3,5}, {-3,5},
    { 1,3}, { 1,3}, { 1,3}, { 1,3}, { 1,3}, { 1,3}, { 1,3}, { 1,3},
    { 1,3}, { 1,3}, { 1,3}, { 1,3}, { 1,3}, { 1,3}, { 1,3}, { 1,3},
    {-1,3}, {-1,3}, {-1,3}, {-1,3}, {-1,3}, {-1,3}, {-1,3}, {-1,3},
    {-1,3}, {-1,3}, {-1,3}, {-1,3}, {-1,3}, {-1,3}, {-1,3}, {-1,3},
    { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1},
    { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1},
    { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1},
    { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1},
    { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1},
    { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1},
    { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1},
    { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}, { 0,1}
};

const zamv_level_lut_entry_t zamv_level_lut7[128] = {
    { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2},
    { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2},
    { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2},
    { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2}, { 1,2},
    {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2},
    {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2},
    {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2},
    {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2}, {-1,2},
    { 2,3}, { 2,3}, { 2,3}, { 2,3}, { 2,3}, { 2,3}, { 2,3}, { 2,3},
    { 2,3}, { 2,3}, { 2,3}, { 2,3}, { 2,3}, { 2,3}, { 2,3}, { 2,3},
    {-2,3}, {-2,3}, {-2,3}, {-2,3}, {-2,3}, {-2,3}, {-2,3}, {-2,3},
    {-2,3}, {-2,3}, {-2,3}, {-2,3}, {-2,3}, {-2,3}, {-2,3}, {-2,3},
    { 3,5}, { 3,5}, { 3,5}, { 3,5}, {-3,5}, {-3,5}, {-3,5}, {-3,5},
    { 4,5}, { 4,5}, { 4,5}, { 4,5}, {-4,5}, {-4,5}, {-4,5}, {-4,5},
    { 5,7}, {-5,7}, { 6,7}, {-6,7}, { 7,7}, {-7,7}, { 8,7}, {-8,7},
    { 0,0}, { 0,0}, { 0,0}, { 0,0}, { 0,0}, { 0,0}, { 0,0}, { 0,0}
};

int32_t zr_get_se_slow(zamv_reader_t *r) {
    uint32_t z=zr_get_ue(r);
    return (z&1)?(int32_t)((z+1)/2):-(int32_t)(z/2);
}

int32_t zr_get_level_slow(zamv_reader_t *r) {
    uint32_t m;
    if(zr_get_bit(r)==0)m=1;
    else if(zr_get_bit(r)==0)m=2;
    else if(zr_get_bit(r)==0)m=3+zr_get_bit(r);
    else if(zr_get_bit(r)==0)m=5+zr_get_bits(r,2);
    else m=9+zr_get_ue(r);
    {uint32_t sign=zr_get_bit(r);if(r->error)return 0;return sign?-(int32_t)m:(int32_t)m;}
}





static int reserve(zamv_buf_t *b, size_t extra) {
    if (b->size + extra <= b->cap) return 0;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->size + extra) nc *= 2;
    uint8_t *p = (uint8_t*)realloc(b->data, nc);
    if (!p) return -1;
    b->data = p; b->cap = nc; return 0;
}
void zb_init(zamv_buf_t *b) { memset(b, 0, sizeof(*b)); }
void zb_free(zamv_buf_t *b) { free(b->data); memset(b,0,sizeof(*b)); }
static int emit_byte(zamv_buf_t *b, uint8_t x) { if (reserve(b,1)<0) return -1; b->data[b->size++]=x; return 0; }
int zb_put_bits(zamv_buf_t *b, uint32_t v, unsigned n) {
    if (!n) return 0;
    v &= n == 32 ? 0xffffffffu : ((1u << n)-1u);
    b->bitbuf = (b->bitbuf << n) | v; b->bits += n;
    while (b->bits >= 8) { unsigned sh=b->bits-8; if (emit_byte(b,(uint8_t)(b->bitbuf>>sh))<0) return -1; b->bits-=8; if (b->bits) b->bitbuf &= ((1ull<<b->bits)-1ull); else b->bitbuf=0; }
    return 0;
}
int zb_put_bit(zamv_buf_t *b, unsigned v) { return zb_put_bits(b,v,1); }
int zb_put_ue(zamv_buf_t *b, uint32_t v) {
    uint32_t x=v+1; unsigned n=0; for(uint32_t t=x;t>>=1;) n++;
    for(unsigned i=0;i<n;i++) if(zb_put_bit(b,0)<0) return -1;
    return zb_put_bits(b,x,n+1);
}
int zb_put_se(zamv_buf_t *b, int32_t v) { uint32_t z = v<=0 ? (uint32_t)(-v)*2u : (uint32_t)v*2u-1u; return zb_put_ue(b,z); }

/* Residual coefficient token: optimized for the overwhelmingly common small levels.
   mag 1: 0s; 2: 10s; 3..4: 110xs; 5..8: 1110xxs; >=9: 1111 + ue(mag-9) + sign. */
int zb_put_level(zamv_buf_t *b, int32_t v) {
    uint32_t m=(uint32_t)(v<0?-v:v), sign=v<0; if(!m)return -1;
    if(m==1){if(zb_put_bit(b,0)<0)return-1;return zb_put_bit(b,sign);}
    if(m==2){if(zb_put_bits(b,2,2)<0)return-1;return zb_put_bit(b,sign);}
    if(m<=4){if(zb_put_bits(b,6,3)<0||zb_put_bit(b,m-3)<0)return-1;return zb_put_bit(b,sign);}
    if(m<=8){if(zb_put_bits(b,14,4)<0||zb_put_bits(b,m-5,2)<0)return-1;return zb_put_bit(b,sign);}
    if(zb_put_bits(b,15,4)<0||zb_put_ue(b,m-9)<0)return-1;
    return zb_put_bit(b,sign);
}

int zb_flush(zamv_buf_t *b) { if (b->bits) { unsigned pad=8-b->bits; return zb_put_bits(b,0,pad); } return 0; }
