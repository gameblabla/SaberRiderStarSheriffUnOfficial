#include "loader_pce.h"
#include "arcade_pce.h"
#include "video_pce.h"
#include "audio_pcm.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "ui_pce.h"
#include "cdda_pce.h"
#include "sgx_pce.h"

uint8_t pce_stall PCE_WORK;
static uint8_t loaded_scene,loaded_ui;

static uint8_t voice_priority;
static uint16_t voice_frames;   /* frames left in the latched ADPCM voice (a countdown: pce_ticks is only 8 bits) */
#define DISC_SECTOR(name) extern char __cd_##name##__sector[]
#ifdef PCE_SGX
#define __cd_s1_bin__sector __cd_s1_packed_bin__sector
#define __cd_s2_bin__sector __cd_s2_packed_bin__sector
#define __cd_s3_bin__sector __cd_s3_packed_bin__sector
#define __cd_s4_bin__sector __cd_s4_packed_bin__sector
#define __cd_s5_bin__sector __cd_s5_packed_bin__sector
#define __cd_s6_bin__sector __cd_s6_packed_bin__sector
#define __cd_s7_bin__sector __cd_s7_packed_bin__sector
#define __cd_ui_bin__sector __cd_ui_packed_bin__sector
void zx02_arcade(void);
uint16_t zx02_source PCE_WORK;
static uint8_t archive_compressed;
static uint8_t archive_blocks,archive_ui_range;
#endif
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
static bool font_result;
PCE_CODE static void font_body(void) {
    font_result=false;
    if(cdda_busy()){++pce_metrics.forbidden_reads;return;}
    uint32_t sector=(uint32_t)__cd_font_bin__sector;
    pce_sector_t s={.lo=sector,.md=sector>>8,.hi=sector>>16};
    ++pce_metrics.disc_reads;
    uint8_t error = pce_cdb_cd_read(s, PCE_CDB_VRAM_BYTES, PCE_FONT_WORD, 3072);
    if (error) { pce_metrics.load_error = error; return; }
    font_result=true;
}
bool loader_font(void) {
    overlay_call(0x69,font_body);return font_result;
}
static uint32_t archive_sector PCE_WORK;
static uint32_t archive_remaining PCE_WORK;
static bool archive_keep_display PCE_WORK;
static bool archive_result PCE_WORK;
/* Arena image A replaces bank $76, which also owns frontend_card. Restore
 * that bank before change_stage calls the card; loader_scene restores the
 * remaining temporary overlays afterwards. Execute from untouched $71. */
__attribute__((noinline,minsize,section(".ram_bank113.text"))) void loader_card_restore(void) {
    uint32_t n=(uint32_t)__cd_app_elf__sector+56;
    pce_sector_t s={.lo=n,.md=n>>8,.hi=n>>16};
    pce_cdb_irq_disable(PCE_CDB_MASK_VBLANK_NO_BIOS | PCE_CDB_MASK_HBLANK_NO_BIOS);
    ++pce_metrics.disc_reads;
    uint8_t error=pce_cdb_cd_read(s,PCE_CDB_BANK_MPR6,0x76,4);
    pce_bank6_set(0x6c);
    pce_cdb_irq_enable(PCE_CDB_MASK_VBLANK_NO_BIOS | PCE_CDB_MASK_HBLANK_NO_BIOS);
    if(error)pce_metrics.load_error=error;
}
__attribute__((noinline,minsize,section(".ram_bank105.text"))) static void loader_archive_body(void) {
    uint32_t sector=archive_sector,remaining=archive_remaining;
    bool keep_display=archive_keep_display;
    audio_stop(); if(!keep_display)video_display(false);
    pce_raster_enabled = 0;
    pce_cdb_irq_disable(PCE_CDB_MASK_VBLANK_NO_BIOS | PCE_CDB_MASK_HBLANK_NO_BIOS);
    uint32_t address=0;
#ifdef PCE_SGX
    uint8_t block=0;
    if(archive_ui_range==2){address=PCE_UI_CORE_BYTES;block=PCE_UI_CORE_BYTES/16384UL;}
    if(archive_compressed) {
        pce_sector_t s={.lo=sector,.md=sector>>8,.hi=sector>>16};
        ++pce_metrics.disc_reads;
        uint8_t error=pce_cdb_cd_read(s,PCE_CDB_BANK_MPR6,0x76,1);
        pce_bank6_set(0x76);
        const uint8_t *header=(const uint8_t*)0xc000;
        uint16_t count=header[4]|(uint16_t)header[5]<<8;
        if(error||header[0]!='Z'||header[1]!='X'||header[2]!='A'||header[3]!='1'||count>128||count!=((archive_ui_range?PCE_UI_BYTES:remaining)+16383UL)/16384UL) {
            pce_metrics.load_error=error?error:0xfd;archive_result=false;pce_bank6_set(0x6c);return;
        }
        /* Keep the directory outside console RAM and the two input banks. */
        arcade_write(2,0x1fd000,header+6,count);
        archive_blocks=count;
        pce_bank6_set(0x6c);++sector;
        for(uint8_t i=0;i<block;++i){uint8_t n;arcade_read(2,0x1fd000+i,&n,1);sector+=n&127;}
    }
    uint8_t batch_left=0;
    uint16_t batch_offset=0;
#endif
    while (remaining) {
        /* $76-$77 are a 16 KiB CD transfer buffer ($78-$7c hold code). Only MPR6 changes;
         * code, IRQs, the stack and all live loader state remain mapped. */
        uint8_t sectors = remaining >= 16384UL ? 8 : remaining >> 11;
#ifdef PCE_SGX
        uint8_t raw=1;
        if(archive_compressed){
            uint8_t entry;
            arcade_read(2,0x1fd000+block++,&entry,1);
            raw=entry&128;sectors=entry&127;
            if(!sectors||sectors>8){pce_metrics.load_error=0xfd;archive_result=false;return;}
        }
#endif
        pce_sector_t s = {.lo=sector, .md=sector>>8, .hi=sector>>16};
        uint8_t error;
#ifdef PCE_SGX
        if(archive_compressed) {
            if(!batch_left) {
                uint8_t total=sectors;
                for(uint8_t next=block;next<archive_blocks;++next) {
                    uint8_t entry;arcade_read(2,0x1fd000+next,&entry,1);
                    uint8_t n=entry&127;
                    if(!n||n>8||total+n>31)break;
                    total+=n;
                }
                pce_cdb_adpcm_reset();++pce_metrics.disc_reads;
                error=pce_cdb_adpcm_read_from_cd(s,total,0);
                if(error){pce_metrics.load_error=error;archive_result=false;return;}
                batch_left=total;batch_offset=0;
            }
            zx02_source=batch_offset;
            error=raw?pce_cdb_adpcm_write_to_ram(batch_offset,PCE_CDB_BANK_MPR6,0x76,(uint16_t)sectors<<11):0;
            batch_offset+=(uint16_t)sectors<<11;batch_left-=sectors;
        } else
#endif
        {
            ++pce_metrics.disc_reads;
            error=pce_cdb_cd_read(s,PCE_CDB_BANK_MPR6,0x76,sectors);
        }
        if (error) { pce_metrics.load_error = error; archive_result=false; return; }
#ifdef PCE_SGX
        if(archive_compressed&&!raw) {
            uint16_t size=remaining>=16384UL?16384:remaining;
            arcade_seek(0,address);overlay_call(0x82,zx02_arcade);
            address+=size;remaining-=size;sector+=sectors;continue;
        }
#endif
        uint16_t chunk_sectors = sectors;
        for (uint8_t bank = 0; chunk_sectors; ++bank) {
            uint16_t size = chunk_sectors >= 4 ? 8192 : chunk_sectors << 11;
            pce_bank6_set(0x76 + bank);
            bool ok = arcade_write(0, address, (const void *)0xc000, size);
            pce_bank6_set(0x6c);
            if (!ok) { pce_metrics.load_error = 0xfe; archive_result=false; return; }
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
        if(error){pce_metrics.load_error=error;archive_result=false;return;}
    }
    pce_cdb_irq_enable(PCE_CDB_MASK_VBLANK_NO_BIOS | PCE_CDB_MASK_HBLANK_NO_BIOS);
    archive_result=true;
}
static bool loader_archive(uint32_t sector,uint32_t remaining,bool keep_display) {
    archive_sector=sector;archive_remaining=remaining;archive_keep_display=keep_display;
    overlay_call(0x69,loader_archive_body);
    return archive_result;
}
PCE_CODE static void ui_archive_body(void) {
    if(loaded_ui){audio_stop();video_display(false);return;}
    loaded_scene=0;
#ifdef PCE_SGX
    archive_compressed=1;archive_ui_range=1;
    loaded_ui=loader_archive((uint32_t)__cd_ui_bin__sector,PCE_UI_CORE_BYTES,false);
#else
    loaded_ui=loader_archive((uint32_t)__cd_ui_bin__sector,PCE_UI_BYTES,false);
#endif
}
bool loader_ui(void) {
    overlay_call(0x69,ui_archive_body);return loaded_ui;
}
#ifdef PCE_SGX
__attribute__((noinline,minsize,section(".ram_bank113.text"))) static void ui_animation_body(void) {
    if(loaded_ui==2)return;
    if(PCE_UI_BYTES==PCE_UI_CORE_BYTES){loaded_ui=2;return;}
    archive_compressed=1;archive_ui_range=2;
    if(loader_archive((uint32_t)__cd_ui_bin__sector,PCE_UI_BYTES-PCE_UI_CORE_BYTES,false))loaded_ui=2;
}
bool loader_ui_animation(void) {
    overlay_call(0x71,ui_animation_body);return loaded_ui==2;
}
#endif
/* One of the 19 victory paintings (each its own small extent of victory.bin). */
bool loader_victory(uint8_t index) {
    loaded_scene=loaded_ui=0;
#ifdef PCE_SGX
    archive_compressed=0;archive_ui_range=0;
#endif
    return loader_archive((uint32_t)__cd_victory_bin__sector+pce_victory_sector[index],pce_victory_bytes[index],false);
}
#ifdef PCE_SGX
PCE_FLOW
#endif
bool loader_scene(uint8_t stage) {
    if(!stage||stage>7)return false;
    /* The NOW LOADING screen stays up for the whole disc read; the archive goes to Arcade RAM, not VRAM. */
    if(loaded_scene!=stage||stage==6) {
        loaded_scene=loaded_ui=0;
#ifdef PCE_SGX
        archive_compressed=1;archive_ui_range=0;
#endif
        if(!loader_archive(sector_of(stage),pce_scenes[stage-1].bytes,true))return false;
        loaded_scene=stage;
    }
    ui_fade_out();   /* the card goes to black (the common fade) before the stage's own screen comes up from black */
    video_display(false);video_scroll(0,0);
    pce_metrics.stage = stage;
    video_scene(&pce_scenes[stage-1]);
#ifdef PCE_SGX
    if(pce_sgx_active)overlay_call(0x78,pce_sgx_gameplay_begin_body);
#endif
    return true;
}
PCE_X3 void stop_body(void) {
    cdda_stop();
    voice_priority=0;voice_frames=0;
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
     * voice is never cut by a lower-priority one. audio_adpcm_reset before each play: the controller is shared with the BIOS transfer that loaded
     * the bank, and a length counter left in the broken $1xxxx range makes every voice mute itself (audio_pcm.c). */
    if(tone>=20){psg_voice=tone>21;psg_script=tone-17;overlay_call(0x75,psg_start);return;}   /* 20 the countdown's pip, 21 GO (voice 0), 22 a bolt striking a Renegade (voice 1): synthesised PSG scripts 3-5 */
    if(tone==1||(tone>=13&&tone<=15)){psg_voice=tone!=1;psg_script=tone==1?0:tone==14?2:1;overlay_call(0x75,psg_start);return;}
    if(tone==2||(tone>=5&&tone<=19)) {
        audio_pcm_voice(tone);
        /* The controller's own "am I still playing?" cannot be trusted: $180c comes back as 0x29, EndReached AND Playing
         * at once, so the BIOS reports the channel busy indefinitely and every later cue is refused - the game goes
         * quiet for the rest of the level after one high-priority cue (a hero death), which is what the playtest
         * reported as enemy-density dependent, and why the boss wreck (priority 2, fired on the single frame
         * boss_time==8) never sounded. So track the voice's lifetime ourselves, from its own bytes and rate: it lasts
         * bytes*2 nibbles at 32000/(16-rate) Hz. The priority gate then compares against a voice that is genuinely
         * still sounding, and voice_priority expires on its own instead of surviving until the next stage change. */
        if(!voice_frames)voice_priority=0;   /* the latched voice has run its length: nothing is owed priority */
        if(!voice_priority||pce_voice.priority>=voice_priority) {
            pce_cdb_adpcm_stop();          /* clears IFU_INT_END/HALF from the mask, so the reset below cannot raise IRQ2 */
            audio_adpcm_reset();           /* clean flags, length counter and pointers: the shared controller's state */
            if(!pce_cdb_adpcm_play(pce_voice.address,pce_voice.bytes,pce_voice.rate,PCE_CDB_ADPCM_ONE_SHOT)) {
                voice_priority=pce_voice.priority;
                /* 32000/(16-rate) Hz, two nibbles a byte; +1 so a voice is never cut in the frame it starts. */
                uint16_t hz=32000/(16-pce_voice.rate);
                voice_frames=(uint16_t)((uint32_t)pce_voice.bytes*2*60/hz)+1;
            }
        }
        return;
    }
    audio_pcm_play(tone==4?1:2);
}
void audio_effect(uint8_t tone) {effect_tone=tone;overlay_call(0x75,effect_body);}
PCE_X3 static void audio_poll(void) {if(psg_live)psg_step();}
void audio_tick(void) {if(voice_frames)--voice_frames;cdda_tick();if(psg_live)overlay_call(0x75,audio_poll);}
/* A retirement wait must not trust the controller's busy flag (see above):
 * back-to-back hurt+death cues can leave EndReached clear with nothing
 * playing (the second play refused while the first was still busy), so the
 * BIOS would report busy indefinitely and the retry would never load. */
bool audio_voice_active(void) {return voice_frames!=0;}
