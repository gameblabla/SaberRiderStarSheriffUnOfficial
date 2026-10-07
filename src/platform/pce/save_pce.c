#include "save_pce.h"
#include "overlay_pce.h"
static const uint8_t filename[12]={0,0,'S','A','B','E','R',' ','P','C','E',' '};
volatile uint8_t pce_save_status;
static uint16_t checksum(const PceSave *s) {
    const uint8_t *p=(const uint8_t*)s;uint16_t n=0x5342;
    for(uint8_t i=0;i<6;++i)n=(n<<1)^(n>>15)^p[i];return n;
}
static uint8_t load(PceSave *s) {
    *(volatile uint16_t*)0x20f8=(uint16_t)filename;
    *(volatile uint16_t*)0x20fa=(uint16_t)s;
    *(volatile uint16_t*)0x20fc=sizeof *s;
    *(volatile uint16_t*)0x20fe=0;
    uint8_t result;
    __attribute__((leaf)) asm volatile("jsr $e04e" : "=a"(result) :: "x","y","p","memory");
    pce_save_status=result;return result;
}
bool save_load(PceSave *s) {
    if(load(s))return false;
    uint16_t sum=s->checksum_lo|(uint16_t)s->checksum_hi<<8;
    if(s->version!=1||s->stage<1||s->stage>7||s->hero>3||s->phase>1||sum!=checksum(s)) {
        pce_save_status=0xfe;return false;
    }
    return true;
}
/* The BIOS BM_WRITE masks IRQs and slows the CPU for the entire directory
 * search/create/checksum. Access one byte at a time instead: the road raster
 * and PCM can run between accesses, with every MPR restored at each boundary.
 * Keep the System Card directory format so BM_READ can still load the file. */
extern volatile uint16_t bram_address;
extern volatile uint8_t bram_value;
void bram_read_byte(void),bram_write_byte(void);
static PceSave checkpoint;
#define SAVE_CODE __attribute__((noinline,minsize,section(".ram_bank114.text")))
SAVE_CODE static uint8_t read_byte(uint16_t address) {
    bram_address=address;bram_read_byte();return bram_value;
}
SAVE_CODE static void write_byte(uint16_t address,uint8_t value) {
    bram_address=address;bram_value=value;bram_write_byte();
}
SAVE_CODE static uint16_t read_word(uint16_t address) {
    uint8_t lo=read_byte(address);return lo|(uint16_t)read_byte(address+1)<<8;
}
SAVE_CODE static void write_word(uint16_t address,uint16_t value) {
    write_byte(address,value);write_byte(address+1,value>>8);
}
SAVE_CODE static void store_body(void) {
    /* Standard 2 KiB backup RAM. Header pointers use the BIOS's $8000 base;
     * the byte accessor maps that same offset through MPR6 instead of MPR4. */
    uint16_t limit=read_word(0x8004),end=read_word(0x8006),entry=0x8010,size=0;
    pce_save_status=2;   /* unformatted */
    if(read_byte(0x8000)!='H'||read_byte(0x8001)!='U'||
       read_byte(0x8002)!='B'||read_byte(0x8003)!='M')return;
    if(limit>0x8800)limit=0x8800;
    pce_save_status=3;   /* corrupt directory */
    if(limit<0x8012||end<entry||end>limit-2||read_word(end)!=0)return;
    bool found=false;
    while(entry<end) {
        size=read_word(entry);
        if(size<16||size>end-entry)return;
        uint8_t k;
        for(k=0;k<sizeof filename;++k)if(read_byte(entry+4+k)!=filename[k])break;
        if(k==sizeof filename){found=true;break;}
        entry+=size;
    }
    if(!found) {
        size=16+sizeof checkpoint;
        pce_save_status=5;   /* no space */
        if(size>limit-2-end)return;
        /* Prepare the new entry and its terminator before publishing the end
         * pointer. Other saves are preserved and no format operation is done. */
        write_word(entry,size);
        for(uint8_t k=0;k<sizeof filename;++k)write_byte(entry+4+k,filename[k]);
        write_word(entry+size,0);
    } else {
        pce_save_status=4;   /* file too short */
        if(size<16+sizeof checkpoint)return;
    }
    const uint8_t *data=(const uint8_t*)&checkpoint;
    for(uint8_t k=0;k<sizeof checkpoint;++k)write_byte(entry+16+k,data[k]);
    uint16_t sum=0;
    for(uint16_t k=4;k<size;++k)sum+=read_byte(entry+k);
    write_word(entry+2,-sum);
    if(!found)write_word(0x8006,entry+size);
    pce_save_status=0;
}
bool save_store(uint8_t stage,uint8_t hero,uint8_t phase) {
    checkpoint=(PceSave){.version=1,.stage=stage,.hero=hero,.phase=phase};
    uint16_t sum=checksum(&checkpoint);checkpoint.checksum_lo=sum;checkpoint.checksum_hi=sum>>8;
    overlay_call(0x72,store_body);
    return pce_save_status==0;
}
