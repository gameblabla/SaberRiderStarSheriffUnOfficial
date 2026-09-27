#include "zamv.h"
#include <stdlib.h>
#include <string.h>

struct zamv_chunk_reader {
    zamv_read_callback_t source;
    void *source_user;
    uint8_t *buf;
    size_t chunk_bytes;
    size_t pos, have;
    int eof, error;
    uint64_t refills, source_bytes;
};

zamv_chunk_reader_t *zamv_chunk_reader_create(zamv_read_callback_t source, void *source_user, size_t chunk_bytes) {
    zamv_chunk_reader_t *r;
    if(!source || chunk_bytes < 2048u || (chunk_bytes & 2047u)) return NULL;
    r=(zamv_chunk_reader_t*)calloc(1,sizeof(*r));
    if(!r) return NULL;
    r->buf=(uint8_t*)malloc(chunk_bytes);
    if(!r->buf){free(r);return NULL;}
    r->source=source; r->source_user=source_user; r->chunk_bytes=chunk_bytes;
    return r;
}
void zamv_chunk_reader_destroy(zamv_chunk_reader_t *r){if(r){free(r->buf);free(r);}}
size_t zamv_chunk_reader_chunk_bytes(const zamv_chunk_reader_t *r){return r?r->chunk_bytes:0;}
uint64_t zamv_chunk_reader_refills(const zamv_chunk_reader_t *r){return r?r->refills:0;}
uint64_t zamv_chunk_reader_source_bytes(const zamv_chunk_reader_t *r){return r?r->source_bytes:0;}

static int refill(zamv_chunk_reader_t *r){
    size_t got=0;
    if(r->error) return -1;
    if(r->eof) return 0;
    if(r->source(r->source_user,r->buf,r->chunk_bytes,&got)<0){r->error=1;return -1;}
    r->source_bytes += got;
    r->pos=0; r->have=got;
    if(!got){r->eof=1;return 0;}
    r->refills++;
    return 1;
}

int zamv_chunk_reader_read(void *user, void *dstv, size_t want, size_t *got_out){
    zamv_chunk_reader_t *r=(zamv_chunk_reader_t*)user;
    uint8_t *dst=(uint8_t*)dstv;
    size_t total=0;
    if(!r || (!dst && want)){if(got_out)*got_out=0;return -1;}
    while(total<want){
        size_t avail,n;
        if(r->pos==r->have){int rr=refill(r);if(rr<0){if(got_out)*got_out=total;return -1;}if(rr==0)break;}
        avail=r->have-r->pos; n=want-total; if(n>avail)n=avail;
        memcpy(dst+total,r->buf+r->pos,n); r->pos+=n; total+=n;
    }
    if(got_out)*got_out=total;
    return 0;
}
