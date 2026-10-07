#ifndef SVM_H
#define SVM_H
#include <stdint.h>
#include <stddef.h>
#define SVM_HEADER_BYTES 96u
#define SVM_MAX_PACKET 70000u
#define SVM_MAX_FRAMES 2048u
static inline uint16_t svm_be16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static inline uint32_t svm_be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
typedef struct {
    uint16_t width,height,fps_num,fps_den,rate;
    int16_t coef1,coef2;
    uint32_t frames,index,data;
} svm_header_t;
typedef struct {
    uint32_t frame_no,pts;
    const uint8_t *video,*audio,*dsp;
    uint32_t video_size,audio_size,dsp_size;
} svm_packet_t;
int svm_header_parse(const uint8_t *p, size_t size, uint32_t file_size, svm_header_t *o);
int svm_packet_parse(const uint8_t *p, uint32_t size, svm_packet_t *o);
/* Unsigned elapsed clock handles timer wrap; use 64-bit only at frame boundaries. */
static inline uint32_t svm_frame_us(uint32_t frame, uint32_t num, uint32_t den) {
    return (uint32_t)(((uint64_t)frame*1000000u*den)/num);
}
#endif
