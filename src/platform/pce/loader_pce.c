#include "loader_pce.h"
#include "arcade_pce.h"
#include "video_pce.h"
#include "audio_pcm.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "ui_pce.h"
#include "cdda_pce.h"

uint8_t pce_stall PCE_WORK;

static uint8_t voice_priority;
#define DISC_SECTOR(name) extern char __cd_##name##__sector[]
DISC_SECTOR(s1_bin); DISC_SECTOR(s2_bin); DISC_SECTOR(s3_bin);
DISC_SECTOR(s4_bin); DISC_SECTOR(s5_bin); DISC_SECTOR(s6_bin);
DISC_SECTOR(s7_bin); DISC_SECTOR(font_bin);
DISC_SECTOR(ui_bin);DISC_SECTOR(victory_bin);
DISC_SECTOR(app_elf);DISC_SECTOR(voice0_bin);DISC_SECTOR(voice1_bin);DISC_SECTOR(voice2_bin);DISC_SECTOR(voice3_bin);
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
void stop_body(void);
void audio_stop(void) {overlay_call(0x75,stop_body);}
bool loader_voice(uint8_t hero) {
    if(hero>3||cdda_busy()){++pce_metrics.forbidden_reads;return false;}
    uint32_t sector=hero==0?(uint32_t)__cd_voice0_bin__sector:hero==1?(uint32_t)__cd_voice1_bin__sector:
        hero==2?(uint32_t)__cd_voice2_bin__sector:(uint32_t)__cd_voice3_bin__sector;
    pce_sector_t s={.lo=sector,.md=sector>>8,.hi=sector>>16};
    uint16_t bytes=audio_pcm_voice_bytes(hero);
    pce_cdb_adpcm_reset();++pce_metrics.disc_reads;
    uint8_t error=pce_cdb_adpcm_read_from_cd(s,((uint32_t)bytes+2047)>>11,0);
    if(error){pce_metrics.load_error=error;return false;}
    return true;
}
bool loader_font(void) {
    if(cdda_busy()){++pce_metrics.forbidden_reads;return false;}
    uint32_t sector=(uint32_t)__cd_font_bin__sector;
    pce_sector_t s={.lo=sector,.md=sector>>8,.hi=sector>>16};
    ++pce_metrics.disc_reads;
    uint8_t error = pce_cdb_cd_read(s, PCE_CDB_VRAM_BYTES, PCE_FONT_WORD, 3072);
    if (error) { pce_metrics.load_error = error; return false; }
    return true;
}
static bool loader_archive(uint32_t sector,uint32_t remaining,bool keep_display) {
    audio_stop(); if(!keep_display)video_display(false);
    pce_raster_enabled = 0;
    pce_cdb_irq_disable(PCE_CDB_MASK_VBLANK_NO_BIOS | PCE_CDB_MASK_HBLANK_NO_BIOS);
    uint32_t address=0;
    while (remaining) {
        /* $76-$77 are a 16 KiB CD transfer buffer ($78-$7c hold code). Only MPR6 changes;
         * code, IRQs, the stack and all live loader state remain mapped. */
        uint8_t sectors = remaining >= 16384UL ? 8 : remaining >> 11;
        pce_sector_t s = {.lo=sector, .md=sector>>8, .hi=sector>>16};
        ++pce_metrics.disc_reads;
        uint8_t error = pce_cdb_cd_read(s, PCE_CDB_BANK_MPR6, 0x76, sectors);
        if (error) { pce_metrics.load_error = error; return false; }
        uint16_t chunk_sectors = sectors;
        for (uint8_t bank = 0; chunk_sectors; ++bank) {
            uint16_t size = chunk_sectors >= 4 ? 8192 : chunk_sectors << 11;
            pce_bank6_set(0x76 + bank);
            bool ok = arcade_write(0, address, (const void *)0xc000, size);
            pce_bank6_set(0x6c);
            if (!ok) { pce_metrics.load_error = 0xfe; return false; }
            address += size; remaining -= size; chunk_sectors -= size >> 11;
        }
        sector += sectors;
    }
    /* $76-$77 held the transfer, and Ramrod's arena overwrote $76, $77, $79 and $7a with its code images: all four go back as they are on the disc
     * (banks 14, 15, 17 and 18 of the 24 in app.elf's flat image, 4 sectors each) */
    for(uint8_t part=0;part<2;++part) {
        uint32_t n=(uint32_t)__cd_app_elf__sector+(part?68:56);
        pce_sector_t s={.lo=n,.md=n>>8,.hi=n>>16};
        uint8_t error=pce_cdb_cd_read(s,PCE_CDB_BANK_MPR6,part?0x79:0x76,8);
        pce_bank6_set(0x6c);
        if(error){pce_metrics.load_error=error;return false;}
    }
    pce_cdb_irq_enable(PCE_CDB_MASK_VBLANK_NO_BIOS | PCE_CDB_MASK_HBLANK_NO_BIOS);
    return true;
}
bool loader_ui(void) {return loader_archive((uint32_t)__cd_ui_bin__sector,PCE_UI_BYTES,false);}
/* One of the 19 victory paintings (each its own small extent of victory.bin). */
bool loader_victory(uint8_t index) {return loader_archive((uint32_t)__cd_victory_bin__sector+pce_victory_sector[index],pce_victory_bytes[index],false);}
bool loader_scene(uint8_t stage) {
    if(!stage||stage>7)return false;
    /* The NOW LOADING screen stays up for the whole disc read; the archive goes to Arcade RAM, not VRAM. */
    if(!loader_archive(sector_of(stage),pce_scenes[stage-1].bytes,true))return false;
    ui_fade_out();   /* the card goes to black (the common fade) before the stage's own screen comes up from black */
    video_display(false);video_scroll(0,0);
    pce_metrics.stage = stage;
    video_scene(&pce_scenes[stage-1]);
    return true;
}
PCE_X3 void stop_body(void) {
    cdda_stop();
    voice_priority=0;
    audio_pcm_stop();
    pce_cdb_adpcm_stop();
}
void audio_music(uint8_t track) {cdda_start(track,true);}
void audio_music_once(uint8_t track) {cdda_start(track,false);}
/* The hero's shot and the flying bosses' guns are native PSG (psg_pce.c); the rest of the effects are the CD ADPCM voices and the DDA samples. Moved
 * to the audio bank ($75): the resident bank is full. */
extern volatile uint8_t psg_live;
extern uint8_t psg_voice,psg_script;
void psg_start(void),psg_step(void);
static uint8_t effect_tone;
__attribute__((noinline,section(".ram_bank117.text"))) static void effect_body(void) {
    uint8_t tone=effect_tone;
    /* 1 the hero's shot, 2 jump, 5 hurt, 6 death, 7/8 enemy hit / death yells, 9 alarm "!", 10 dialogue line, 11 fall, 12-17 the flying bosses'
     * engine pass, gun, rider's gun, both, blast and big bang: CD ADPCM voices (the bank picks a variant) except the guns, which are PSG; a hero
     * voice is never cut by a lower-priority one. */
    if(tone>=20){psg_voice=tone>21;psg_script=tone-17;overlay_call(0x75,psg_start);return;}   /* 20 the countdown's pip, 21 GO (voice 0), 22 a bolt striking a Renegade (voice 1): synthesised PSG scripts 3-5 */
    if(tone==1||(tone>=13&&tone<=15)){psg_voice=tone!=1;psg_script=tone==1?0:tone==14?2:1;overlay_call(0x75,psg_start);return;}
    if(tone==2||(tone>=5&&tone<=19)) {
        audio_pcm_voice(tone);
        if((pce_cdb_adpcm_status()&ADPCM_STOPPED)||pce_voice.priority>=voice_priority) {
            pce_cdb_adpcm_stop();
            pce_cdb_adpcm_play(pce_voice.address,pce_voice.bytes,pce_voice.rate,PCE_CDB_ADPCM_ONE_SHOT);
            voice_priority=pce_voice.priority;
        }
        return;
    }
    audio_pcm_play(tone==4?1:2);
}
void audio_effect(uint8_t tone) {effect_tone=tone;overlay_call(0x75,effect_body);}
PCE_X3 static void audio_poll(void) {if(psg_live)psg_step();}
void audio_tick(void) {cdda_tick();if(psg_live)overlay_call(0x75,audio_poll);}
