#include "zamv_dc_helpers.h"
#include "zamv_internal.h"
#include <sh4zam/shz_sh4zam.h>
#include <stdint.h>
#include <string.h>

#ifdef __DREAMCAST__
#include <dc/pvr.h>
#include <dc/sq.h>
#endif

size_t zamv_dc_mb_row_bytes(int width) {
    if(width <= 0 || (width & 15)) return 0;
    return (size_t)(width >> 4) * 384u;
}

/* Emit one 16x16 luma macroblock in the order the TA YUV converter wants it:
   four 8x8 quadrants, each 8 rows of 8 bytes, laid out
   (left,top) (right,top) (left,bottom) (right,bottom). The converter does *not*
   take the block as 16 consecutive full-width rows; feeding it row-major
   transposes each 16x16 block by its 8x8 quadrants, which shows up on screen as
   blocky vertical ghosting. This is the layout of the KOS YUV420 example
   (kallistiOS/examples/dreamcast/pvr/yuv_converter/YUV420). */
static void zamv_dc_pack_mb_luma(const uint8_t *y, int pitch, uint8_t *d) {
    for(int q=0;q<4;q++) {
        const uint8_t *s=y+(q>>1)*8*pitch+(q&1)*8;
#ifdef SHZ_HAS_TINY_FIXED_COPY
        for(int r=0;r<8;r++) { shz_memcpy4_2(d,s); d+=8; s+=pitch; }
#else
        for(int r=0;r<8;r++) { shz_memcpy4(d,s,8); d+=8; s+=pitch; }
#endif
    }
}

size_t zamv_dc_pack_mb_span(const zamv_frame_t *s, int my, int mx0, int count, uint8_t *d) {
    int mbw,cw;
    if(!s || !d || s->width<=0 || s->height<=0 || (s->width&15) || (s->height&15) ||
       my<0 || my>=s->height/16 || mx0<0 || count<0) return 0;
    mbw=s->width/16;cw=s->width/2;
    if(mx0>mbw || count>mbw-mx0) return 0;
    for(int mx=mx0;mx<mx0+count;mx++) {
        SHZ_PREFETCH(s->u+(my*8)*cw+mx*8);
        SHZ_PREFETCH(s->v+(my*8)*cw+mx*8);
        SHZ_PREFETCH(s->y+(my*16)*s->width+mx*16);
#ifdef SHZ_HAS_TINY_FIXED_COPY
        for(int y=0;y<8;y++) { shz_memcpy4_2(d,s->u+(my*8+y)*cw+mx*8); d+=8; }
        for(int y=0;y<8;y++) { shz_memcpy4_2(d,s->v+(my*8+y)*cw+mx*8); d+=8; }
#else
        for(int y=0;y<8;y++) { shz_memcpy4(d,s->u+(my*8+y)*cw+mx*8,8); d+=8; }
        for(int y=0;y<8;y++) { shz_memcpy4(d,s->v+(my*8+y)*cw+mx*8,8); d+=8; }
#endif
        zamv_dc_pack_mb_luma(s->y+(my*16)*s->width+mx*16,s->width,d);
        d+=256;
    }
    return (size_t)count*384u;
}

size_t zamv_dc_pack_mb_row(const zamv_frame_t *s, int my, uint8_t *d) {
    if(!s || s->width<=0 || (s->width&15)) return 0;
    return zamv_dc_pack_mb_span(s,my,0,s->width/16,d);
}

size_t zamv_dc_recommended_staging_bytes(int width) {
    size_t row=zamv_dc_mb_row_bytes(width);
    if(!row)return 0;
    return row < 1536u ? row : 1536u;
}

void zamv_dc_sq_send_pvr(void *port, const uint8_t *src, size_t bytes) {
    if(!port || !src || !bytes || (bytes % 384u)) return;
#ifdef __DREAMCAST__
    /* The PVR YUV converter is a FIFO-like aperture, not linear VRAM. Each
       16x16 YUV420 macroblock is twelve 32-byte SQ bursts at offsets 0..352,
       and the next macroblock starts again at offset zero. */
# ifdef SHZ_HAS_SQ_WRAP_COPY
    shz_sq_memcpy32_wrap(port, src, bytes, 384u);
# else
    while(bytes) {
        shz_sq_memcpy32(port, src, 384u);
        src += 384u;
        bytes -= 384u;
    }
# endif
#else
    /* Software-backend oracle: preserve the logical FIFO byte stream. */
    memcpy(port, src, bytes);
#endif
}

int zamv_dc_transform_selftest(void) { return zamv_sh4_transform_selftest(); }

int zamv_dc_configure_yuv420(void *pvr_texture, int width, int height) {
    if(!pvr_texture || width < 16 || height < 16 || (width & 15) || (height & 15))
        return -1;
    /* PVR_YUV_CFG stores (macroblocks - 1) in 6-bit X/Y fields. */
    if(width > 1024 || height > 1024)
        return -1;
#ifdef __DREAMCAST__
    PVR_SET(PVR_YUV_ADDR, ((uint32_t)(uintptr_t)pvr_texture) & 0x00ffffffu);
    PVR_SET(PVR_YUV_CFG,
            (0x00u << 24) | /* YUV420 input */
            (((uint32_t)(height / 16 - 1) & 0x3fu) << 8) |
            ((uint32_t)(width / 16 - 1) & 0x3fu));
    /* KOS/hardware documentation recommends one read after programming CFG. */
    (void)PVR_GET(PVR_YUV_CFG);
    return 0;
#else
    (void)width;
    (void)height;
    return -2;
#endif
}

int zamv_dc_row_uploader_begin(zamv_dc_row_uploader_t *u) {
    if(!u) return -1;
    /* staging_bytes==0 selects the direct gather uploader, which needs no RAM
       staging surface. The legacy pack+SQ callback still requires aligned staging. */
    if(u->staging_bytes && (!u->staging || ((uintptr_t)u->staging & 31u))) return -1;
    if(u->sq_locked) return -1;
#ifdef __DREAMCAST__
    /* sq_lock both serializes SQ ownership and programs QACR. Its return value is
       already translated into the P4 Store Queue address range expected by the
       direct SH4ZAM SQ writer. */
    u->pvr_yuv_port = (void *)sq_lock((void *)PVR_TA_YUV_CONV);
    if(!u->pvr_yuv_port) return -1;
    u->sq_locked = 1;
    return 0;
#else
    return -2;
#endif
}

void zamv_dc_row_uploader_end(zamv_dc_row_uploader_t *u) {
    if(!u || !u->sq_locked) return;
#ifdef __DREAMCAST__
    sq_wait();
    sq_unlock();
#endif
    u->sq_locked = 0;
    u->pvr_yuv_port = NULL;
}

void zamv_dc_upload_row_cb(void *user, const zamv_frame_t *frame, int my) {
    zamv_dc_row_uploader_t *u=(zamv_dc_row_uploader_t *)user;
    int mbw,group,mx;
    if(!u || !frame || !u->staging || !u->pvr_yuv_port || !u->sq_locked ||
       ((uintptr_t)u->staging&31u) || u->staging_bytes<384u) return;
    mbw=frame->width/16;
    group=(int)(u->staging_bytes/384u);
    if(group<1)return;
    if(group>mbw)group=mbw;
    for(mx=0;mx<mbw;mx+=group) {
        int nmb=group;if(nmb>mbw-mx)nmb=mbw-mx;
        size_t n=zamv_dc_pack_mb_span(frame,my,mx,nmb,u->staging);
        if(n!=(size_t)nmb*384u)return;
        /* zamv_dc_sq_send_pvr resets the 384-byte converter aperture for each
           packed macroblock even when several MBs share this RAM staging group. */
        zamv_dc_sq_send_pvr(u->pvr_yuv_port,u->staging,n);
    }
}

#if defined(__DREAMCAST__) && (SHZ_BACKEND == SHZ_SH4)
/* This writer assumes FPSCR.SZ=1 and a QACR-translated Store Queue pointer.
   Sources are naturally 8-byte aligned for ZAMV420 surfaces at supported widths:
   U/V rows and each 8x8 luma quadrant row are 8-byte MB aligned. It uses only
   DR0..DR6, leaving the XF bank/XMTRX untouched. */
static inline void zamv_sq_gather4x8_sz1(uint8_t *dst,
                                         const uint8_t *s0,const uint8_t *s1,
                                         const uint8_t *s2,const uint8_t *s3) {
    void *d=dst;
    asm volatile(
        "fmov.d @%[s0], dr0\n\t"
        "fmov.d @%[s1], dr2\n\t"
        "fmov.d @%[s2], dr4\n\t"
        "fmov.d @%[s3], dr6\n\t"
        "add #32, %[d]\n\t"
        "fmov.d dr6, @-%[d]\n\t"
        "fmov.d dr4, @-%[d]\n\t"
        "fmov.d dr2, @-%[d]\n\t"
        "fmov.d dr0, @-%[d]\n\t"
        "pref @%[d]"
        : [d] "+r"(d)
        : [s0] "r"(s0),[s1] "r"(s1),[s2] "r"(s2),[s3] "r"(s3)
        : "fr0","fr1","fr2","fr3","fr4","fr5","fr6","fr7","memory");
}
#endif

int zamv_dc_sq_send_row_direct(void *port,const zamv_frame_t *f,int my) {
    if(!port||!f||my<0||my>=f->height/16||(f->width&15)||(f->height&15))return -1;
#if defined(__DREAMCAST__) && (SHZ_BACKEND == SHZ_SH4)
    const int cw=f->width/2,mbw=f->width/16;
    const int pitch=f->width;
    uint8_t *sq=(uint8_t*)port;
    asm volatile("fschg" ::: "memory");
    for(int mx=0;mx<mbw;mx++) {
        const uint8_t *u=f->u+(my*8)*cw+mx*8;
        const uint8_t *v=f->v+(my*8)*cw+mx*8;
        const uint8_t *y=f->y+(my*16)*pitch+mx*16;
        /* The YUV converter consumes one 384-byte macroblock and then expects
           the next macroblock at the same aperture addresses. */
        sq=(uint8_t*)port;
        zamv_sq_gather4x8_sz1(sq+0,u,u+cw,u+2*cw,u+3*cw);
        zamv_sq_gather4x8_sz1(sq+32,u+4*cw,u+5*cw,u+6*cw,u+7*cw);
        zamv_sq_gather4x8_sz1(sq+64,v,v+cw,v+2*cw,v+3*cw);
        zamv_sq_gather4x8_sz1(sq+96,v+4*cw,v+5*cw,v+6*cw,v+7*cw);
        /* Luma is four 8x8 quadrants, not sixteen 16-byte rows: see
           zamv_dc_pack_mb_luma. Each quadrant is 64 bytes = two 32-byte bursts. */
        for(int q=0;q<4;q++) {
            const uint8_t *qy=y+(q>>1)*8*pitch+(q&1)*8;
            uint8_t *dst=sq+128+q*64;
            for(int r=0;r<8;r+=4) {
                zamv_sq_gather4x8_sz1(dst+r*8,qy+r*pitch,qy+(r+1)*pitch,
                                      qy+(r+2)*pitch,qy+(r+3)*pitch);
            }
        }
    }
    asm volatile("fschg" ::: "memory");
    return 0;
#else
    /* Host oracle: emit the exact bytes that the SH-4 gather bursts would send.
       This lets tests validate burst ordering/source addressing against the
       normative PVR420 packer even though Store Queues themselves do not exist. */
    {
        const int cw=f->width/2,mbw=f->width/16;uint8_t *d=(uint8_t*)port;
        for(int mx=0;mx<mbw;mx++){
            const uint8_t *u=f->u+(my*8)*cw+mx*8;
            const uint8_t *v=f->v+(my*8)*cw+mx*8;
            const uint8_t *y=f->y+(my*16)*f->width+mx*16;
            for(int ry=0;ry<8;ry++){memcpy(d,u+ry*cw,8);d+=8;}
            for(int ry=0;ry<8;ry++){memcpy(d,v+ry*cw,8);d+=8;}
            for(int q=0;q<4;q++){const uint8_t *qy=y+(q>>1)*8*f->width+(q&1)*8;
                for(int ry=0;ry<8;ry++){memcpy(d,qy+ry*f->width,8);d+=8;}}
        }
    }
    return 0;
#endif
}

void zamv_dc_upload_row_direct_cb(void *user,const zamv_frame_t *frame,int my) {
    zamv_dc_row_uploader_t *u=(zamv_dc_row_uploader_t*)user;
    if(!u||!u->sq_locked||!u->pvr_yuv_port)return;
    (void)zamv_dc_sq_send_row_direct(u->pvr_yuv_port,frame,my);
}

/* Cross-check the Store Queue gather against the normative packer on real
   hardware. tests/test_dc_helpers.c can only compile the #else memcpy oracle,
   so the SH-4 burst sources, the per-macroblock aperture reset and the 32-byte
   aperture offsets are otherwise never executed by any test: a bad gather
   writes stale bytes into some macroblocks of the texture and surfaces only as
   a small patch of wrong pixels, which is easy to mistake for codec noise.
   Returns 0 on exact agreement. */
int zamv_dc_gather_selftest(void) {
#ifdef __DREAMCAST__
    enum { MAX_MBW=4, MAX_FW=MAX_MBW*16, FH=16, MAX_BYTES=MAX_MBW*384 };
    static uint8_t scratch[MAX_BYTES] __attribute__((aligned(32)));
    uint8_t expect[MAX_BYTES] __attribute__((aligned(32)));
    /* The PVR YUV converter is a FIFO aperture: every macroblock restarts at SQ
       offset 0. A Store Queue gather aimed at real RAM therefore overwrites the
       same 384 bytes once per macroblock instead of producing a linear MBW*384
       stream, so comparing that RAM image against the linear normative packer
       can never agree for MBW>1 even when the gather is exactly right for the
       converter (it rewards a linear, converter-wrong gather and punishes the
       correct reset, and the player then aborts before showing anything).
       Test each width 1..4 macroblocks instead: the last macroblock's 384
       bytes must match the corresponding slice of the linear pack, and everything
       past the first 384 bytes must stay untouched, which proves the reset.
       Together the four widths cover every mx offset (each mx appears as the
       last macroblock once) and the reset itself. */
    for(int mbw=1;mbw<=MAX_MBW;mbw++) {
        int fw=mbw*16;
        zamv_frame_t f;
        int rc;
        if(zamv_frame_alloc(&f,fw,FH)) return -2;
        for(int y=0;y<FH;y++)
            for(int x=0;x<fw;x++)
                f.y[y*fw+x]=(uint8_t)(x*7u+y*13u+(x*y));
        for(int y=0;y<FH/2;y++)
            for(int x=0;x<fw/2;x++) {
                f.u[y*(fw/2)+x]=(uint8_t)(x*3u+y*5u+1u);
                f.v[y*(fw/2)+x]=(uint8_t)(x*11u+y*2u+9u);
            }
        if(zamv_dc_pack_mb_span(&f,0,0,mbw,expect)!=(size_t)mbw*384u) {
            zamv_frame_free(&f);
            return -3;
        }
        memset(scratch,0xA5,sizeof(scratch));
        {
            uint32_t *port=sq_lock(scratch);
            if(!port){zamv_frame_free(&f);return -4;}
            rc=zamv_dc_sq_send_row_direct(port,&f,0);
            sq_wait();
            sq_unlock();
            if(rc<0){zamv_frame_free(&f);return -5;}
        }
        /* Last macroblock must match its linear slice; a mismatch means the
           converter aperture would be fed a macroblock it never asked for. */
        if(memcmp(scratch,expect+(mbw-1)*384,384)) {
            zamv_frame_free(&f);
            return -1;
        }
        /* Bytes past the first aperture must stay untouched: a linear advance
           (missing per-macroblock reset) would have written them. */
        for(int i=384;i<MAX_BYTES;i++)
            if(scratch[i]!=0xA5){zamv_frame_free(&f);return -1;}
        zamv_frame_free(&f);
    }
    return 0;
#else
    /* Host builds exercise the memcpy oracle through test_dc_helpers instead. */
    return -2;
#endif
}
