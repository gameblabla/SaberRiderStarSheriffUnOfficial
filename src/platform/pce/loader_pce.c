#include "loader_pce.h"
#include "arcade_pce.h"
#include "video_pce.h"
#include "samples.h"
#include "pcm.h"
#include "audio_pcm.h"
#include "campaign_pce.h"

static bool music_active;
volatile uint8_t pce_music_status;

static uint8_t voice_hero,voice_priority;
#define DISC_SECTOR(name) extern char __cd_##name##__sector[]
DISC_SECTOR(s1_bin); DISC_SECTOR(s2_bin); DISC_SECTOR(s3_bin);
DISC_SECTOR(s4_bin); DISC_SECTOR(s5_bin); DISC_SECTOR(s6_bin);
DISC_SECTOR(s7_bin); DISC_SECTOR(font_bin);
DISC_SECTOR(ui_bin);
DISC_SECTOR(voice0_bin);DISC_SECTOR(voice1_bin);DISC_SECTOR(voice2_bin);DISC_SECTOR(voice3_bin);
static uint32_t sector_of(uint8_t stage) {
    switch (stage) {
    case 1: return (uint32_t)__cd_s1_bin__sector;
    case 2: return (uint32_t)__cd_s2_bin__sector;
    case 3: return (uint32_t)__cd_s3_bin__sector;
    case 4: return (uint32_t)__cd_s4_bin__sector;
    case 5: return (uint32_t)__cd_s5_bin__sector;
    case 6: return (uint32_t)__cd_s6_bin__sector;
    default: return (uint32_t)__cd_s7_bin__sector;
    }
}
void audio_stop(void) {
    if (music_active) pce_cdb_cdda_pause();
    music_active = false;
    voice_priority=0;
    audio_pcm_stop();
    pce_cdb_adpcm_stop();
}
bool loader_voice(uint8_t hero) {
    if(hero>3||music_active){++pce_metrics.forbidden_reads;return false;}
    uint32_t sector=hero==0?(uint32_t)__cd_voice0_bin__sector:hero==1?(uint32_t)__cd_voice1_bin__sector:
        hero==2?(uint32_t)__cd_voice2_bin__sector:(uint32_t)__cd_voice3_bin__sector;
    pce_sector_t s={.lo=sector,.md=sector>>8,.hi=sector>>16};
    uint16_t bytes=voice_samples[hero][4][0]+voice_samples[hero][4][1];
    pce_cdb_adpcm_reset();++pce_metrics.disc_reads;
    uint8_t error=pce_cdb_adpcm_read_from_cd(s,((uint32_t)bytes+2047)>>11,0);
    if(error){pce_metrics.load_error=error;return false;}
    voice_hero=hero;return true;
}
bool loader_font(void) {
    if(music_active){++pce_metrics.forbidden_reads;return false;}
    uint32_t sector=(uint32_t)__cd_font_bin__sector;
    pce_sector_t s={.lo=sector,.md=sector>>8,.hi=sector>>16};
    ++pce_metrics.disc_reads;
    uint8_t error = pce_cdb_cd_read(s, PCE_CDB_VRAM_BYTES, PCE_FONT_WORD, 3072);
    if (error) { pce_metrics.load_error = error; return false; }
    return true;
}
static bool loader_archive(uint32_t sector,uint32_t remaining) {
    audio_stop(); video_display(false); pce_raster_enabled = 0;
    pce_cdb_irq_disable(PCE_CDB_MASK_VBLANK_NO_BIOS | PCE_CDB_MASK_HBLANK_NO_BIOS);
    uint32_t address=0;
    while (remaining) {
        /* $75-$7c are a 64 KiB CD transfer buffer. Only MPR6 changes;
         * code, IRQs, the stack and all live loader state remain mapped. */
        uint8_t sectors = remaining >= 65536UL ? 32 : remaining >> 11;
        pce_sector_t s = {.lo=sector, .md=sector>>8, .hi=sector>>16};
        ++pce_metrics.disc_reads;
        uint8_t error = pce_cdb_cd_read(s, PCE_CDB_BANK_MPR6, 0x75, sectors);
        if (error) { pce_metrics.load_error = error; return false; }
        uint16_t chunk_sectors = sectors;
        for (uint8_t bank = 0; chunk_sectors; ++bank) {
            uint16_t size = chunk_sectors >= 4 ? 8192 : chunk_sectors << 11;
            pce_bank6_set(0x75 + bank);
            bool ok = arcade_write(0, address, (const void *)0xc000, size);
            pce_bank6_set(0x6c);
            if (!ok) { pce_metrics.load_error = 0xfe; return false; }
            address += size; remaining -= size; chunk_sectors -= size >> 11;
        }
        sector += sectors;
    }
    pce_cdb_irq_enable(PCE_CDB_MASK_VBLANK_NO_BIOS | PCE_CDB_MASK_HBLANK_NO_BIOS);
    return true;
}
bool loader_ui(void) {return loader_archive((uint32_t)__cd_ui_bin__sector,PCE_UI_BYTES);}
bool loader_scene(uint8_t stage) {
    if(!stage||stage>7)return false;
    if(!loader_archive(sector_of(stage),pce_scenes[stage-1].bytes))return false;
    pce_metrics.stage = stage;
    video_scene(&pce_scenes[stage-1]);
    return true;
}
void audio_music(uint8_t track) {
    if(track>=18)return;
    /* Volume blocks (HIGH, MEDIUM, LOW) hold the same 18 tracks at different
     * levels; OFF plays nothing. Track 1 is the data track. */
    static const uint8_t block[4]={0,40,21,2};
    uint8_t level=pce_options.music&3;
    if(!level){audio_stop();return;}
    track+=block[level];
    /* BIOS track parameters occupy AL/CL (sector.hi), in BCD. The SDK's
     * sector.track alias occupies BL/DL and cannot be used for this call. */
    pce_sector_t start = {.hi=(track/10)*16+track%10};
    ++track;
    pce_sector_t end = {.hi=(track/10)*16+track%10};
    pce_music_status=pce_cdb_cdda_play(PCE_CDB_LOCATION_TYPE_TRACK, start,
        PCE_CDB_LOCATION_TYPE_TRACK, end, PCE_CDB_CDDA_PLAY_REPEAT);
    music_active = pce_music_status == 0;
}
void audio_effect(uint8_t tone) {
    if(tone==2||tone==5||tone==6||tone==7||tone==8) {
        /* 7/8: enemy hit / death yells share the hero's ADPCM bank and never cut a hero voice. */
        uint8_t event=tone==2?0:tone==5?1:tone==6?2:tone==7?3:4,priority=event>2?1:event+1;
        if((pce_cdb_adpcm_status()&ADPCM_STOPPED)||priority>=voice_priority) {
            pce_cdb_adpcm_stop();
            pce_cdb_adpcm_play(voice_samples[voice_hero][event][0],voice_samples[voice_hero][event][1],12,PCE_CDB_ADPCM_ONE_SHOT);
            voice_priority=priority;
        }
    }
    if(tone==2||tone==5||tone==6||tone==7||tone==8)return;
    audio_pcm_play(tone==1?0:tone==4?1:2);
}
void audio_tick(void) {}
void audio_pcm_play(uint8_t sample) {
    if(sample>2)return;
    pce_cpu_irq_disable();
    *IO_TIMER_CONTROL=0;
    pce_pcm_left=pcm_samples[sample][1];
    pce_pcm_bankid=pcm_samples[sample][0];
    uint16_t address=0xc000;
    pce_pcm_read[1]=address;pce_pcm_read[2]=address>>8;
    *IO_PSG_VOLUME=0xff;*IO_PSG_CH_SELECT=0;
    *IO_PSG_CH_CONTROL=0;*IO_PSG_CH_VOLUME=0xff;
    *IO_PSG_CH_CONTROL=0xdf;*IO_PSG_CH_SAMPLE=16;
    *IO_TIMER_COUNTER=0;*IO_IRQ_ACK=0;
    pce_irq_enable(IRQ_TIMER);*IO_TIMER_CONTROL=1;
    pce_cpu_irq_enable();
}
