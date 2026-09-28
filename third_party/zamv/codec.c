#include "zamv_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct zamv_encoder {
    zamv_encoder_config_t cfg;
    zamv_frame_t last, golden, cur;
    uint32_t frame_no;
    int have_last, have_golden;
    int q;
    double rate_debt;
    uint32_t *vbv_ring;
    int vbv_window_frames, vbv_pos, vbv_count;
    uint64_t vbv_sum;
};
struct zamv_decoder {
    zamv_file_header_t hdr;
    zamv_frame_t last, golden, cur;
    uint32_t frame_no;
    int have_last, have_golden, have_header;
    int golden_alias_last; /* logical GOLDEN currently shares LAST pixels; physical golden is free */
    zamv_row_callback_t row_cb;
    void *row_user;
    uint8_t *payload;
    size_t payload_cap;
    zamv_decoder_stats_t stats;
};

/* 64-bit statistics increments are surprisingly expensive on SH-4 and are not
   needed in a shipping player. Desktop/reference builds retain full stats; define
   ZAMV_ENABLE_DECODER_STATS on Dreamcast when profiling is explicitly wanted. */
#if defined(__DREAMCAST__) && !defined(ZAMV_ENABLE_DECODER_STATS)
#define ZAMV_STAT_INC(d, field) ((void)0)
#define ZAMV_STAT_ADD(d, field, value) ((void)0)
#define ZAMV_STAT_PTR(d) ((zamv_decoder_stats_t*)0)
#define ZAMV_STAT_MV(d, x, y) ((void)0)
#else
#define ZAMV_STAT_INC(d, field) ((d)->stats.field++)
#define ZAMV_STAT_ADD(d, field, value) ((d)->stats.field += (uint64_t)(value))
#define ZAMV_STAT_PTR(d) (&(d)->stats)
#define ZAMV_STAT_MV(d, x, y) count_mv_region(&(d)->stats,(x),(y))
#endif

static void put16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}static void put32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}static uint16_t get16(const uint8_t*p){return(uint16_t)(p[0]|p[1]<<8);}static uint32_t get32(const uint8_t*p){return(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static inline int clampi(int v,int lo,int hi){return v<lo?lo:v>hi?hi:v;}

static void mean_mb(const zamv_frame_t*s,int mx,int my,uint8_t *yy,uint8_t*uu,uint8_t*vv){
    unsigned sy=0,su=0,sv=0; int x0=mx*16,y0=my*16,cw=s->width/2;
    for(int y=0;y<16;y++)for(int x=0;x<16;x++)sy+=s->y[(y0+y)*s->width+x0+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++){su+=s->u[(my*8+y)*cw+mx*8+x];sv+=s->v[(my*8+y)*cw+mx*8+x];}
    *yy=(uint8_t)((sy+128)/256);*uu=(uint8_t)((su+32)/64);*vv=(uint8_t)((sv+32)/64);
}
static int fill_cost(const zamv_frame_t*s,int mx,int my,uint8_t yy,uint8_t uu,uint8_t vv){
    int cost=0,x0=mx*16,y0=my*16,cw=s->width/2;for(int y=0;y<16;y++)for(int x=0;x<16;x++){int d=(int)s->y[(y0+y)*s->width+x0+x]-yy;cost+=d<0?-d:d;}
    for(int y=0;y<8;y++)for(int x=0;x<8;x++){int d=(int)s->u[(my*8+y)*cw+mx*8+x]-uu;cost+=(d<0?-d:d)*2;d=(int)s->v[(my*8+y)*cw+mx*8+x]-vv;cost+=(d<0?-d:d)*2;}
    return cost;
}
static void fill_mb(zamv_frame_t*d,int mx,int my,uint8_t yy,uint8_t uu,uint8_t vv){int x0=mx*16,y0=my*16,cw=d->width/2;for(int y=0;y<16;y++)zamv_fill_row16(d->y+(y0+y)*d->width+x0,yy);for(int y=0;y<8;y++){zamv_fill_row8(d->u+(my*8+y)*cw+mx*8,uu);zamv_fill_row8(d->v+(my*8+y)*cw+mx*8,vv);}}

/* Copy exactly one reconstructed 16x16 YUV420 macroblock.  This is the lazy
   alternative to a 450 KiB full-frame LAST seed: 16x16 luma plus two 8x8
   chroma planes = 384 bytes.  All row widths hit SH4ZAM's fixed tiny-copy
   kernels when enabled. */
static void copy_ref_mb_to_frame(zamv_frame_t *d,const zamv_frame_t *s,int mx,int my){
    int x0=mx*16,y0=my*16,cw=d->width/2;
#ifdef ZAMV_USE_SH4ZAM
    SHZ_PREFETCH(s->y+y0*s->width+x0);
    SHZ_PREFETCH(s->u+(my*8)*cw+mx*8);
    SHZ_PREFETCH(s->v+(my*8)*cw+mx*8);
#endif
    for(int y=0;y<16;y++)zamv_copy_row16(d->y+(y0+y)*d->width+x0,s->y+(y0+y)*s->width+x0);
    for(int y=0;y<8;y++){
        zamv_copy_row8(d->u+(my*8+y)*cw+mx*8,s->u+(my*8+y)*cw+mx*8);
        zamv_copy_row8(d->v+(my*8+y)*cw+mx*8,s->v+(my*8+y)*cw+mx*8);
    }
}

static int sad_pred(const zamv_frame_t*s,int mx,int my,const uint8_t py[256],const uint8_t pu[64],const uint8_t pv[64]);
static inline uint8_t clip8i(int v){return (uint8_t)(v<0?0:v>255?255:v);}
static int make_intra_pred(const zamv_frame_t *cur,int mx,int my,int mode,uint8_t iy,uint8_t iu,uint8_t iv,
                           uint8_t py[256],uint8_t pu[64],uint8_t pv[64]){
    int w=cur->width,cw=w/2,x0=mx*16,y0=my*16,cx=mx*8,cy=my*8;
    if(mode==ZAMV_INTRA_DC){zamv_fill_bytes(py,iy,256);zamv_fill_bytes(pu,iu,64);zamv_fill_bytes(pv,iv,64);return 0;}
    if(mode==ZAMV_INTRA_V){
        if(my==0)return -1;
        for(int y=0;y<16;y++)zamv_copy_bytes(py+y*16,cur->y+(y0-1)*w+x0,16);
        for(int y=0;y<8;y++){zamv_copy_bytes(pu+y*8,cur->u+(cy-1)*cw+cx,8);zamv_copy_bytes(pv+y*8,cur->v+(cy-1)*cw+cx,8);}return 0;
    }
    if(mode==ZAMV_INTRA_H){
        if(mx==0)return -1;
        for(int y=0;y<16;y++)zamv_fill_row16(py+y*16,cur->y[(y0+y)*w+x0-1]);
        for(int y=0;y<8;y++){zamv_fill_row8(pu+y*8,cur->u[(cy+y)*cw+cx-1]);zamv_fill_row8(pv+y*8,cur->v[(cy+y)*cw+cx-1]);}return 0;
    }
    if(mode==ZAMV_INTRA_TM){
        if(mx==0||my==0)return -1;
        int tl=cur->y[(y0-1)*w+x0-1];for(int y=0;y<16;y++){int l=cur->y[(y0+y)*w+x0-1];for(int x=0;x<16;x++)py[y*16+x]=clip8i(l+cur->y[(y0-1)*w+x0+x]-tl);}
        int tlu=cur->u[(cy-1)*cw+cx-1],tlv=cur->v[(cy-1)*cw+cx-1];
        for(int y=0;y<8;y++){int lu=cur->u[(cy+y)*cw+cx-1],lv=cur->v[(cy+y)*cw+cx-1];for(int x=0;x<8;x++){pu[y*8+x]=clip8i(lu+cur->u[(cy-1)*cw+cx+x]-tlu);pv[y*8+x]=clip8i(lv+cur->v[(cy-1)*cw+cx+x]-tlv);}}return 0;
    }
    return -1;
}
static int choose_intra(const zamv_frame_t *src,const zamv_frame_t *cur,int mx,int my,uint8_t iy,uint8_t iu,uint8_t iv,
                        uint8_t py[256],uint8_t pu[64],uint8_t pv[64]){
    uint8_t ty[256],tu[64],tv[64];int best=ZAMV_INTRA_DC;make_intra_pred(cur,mx,my,best,iy,iu,iv,py,pu,pv);int bc=sad_pred(src,mx,my,py,pu,pv);
    for(int mode=ZAMV_INTRA_V;mode<=ZAMV_INTRA_TM;mode++)if(make_intra_pred(cur,mx,my,mode,iy,iu,iv,ty,tu,tv)==0){int c=sad_pred(src,mx,my,ty,tu,tv);if(c<bc){bc=c;best=mode;zamv_copy_bytes(py,ty,256);zamv_copy_bytes(pu,tu,64);zamv_copy_bytes(pv,tv,64);}}
    return best;
}

static void pred_mb(const zamv_frame_t*ref,int mx,int my,int mvx,int mvy,uint8_t py[256],uint8_t pu[64],uint8_t pv[64]){
    int x=mx*16,y=my*16,cw=ref->width/2,ch=ref->height/2;
    zamv_predict_qpel_block(ref->y,ref->width,ref->width,ref->height,x,y,mvx,mvy,py,16,16,16);
    /* quarter-luma MV becomes eighth-luma == quarter-chroma / 2. */
    int cmvx=mvx/2, cmvy=mvy/2;
    zamv_predict_qpel_block(ref->u,cw,cw,ch,mx*8,my*8,cmvx,cmvy,pu,8,8,8);
    zamv_predict_qpel_block(ref->v,cw,cw,ch,mx*8,my*8,cmvx,cmvy,pv,8,8,8);
}
static void pred_mb_split(const zamv_frame_t*ref,int mx,int my,const int mvx[4],const int mvy[4],uint8_t py[256],uint8_t pu[64],uint8_t pv[64]){
    int cw=ref->width/2,ch=ref->height/2;
    for(int q=0;q<4;q++){
        int qx=(q&1)*8,qy=(q>>1)*8;
        zamv_predict_qpel_block(ref->y,ref->width,ref->width,ref->height,mx*16+qx,my*16+qy,mvx[q],mvy[q],py+qy*16+qx,16,8,8);
        int cx=(q&1)*4,cy=(q>>1)*4;
        zamv_predict_qpel_block(ref->u,cw,cw,ch,mx*8+cx,my*8+cy,mvx[q]/2,mvy[q]/2,pu+cy*8+cx,8,4,4);
        zamv_predict_qpel_block(ref->v,cw,cw,ch,mx*8+cx,my*8+cy,mvx[q]/2,mvy[q]/2,pv+cy*8+cx,8,4,4);
    }
}

/* Decoder-only prediction directly into CURRENT. This removes the old 384-byte
   temporary predictor plus the second predictor->frame copy. Residuals are then
   applied in place. LAST+(0,0) can skip even this because CURRENT is frame-seeded. */
static void pred_mb_to_frame(const zamv_frame_t*ref,zamv_frame_t*d,int mx,int my,int mvx,int mvy){
    int x=mx*16,y=my*16,cw=ref->width/2,ch=ref->height/2;
    zamv_predict_qpel_block(ref->y,ref->width,ref->width,ref->height,x,y,mvx,mvy,
                            d->y+y*d->width+x,d->width,16,16);
    zamv_predict_qpel_block(ref->u,cw,cw,ch,mx*8,my*8,mvx/2,mvy/2,
                            d->u+my*8*cw+mx*8,cw,8,8);
    zamv_predict_qpel_block(ref->v,cw,cw,ch,mx*8,my*8,mvx/2,mvy/2,
                            d->v+my*8*cw+mx*8,cw,8,8);
}
static void pred_mb_split_to_frame(const zamv_frame_t*ref,zamv_frame_t*d,int mx,int my,const int mvx[4],const int mvy[4]){
    int cw=ref->width/2,ch=ref->height/2;
    for(int q=0;q<4;q++){
        int qx=(q&1)*8,qy=(q>>1)*8;
        zamv_predict_qpel_block(ref->y,ref->width,ref->width,ref->height,mx*16+qx,my*16+qy,mvx[q],mvy[q],
                                d->y+(my*16+qy)*d->width+mx*16+qx,d->width,8,8);
        int cx=(q&1)*4,cy=(q>>1)*4;
        zamv_predict_qpel_block(ref->u,cw,cw,ch,mx*8+cx,my*8+cy,mvx[q]/2,mvy[q]/2,
                                d->u+(my*8+cy)*cw+mx*8+cx,cw,4,4);
        zamv_predict_qpel_block(ref->v,cw,cw,ch,mx*8+cx,my*8+cy,mvx[q]/2,mvy[q]/2,
                                d->v+(my*8+cy)*cw+mx*8+cx,cw,4,4);
    }
}
static int sad_luma_region(const zamv_frame_t*s,const zamv_frame_t*r,int x,int y,int bw,int bh,int mvx,int mvy,uint8_t *tmp){
    zamv_predict_qpel_block(r->y,r->width,r->width,r->height,x,y,mvx,mvy,tmp,bw,bw,bh);
    return zamv_block_sad(s->y+y*s->width+x,s->width,tmp,bw,bw,bh);
}
static void motion_refine8(const zamv_encoder_t*e,const zamv_frame_t*s,const zamv_frame_t*r,int x,int y,int sx,int sy,int *bestx,int *besty){
    uint8_t tmp[64];int bx=sx,by=sy,bc=sad_luma_region(s,r,x,y,8,8,bx,by,tmp);int lim=e->cfg.search_range*4;
    for(int step=2;step>=1;step--){int improved=1;while(improved){improved=0;static const int dx[8]={-1,1,0,0,-1,1,-1,1};static const int dy[8]={0,0,-1,1,-1,-1,1,1};for(int k=0;k<8;k++){int nx=bx+dx[k]*step*4,ny=by+dy[k]*step*4;if(nx< -lim||nx>lim||ny< -lim||ny>lim)continue;int c=sad_luma_region(s,r,x,y,8,8,nx,ny,tmp);if(c<bc){bc=c;bx=nx;by=ny;improved=1;}}}}
    if(e->cfg.enable_qpel){for(int pass=0;pass<2;pass++){int ox=bx,oy=by;for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){if(!dx&&!dy)continue;int nx=ox+dx,ny=oy+dy;if(nx< -lim||nx>lim||ny< -lim||ny>lim)continue;int c=sad_luma_region(s,r,x,y,8,8,nx,ny,tmp);if(c<bc){bc=c;bx=nx;by=ny;}}}}
    *bestx=bx;*besty=by;
}

static int sad_pred(const zamv_frame_t*s,int mx,int my,const uint8_t py[256],const uint8_t pu[64],const uint8_t pv[64]){
    int x=mx*16,y=my*16,cw=s->width/2;int sad=zamv_block_sad(s->y+y*s->width+x,s->width,py,16,16,16);sad+=2*zamv_block_sad(s->u+my*8*cw+mx*8,cw,pu,8,8,8);sad+=2*zamv_block_sad(s->v+my*8*cw+mx*8,cw,pv,8,8,8);return sad;
}
static void motion_search(const zamv_encoder_t*e,const zamv_frame_t*s,const zamv_frame_t*r,int mx,int my,int *bestx,int*besty,int *best_sad){
    uint8_t py[256],pu[64],pv[64]; int bx=0,by=0; pred_mb(r,mx,my,0,0,py,pu,pv);int bc=sad_pred(s,mx,my,py,pu,pv);
    int range=e->cfg.search_range; int step=1;while(step*2<=range)step*=2;
    for(;step>=1;step/=2){int improved=1;while(improved){improved=0;static const int dx[8]={-1,1,0,0,-1,1,-1,1};static const int dy[8]={0,0,-1,1,-1,-1,1,1};for(int k=0;k<8;k++){int nx=bx+dx[k]*step*4,ny=by+dy[k]*step*4;if(nx < -range*4||nx>range*4||ny< -range*4||ny>range*4)continue;pred_mb(r,mx,my,nx,ny,py,pu,pv);int c=sad_pred(s,mx,my,py,pu,pv);if(c<bc){bc=c;bx=nx;by=ny;improved=1;}}}}
    if(e->cfg.enable_qpel){for(int pass=0;pass<2;pass++){int obx=bx,oby=by;for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){if(!dx&&!dy)continue;int nx=obx+dx,ny=oby+dy;pred_mb(r,mx,my,nx,ny,py,pu,pv);int c=sad_pred(s,mx,my,py,pu,pv);if(c<bc){bc=c;bx=nx;by=ny;}}}}
    *bestx=bx;*besty=by;*best_sad=bc;
}

static void block_ptrs_enc(const zamv_frame_t *s,zamv_frame_t *d,int mx,int my,
                           const uint8_t py[256],const uint8_t pu[64],const uint8_t pv[64],int bi,
                           const uint8_t **src,int *ss,uint8_t **dst,int *ds,const uint8_t **pred,int *ps){
    int cw=s->width/2;
    if(bi<16){int bx=bi&3,by=bi>>2;*src=s->y+(my*16+by*4)*s->width+mx*16+bx*4;*ss=s->width;*dst=d->y+(my*16+by*4)*d->width+mx*16+bx*4;*ds=d->width;*pred=py+by*4*16+bx*4;*ps=16;}
    else if(bi<20){int j=bi-16,bx=j&1,by=j>>1;*src=s->u+(my*8+by*4)*cw+mx*8+bx*4;*ss=cw;*dst=d->u+(my*8+by*4)*cw+mx*8+bx*4;*ds=cw;*pred=pu+by*4*8+bx*4;*ps=8;}
    else{int j=bi-20,bx=j&1,by=j>>1;*src=s->v+(my*8+by*4)*cw+mx*8+bx*4;*ss=cw;*dst=d->v+(my*8+by*4)*cw+mx*8+bx*4;*ds=cw;*pred=pv+by*4*8+bx*4;*ps=8;}
}
static void block_dst_dec(zamv_frame_t *d,int mx,int my,int bi,uint8_t **dst,int *ds){
    int cw=d->width/2;
    if(bi<16){int bx=bi&3,by=bi>>2;*dst=d->y+(my*16+by*4)*d->width+mx*16+bx*4;*ds=d->width;}
    else if(bi<20){int j=bi-16,bx=j&1,by=j>>1;*dst=d->u+(my*8+by*4)*cw+mx*8+bx*4;*ds=cw;}
    else{int j=bi-20,bx=j&1,by=j>>1;*dst=d->v+(my*8+by*4)*cw+mx*8+bx*4;*ds=cw;}
}

static unsigned popcount24(uint32_t x){
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_popcount(x & 0x00ffffffu);
#else
    unsigned n=0;x&=0x00ffffffu;while(x){x&=x-1;n++;}return n;
#endif
}
/* For dense 24-bit residual maps, scan six nibbles instead of testing all
   24 bit positions. Each entry packs the set bit offsets, low-to-high, in two
   bits apiece. The table is only 32 bytes and avoids relying on a CTZ instruction
   that SH-4 does not have. */
static const uint8_t zamv_nibble_count[16]={0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4};
static const uint8_t zamv_nibble_pos[16]={
    0x00,0x00,0x01,0x04,0x02,0x08,0x09,0x24,
    0x03,0x0c,0x0d,0x34,0x0e,0x38,0x39,0xe4
};

static int decode_one_residual(zamv_reader_t *r,zamv_frame_t *d,int mx,int my,
                               int bi,int q,zamv_decoder_stats_t *stats){
    uint8_t *dp;int ds,path=0;block_dst_dec(d,mx,my,bi,&dp,&ds);
    int cls=zr_get_bit(r)?ZAMV_BLOCK_FULL:ZAMV_BLOCK_DC;
    if(zamv_read_qblock(r,cls,q,dp,ds,dp,ds,&path)<0)return -1;
    if(stats){
        if(path==1)stats->blocks_dc++;
        else if(path>=2 && path<=7){stats->blocks_1d++;stats->blocks_1d_kind[path-2]++;}
        else if(path==8)stats->blocks_full++;
    }
    return 0;
}

/* Apply only the coded residuals. Sparse maps are decoded directly from their
   <=4 sorted block indices instead of first rebuilding a 24-bit mask and then
   scanning all 24 bit positions. This is especially valuable on SH-4 because
   skip-heavy animation produces many sparse residual maps and the syntax already
   paid to transmit the positions explicitly. Dense maps retain their 24-bit mask. */
static int decode_blocks_inplace(zamv_reader_t*r,zamv_frame_t*d,int mx,int my,int q,zamv_decoder_stats_t *stats){
    if(zr_get_bit(r)==0){
        if(stats)stats->residual_maps_sparse++;
        int nz=(int)zr_get_bits(r,3);uint8_t pos[4];
        if(nz>4){r->error=1;return -1;}
        int prev=-1;
        for(int i=0;i<nz;i++){
            int bi=(int)zr_get_bits(r,5);
            if(bi>=24||bi<=prev){r->error=1;return -1;}
            pos[i]=(uint8_t)bi;prev=bi;
        }
        if(stats)stats->blocks_zero+=24u-(unsigned)nz;
        for(int i=0;i<nz;i++)if(decode_one_residual(r,d,mx,my,pos[i],q,stats)<0)return -1;
        return r->error?-1:0;
    } else {
        if(stats)stats->residual_maps_dense++;
        uint32_t mask=zr_get_bits(r,24);
        if(r->error)return -1;
        if(stats)stats->blocks_zero+=24u-popcount24(mask);
        for(int base=0;base<24;base+=4,mask>>=4){
            uint8_t n=(uint8_t)(mask&15u);
            uint8_t packed=zamv_nibble_pos[n];
            unsigned count=zamv_nibble_count[n];
            for(unsigned j=0;j<count;j++){
                int bi=base+(int)(packed&3u);packed>>=2;
                if(decode_one_residual(r,d,mx,my,bi,q,stats)<0)return -1;
            }
        }
        return r->error?-1:0;
    }
}

static void copy_pred_mb_to_frame(zamv_frame_t*d,int mx,int my,const uint8_t py[256],const uint8_t pu[64],const uint8_t pv[64]){
    int x0=mx*16,y0=my*16,cw=d->width/2;
    for(int y=0;y<16;y++)zamv_copy_row16(d->y+(y0+y)*d->width+x0,py+y*16);
    for(int y=0;y<8;y++){
        zamv_copy_row8(d->u+(my*8+y)*cw+mx*8,pu+y*8);
        zamv_copy_row8(d->v+(my*8+y)*cw+mx*8,pv+y*8);
    }
}

static int decode_blocks_from_pred(zamv_reader_t*r,zamv_frame_t*d,int mx,int my,const uint8_t py[256],const uint8_t pu[64],const uint8_t pv[64],int q,zamv_decoder_stats_t *stats){
    /* Preserve bitstream order: mask is the first residual syntax element. Copying
       prediction does not consume input, so doing it before read_block_mask is safe. */
    copy_pred_mb_to_frame(d,mx,my,py,pu,pv);
    return decode_blocks_inplace(r,d,mx,my,q,stats);
}
static int encode_blocks(zamv_buf_t*b,const zamv_frame_t*s,zamv_frame_t*d,int mx,int my,const uint8_t py[256],const uint8_t pu[64],const uint8_t pv[64],int q){
    zamv_qblock_t qb[24];uint32_t mask=0;int nz=0;
    for(int bi=0;bi<24;bi++){
        const uint8_t *sp,*pp;uint8_t *dp;int ss,ds,ps;
        block_ptrs_enc(s,d,mx,my,py,pu,pv,bi,&sp,&ss,&dp,&ds,&pp,&ps);
        if(zamv_analyze_residual_block(sp,ss,dp,ds,pp,ps,q,&qb[bi])){mask|=1u<<bi;nz++;}
    }
    if(nz<=4){
        if(zb_put_bit(b,0)<0||zb_put_bits(b,(uint32_t)nz,3)<0)return-1;
        for(int bi=0;bi<24;bi++)if(mask&(1u<<bi))if(zb_put_bits(b,(uint32_t)bi,5)<0)return-1;
    }else{
        if(zb_put_bit(b,1)<0||zb_put_bits(b,mask,24)<0)return-1;
    }
    for(int bi=0;bi<24;bi++)if(mask&(1u<<bi)){
        int full=qb[bi].cls==ZAMV_BLOCK_FULL;
        if(zb_put_bit(b,(unsigned)full)<0||zamv_write_qblock(b,&qb[bi])<0)return-1;
    }
    return nz;
}


zamv_encoder_t*zamv_encoder_create(const zamv_encoder_config_t*c){
    if(!c||c->width%16||c->height%16||!c->fps_num||!c->fps_den)return NULL;
    zamv_encoder_t*e=(zamv_encoder_t*)calloc(1,sizeof(*e));if(!e)return NULL;
    e->cfg=*c;e->q=clampi(c->qinitial,c->qmin,c->qmax);
    if(c->max_total_bps&&c->vbv_window_ms>0){
        double fps=(double)c->fps_num/c->fps_den;
        int n=(int)ceil(fps*(double)c->vbv_window_ms/1000.0);
        if(n<1)n=1;
        e->vbv_ring=(uint32_t*)calloc((size_t)n,sizeof(*e->vbv_ring));
        if(!e->vbv_ring){free(e);return NULL;}
        e->vbv_window_frames=n;
    }
    if(zamv_frame_alloc(&e->last,c->width,c->height)||zamv_frame_alloc(&e->golden,c->width,c->height)||zamv_frame_alloc(&e->cur,c->width,c->height)){zamv_encoder_destroy(e);return NULL;}
    zamv_sh4_prepare_transform();return e;
}
void zamv_encoder_destroy(zamv_encoder_t*e){if(!e)return;free(e->vbv_ring);zamv_frame_free(&e->last);zamv_frame_free(&e->golden);zamv_frame_free(&e->cur);free(e);}

int zamv_encoder_write_header(zamv_encoder_t*e,FILE*out,uint32_t n){uint8_t h[64]={0};memcpy(h,ZAMV_MAGIC,4);h[4]=ZAMV_VERSION;h[5]=ZAMV_CHROMA_420;put16(h+6,64);put16(h+8,(uint16_t)e->cfg.width);put16(h+10,(uint16_t)e->cfg.height);put32(h+12,e->cfg.fps_num);put32(h+16,e->cfg.fps_den);put32(h+20,n);put32(h+24,e->cfg.target_video_bps);put32(h+28,e->cfg.reserved_audio_bps);put16(h+32,(uint16_t)e->cfg.keyint);return fwrite(h,1,64,out)==64?0:-1;}

static int encode_frame_payload(zamv_encoder_t *e, const zamv_frame_t *s,
                                int key, int q, zamv_buf_t *b) {
    int mbw=e->cfg.width/16,mbh=e->cfg.height/16;
    uint8_t py[256],pu[64],pv[64];

    /* Inter frames start as a full LAST reference copy. On SH-4, zamv_copy_frame()
       resolves to SH4ZAM's optimized copy path. This makes the overwhelmingly common
       SKIP mode a true no-op and turns many tiny row copies into one sequential copy.
       This copy is also what makes VBV retries safe: every retry reconstructs CURRENT
       from the same accepted LAST reference. */
    if(!key) zamv_copy_frame(&e->cur,&e->last);

    for(int my=0;my<mbh;my++)for(int mx=0;mx<mbw;mx++){
        uint8_t iy,iu,iv;mean_mb(s,mx,my,&iy,&iu,&iv);int fc=fill_cost(s,mx,my,iy,iu,iv);
        int intra_mode=choose_intra(s,&e->cur,mx,my,iy,iu,iv,py,pu,pv);int intra_cost=sad_pred(s,mx,my,py,pu,pv);
        if(fc < q*48){
            if(zb_put_bits(b,ZAMV_MB_FILL,2)<0||zb_put_bits(b,iy,8)<0||zb_put_bits(b,iu,8)<0||zb_put_bits(b,iv,8)<0)return-1;
            fill_mb(&e->cur,mx,my,iy,iu,iv);continue;
        }
        if(key){
            if(zb_put_bits(b,ZAMV_MB_INTRA_DC,2)<0||zb_put_bits(b,(uint32_t)intra_mode,2)<0)return-1;
            if(intra_mode==ZAMV_INTRA_DC&&(zb_put_bits(b,iy,8)<0||zb_put_bits(b,iu,8)<0||zb_put_bits(b,iv,8)<0))return-1;
            if(encode_blocks(b,s,&e->cur,mx,my,py,pu,pv,q)<0)return-1;
            continue;
        }
        int skip; pred_mb(&e->last,mx,my,0,0,py,pu,pv);skip=sad_pred(s,mx,my,py,pu,pv);
        if(skip < q*40){if(zb_put_bits(b,ZAMV_MB_SKIP,2)<0)return-1;continue;}
        int lx,ly,ls;motion_search(e,s,&e->last,mx,my,&lx,&ly,&ls);int ref=ZAMV_REF_LAST,bx=lx,by=ly,bc=ls;
        if(e->have_golden){int gx,gy,gs;motion_search(e,s,&e->golden,mx,my,&gx,&gy,&gs);if(gs+48<bc){ref=ZAMV_REF_GOLDEN;bx=gx;by=gy;bc=gs;}}
        if(intra_cost + q*32 < bc){
            if(zb_put_bits(b,ZAMV_MB_INTRA_DC,2)<0||zb_put_bits(b,(uint32_t)intra_mode,2)<0)return-1;
            if(intra_mode==ZAMV_INTRA_DC&&(zb_put_bits(b,iy,8)<0||zb_put_bits(b,iu,8)<0||zb_put_bits(b,iv,8)<0))return-1;
            make_intra_pred(&e->cur,mx,my,intra_mode,iy,iu,iv,py,pu,pv);
            if(encode_blocks(b,s,&e->cur,mx,my,py,pu,pv,q)<0)return-1;
        }
        else{
            const zamv_frame_t *rf=ref==ZAMV_REF_LAST?&e->last:&e->golden;
            int smvx[4]={bx,bx,bx,bx},smvy[4]={by,by,by,by};int split=0;
            if(e->cfg.enable_split8){
                for(int qi=0;qi<4;qi++)motion_refine8(e,s,rf,mx*16+(qi&1)*8,my*16+(qi>>1)*8,bx,by,&smvx[qi],&smvy[qi]);
                pred_mb_split(rf,mx,my,smvx,smvy,py,pu,pv);int split_sad=sad_pred(s,mx,my,py,pu,pv);split=(split_sad + q*48 < bc);
            }
            if(zb_put_bits(b,ZAMV_MB_INTER,2)<0||zb_put_bit(b,(unsigned)ref)<0||zb_put_bit(b,(unsigned)split)<0||zb_put_se(b,bx)<0||zb_put_se(b,by)<0)return-1;
            if(split){
                for(int qi=0;qi<4;qi++)if(zb_put_se(b,smvx[qi]-bx)<0||zb_put_se(b,smvy[qi]-by)<0)return-1;
                pred_mb_split(rf,mx,my,smvx,smvy,py,pu,pv);
            }
            else pred_mb(rf,mx,my,bx,by,py,pu,pv);
            if(encode_blocks(b,s,&e->cur,mx,my,py,pu,pv,q)<0)return-1;
        }
    }
    return zb_flush(b);
}

static int vbv_would_overflow(const zamv_encoder_t *e, size_t frame_bytes) {
    double fps, video_bps, limit_bytes;
    uint64_t base;
    if(!e->vbv_ring || e->vbv_window_frames <= 0 || !e->cfg.max_total_bps)
        return 0;
    if(e->cfg.max_total_bps <= e->cfg.reserved_audio_bps)
        return 1;
    fps=(double)e->cfg.fps_num/e->cfg.fps_den;
    if(fps<=0.0)return 0;
    video_bps=(double)(e->cfg.max_total_bps-e->cfg.reserved_audio_bps);
    limit_bytes=video_bps/8.0*(double)e->vbv_window_frames/fps;
    base=e->vbv_sum;
    /* vbv_pos points to the oldest element once the ring is full. The candidate
       replaces that element, so test exactly the same fixed-length window that
       zamvinfo reports after acceptance. */
    if(e->vbv_count>=e->vbv_window_frames)
        base-=e->vbv_ring[e->vbv_pos];
    return (double)base+(double)frame_bytes > limit_bytes;
}

static void vbv_accept(zamv_encoder_t *e, uint32_t frame_bytes) {
    if(!e->vbv_ring || e->vbv_window_frames<=0)return;
    if(e->vbv_count>=e->vbv_window_frames)
        e->vbv_sum-=e->vbv_ring[e->vbv_pos];
    else
        e->vbv_count++;
    e->vbv_ring[e->vbv_pos]=frame_bytes;
    e->vbv_sum+=frame_bytes;
    e->vbv_pos++;
    if(e->vbv_pos>=e->vbv_window_frames)e->vbv_pos=0;
}

int zamv_encoder_encode(zamv_encoder_t*e,const zamv_frame_t*s,FILE*out){
    int key=!e->have_last || (e->cfg.keyint>0 && e->frame_no%(uint32_t)e->cfg.keyint==0);
    int q=e->q;
    zamv_buf_t b;
    size_t frame_bytes;

    /* Hard rolling optical guard. A frame that would overflow the configured
       CD+audio window is re-encoded at a coarser quantizer. The decoder syntax
       does not change; only the frame-header q byte does. */
    for(;;){
        zb_init(&b);
        if(encode_frame_payload(e,s,key,q,&b)<0){zb_free(&b);return-1;}
        frame_bytes=b.size+ZAMV_FRAME_HEADER_BYTES;
        if(!vbv_would_overflow(e,frame_bytes)||q>=e->cfg.qmax)break;
        zb_free(&b);
        q++;
    }

    uint8_t fh[16]={0};put32(fh,(uint32_t)b.size);put32(fh+4,e->frame_no);fh[8]=(uint8_t)(key?ZAMV_FRAME_KEY:ZAMV_FRAME_INTER);fh[9]=(uint8_t)q;int refresh=key||(e->cfg.golden_period>0&&e->frame_no%(uint32_t)e->cfg.golden_period==0);fh[10]=(uint8_t)(refresh?1:0);
    if(fwrite(fh,1,16,out)!=16||fwrite(b.data,1,b.size,out)!=b.size){zb_free(&b);return-1;}
    vbv_accept(e,(uint32_t)frame_bytes);

    /* Refresh GOLDEN before rotating reconstruction surfaces. LAST then becomes the
       just-decoded frame by pointer/descriptor swap instead of another 450 KiB copy. */
    if(refresh){zamv_copy_frame(&e->golden,&e->cur);e->have_golden=1;}
    { zamv_frame_t tmp=e->last; e->last=e->cur; e->cur=tmp; }
    e->have_last=1;
    e->q=q;
    double fps=(double)e->cfg.fps_num/e->cfg.fps_den;
    double target=(double)e->cfg.target_video_bps/8.0/fps;
    /* Slow reservoir RC chooses the normal operating point. The hard VBV guard above
       only intervenes on bursts that cannot fit the configured optical window. */
    e->rate_debt += (double)b.size - target;
    double threshold=target*24.0;
    if(e->rate_debt>threshold && e->q<e->cfg.qmax){e->q++;e->rate_debt-=threshold*0.5;}
    else if(e->rate_debt<-threshold && e->q>e->cfg.qmin){e->q--;e->rate_debt+=threshold*0.5;}
    e->frame_no++;zb_free(&b);return 0;
}

static void count_mv_region(zamv_decoder_stats_t *s,int mvx,int mvy){
    int fx=mvx%4,fy=mvy%4;if(fx<0)fx+=4;if(fy<0)fy+=4;
    s->mv_phase_q4[fy*4+fx]++;
    if(!fx&&!fy)s->mv_regions_integer++;
    else if(fx&&!fy)s->mv_regions_hfrac++;
    else if(!fx&&fy)s->mv_regions_vfrac++;
    else s->mv_regions_xyfrac++;
}

zamv_decoder_t*zamv_decoder_create(void){return(zamv_decoder_t*)calloc(1,sizeof(zamv_decoder_t));}
void zamv_decoder_destroy(zamv_decoder_t*d){if(!d)return;free(d->payload);zamv_frame_free(&d->last);zamv_frame_free(&d->golden);zamv_frame_free(&d->cur);free(d);}
void zamv_decoder_set_row_callback(zamv_decoder_t*d,zamv_row_callback_t cb,void*user){if(d){d->row_cb=cb;d->row_user=user;}}
void zamv_decoder_get_stats(const zamv_decoder_t*d,zamv_decoder_stats_t*s){if(d&&s)*s=d->stats;}
void zamv_decoder_reset_stats(zamv_decoder_t*d){if(d)memset(&d->stats,0,sizeof(d->stats));}
static int zamv_file_read_cb(void *user, void *dst, size_t want, size_t *got){
    FILE *f=(FILE*)user;
    size_t n=fread(dst,1,want,f);
    if(got)*got=n;
    if(n<want && ferror(f))return -1;
    return 0;
}

/* Return 1 when exactly n bytes were read, 0 for clean EOF before the first
   byte when allow_clean_eof is true, and -1 for truncation/source error. */
static int read_exact_cb(zamv_read_callback_t cb,void *user,void *dst,size_t n,int allow_clean_eof){
    uint8_t *p=(uint8_t*)dst; size_t done=0;
    if(!cb)return -1;
    while(done<n){
        size_t got=0;
        if(cb(user,p+done,n-done,&got)<0)return -1;
        if(got>n-done)return -1;
        if(!got)return (allow_clean_eof && !done)?0:-1;
        done+=got;
    }
    return 1;
}

int zamv_decoder_read_header_cb(zamv_decoder_t*d,zamv_read_callback_t read_cb,void*read_user,zamv_file_header_t*h){
    uint8_t x[64];
    if(read_exact_cb(read_cb,read_user,x,64,0)<=0)return-1;
    if(memcmp(x,ZAMV_MAGIC,4)||x[4]!=ZAMV_VERSION||get16(x+6)!=64)return-2;
    d->hdr.version=x[4];d->hdr.chroma=x[5];d->hdr.width=get16(x+8);d->hdr.height=get16(x+10);
    d->hdr.fps_num=get32(x+12);d->hdr.fps_den=get32(x+16);d->hdr.frame_count=get32(x+20);
    d->hdr.target_video_bps=get32(x+24);d->hdr.reserved_audio_bps=get32(x+28);d->hdr.keyint=get16(x+32);
    if(d->hdr.width%16||d->hdr.height%16)return-3;
    if(zamv_frame_alloc(&d->last,d->hdr.width,d->hdr.height)||zamv_frame_alloc(&d->golden,d->hdr.width,d->hdr.height)||zamv_frame_alloc(&d->cur,d->hdr.width,d->hdr.height))return-4;
    d->have_header=1;zamv_sh4_prepare_transform();if(h)*h=d->hdr;return 0;
}
int zamv_decoder_read_header(zamv_decoder_t*d,FILE*in,zamv_file_header_t*h){return zamv_decoder_read_header_cb(d,zamv_file_read_cb,in,h);}

int zamv_decoder_decode_cb(zamv_decoder_t*d,zamv_read_callback_t read_cb,void*read_user,zamv_frame_t*out){

    if(!d->have_header)return-2;
    /* CD-authored files may be padded to a 2048-byte sector boundary. Once a
       finalized frame count is present, stop before touching that padding. */
    if(d->hdr.frame_count && d->frame_no>=d->hdr.frame_count)return 0;
    uint8_t fh[16];
    int rr=read_exact_cb(read_cb,read_user,fh,16,1);
    if(rr==0)return 0;
    if(rr<0)return-3;
    uint32_t n=get32(fh),fno=get32(fh+4);
    int type=fh[8],q=fh[9],refresh=fh[10]&1;
    if(n>32u*1024u*1024u||!q)return-4;
    if(n>d->payload_cap){size_t nc=d->payload_cap?d->payload_cap:65536u;while(nc<n)nc*=2u;uint8_t*np=(uint8_t*)realloc(d->payload,nc);if(!np)return-5;d->payload=np;d->payload_cap=nc;}
    if(read_exact_cb(read_cb,read_user,d->payload,n,0)<=0)return-6;
    zamv_reader_t r;
    zr_init(&r,d->payload,n);
    uint32_t fp_cookie=zamv_sh4_fp_enter();
    /* The IDCT runs through the resident XMTRX, which the rest of the program is free to use between frames (the
       game's renderer loads a rotation there for every rotated sprite): load it for every frame, not just once when
       the header was read. */
    zamv_sh4_prepare_transform();
#define ZAMV_DEC_RETURN(code) do { zamv_sh4_fp_leave(fp_cookie); return (code); } while(0)
    int mbw=d->hdr.width/16,mbh=d->hdr.height/16;
    uint8_t py[256],pu[64],pv[64];
    /* Decoder default is lazy LAST seeding.  A full 640x480 YUV420 seed moves
       460,800 bytes every inter frame even when most MBs are overwritten by
       prediction/fill/intra.  Lazy mode copies only SKIP and LAST+(0,0) MBs.
       Define ZAMV_EAGER_FRAME_SEED for A/B validation or workloads where nearly
       the entire frame is unchanged. */
    if(type==ZAMV_FRAME_INTER){
        if(!d->have_last)ZAMV_DEC_RETURN(-8);
#ifdef ZAMV_EAGER_FRAME_SEED
        zamv_copy_frame(&d->cur,&d->last);
        ZAMV_STAT_ADD(d,frame_seed_bytes,zamv_frame_bytes(d->hdr.width,d->hdr.height));
#endif
    }
    for(int my=0;my<mbh;my++) {
        for(int mx=0;mx<mbw;mx++){int mode=(int)zr_get_bits(&r,2);if(r.error){ZAMV_DEC_RETURN(-7);}
        if(mode==ZAMV_MB_SKIP){if(!d->have_last)ZAMV_DEC_RETURN(-8);ZAMV_STAT_INC(d,mb_skip);
#ifndef ZAMV_EAGER_FRAME_SEED
            copy_ref_mb_to_frame(&d->cur,&d->last,mx,my);ZAMV_STAT_ADD(d,lazy_seed_bytes,384u);
#endif
            continue;}
        if(mode==ZAMV_MB_FILL){ZAMV_STAT_INC(d,mb_fill);uint8_t iy=(uint8_t)zr_get_bits(&r,8),iu=(uint8_t)zr_get_bits(&r,8),iv=(uint8_t)zr_get_bits(&r,8);fill_mb(&d->cur,mx,my,iy,iu,iv);continue;}
        if(mode==ZAMV_MB_INTRA_DC){ZAMV_STAT_INC(d,mb_intra);int im=(int)zr_get_bits(&r,2);uint8_t iy=128,iu=128,iv=128;if(im==ZAMV_INTRA_DC){iy=(uint8_t)zr_get_bits(&r,8);iu=(uint8_t)zr_get_bits(&r,8);iv=(uint8_t)zr_get_bits(&r,8);}if(make_intra_pred(&d->cur,mx,my,im,iy,iu,iv,py,pu,pv)<0){ZAMV_DEC_RETURN(-9);}if(decode_blocks_from_pred(&r,&d->cur,mx,my,py,pu,pv,q,ZAMV_STAT_PTR(d))<0){ZAMV_DEC_RETURN(-9);}continue;}
        if(mode==ZAMV_MB_INTER){
            ZAMV_STAT_INC(d,mb_inter);
            int ref=(int)zr_get_bit(&r),split=(int)zr_get_bit(&r),mvx=zr_get_se(&r),mvy=zr_get_se(&r);const zamv_frame_t*rf=ref?(d->golden_alias_last?&d->last:&d->golden):&d->last;
            if((ref&&!d->have_golden)||(!ref&&!d->have_last)){ZAMV_DEC_RETURN(-10);}
            if(split){
                int smvx[4],smvy[4],seeded=(ref==ZAMV_REF_LAST || (ref==ZAMV_REF_GOLDEN && d->golden_alias_last));
                for(int qi=0;qi<4;qi++){smvx[qi]=mvx+zr_get_se(&r);smvy[qi]=mvy+zr_get_se(&r);if(smvx[qi]||smvy[qi])seeded=0;}
                if(seeded){
                    ZAMV_STAT_INC(d,mb_inter_seed_reuse);
#ifndef ZAMV_EAGER_FRAME_SEED
                    copy_ref_mb_to_frame(&d->cur,&d->last,mx,my);ZAMV_STAT_ADD(d,lazy_seed_bytes,384u);
#endif
                } else {for(int qi=0;qi<4;qi++)ZAMV_STAT_MV(d,smvx[qi],smvy[qi]);pred_mb_split_to_frame(rf,&d->cur,mx,my,smvx,smvy);}
            } else {
                if((ref==ZAMV_REF_LAST || (ref==ZAMV_REF_GOLDEN && d->golden_alias_last)) && mvx==0 && mvy==0){
                    ZAMV_STAT_INC(d,mb_inter_seed_reuse);
#ifndef ZAMV_EAGER_FRAME_SEED
                    copy_ref_mb_to_frame(&d->cur,&d->last,mx,my);ZAMV_STAT_ADD(d,lazy_seed_bytes,384u);
#endif
                } else {ZAMV_STAT_MV(d,mvx,mvy);pred_mb_to_frame(rf,&d->cur,mx,my,mvx,mvy);}
            }
            if(decode_blocks_inplace(&r,&d->cur,mx,my,q,ZAMV_STAT_PTR(d))<0){ZAMV_DEC_RETURN(-11);}continue;
        }
        }
        if(d->row_cb)d->row_cb(d->row_user,&d->cur,my);
    }
    if(r.error)ZAMV_DEC_RETURN(-12);
    if(out){if(out->width!=d->hdr.width||out->height!=d->hdr.height||!out->storage)ZAMV_DEC_RETURN(-13);zamv_copy_frame(out,&d->cur);}
#ifdef ZAMV_COPY_GOLDEN_REFRESH
    if(refresh||type==ZAMV_FRAME_KEY){
        zamv_copy_frame(&d->golden,&d->cur);
        ZAMV_STAT_ADD(d,golden_refresh_copy_bytes,zamv_frame_bytes(d->hdr.width,d->hdr.height));
        d->have_golden=1;
    }
    { zamv_frame_t tmp=d->last; d->last=d->cur; d->cur=tmp; }
    d->golden_alias_last=0;
#else
    /* Three surfaces are enough to refresh GOLDEN without copying a frame.
       On a refresh CURRENT becomes both logical LAST and logical GOLDEN, while
       the physical GOLDEN surface is temporarily free.  On the next non-refresh
       frame, rotate that free surface into CURRENT and move the old LAST surface
       into physical GOLDEN. */
    if(refresh||type==ZAMV_FRAME_KEY){
        zamv_frame_t tmp=d->last; d->last=d->cur; d->cur=tmp;
        d->golden_alias_last=1;
        d->have_golden=1;
    } else if(d->golden_alias_last){
        zamv_frame_t tmp=d->golden;
        d->golden=d->last;
        d->last=d->cur;
        d->cur=tmp;
        d->golden_alias_last=0;
    } else {
        zamv_frame_t tmp=d->last; d->last=d->cur; d->cur=tmp;
    }
#endif
    d->have_last=1;
    d->frame_no=fno+1;
    ZAMV_STAT_INC(d,frames);
    zamv_sh4_fp_leave(fp_cookie);
#undef ZAMV_DEC_RETURN
    return 1;

}
int zamv_decoder_decode(zamv_decoder_t*d,FILE*in,zamv_frame_t*out){return zamv_decoder_decode_cb(d,zamv_file_read_cb,in,out);}
