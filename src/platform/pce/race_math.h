#pragma once
#include "pce_config.h"
/* Small shared helpers of the race code (each bank that includes this keeps its own copies and sine table: define
 * RACE_SIN_SECTION as the section string before including). */
#ifndef RACE_SIN_SECTION
#error "define RACE_SIN_SECTION"
#endif
static const int8_t SIN[256] __attribute__((section(RACE_SIN_SECTION)))={
0,3,6,9,12,16,19,22,25,28,31,34,37,40,43,46,49,51,54,57,60,63,65,68,71,73,76,78,81,83,85,88,90,92,94,96,98,100,102,104,106,107,109,111,112,113,115,116,
117,118,120,121,122,122,123,124,125,125,126,126,126,127,127,127,127,127,127,127,126,126,126,125,125,124,123,122,122,121,120,118,117,116,115,113,112,111,109,107,
106,104,102,100,98,96,94,92,90,88,85,83,81,78,76,73,71,68,65,63,60,57,54,51,49,46,43,40,37,34,31,28,25,22,19,16,12,9,6,3,
0,-3,-6,-9,-12,-16,-19,-22,-25,-28,-31,-34,-37,-40,-43,-46,-49,-51,-54,-57,-60,-63,-65,-68,-71,-73,-76,-78,-81,-83,-85,-88,-90,-92,-94,-96,-98,-100,-102,-104,-106,-107,-109,-111,-112,-113,-115,-116,
-117,-118,-120,-121,-122,-122,-123,-124,-125,-125,-126,-126,-126,-127,-127,-127,-127,-127,-127,-127,-126,-126,-126,-125,-125,-124,-123,-122,-122,-121,-120,-118,-117,-116,-115,-113,-112,-111,-109,-107,
-106,-104,-102,-100,-98,-96,-94,-92,-90,-88,-85,-83,-81,-78,-76,-73,-71,-68,-65,-63,-60,-57,-54,-51,-49,-46,-43,-40,-37,-34,-31,-28,-25,-22,-19,-16,-12,-9,-6,-3};
static inline int16_t absolute(int16_t n) {return n<0?-n:n;}
static inline int16_t wrapdiff(uint16_t a,uint16_t b) {
    int16_t d=(int16_t)(a-b);
    return d>4096?d-8192:d<-4096?d+8192:d;
}
static inline int8_t cosine(uint16_t angle) {return SIN[(uint8_t)((angle>>8)+64)];}
static inline int8_t sine(uint16_t angle) {return SIN[(uint8_t)(angle>>8)];}
/* distance to a point, within 12% */
static inline int16_t hypot16(int16_t dx,int16_t dy) {
    int16_t a=absolute(dx),b=absolute(dy);
    return a>b?a+(b>>1):b+(a>>1);
}
/* (a * b) / 128 for |b| <= 127, in 16 bits (the magnitude of a is cut to 14 bits' worth of precision) */
__attribute__((noinline,unused)) static int16_t muls(int16_t a,int8_t b) {
    uint16_t m=(uint16_t)(absolute(a)>>2)*(uint8_t)(b<0?-b:b)>>5;
    return (a<0)!=(b<0)?-(int16_t)m:(int16_t)m;
}
