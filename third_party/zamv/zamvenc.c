#include "zamv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *p){fprintf(stderr,"usage: %s in.yuv|- out.zamv width height fps_num fps_den [--target-kbps N] [--audio-kbps N] [--cd-kbps N] [--vbv-ms N] [--q N] [--qmin N] [--qmax N] [--keyint N] [--search N] [--no-qpel] [--no-split] [--golden N] [--frames N] [--pad-sector]\n",p);}
int main(int ac,char**av){
    if(ac<7){usage(av[0]);return 2;}const char*ip=av[1],*op=av[2];int w=atoi(av[3]),h=atoi(av[4]);uint32_t fn=(uint32_t)strtoul(av[5],0,10),fd=(uint32_t)strtoul(av[6],0,10);uint32_t frames=0;int pad_sector=0;
    zamv_encoder_config_t c={0};c.width=w;c.height=h;c.fps_num=fn;c.fps_den=fd;c.target_video_bps=3400000;c.reserved_audio_bps=384000;c.max_total_bps=4800000;c.vbv_window_ms=500;c.keyint=120;c.search_range=12;c.qmin=3;c.qmax=72;c.qinitial=9;c.enable_qpel=1;c.enable_split8=1;c.golden_period=30;
    for(int i=7;i<ac;i++){if(!strcmp(av[i],"--target-kbps")&&i+1<ac)c.target_video_bps=(uint32_t)atoi(av[++i])*1000u;else if(!strcmp(av[i],"--audio-kbps")&&i+1<ac)c.reserved_audio_bps=(uint32_t)atoi(av[++i])*1000u;else if(!strcmp(av[i],"--cd-kbps")&&i+1<ac)c.max_total_bps=(uint32_t)atoi(av[++i])*1000u;else if(!strcmp(av[i],"--vbv-ms")&&i+1<ac)c.vbv_window_ms=atoi(av[++i]);else if(!strcmp(av[i],"--q")&&i+1<ac)c.qinitial=atoi(av[++i]);else if(!strcmp(av[i],"--qmin")&&i+1<ac)c.qmin=atoi(av[++i]);else if(!strcmp(av[i],"--qmax")&&i+1<ac)c.qmax=atoi(av[++i]);else if(!strcmp(av[i],"--keyint")&&i+1<ac)c.keyint=atoi(av[++i]);else if(!strcmp(av[i],"--search")&&i+1<ac)c.search_range=atoi(av[++i]);else if(!strcmp(av[i],"--golden")&&i+1<ac)c.golden_period=atoi(av[++i]);else if(!strcmp(av[i],"--frames")&&i+1<ac)frames=(uint32_t)atoi(av[++i]);else if(!strcmp(av[i],"--no-qpel"))c.enable_qpel=0;else if(!strcmp(av[i],"--no-split"))c.enable_split8=0;else if(!strcmp(av[i],"--pad-sector"))pad_sector=1;else{usage(av[0]);return 2;}}
    FILE*in=!strcmp(ip,"-")?stdin:fopen(ip,"rb");FILE*out=fopen(op,"wb");if(!in||!out){perror("open");return 1;}zamv_encoder_t*e=zamv_encoder_create(&c);if(!e){fprintf(stderr,"encoder init failed (dimensions must be multiples of 16)\n");return 1;}if(zamv_encoder_write_header(e,out,frames)<0){fprintf(stderr,"header write failed\n");return 1;}
    zamv_frame_t f;if(zamv_frame_alloc(&f,w,h)<0)return 1;size_t fb=zamv_frame_bytes(w,h);uint32_t n=0;long start=ftell(out);while(!frames||n<frames){size_t got=fread(f.storage,1,fb,in);if(got==0)break;if(got!=fb){fprintf(stderr,"short raw frame at %u\n",n);break;}if(zamv_encoder_encode(e,&f,out)<0){fprintf(stderr,"encode failed at frame %u\n",n);return 1;}n++;if(!(n%30)){long pos=ftell(out);double sec=(double)n*fd/fn;fprintf(stderr,"\r%u frames, %.1f kbps video",n,sec>0?(pos-start)*8.0/sec/1000.0:0);}}
    long end=ftell(out);double sec=(double)n*fd/fn;
    if(fseek(out,20,SEEK_SET)==0){unsigned char fc[4]={(unsigned char)n,(unsigned char)(n>>8),(unsigned char)(n>>16),(unsigned char)(n>>24)};fwrite(fc,1,4,out);fseek(out,end,SEEK_SET);}
    if(pad_sector){long rem=end & 2047L;if(rem){unsigned char z[2048]={0};size_t pn=(size_t)(2048-rem);if(fwrite(z,1,pn,out)!=pn){fprintf(stderr,"sector padding failed\n");return 1;}end+=pn;}}
    fprintf(stderr,"\r%u frames encoded, %.1f kbps video, %.2f MiB%s\n",n,sec>0?(end-start)*8.0/sec/1000.0:0,(end-start)/1048576.0,pad_sector?" (sector padded)":"");
    zamv_frame_free(&f);zamv_encoder_destroy(e);if(in!=stdin)fclose(in);fclose(out);return 0;
}
