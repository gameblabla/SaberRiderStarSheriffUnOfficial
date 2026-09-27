#include "zamv_internal.h"
#include <math.h>
#include <string.h>

/* Orthonormal 4-point DCT-II. It is deliberately a true 4x4 matrix transform:
   SH-4 builds keep this matrix resident in XMTRX and use FTRV. */
static const float T[16] = {
    0.500000000f,  0.500000000f,  0.500000000f,  0.500000000f,
    0.653281482f,  0.270598050f, -0.270598050f, -0.653281482f,
    0.500000000f, -0.500000000f, -0.500000000f,  0.500000000f,
    0.270598050f, -0.653281482f,  0.653281482f, -0.270598050f
};

void zamv_fdct4x4(const int16_t in[16], float out[16]) {
    float tmp[16];
    for(int y=0;y<4;y++) for(int u=0;u<4;u++) {
        float s=0; for(int x=0;x<4;x++) s += T[u*4+x]*(float)in[y*4+x]; tmp[y*4+u]=s;
    }
    for(int v=0;v<4;v++) for(int u=0;u<4;u++) {
        float s=0; for(int y=0;y<4;y++) s += T[v*4+y]*tmp[y*4+u]; out[v*4+u]=s;
    }
}

void zamv_idct4x4_scalar(const float in[16], int16_t out[16]) {
    float tmp[16];
    for(int y=0;y<4;y++) for(int u=0;u<4;u++) {
        float s=0; for(int v=0;v<4;v++) s += T[v*4+y]*in[v*4+u]; tmp[y*4+u]=s;
    }
    for(int y=0;y<4;y++) for(int x=0;x<4;x++) {
        float s=0; for(int u=0;u<4;u++) s += tmp[y*4+u]*T[u*4+x];
        int v=(int)zamv_round_even_f32(s); if(v<-32768)v=-32768;if(v>32767)v=32767;out[y*4+x]=(int16_t)v;
    }
}

#ifndef ZAMV_USE_SH4ZAM
void zamv_sh4_prepare_transform(void) {}
uint32_t zamv_sh4_fp_enter(void){return 0;}
void zamv_sh4_fp_leave(uint32_t cookie){(void)cookie;}
int zamv_sh4_transform_selftest(void){return 0;}
void zamv_idct4x4(const float in[16], int16_t out[16]) { zamv_idct4x4_scalar(in,out); }
void zamv_idct4x4_lowrow(const float in[16], int16_t out[16]) { zamv_idct4x4_scalar(in,out); }
void zamv_idct4x4_lowcol(const float in[16], int16_t out[16]) { zamv_idct4x4_scalar(in,out); }
void zamv_idct4x4_row2(const float in[16], int16_t out[16]) { zamv_idct4x4_scalar(in,out); }
void zamv_idct4x4_col(const float in[16], int col, int16_t out[16]) { (void)col; zamv_idct4x4_scalar(in,out); }
static void zamv_add_spatial_scalar(const int16_t sp[16], uint8_t *dst, int stride) {
    for(int y=0;y<4;y++)for(int x=0;x<4;x++){int v=(int)dst[y*stride+x]+sp[y*4+x];dst[y*stride+x]=(uint8_t)(v<0?0:v>255?255:v);}
}
void zamv_idct4x4_add(const float in[16], uint8_t *dst, int stride){int16_t sp[16];zamv_idct4x4_scalar(in,sp);zamv_add_spatial_scalar(sp,dst,stride);}
void zamv_idct4x4_lowrow_add(const float in[16], uint8_t *dst, int stride){zamv_idct4x4_add(in,dst,stride);}
void zamv_idct4x4_row2_add(const float in[16], uint8_t *dst, int stride){zamv_idct4x4_add(in,dst,stride);}
void zamv_idct4x4_col_add(const float in[16], int col, uint8_t *dst, int stride){(void)col;zamv_idct4x4_add(in,dst,stride);}
#endif
