#include "zamv_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef __DREAMCAST__
#include <malloc.h>
#endif
#ifdef ZAMV_USE_SH4ZAM
#include <sh4zam/shz_sh4zam.h>
#endif

static inline int clampi(int v,int lo,int hi){return v<lo?lo:v>hi?hi:v;}
#ifdef ZAMV_USE_SH4ZAM
#define ZAMV_QPEL_PREFETCH(p) SHZ_PREFETCH(p)
#else
#define ZAMV_QPEL_PREFETCH(p) ((void)0)
#endif

static inline uint8_t clip8(int v){return (uint8_t)clampi(v,0,255);}

size_t zamv_frame_bytes(int w,int h){return (size_t)w*h + 2u*(size_t)((w+1)/2)*((h+1)/2);}
int zamv_frame_alloc(zamv_frame_t*f,int w,int h){
    memset(f,0,sizeof(*f)); f->width=w;f->height=h;
    size_t n=zamv_frame_bytes(w,h);
#ifdef __DREAMCAST__
    /* Reference/current surfaces are deliberately 32-byte aligned so whole-frame
       reference seeding can use SH4ZAM's 128-byte copy kernel. */
    f->storage=(uint8_t*)memalign(32,n);
#else
    f->storage=(uint8_t*)malloc(n);
#endif
    if(!f->storage)return -1;
    size_t ys=(size_t)w*h, cs=(size_t)((w+1)/2)*((h+1)/2);
    f->y=f->storage;f->u=f->y+ys;f->v=f->u+cs; return 0;
}
void zamv_frame_free(zamv_frame_t*f){free(f->storage);memset(f,0,sizeof(*f));}
#ifdef ZAMV_USE_SH4ZAM
void *zamv_copy_bytes(void*d,const void*s,size_t n){return shz_memcpy(d,s,n);}
void *zamv_fill_bytes(void*d,uint8_t v,size_t n){
    if((((uintptr_t)d)&7u)==0 && (n&7u)==0) {
        uint32_t p=zamv_repeat_byte32(v);
        uint64_t x=(uint64_t)p|((uint64_t)p<<32);
        return shz_memset8(d,x,n);
    }
    return memset(d,v,n);
}
#else
void *zamv_copy_bytes(void*d,const void*s,size_t n){return memcpy(d,s,n);}
void *zamv_fill_bytes(void*d,uint8_t v,size_t n){return memset(d,v,n);}
#endif
void zamv_copy_frame(zamv_frame_t*d,const zamv_frame_t*s){
    size_t n=zamv_frame_bytes(s->width,s->height);
#ifdef ZAMV_USE_SH4ZAM
    if(!((uintptr_t)d->storage&31u) && !((uintptr_t)s->storage&7u) && !(n&127u)) {
        shz_memcpy128(d->storage,s->storage,n);
        return;
    }
#endif
    zamv_copy_bytes(d->storage,s->storage,n);
}
void zamv_fill_plane(uint8_t*p,int st,int w,int h,uint8_t v){for(int y=0;y<h;y++)zamv_fill_bytes(p+y*st,v,(size_t)w);}

int zamv_block_sad(const uint8_t*a,int as,const uint8_t*b,int bs,int w,int h){
    int sad=0;for(int y=0;y<h;y++)for(int x=0;x<w;x++){int d=(int)a[y*as+x]-b[y*bs+x];sad+=d<0?-d:d;}return sad;
}

void zamv_predict_qpel_block(const uint8_t *ref,int st,int width,int height,int x,int y,int mvx_q4,int mvy_q4,uint8_t*dst,int ds,int bw,int bh){
    /* C division truncates toward zero, while motion-vector decomposition needs floor. */
    int qx=mvx_q4/4, qy=mvy_q4/4;
    int fx=mvx_q4-qx*4, fy=mvy_q4-qy*4;
    if(fx<0){fx+=4;qx--;}
    if(fy<0){fy+=4;qy--;}
    int bx=x+qx, by=y+qy;
    if(fx==0 && fy==0 && bx>=0 && by>=0 && bx+bw<=width && by+bh<=height) {
#ifdef ZAMV_USE_SH4ZAM
        /* Keep the upcoming source cache line moving toward L1 while copying rows. */
        SHZ_PREFETCH(ref + by * st + bx);
#endif
        for(int j=0;j<bh;j++) {
            const uint8_t *sp=ref+(by+j)*st+bx;
            uint8_t *dp=dst+j*ds;
#ifdef ZAMV_USE_SH4ZAM
            if(j+2<bh) SHZ_PREFETCH(ref+(by+j+2)*st+bx);
#endif
            if(bw==16) zamv_copy_row16(dp,sp);
            else if(bw==8) zamv_copy_row8(dp,sp);
            else if(bw==4) zamv_copy_row4(dp,sp);
            else zamv_copy_bytes(dp,sp,(size_t)bw);
        }
        return;
    }

    /* Fractional predictors are overwhelmingly interior blocks.  Keep this
       kernel compact for the SH-4 I-cache: bilinear interpolation can be
       written as 4*a + phase*(b-a), avoiding nine duplicated phase loops.
       phase is constant for the whole block and remains in a register.

       Every kernel carries the previously loaded sample forward in `prev`
       instead of re-reading sp[i] and sp[i+1] for each output. The SH-4 has no
       byte ALU, so one source byte costs a MOV.B plus an EXTU.B; the carry
       turns two such pairs per output into one. The arithmetic is unchanged:
       sp[i] is by construction the previous iteration's sp[i+1]. */
    if(bx>=0 && by>=0 && bx+bw+(fx!=0)<=width && by+bh+(fy!=0)<=height) {
        if(fy==0) {
            for(int j=0;j<bh;j++) {
                const uint8_t *sp=ref+(by+j)*st+bx; uint8_t *dp=dst+j*ds;
                int prev=sp[0];
#ifdef ZAMV_USE_SH4ZAM
                if(j+2<bh) SHZ_PREFETCH(ref+(by+j+2)*st+bx);
#endif
                /* Specialising the two cheap phases removes the multiply and
                   shortens the expression. The identities are exact:
                     ((4a+d+2)>>2) == a + ((d+2)>>2)
                     ((4a+2d+2)>>2) == a + ((2d+2)>>2) == a + ((d+1)>>1)
                   Both rely on >> being an arithmetic (floor) shift, which is
                   what the general path already assumes. */
                if(fx==1) {
                    for(int i=0;i<bw;i++) {
                        int cur=sp[i+1];
                        dp[i]=(uint8_t)(prev+(((cur-prev)+2)>>2));
                        prev=cur;
                    }
                } else if(fx==2) {
                    for(int i=0;i<bw;i++) {
                        int cur=sp[i+1];
                        dp[i]=(uint8_t)(prev+(((cur-prev)+1)>>1));
                        prev=cur;
                    }
                } else {
                    for(int i=0;i<bw;i++) {
                        int cur=sp[i+1];
                        dp[i]=(uint8_t)(((prev<<2)+(cur-prev)*fx+2)>>2);
                        prev=cur;
                    }
                }
            }
            return;
        }
        if(fx==0) {
            /* Vertical only: the filter needs sp0[i] and sp1[i], which do not
               overlap, so there is no load to carry. */
            for(int j=0;j<bh;j++) {
                const uint8_t *sp0=ref+(by+j)*st+bx,*sp1=sp0+st; uint8_t *dp=dst+j*ds;
#ifdef ZAMV_USE_SH4ZAM
                if(j+2<bh) SHZ_PREFETCH(ref+(by+j+2)*st+bx);
#endif
                if(fy==1) {
                    for(int i=0;i<bw;i++) {
                        int a=sp0[i],b=sp1[i];
                        dp[i]=(uint8_t)(a+(((b-a)+2)>>2));
                    }
                } else if(fy==2) {
                    for(int i=0;i<bw;i++) {
                        int a=sp0[i],b=sp1[i];
                        dp[i]=(uint8_t)(a+(((b-a)+1)>>1));
                    }
                } else {
                    for(int i=0;i<bw;i++) {
                        int a=sp0[i];
                        dp[i]=(uint8_t)(((a<<2)+((int)sp1[i]-a)*fy+2)>>2);
                    }
                }
            }
            return;
        }
        for(int j=0;j<bh;j++) {
            const uint8_t *sp0=ref+(by+j)*st+bx,*sp1=sp0+st; uint8_t *dp=dst+j*ds;
            int a0=sp0[0],c0=sp1[0];
#ifdef ZAMV_USE_SH4ZAM
            if(j+2<bh) SHZ_PREFETCH(ref+(by+j+2)*st+bx);
#endif
            for(int i=0;i<bw;i++) {
                int an=sp0[i+1],cn=sp1[i+1];
                int top=(a0<<2)+(an-a0)*fx;
                int bot=(c0<<2)+(cn-c0)*fx;
                dp[i]=(uint8_t)(((top<<2)+(bot-top)*fy+8)>>4);
                a0=an;c0=cn;
            }
        }
        return;
    }

    if(fy==0) {
        for(int j=0;j<bh;j++){
            int sy=clampi(by+j,0,height-1);
#ifdef ZAMV_USE_SH4ZAM
            if(j+2<bh)SHZ_PREFETCH(ref+clampi(by+j+2,0,height-1)*st+clampi(bx,0,width-1));
#endif
            for(int i=0;i<bw;i++){
                int sx=clampi(bx+i,0,width-1),sx1=clampi(sx+1,0,width-1);
                int a=ref[sy*st+sx],b=ref[sy*st+sx1];
                dst[j*ds+i]=(uint8_t)((a*(4-fx)+b*fx+2)>>2);
            }
        }
        return;
    }
    if(fx==0) {
        for(int j=0;j<bh;j++){
            int sy=clampi(by+j,0,height-1),sy1=clampi(sy+1,0,height-1);
#ifdef ZAMV_USE_SH4ZAM
            if(j+2<bh)SHZ_PREFETCH(ref+clampi(by+j+2,0,height-1)*st+clampi(bx,0,width-1));
#endif
            for(int i=0;i<bw;i++){
                int sx=clampi(bx+i,0,width-1);
                int a=ref[sy*st+sx],c=ref[sy1*st+sx];
                dst[j*ds+i]=(uint8_t)((a*(4-fy)+c*fy+2)>>2);
            }
        }
        return;
    }
    for(int j=0;j<bh;j++){
        int sy=clampi(by+j,0,height-1),sy1=clampi(sy+1,0,height-1);
#ifdef ZAMV_USE_SH4ZAM
        if(j+2<bh)SHZ_PREFETCH(ref+clampi(by+j+2,0,height-1)*st+clampi(bx,0,width-1));
#endif
        for(int i=0;i<bw;i++){
            int sx=clampi(bx+i,0,width-1),sx1=clampi(sx+1,0,width-1);
            int a=ref[sy*st+sx],b=ref[sy*st+sx1],c=ref[sy1*st+sx],d=ref[sy1*st+sx1];
            int top=a*(4-fx)+b*fx,bot=c*(4-fx)+d*fx;
            dst[j*ds+i]=(uint8_t)((top*(4-fy)+bot*fy+8)>>4);
        }
    }
}

const uint8_t zamv_zigzag[16]={0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15};

int zamv_analyze_residual_block(const uint8_t *src,int ss,uint8_t *recon,int rs,
                                const uint8_t *pred,int ps,int q,zamv_qblock_t *qb){
    int16_t r[16], spatial[16]; float c[16], dq[16]; int non_dc=0,last=-1;
    for(int y=0;y<4;y++)for(int x=0;x<4;x++)r[y*4+x]=(int16_t)((int)src[y*ss+x]-pred[y*ps+x]);
    zamv_fdct4x4(r,c);
    memset(qb,0,sizeof(*qb));
    for(int i=0;i<16;i++){
        int v=(int)zamv_round_even_f32(c[i]/(float)q);if(v<-2047)v=-2047;if(v>2047)v=2047;
        qb->qc[i]=(int16_t)v;dq[i]=(float)(v*q);
    }
    for(int k=0;k<16;k++)if(qb->qc[zamv_zigzag[k]]){last=k;if(k)non_dc=1;}
    if(last<0){qb->cls=ZAMV_BLOCK_ZERO;for(int y=0;y<4;y++)zamv_copy_row4(recon+y*rs,pred+y*ps);return 0;}
    qb->cls=non_dc?ZAMV_BLOCK_FULL:ZAMV_BLOCK_DC;
    qb->last=(uint8_t)last;
    zamv_idct4x4(dq,spatial);
    for(int y=0;y<4;y++)for(int x=0;x<4;x++)recon[y*rs+x]=clip8((int)pred[y*ps+x]+spatial[y*4+x]);
    return 1;
}

int zamv_write_qblock(zamv_buf_t *b,const zamv_qblock_t *qb){
    if(qb->cls==ZAMV_BLOCK_DC) return zb_put_level(b,qb->qc[0]);
    if(qb->cls!=ZAMV_BLOCK_FULL) return -1;
    int last=qb->last;
    if(last<1||last>15)return -1;
    if(zb_put_bits(b,(uint32_t)last,4)<0)return -1;
    for(int k=0;k<last;k++){
        int16_t v=qb->qc[zamv_zigzag[k]];
        if(zb_put_bit(b,v!=0)<0)return -1;
        if(v&&zb_put_level(b,v)<0)return -1;
    }
    return zb_put_level(b,qb->qc[zamv_zigzag[last]]);
}

int zamv_read_qblock(zamv_reader_t *r,int cls,int q,uint8_t *dst,int ds,const uint8_t *pred,int ps,int *path){
    /* Leave coefficients uninitialized until their mask is known. Sparse 1-D
       blocks then touch only four floats instead of clearing a 64-byte array. */
    float c[16];
    if(cls==ZAMV_BLOCK_ZERO){if(path)*path=0;for(int y=0;y<4;y++)zamv_copy_row4(dst+y*ds,pred+y*ps);return 0;}
    if(cls==ZAMV_BLOCK_DC){
        /* For the orthonormal 4x4 DCT, a lone DC coefficient reconstructs to the
           same value in all 16 pixels: round((level*q) * 0.25). This exactly
           matches the scalar/FTRV transform while bypassing eight FTRVs. */
        int level=zr_get_level(r);
        if(r->error)return -1;
        if(path)*path=1;
        int add=(int)zamv_round_div4_even(level*q);
        /* DC is a constant residual over all 16 pixels. Hoist its sign/range
           decisions out of the pixel loop: a positive DC can only overflow high,
           a negative DC can only overflow low. Extreme values become four tiny
           fixed fills, and a rounded zero becomes prediction reuse/copy. */
        if(add==0){
            if(pred!=dst || ps!=ds)for(int y=0;y<4;y++)zamv_copy_row4(dst+y*ds,pred+y*ps);
            return 0;
        }
        if(add>=255){for(int y=0;y<4;y++)zamv_fill_row4(dst+y*ds,255);return 0;}
        if(add<=-255){for(int y=0;y<4;y++)zamv_fill_row4(dst+y*ds,0);return 0;}
        if(add>0){
            for(int y=0;y<4;y++)for(int x=0;x<4;x++){int v=(int)pred[y*ps+x]+add;dst[y*ds+x]=(uint8_t)(v>255?255:v);}
        }else{
            for(int y=0;y<4;y++)for(int x=0;x<4;x++){int v=(int)pred[y*ps+x]+add;dst[y*ds+x]=(uint8_t)(v<0?0:v);}
        }
        return 0;
    }
    uint16_t cmask=0;
    if(cls==ZAMV_BLOCK_FULL){
        int last=(int)zr_get_bits(r,4);if(last<1||last>15){r->error=1;return -1;}
        for(int k=0;k<last;k++)if(zr_get_bit(r)){int ci=zamv_zigzag[k];c[ci]=(float)(zr_get_level(r)*q);cmask|=(uint16_t)(1u<<ci);}
        {int ci=zamv_zigzag[last];c[ci]=(float)(zr_get_level(r)*q);cmask|=(uint16_t)(1u<<ci);}
    } else {r->error=1;return -1;}
    if(r->error)return -1;
#ifndef ZAMV_USE_SH4ZAM
    /* The normative scalar backend's sparse helpers delegate to the full scalar
       IDCT, so materialize all missing zeros there. SH4ZAM sparse helpers below
       consume only their four active coefficients and avoid this 64-byte clear. */
    for(int i=0;i<16;i++)if(!(cmask&(1u<<i)))c[i]=0.0f;
#endif
    /* Prediction is usually already in dst in the decoder's in-place path. If a
       legacy caller provides a separate predictor, seed the 4x4 block once. */
    if(pred!=dst || ps!=ds)for(int y=0;y<4;y++)zamv_copy_row4(dst+y*ds,pred+y*ps);
    if((cmask & 0xfff0u)==0) {
        for(int i=0;i<4;i++)if(!(cmask&(1u<<i)))c[i]=0.0f;
        if(path)*path=2;
        zamv_idct4x4_lowrow_add(c,dst,ds);
    } else if((cmask & 0xf0ffu)==0) {
        for(int i=8;i<12;i++)if(!(cmask&(1u<<i)))c[i]=0.0f;
        if(path)*path=3;
        zamv_idct4x4_row2_add(c,dst,ds);
    } else if((cmask & 0xeeeeu)==0) {
        for(int i=0;i<16;i+=4)if(!(cmask&(1u<<i)))c[i]=0.0f;
        if(path)*path=4;
        zamv_idct4x4_col_add(c,0,dst,ds);
    } else if((cmask & 0xddddu)==0) {
        for(int i=1;i<16;i+=4)if(!(cmask&(1u<<i)))c[i]=0.0f;
        if(path)*path=5;
        zamv_idct4x4_col_add(c,1,dst,ds);
    } else if((cmask & 0xbbbbu)==0) {
        for(int i=2;i<16;i+=4)if(!(cmask&(1u<<i)))c[i]=0.0f;
        if(path)*path=6;
        zamv_idct4x4_col_add(c,2,dst,ds);
    } else if((cmask & 0x7777u)==0) {
        for(int i=3;i<16;i+=4)if(!(cmask&(1u<<i)))c[i]=0.0f;
        if(path)*path=7;
        zamv_idct4x4_col_add(c,3,dst,ds);
    } else {
        for(int i=0;i<16;i++)if(!(cmask&(1u<<i)))c[i]=0.0f;
        if(path)*path=8;
        zamv_idct4x4_add(c,dst,ds);
    }
    return 0;
}

/* Legacy single-block helpers are retained for API-internal tests; v2 macroblock syntax
   uses zamv_analyze_residual_block + sparse/dense maps instead. */
int zamv_encode_residual_block(zamv_buf_t*b,const uint8_t*src,int ss,uint8_t*recon,int rs,const uint8_t*pred,int ps,int q){
    zamv_qblock_t qb;int nz=zamv_analyze_residual_block(src,ss,recon,rs,pred,ps,q,&qb);
    if(zb_put_bits(b,qb.cls,2)<0)return -1;
    if(qb.cls!=ZAMV_BLOCK_ZERO&&zamv_write_qblock(b,&qb)<0)return -1;
    return nz;
}
int zamv_decode_residual_block(zamv_reader_t*r,uint8_t*dst,int ds,const uint8_t*pred,int ps,int q){
    int cls=(int)zr_get_bits(r,2);return zamv_read_qblock(r,cls,q,dst,ds,pred,ps,NULL);
}

size_t zamv_pvr420_packed_bytes(int w,int h){return (size_t)((w+15)/16)*((h+15)/16)*384u;}
void zamv_pack_pvr420(const zamv_frame_t*s,uint8_t*d){
    int mbw=(s->width+15)/16,mbh=(s->height+15)/16,cw=(s->width+1)/2,ch=(s->height+1)/2;
    for(int my=0;my<mbh;my++)for(int mx=0;mx<mbw;mx++){
        for(int yy=0;yy<8;yy++)for(int xx=0;xx<8;xx++){int x=clampi(mx*8+xx,0,cw-1),y=clampi(my*8+yy,0,ch-1);*d++=s->u[y*cw+x];}
        for(int yy=0;yy<8;yy++)for(int xx=0;xx<8;xx++){int x=clampi(mx*8+xx,0,cw-1),y=clampi(my*8+yy,0,ch-1);*d++=s->v[y*cw+x];}
        for(int yy=0;yy<16;yy++)for(int xx=0;xx<16;xx++){int x=clampi(mx*16+xx,0,s->width-1),y=clampi(my*16+yy,0,s->height-1);*d++=s->y[y*s->width+x];}
    }
}
