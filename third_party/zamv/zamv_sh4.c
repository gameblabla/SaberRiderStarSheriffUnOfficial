/* Dreamcast SH-4 acceleration backend.
   Build this file only for SH-4/KOS and define ZAMV_USE_SH4ZAM. */
#include "zamv_internal.h"
#include <sh4zam/shz_sh4zam.h>
#include <math.h>

static const float kT[16] = {
    0.500000000f,  0.500000000f,  0.500000000f,  0.500000000f,
    0.653281482f,  0.270598050f, -0.270598050f, -0.653281482f,
    0.500000000f, -0.500000000f, -0.500000000f,  0.500000000f,
    0.270598050f, -0.653281482f,  0.653281482f, -0.270598050f
};

/* Transform four independent vectors against the resident XMTRX in one batch.
   SH4ZAM's single-vector primitive correctly pins FV8; for a full 4x4 inverse
   transform we can do better by occupying the vector register groups and
   issuing FTRV on FV0/FV4/FV8/FV12 before materializing results. The software
   backend intentionally uses the public SH4ZAM primitive four times, making this
   path testable on the host while preserving a real-SH4 scheduling specialization.

   The batch is issued as 3 + 1 because SH-4 GCC rejects an `asm` statement with
   more than 15 operand entries, and a four-vector FTRV needs 16. The first
   statement pins FV0/FV4/FV8 with local register variables and declares
   FR12..FR15 as clobbers; that clobber list is what stops the compiler from
   recycling a bank whose FTRV result has not been written back yet, and it also
   forces the second statement to reload the fourth vector straight from memory
   into FR12..FR15.

   This must be force-inlined. With 16 live FP operands GCC otherwise keeps it
   out of line, and because the SH-4 ABI makes FR12..FR15 callee-saved every
   call then spills and reloads all four plus builds a stack frame - pure
   overhead on a path that runs twice per 2-D residual block. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline void zamv_ftrv4_batch(shz_vec4_t *a,shz_vec4_t *b,
                                    shz_vec4_t *c,shz_vec4_t *d) {
#if SHZ_BACKEND == SHZ_SH4 && !defined(ZAMV_DISABLE_BATCH_FTRV)
    register float a0 asm("fr0")=a->x, a1 asm("fr1")=a->y;
    register float a2 asm("fr2")=a->z, a3 asm("fr3")=a->w;
    register float b0 asm("fr4")=b->x, b1 asm("fr5")=b->y;
    register float b2 asm("fr6")=b->z, b3 asm("fr7")=b->w;
    register float c0 asm("fr8")=c->x, c1 asm("fr9")=c->y;
    register float c2 asm("fr10")=c->z,c3 asm("fr11")=c->w;
    asm volatile(
        "ftrv xmtrx, fv0\n\t"
        "ftrv xmtrx, fv4\n\t"
        "ftrv xmtrx, fv8"
        : "+f"(a0),"+f"(a1),"+f"(a2),"+f"(a3),
          "+f"(b0),"+f"(b1),"+f"(b2),"+f"(b3),
          "+f"(c0),"+f"(c1),"+f"(c2),"+f"(c3)
        :
        : "fr12","fr13","fr14","fr15");
    /* Park the first three results before the fourth FTRV is issued. */
    *a=shz_vec4_init(a0,a1,a2,a3);
    *b=shz_vec4_init(b0,b1,b2,b3);
    *c=shz_vec4_init(c0,c1,c2,c3);
    *d=shz_xmtrx_transform_vec4(*d);
#else
    *a=shz_xmtrx_transform_vec4(*a); *b=shz_xmtrx_transform_vec4(*b);
    *c=shz_xmtrx_transform_vec4(*c); *d=shz_xmtrx_transform_vec4(*d);
#endif
}

/* For the inverse DCT, each 1-D pass multiplies by T^T. XMTRX is column-major,
   so loading the transpose of our row-major coefficient table gives the desired map. */
void zamv_sh4_prepare_transform(void) {
    shz_xmtrx_load_unaligned_4x4(kT);
}

/* ZAMV's desktop reference arithmetic is IEEE round-to-nearest/even.  SH-4
   resets FPSCR.RM to round-toward-zero, so blindly executing FTRV under the
   reset state can differ in the last bit from the reference reconstruction.
   Enter nearest mode once per decoded frame and restore the caller's FPSCR
   afterward.  SZ/PR/FR and all exception/denormal bits are preserved. */
uint32_t zamv_sh4_fp_enter(void) {
#if SHZ_BACKEND == SHZ_SH4
    uint32_t oldv,newv;
    asm volatile("sts fpscr,%0" : "=r"(oldv));
    newv=oldv & ~3u;
    if(newv!=oldv) asm volatile("lds %0,fpscr" :: "r"(newv) : "memory");
    return oldv;
#else
    return 0;
#endif
}

void zamv_sh4_fp_leave(uint32_t cookie) {
#if SHZ_BACKEND == SHZ_SH4
    uint32_t now;
    asm volatile("sts fpscr,%0" : "=r"(now));
    /* Only RM was changed by enter().  Avoid an FPSCR write when the caller was
       already in round-nearest mode. */
    if((now&3u)!=(cookie&3u)) {
        uint32_t restored=(now&~3u)|(cookie&3u);
        asm volatile("lds %0,fpscr" :: "r"(restored) : "memory");
    }
#else
    (void)cookie;
#endif
}

/* With zamv_sh4_fp_enter() active, FADD/FSUB use nearest/even.  For the
   codec's bounded residual range (<2^22), the 2^23 bias trick converts to the
   nearest integral float with ties-to-even using two FPU ops, after which FTRC
   merely transfers that exact integer.  The software backend executes the same
   IEEE operation and is covered by the scalar equivalence tests.

   The bias has to carry x's sign, and SH4ZAM's shz_copysignf() builds it
   entirely in the FPU: FABS on the constant plus a compare and a conditional
   negate. The obvious alternative - read x's sign bit out of the IEEE pattern
   in integer registers - costs two FP/GPR transfers (FMOV out, FMOV back in)
   per call, and this runs sixteen times per 2-D residual block. */
static inline int32_t zamv_sh4_round_even_fast(float x){
    const float bias = shz_copysignf(8388608.0f, x);   /* +/- 2^23 */
    return (int32_t)((x+bias)-bias);
}


void zamv_idct4x4(const float in[16], int16_t out[16]) {
    /* First pass: columns. FTRV computes T^T * column. */
    shz_vec4_t c0={{in[0],in[4],in[8],in[12]}};
    shz_vec4_t c1={{in[1],in[5],in[9],in[13]}};
    shz_vec4_t c2={{in[2],in[6],in[10],in[14]}};
    shz_vec4_t c3={{in[3],in[7],in[11],in[15]}};
    zamv_ftrv4_batch(&c0,&c1,&c2,&c3);

    /* Second pass needs row * T, equivalently T^T * row-vector-as-column. */
    shz_vec4_t r0={{c0.x,c1.x,c2.x,c3.x}};
    shz_vec4_t r1={{c0.y,c1.y,c2.y,c3.y}};
    shz_vec4_t r2={{c0.z,c1.z,c2.z,c3.z}};
    shz_vec4_t r3={{c0.w,c1.w,c2.w,c3.w}};
    zamv_ftrv4_batch(&r0,&r1,&r2,&r3);
    const shz_vec4_t rr[4]={r0,r1,r2,r3};
    for(int y=0;y<4;y++) for(int x=0;x<4;x++) {
        int v=(int)zamv_sh4_round_even_fast(rr[y].e[x]); if(v<-32768)v=-32768; if(v>32767)v=32767;
        out[y*4+x]=(int16_t)v;
    }
}


/* Fast inverse paths for 1-D residuals. These are mathematically the same as
   zamv_idct4x4(), but collapse the zero transform dimension. They retain the
   same FTRV arithmetic on the nonzero dimension so rounding stays bit-exact. */
void zamv_idct4x4_lowrow(const float in[16], int16_t out[16]) {
    /* Only C[0][u] is nonzero. The column pass would multiply every value by
       T[0][y] == 0.5, yielding four identical rows. */
    shz_vec4_t r={{in[0]*0.5f,in[1]*0.5f,in[2]*0.5f,in[3]*0.5f}};
    r=shz_xmtrx_transform_vec4(r);
    for(int y=0;y<4;y++) for(int x=0;x<4;x++) {
        int v=(int)zamv_sh4_round_even_fast(r.e[x]); if(v<-32768)v=-32768; if(v>32767)v=32767;
        out[y*4+x]=(int16_t)v;
    }
}

void zamv_idct4x4_lowcol(const float in[16], int16_t out[16]) {
    /* Only C[v][0] is nonzero. One column FTRV produces the vertical signal;
       the row pass would then multiply it by T[0][x] == 0.5 for every x. */
    shz_vec4_t c={{in[0],in[4],in[8],in[12]}};
    c=shz_xmtrx_transform_vec4(c);
    for(int y=0;y<4;y++) {
        int v=(int)zamv_sh4_round_even_fast(c.e[y]*0.5f); if(v<-32768)v=-32768; if(v>32767)v=32767;
        for(int x=0;x<4;x++) out[y*4+x]=(int16_t)v;
    }
}


/* Additional bit-exact one-FTRV sparse paths. Row 2 is exact because its DCT
   basis is entirely +/-0.5. Any single coefficient column is exact because the
   first dimension is identical to the full transform and the second dimension
   degenerates to one multiply per output sample. Rows 1/3 intentionally remain
   on the full path: regrouping their irrational products can change final rounding. */
void zamv_idct4x4_row2(const float in[16], int16_t out[16]) {
    shz_vec4_t r={{in[8],in[9],in[10],in[11]}};
    r=shz_xmtrx_transform_vec4(r);
    for(int y=0;y<4;y++) {
        float k=kT[8+y];
        for(int x=0;x<4;x++) {
            int v=(int)zamv_sh4_round_even_fast(r.e[x]*k); if(v<-32768)v=-32768; if(v>32767)v=32767;
            out[y*4+x]=(int16_t)v;
        }
    }
}

void zamv_idct4x4_col(const float in[16], int col, int16_t out[16]) {
    shz_vec4_t c={{in[col],in[4+col],in[8+col],in[12+col]}};
    c=shz_xmtrx_transform_vec4(c);
    for(int y=0;y<4;y++) for(int x=0;x<4;x++) {
        int v=(int)zamv_sh4_round_even_fast(c.e[y]*kT[col*4+x]); if(v<-32768)v=-32768; if(v>32767)v=32767;
        out[y*4+x]=(int16_t)v;
    }
}


static inline uint8_t zamv_sh4_clipadd(uint8_t p,float r){
    int ri=(int)zamv_sh4_round_even_fast(r);
    int v=(int)p+ri;
    if(SHZ_LIKELY((uint32_t)v<=255u))return (uint8_t)v;
    return (uint8_t)(ri<0?0:255);
}
static inline uint8_t zamv_sh4_clipadd_i(uint8_t p,int ri){
    int v=(int)p+ri;
    if(SHZ_LIKELY((uint32_t)v<=255u))return (uint8_t)v;
    return (uint8_t)(ri<0?0:255);
}

void zamv_idct4x4_add(const float in[16], uint8_t *dst, int stride) {
    shz_vec4_t c0={{in[0],in[4],in[8],in[12]}};
    shz_vec4_t c1={{in[1],in[5],in[9],in[13]}};
    shz_vec4_t c2={{in[2],in[6],in[10],in[14]}};
    shz_vec4_t c3={{in[3],in[7],in[11],in[15]}};
    zamv_ftrv4_batch(&c0,&c1,&c2,&c3);
    shz_vec4_t r0={{c0.x,c1.x,c2.x,c3.x}};
    shz_vec4_t r1={{c0.y,c1.y,c2.y,c3.y}};
    shz_vec4_t r2={{c0.z,c1.z,c2.z,c3.z}};
    shz_vec4_t r3={{c0.w,c1.w,c2.w,c3.w}};
    zamv_ftrv4_batch(&r0,&r1,&r2,&r3);
    const shz_vec4_t rr[4]={r0,r1,r2,r3};
    for(int y=0;y<4;y++)for(int x=0;x<4;x++)dst[y*stride+x]=zamv_sh4_clipadd(dst[y*stride+x],rr[y].e[x]);
}

void zamv_idct4x4_lowrow_add(const float in[16], uint8_t *dst, int stride) {
    shz_vec4_t r={{in[0]*0.5f,in[1]*0.5f,in[2]*0.5f,in[3]*0.5f}};
    int ri[4];
    r=shz_xmtrx_transform_vec4(r);
    /* All four spatial rows are identical: round each unique value once. */
    for(int x=0;x<4;x++)ri[x]=(int)zamv_sh4_round_even_fast(r.e[x]);
    for(int y=0;y<4;y++)for(int x=0;x<4;x++)dst[y*stride+x]=zamv_sh4_clipadd_i(dst[y*stride+x],ri[x]);
}

void zamv_idct4x4_row2_add(const float in[16], uint8_t *dst, int stride) {
    shz_vec4_t r={{in[8],in[9],in[10],in[11]}};int ri[4];
    r=shz_xmtrx_transform_vec4(r);
    /* DCT row 2 is [+1/2,-1/2,-1/2,+1/2]. Round(+x/2) once; nearest-even
       is odd-symmetric, so the middle rows are exactly -ri. */
    for(int x=0;x<4;x++)ri[x]=(int)zamv_sh4_round_even_fast(r.e[x]*0.5f);
    for(int y=0;y<4;y++){int neg=(y==1||y==2);for(int x=0;x<4;x++){int v=neg?-ri[x]:ri[x];dst[y*stride+x]=zamv_sh4_clipadd_i(dst[y*stride+x],v);}}
}

void zamv_idct4x4_col_add(const float in[16], int col, uint8_t *dst, int stride) {
    shz_vec4_t c={{in[col],in[4+col],in[8+col],in[12+col]}};c=shz_xmtrx_transform_vec4(c);
    if(col==0 || col==2){
        /* Column 0 is all +1/2; column 2 is [+1/2,-1/2,-1/2,+1/2].
           Each row therefore has one unique rounded residual, not four. */
        for(int y=0;y<4;y++){
            int ri=(int)zamv_sh4_round_even_fast(c.e[y]*0.5f);
            for(int x=0;x<4;x++){int neg=(col==2&&(x==1||x==2));int v=neg?-ri:ri;dst[y*stride+x]=zamv_sh4_clipadd_i(dst[y*stride+x],v);}
        }
        return;
    }
    for(int y=0;y<4;y++)for(int x=0;x<4;x++)dst[y*stride+x]=zamv_sh4_clipadd(dst[y*stride+x],c.e[y]*kT[col*4+x]);
}


/* Small hardware/runtime oracle.  These expected values were generated by the
   normative desktop scalar transform in round-to-nearest/even mode.  It catches
   matrix orientation, FTRV arithmetic-mode and sparse/full backend mistakes on
   the actual console before playback. */
int zamv_sh4_transform_selftest(void) {
    static const float in[8][16]={
        {36,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {9,-18,27,-36,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,7,0,0,0,-21,0,0,0,13,0,0,0,-5,0,0},
        {45,-9,18,0,-27,36,0,9,0,-18,27,-36,9,0,-9,18},
        {-99,1,-2,3,4,-5,6,-7,8,-9,10,-11,12,-13,14,-15},
        {207,-144,63,18,-81,45,-27,9,72,-54,36,-18,-9,27,-45,81},
        {2047,-2047,2047,-2047,-2047,2047,-2047,2047,2047,-2047,2047,-2047,-2047,2047,-2047,2047},
        {81,0,0,0,0,0,-63,0,0,45,0,0,-27,0,0,9}
    };
    static const int16_t expect[8][16]={
        {9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9},
        {-2,5,-14,20,-2,5,-14,20,-2,5,-14,20,-2,5,-14,20},
        {-3,-1,1,3,-4,-1,1,4,0,0,0,0,16,7,-7,-16},
        {18,1,-14,9,17,2,6,-6,17,4,42,9,-1,15,-2,63},
        {-24,-20,-31,-4,-25,-30,-17,-49,-24,-24,-24,-21,-24,-26,-22,-31},
        {19,-3,67,85,2,15,0,85,30,-9,102,45,41,39,76,233},
        {12,60,-60,300,60,300,-300,1507,-60,-300,300,-1507,300,1507,-1507,7577},
        {11,42,33,-19,4,35,40,37,7,-7,13,33,59,11,-4,30}
    };
    int16_t out[16];
    uint32_t fp=zamv_sh4_fp_enter();
    zamv_sh4_prepare_transform();
    for(int k=0;k<8;k++){
        zamv_idct4x4(in[k],out);
        for(int i=0;i<16;i++)if(out[i]!=expect[k][i]){zamv_sh4_fp_leave(fp);return -(k*16+i+1);}
    }
    zamv_sh4_fp_leave(fp);
    return 0;
}
