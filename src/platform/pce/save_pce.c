#include "save_pce.h"
static const uint8_t filename[12]={0,0,'S','A','B','E','R',' ','P','C','E',' '};
volatile uint8_t pce_save_status;
static uint16_t checksum(const PceSave *s) {
    const uint8_t *p=(const uint8_t*)s;uint16_t n=0x5342;
    for(uint8_t i=0;i<6;++i)n=(n<<1)^(n>>15)^p[i];return n;
}
static uint8_t transfer(PceSave *s,bool write) {
    *(volatile uint16_t*)0x20f8=(uint16_t)filename;
    *(volatile uint16_t*)0x20fa=(uint16_t)s;
    *(volatile uint16_t*)0x20fc=sizeof *s;
    *(volatile uint16_t*)0x20fe=0;
    uint8_t result;
    if(write) {
        __attribute__((leaf)) asm volatile("jsr $e051" : "=a"(result) :: "x","y","p","memory");
    } else {
        __attribute__((leaf)) asm volatile("jsr $e04e" : "=a"(result) :: "x","y","p","memory");
    }
    pce_save_status=result;return result;
}
bool save_load(PceSave *s) {
    if(transfer(s,false))return false;
    uint16_t sum=s->checksum_lo|(uint16_t)s->checksum_hi<<8;
    if(s->version!=1||s->stage<1||s->stage>7||s->hero>3||s->phase>1||sum!=checksum(s)) {
        pce_save_status=0xfe;return false;
    }
    return true;
}
bool save_store(uint8_t stage,uint8_t hero,uint8_t phase) {
    PceSave s={.version=1,.stage=stage,.hero=hero,.phase=phase};
    uint16_t sum=checksum(&s);s.checksum_lo=sum;s.checksum_hi=sum>>8;
    return transfer(&s,true)==0;
}
