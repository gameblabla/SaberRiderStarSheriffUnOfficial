#ifndef ZAMV_INTERNAL_H
#define ZAMV_INTERNAL_H
#include "zamv.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef ZAMV_USE_SH4ZAM
#include <sh4zam/shz_sh4zam.h>
#endif

#define ZAMV_FRAME_KEY 0
#define ZAMV_FRAME_INTER 1

#define ZAMV_MB_SKIP 0
#define ZAMV_MB_INTER 1
#define ZAMV_MB_INTRA_DC 2
#define ZAMV_MB_FILL 3

#define ZAMV_REF_LAST 0
#define ZAMV_REF_GOLDEN 1

#define ZAMV_INTRA_DC 0
#define ZAMV_INTRA_V 1
#define ZAMV_INTRA_H 2
#define ZAMV_INTRA_TM 3

#define ZAMV_BLOCK_ZERO 0
#define ZAMV_BLOCK_DC 1
#define ZAMV_BLOCK_FULL 2

#define ZAMV_FRAME_HEADER_BYTES 16

typedef struct zamv_buf {
    uint8_t *data;
    size_t size, cap;
    uint64_t bitbuf;
    unsigned bits;
} zamv_buf_t;

typedef struct zamv_qblock {
    int16_t qc[16];
    uint8_t cls;
    uint8_t last;
} zamv_qblock_t;

extern const uint8_t zamv_zigzag[16];
typedef struct zamv_reader {
    const uint8_t *p, *end;
    /* Decoder reads are capped at 24 bits. Keeping this cache 32-bit avoids
       expensive 64-bit shifts/masks on SH-4 while still allowing up to 31
       cached bits after a byte refill. */
    uint32_t bitbuf;
    unsigned bits;
    int error;
} zamv_reader_t;

void zb_init(zamv_buf_t *b);
void zb_free(zamv_buf_t *b);
int zb_put_bits(zamv_buf_t *b, uint32_t v, unsigned n);
int zb_put_bit(zamv_buf_t *b, unsigned v);
int zb_put_ue(zamv_buf_t *b, uint32_t v);
int zb_put_se(zamv_buf_t *b, int32_t v);
int zb_put_level(zamv_buf_t *b, int32_t v);
int zb_flush(zamv_buf_t *b);

/* Decoder entropy primitives are header-inline because they dominate residual
   parsing. This also lets SH-4 avoid call/return overhead for individual bits. */
static inline void zr_init(zamv_reader_t *r, const uint8_t *p, size_t n) {
    r->p=p; r->end=p+n; r->bitbuf=0; r->bits=0; r->error=0;
}
static inline int zr_ensure(zamv_reader_t *r, unsigned n) {
    if(n>24u){r->error=1;return -1;}
    while(r->bits<n){
        if(r->p>=r->end){r->error=1;return -1;}
        r->bitbuf=(r->bitbuf<<8)|*r->p++;
        r->bits+=8;
    }
    return 0;
}
static inline uint32_t zr_get_bits(zamv_reader_t *r, unsigned n) {
    if(!n)return 0;
    if(zr_ensure(r,n)<0)return 0;
    unsigned sh=r->bits-n;
    uint32_t v=(r->bitbuf>>sh)&((1u<<n)-1u);
    r->bits-=n;
    /* No accumulator mask is needed: unread data is defined solely by r->bits
       and always occupies the low portion of bitbuf. */
    return v;
}
static inline uint32_t zr_get_bit(zamv_reader_t *r) {
    if(!r->bits){
        if(r->p>=r->end){r->error=1;return 0;}
        r->bitbuf=*r->p++; r->bits=8;
    }
    r->bits--;
    return (r->bitbuf>>r->bits)&1u;
}
static inline uint32_t zr_get_ue(zamv_reader_t *r) {
    unsigned z=0;
    while(!r->error&&zr_get_bit(r)==0){if(++z>31){r->error=1;return 0;}}
    if(r->error)return 0;
    {uint32_t tail=z?zr_get_bits(r,z):0; return ((1u<<z)|tail)-1u;}
}
typedef struct zamv_se_lut_entry { int8_t value; uint8_t bits; } zamv_se_lut_entry_t;
extern const zamv_se_lut_entry_t zamv_se_lut7[128];
int32_t zr_get_se_slow(zamv_reader_t *r);
static inline int32_t zr_get_se(zamv_reader_t *r) {
    unsigned avail=r->bits+(unsigned)((r->end-r->p)*8u);
    if(avail>=7u && zr_ensure(r,7)==0){
        unsigned sh=r->bits-7u;
        const zamv_se_lut_entry_t e=zamv_se_lut7[(r->bitbuf>>sh)&127u];
        if(e.bits){r->bits-=e.bits;return (int32_t)e.value;}
    }
    return zr_get_se_slow(r);
}
typedef struct zamv_level_lut_entry {
    int8_t level;
    uint8_t bits;
} zamv_level_lut_entry_t;

/* Complete decode of the common level tokens from a single 7-bit lookahead.
   The entry includes sign, so magnitudes 1..8 require one table load and one
   bit-count decrement. The 1111 escape prefix has bits=0 and falls through to
   the existing Exp-Golomb path. 256 bytes total: deliberately small enough for
   the SH-4 D-cache while removing several dependent branches per coefficient. */
extern const zamv_level_lut_entry_t zamv_level_lut7[128];
int32_t zr_get_level_slow(zamv_reader_t *r);
static inline int32_t zr_get_level(zamv_reader_t *r) {
    unsigned avail=r->bits+(unsigned)((r->end-r->p)*8u);
    if(avail>=7u && zr_ensure(r,7)==0){
        unsigned sh=r->bits-7u;
        const zamv_level_lut_entry_t e=zamv_level_lut7[(r->bitbuf>>sh)&127u];
        if(e.bits){r->bits-=e.bits;return (int32_t)e.level;}
    }
    return zr_get_level_slow(r);
}

/* Default codec rounding is IEEE round-to-nearest, ties-to-even. Avoid lrintf()
   in hot Dreamcast paths: a float->int cast maps naturally to SH-4 FTRC, then
   only the half-way correction remains. Valid for the bounded codec ranges. */
static inline int32_t zamv_round_even_f32(float x) {
    int32_t i=(int32_t)x;
    float f=x-(float)i;
    if(f>0.5f || (f==0.5f && ((uint32_t)i&1u))) ++i;
    else if(f< -0.5f || (f== -0.5f && ((uint32_t)i&1u))) --i;
    return i;
}
static inline int32_t zamv_round_div4_even(int32_t n) {
    uint32_t a=(uint32_t)(n<0?-n:n); /* codec range is far from INT32_MIN */
    int32_t q=(int32_t)(a>>2);
    uint32_t r=a&3u;
    if(r>2u || (r==2u && (q&1))) ++q;
    return n<0?-q:q;
}

void *zamv_copy_bytes(void *dst, const void *src, size_t n);
/* Tiny row operations must be inline: an out-of-line wrapper would reintroduce a
   call/return around the 1-4 actual SH-4 loads/stores we are trying to optimize. */
static inline void zamv_copy_row4(void *d,const void *s){
#ifdef ZAMV_USE_SH4ZAM
    uintptr_t a=(uintptr_t)d|(uintptr_t)s;
# ifdef SHZ_HAS_TINY_FIXED_COPY
    if((a&3u)==0)shz_memcpy4_1(d,s);else if((a&1u)==0)shz_memcpy2_2(d,s);else shz_memcpy1_4(d,s);
# else
    if((a&3u)==0)shz_memcpy4(d,s,4);else if((a&1u)==0)shz_memcpy2(d,s,4);else shz_memcpy1(d,s,4);
# endif
#else
    memcpy(d,s,4);
#endif
}
static inline void zamv_copy_row8(void *d,const void *s){
#ifdef ZAMV_USE_SH4ZAM
    uintptr_t a=(uintptr_t)d|(uintptr_t)s;
# ifdef SHZ_HAS_TINY_FIXED_COPY
    if((a&3u)==0)shz_memcpy4_2(d,s);else if((a&1u)==0)shz_memcpy2_4(d,s);else shz_memcpy1_8(d,s);
# else
    if((a&3u)==0)shz_memcpy4(d,s,8);else if((a&1u)==0)shz_memcpy2(d,s,8);else shz_memcpy1(d,s,8);
# endif
#else
    memcpy(d,s,8);
#endif
}
static inline void zamv_copy_row16(void *d,const void *s){
#ifdef ZAMV_USE_SH4ZAM
    uintptr_t a=(uintptr_t)d|(uintptr_t)s;
# ifdef SHZ_HAS_TINY_FIXED_COPY
    if((a&3u)==0)shz_memcpy4_4(d,s);else if((a&1u)==0)shz_memcpy2_8(d,s);else shz_memcpy1_16(d,s);
# else
    if((a&1u)==0)shz_memcpy2_8(d,s);else shz_memcpy1(d,s,16);
# endif
#else
    memcpy(d,s,16);
#endif
}
static inline uint32_t zamv_repeat_byte32(uint8_t v){uint32_t x=(uint32_t)v;x|=x<<8;x|=x<<16;return x;}
static inline void zamv_fill_row4(void *d,uint8_t v){
#if defined(ZAMV_USE_SH4ZAM) && defined(SHZ_HAS_TINY_FIXED_FILL)
    if(!((uintptr_t)d&3u)){shz_memset4_1(d,zamv_repeat_byte32(v));return;}
#endif
    memset(d,v,4);
}
static inline void zamv_fill_row8(void *d,uint8_t v){
#if defined(ZAMV_USE_SH4ZAM) && defined(SHZ_HAS_TINY_FIXED_FILL)
    if(!((uintptr_t)d&3u)){shz_memset4_2(d,zamv_repeat_byte32(v));return;}
#endif
    memset(d,v,8);
}
static inline void zamv_fill_row16(void *d,uint8_t v){
#if defined(ZAMV_USE_SH4ZAM) && defined(SHZ_HAS_TINY_FIXED_FILL)
    if(!((uintptr_t)d&3u)){shz_memset4_4(d,zamv_repeat_byte32(v));return;}
#endif
    memset(d,v,16);
}
void *zamv_fill_bytes(void *dst, uint8_t value, size_t n);
void zamv_copy_frame(zamv_frame_t *dst, const zamv_frame_t *src);
void zamv_fill_plane(uint8_t *p, int stride, int w, int h, uint8_t v);

void zamv_fdct4x4(const int16_t in[16], float out[16]);
void zamv_idct4x4_scalar(const float in[16], int16_t out[16]);
void zamv_idct4x4(const float in[16], int16_t out[16]);
void zamv_idct4x4_lowrow(const float in[16], int16_t out[16]);
void zamv_idct4x4_lowcol(const float in[16], int16_t out[16]);
void zamv_idct4x4_row2(const float in[16], int16_t out[16]);
void zamv_idct4x4_col(const float in[16], int col, int16_t out[16]);
/* Fused inverse-transform + predictor add. SH-4 keeps FTRV results in registers
   and writes final clipped pixels directly, avoiding a 32-byte int16_t scratch block. */
void zamv_idct4x4_add(const float in[16], uint8_t *dst, int stride);
void zamv_idct4x4_lowrow_add(const float in[16], uint8_t *dst, int stride);
void zamv_idct4x4_row2_add(const float in[16], uint8_t *dst, int stride);
void zamv_idct4x4_col_add(const float in[16], int col, uint8_t *dst, int stride);
void zamv_sh4_prepare_transform(void);
uint32_t zamv_sh4_fp_enter(void);
void zamv_sh4_fp_leave(uint32_t cookie);
int zamv_sh4_transform_selftest(void);

int zamv_analyze_residual_block(const uint8_t *src, int sstride, uint8_t *recon, int rstride,
                                const uint8_t *pred, int pstride, int q, zamv_qblock_t *qb);
int zamv_write_qblock(zamv_buf_t *b, const zamv_qblock_t *qb);
int zamv_read_qblock(zamv_reader_t *r, int cls, int q, uint8_t *dst, int dstride,
                     const uint8_t *pred, int pstride, int *path);

int zamv_encode_residual_block(zamv_buf_t *b, const uint8_t *src, int sstride,
                               uint8_t *recon, int rstride, const uint8_t *pred,
                               int pstride, int q);
int zamv_decode_residual_block(zamv_reader_t *r, uint8_t *dst, int dstride,
                               const uint8_t *pred, int pstride, int q);

void zamv_predict_qpel_block(const uint8_t *ref, int stride, int width, int height,
                             int x, int y, int mvx_q4, int mvy_q4,
                             uint8_t *dst, int dstride, int bw, int bh);
int zamv_block_sad(const uint8_t *a, int as, const uint8_t *b, int bs, int w, int h);

#endif
