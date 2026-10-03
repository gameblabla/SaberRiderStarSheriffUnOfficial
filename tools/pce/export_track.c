/* Host-only access to the source's generated circuit. No shared source edit. */
#include "../../src/mode7.c"
Ren *rnull_renderer(void);
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    Mode7 *m=mode7_create(rnull_renderer(),256,224,1,3,false);
    if(!m)return 1;
    FILE *f=fopen(argv[1],"wb");if(!f)return 1;
    /* 256 samples retain all sixteen spline sections. Each serialized record
     * includes the source world position and a 32-heading tangent. */
    for(int i=0;i<256;++i) {
        int j=i*4,k=(j+4)%TRACK_N;
        unsigned x=(unsigned)lroundf(m->tx[j]),y=(unsigned)lroundf(m->ty[j]);
        int h=(int)lroundf(atan2f(m->ty[k]-m->ty[j],m->tx[k]-m->tx[j])*16/PI)&31;
        fputc(x,f);fputc(x>>8,f);fputc(y,f);fputc(y>>8,f);fputc(h,f);
    }
    fclose(f);mode7_destroy(m);return 0;
}
