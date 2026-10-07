#include "svm.h"
int svm_header_parse(const uint8_t *p,size_t size,uint32_t file_size,svm_header_t *o)
{
    if(!p||!o||size<SVM_HEADER_BYTES||svm_be32(p)!=0x53564d31u||svm_be16(p+4)!=0x100u||svm_be16(p+6)!=96u)return -1;
    o->width=svm_be16(p+8);o->height=svm_be16(p+10);o->fps_num=svm_be16(p+12);o->fps_den=svm_be16(p+14);
    o->frames=svm_be32(p+16);o->index=svm_be32(p+20);o->data=svm_be32(p+24);
    uint32_t rate=svm_be32(p+28);
    o->rate=(uint16_t)rate;o->coef1=(int16_t)svm_be16(p+48);o->coef2=(int16_t)svm_be16(p+50);
    /* Offset 54 identifies source-sample Q13 coefficients. Older files
     * carried half-Q12 DSP coefficients; normalize those at the boundary. */
    uint16_t coef_bits=svm_be16(p+54);
    if(coef_bits==0) {
        int32_t a=(int32_t)o->coef1*4,b=(int32_t)o->coef2*4;
        if(a>32767||a<-32768||b>32767||b<-32768)return -2;
        o->coef1=(int16_t)a;o->coef2=(int16_t)b;
    } else if(coef_bits!=13)return -2;
    if(!o->width||o->width>352u||!o->height||o->height>240u||!o->fps_num||!o->fps_den||
       o->fps_num>30u*o->fps_den||!o->frames||o->frames>SVM_MAX_FRAMES||rate<12000u||rate>44100u||
       svm_be32(p+32)!=2u||svm_be32(p+36)!=18u||svm_be32(p+40)!=32u)return -2;
    if(o->index<96u||o->index>file_size||o->frames>(file_size-o->index)/8u||
       o->data<o->index+o->frames*8u||o->data>=file_size||(o->data&2047u))return -3;
    return 0;
}
int svm_packet_parse(const uint8_t *p,uint32_t size,svm_packet_t *o)
{
    if(!p||!o||size<32u||size>SVM_MAX_PACKET||svm_be32(p)!=0x504b5430u||svm_be32(p+4)!=size)return -1;
    uint32_t vs=svm_be32(p+16),as=svm_be32(p+20),ds=svm_be32(p+28),left=size-32u;
    if(ds>left)return -2;left-=ds;if(vs>left||vs>65536u)return -2;left-=vs;if(as!=left)return -2;
    o->frame_no=svm_be32(p+8);o->pts=svm_be32(p+12);
    o->dsp=p+32u;o->dsp_size=ds;o->video=p+32u+ds;o->video_size=vs;o->audio=o->video+vs;o->audio_size=as;
    return 0;
}
