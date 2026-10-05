#include "video_pce.h"
#include "arcade_pce.h"
#include "sprite_cache_pce.h"
#include "overlay_pce.h"
#include "loader_pce.h"
#include <string.h>

volatile PceTelemetry pce_metrics;
volatile uint8_t pce_ticks, pce_raster_enabled, pce_floor_page, pce_raster_row;
uint8_t pce_road_off,pce_road_prev;   /* irq.S: the road's table half on display (0 or 128), the picture copy the last scanline read */
volatile uint16_t pce_draws;
volatile uint8_t pce_vdc_index;
volatile uint16_t pce_scroll_x, pce_scroll_y;
volatile uint8_t pce_scroll_hold;
volatile uint8_t pce_floor_pending;
volatile uint8_t pce_race_dialog;
static const PceScene *scene;
const PceScene *video_scene_ptr;
static uint16_t cache_ids[PCE_BG_MAX_TILES] PCE_WORK;
uint16_t cache_refs[PCE_BG_MAX_TILES] PCE_WORK;   /* (the race borrows it for its quarter-square table: race_proj.c) */
uint16_t columns[33][30] PCE_WORK;
static uint16_t first_column, last_column;
/* A tile slot whose last column has scrolled out stays unavailable until the next call: the scroll that drops the column only
 * takes effect at the next VBlank, and until then the old picture (the left edge column) is still displayed from these tiles.
 * Reusing the slot at once showed another column's tiles there on busy frames (several columns a step, a nearly full cache). */
#define HOLD 0x4000
static uint16_t held[48] PCE_WORK;
static uint8_t held_count;
static uint16_t free_cursor,bg_tiles=PCE_BG_MAX_TILES;   /* bg_tiles: Ramrod's arena (stage 6) keeps the cache's top 384 characters ($2800-$3fff) for the arm's and a mech's pattern buffers (m6_d.c) */
uint8_t buffer[2048] PCE_STAGE;
uint8_t sprite_occupancy[240], sprite_line_lo, sprite_line_hi, sprite_line_ok, sprite_exact;
extern void sprite_lines_reserve(void), sprite_lines_release(void);
extern void sprite_lines_clear(void),sprite_cache_begin(void);
uint8_t clipped_pattern[128];
uint8_t clipped_count;
uint8_t sprite_screen_height=224;
vdc_sprite_t sat[2][64];
uint8_t sat_page, sat_count;
uint16_t generic_id;int16_t generic_x,generic_y;uint8_t generic_flip,generic_scale,generic_count,generic_slot,generic_ok;
static uint8_t front_start,front_keep,sat_previous=64;
int16_t sprite_emit_x,sprite_emit_y;
uint16_t sprite_emit_id,video_nsprites;
uint8_t sprite_optional;   /* set by video_sprite_optional: a refusal is not an essential overflow */
uint8_t sprite_emit_flip,sprite_fast_miss,sprite_emit_ok;
extern void sprite_fast(void);
extern void sprite_generic(void);

uint8_t descriptor[384] PCE_STAGE;

PCE_RENDER void video_vdc(uint8_t index, uint16_t value) {
    pce_cpu_irq_disable();
    pce_vdc_index = index;
    *(volatile uint8_t *)0x20f7 = index;
    if (index == VDC_REG_CONTROL) *(volatile uint16_t *)0x20f3 = value;
    *IO_VDC_INDEX = index;
    *IO_VDC_DATA_LO = value;
    *IO_VDC_DATA_HI = value >> 8;
    pce_cpu_irq_enable();
}
/* Ramrod's arena (m6/m6_state.h): its four 8 KiB code images, read from the stage's archive into the code banks $76, $77, $79 and $7a. Here, in the renderer bank
 * that is always mapped, so the window ($6000) can change under the caller. */
PCE_RENDER void m6_load(void) {
    static const uint8_t bank[4]={0x76,0x77,0x79,0x7a};
    uint8_t previous=pce_bank3_get();
    for(uint8_t k=0;k<4;++k){pce_bank3_set(bank[k]);arcade_read(2,PCE_M6_IMAGE+(uint32_t)k*8192,(void*)0x6000,8192);}
    pce_bank3_set(previous);
}
PCE_RENDER void video_init(void) {
    __attribute__((leaf)) asm volatile("csh" ::: "memory");
    pce_cpu_irq_disable();
    pce_vdc_set_resolution(256, 224, 0);
    pce_vdc_bg_set_size(VDC_BG_SIZE_64_32);
    /* Installed SDK labels IDs 4/5 backwards: BIOS vector 4 is VBlank. */
    pce_cdb_irq_set(4, pce_vblank);
    pce_cdb_irq_set(5, pce_hblank);
    pce_cdb_irq_enable(PCE_CDB_MASK_VBLANK | PCE_CDB_MASK_VBLANK_NO_BIOS |
                       PCE_CDB_MASK_HBLANK | PCE_CDB_MASK_HBLANK_NO_BIOS);
    pce_vdc_index = 5;
    video_vdc(VDC_REG_CONTROL, VDC_CONTROL_IRQ_VBLANK);
    pce_irq_enable(IRQ_VDC);
    pce_cpu_irq_enable();
}
PCE_RENDER static void timing(bool wide) {
    pce_cpu_irq_disable();
    *IO_VCE_CONTROL = wide ? 2 : 0;
    video_vdc(VDC_REG_MEMORY, 0x0010);
    video_vdc(VDC_REG_TIMING_HSYNC, wide ? 0x0b02 : 0x0202);
    video_vdc(VDC_REG_TIMING_HDISP, wide ? 0x043f : 0x041f);
    video_vdc(VDC_REG_TIMING_VSYNC, sprite_screen_height==240?0x0c02:0x1702);
    video_vdc(VDC_REG_TIMING_VDISP, sprite_screen_height-1);
    video_vdc(VDC_REG_TIMING_VDISPEND, 12);
    video_vdc(VDC_REG_DMA_CONTROL, 0);
    pce_cpu_irq_enable();
}
/* 320x224 timing for the front end: the 7.16 MHz dot clock, 40 characters. */
PCE_RENDER void video_mode_ui(void) {
    pce_arena_raster=0;
    sprite_screen_height=224;sprite_exact=1;
    pce_cpu_irq_disable();
    *IO_VCE_CONTROL = 1;
    video_vdc(VDC_REG_MEMORY, 0x0010);
    video_vdc(VDC_REG_TIMING_HSYNC, 0x0503);
    video_vdc(VDC_REG_TIMING_HDISP, 0x0627);
    video_vdc(VDC_REG_TIMING_VSYNC, 0x1702);
    video_vdc(VDC_REG_TIMING_VDISP, 223);
    video_vdc(VDC_REG_TIMING_VDISPEND, 12);
    video_vdc(VDC_REG_DMA_CONTROL, 0);
    pce_cpu_irq_enable();
}
PCE_RENDER void video_wait(void) {
    uint8_t previous = pce_ticks;
    while (previous == pce_ticks) {}
    ++pce_metrics.frames;
    audio_tick();   /* the PSG effects' envelopes keep running through the fades and waits that never reach the main loop */
}
extern uint8_t vce_arg_index,vce_arg_count;extern const void *vce_arg_ptr;extern uint16_t vce_arg_value;
void vce_copy_body(void),vce_set_body(void),vce_read_body(void);
PCE_RENDER void vce_copy(uint8_t index,const void *source,uint8_t count) {vce_arg_index=index;vce_arg_ptr=source;vce_arg_count=count;overlay_call(0x75,vce_copy_body);}
PCE_RENDER void vce_set(uint16_t index,uint16_t value) {vce_arg_index=index>>4;vce_arg_count=index&15;vce_arg_value=value;overlay_call(0x75,vce_set_body);}
PCE_RENDER void vce_read(void *dest,uint8_t index,uint8_t count) {vce_arg_index=index;vce_arg_ptr=dest;vce_arg_count=count;overlay_call(0x75,vce_read_body);}
#pragma push_macro("pce_vce_copy_palette")
#undef pce_vce_copy_palette
PCE_RENDER void vce_copy_now(uint8_t index,const void *source,uint8_t count) {pce_vce_copy_palette(index,source,count);}
#pragma pop_macro("pce_vce_copy_palette")
volatile uint8_t pce_display_on;   /* palette RAM is only written in the vertical blank while this is set (pce_config.h) */
PCE_RENDER void video_display(bool enable) {
    pce_display_on=enable;
    video_vdc(VDC_REG_CONTROL, VDC_CONTROL_IRQ_VBLANK | VDC_CONTROL_IRQ_SCANLINE |
              (enable ? VDC_CONTROL_ENABLE_BG | VDC_CONTROL_ENABLE_SPRITE : 0));
}
PCE_RENDER void video_scroll(uint16_t x, uint16_t y) { pce_scroll_x = x; pce_scroll_y = y; }
extern void foreground_reset(void);
PCE_RENDER void video_scene(const PceScene *s) {
    pce_race_dialog=0;
    foreground_reset();vce_hold=0;
    scene = s;video_scene_ptr=s;sat_previous=64;video_nsprites=s->nsprites;
    memset(sprite_slot_of,0xff,sizeof sprite_slot_of);
    sprite_screen_height=224;sprite_exact=1;   /* 224-line mode everywhere: playfield rows 28-29 are simply not shown */
    timing(false);
    memset(cache_refs, 0, sizeof cache_refs);held_count=0;
    memset(cache_ids, 0xff, sizeof cache_ids);
    memset(columns, 0xff, sizeof columns);
    memset(sprite_ids, 0xff, sizeof sprite_ids);
    memset(sprite_used,0,sizeof sprite_used);
    memset(sprite_pinned,0,sizeof sprite_pinned);
    memset(pattern_owner,0,sizeof pattern_owner);
    first_column = last_column = 0xffff;
    pce_arena_raster=0;free_cursor=0;bg_tiles=s==&pce_scenes[5]?512:PCE_BG_MAX_TILES;
    for(uint8_t page=0;page<32;++page)arcade_fill(0x1e0000UL+(uint32_t)page*4096,0xff,4096);
    arcade_read(2, s->pal, buffer, 512);
    pce_vce_copy_palette(0, buffer, 16);
    pce_vce_set_color(255, 0x1ff);
    pce_vdc_index = 0;
}
/* Tile directory (Arcade $1e0000, two bytes per tile id): port 3 base is the
 * only register that changes between lookups. */
static inline uint16_t directory_get(uint16_t id) {
    volatile uint8_t *r=(volatile uint8_t*)0x1a30;
    uint16_t a=id<<1;
    r[2]=a;r[3]=a>>8;r[4]=30+(id>>15);
    uint8_t lo=r[0];return lo|(uint16_t)r[0]<<8;
}
static inline void directory_set(uint16_t id,uint16_t slot) {
    volatile uint8_t *r=(volatile uint8_t*)0x1a30;
    uint16_t a=id<<1;
    r[2]=a;r[3]=a>>8;r[4]=30+(id>>15);
    r[0]=slot;r[0]=slot>>8;
}
PCE_RENDER static void column_release(const uint16_t *refs) {
    for (uint8_t y = 0; y < 30; ++y) {
        uint16_t slot = refs[y];
        if (slot == 0xffff || --cache_refs[slot]) continue;
        if (held_count < 48) {cache_refs[slot] = HOLD; held[held_count++] = slot;}
    }
}
PCE_RENDER static void held_free(void) {
    for (uint8_t k = 0; k < held_count; ++k) cache_refs[held[k]] -= HOLD;
    held_count = 0;
}
/* A column's 30 BAT entries are written with the VDC address increment set to
 * 64 words, so only one address is programmed. IRQs stay masked meanwhile. */
PCE_RENDER static bool column_load(uint16_t world) {
    uint8_t *cb=buffer+1920;   /* the last 128 bytes: the palette snapshot of a fade (fade_pce.c) lives in the first 1024 */
    if (!arcade_read(1, scene->map + (uint32_t)(world % scene->cols) * 90, cb, 90)) return false;
    uint16_t *refs = columns[world % 33];
    arcade_seek(3,0x1e0000UL);
    for (uint8_t y = 0; y < 30; ++y) {
        uint16_t id = cb[y * 3] | (uint16_t)cb[y * 3 + 1] << 8;
        uint16_t slot=directory_get(id);
        if (slot == 0xffff) {
            slot=free_cursor;
            uint16_t checked=0;
            while(cache_refs[slot]) {
                if(++checked==bg_tiles){if(!held_count)return false;held_free();checked=0;}   /* a full cache gives its held slots up */
                if(++slot==bg_tiles)slot=0;
            }
            free_cursor=slot+1;if(free_cursor==bg_tiles)free_cursor=0;
            if(cache_ids[slot]!=0xffff)directory_set(cache_ids[slot],0xffff);
            pce_vdc_index = 2; *(volatile uint8_t *)0x20f7 = 2;
            if (!arcade_vram(scene->tiles + (uint32_t)id * 32,
                             PCE_BG_WORD + slot * 16, 32)) return false;
            cache_ids[slot] = id;
            directory_set(id,slot);
            pce_metrics.uploads += 32;
        }
        ++cache_refs[slot]; refs[y] = slot;
        uint16_t word=(PCE_BG_WORD>>4)+slot+((uint16_t)cb[y*3+2]<<12);
        cb[y*3]=word;cb[y*3+1]=word>>8;
    }
    uint16_t control=*(volatile uint16_t *)0x20f3;
    pce_cpu_irq_disable();
    *(volatile uint8_t *)0x20f7 = 5;
    *IO_VDC_INDEX = 5;
    *IO_VDC_DATA_LO = control;
    *IO_VDC_DATA_HI = (control>>8)|0x10;
    *(volatile uint8_t *)0x20f7 = 0;
    *IO_VDC_INDEX = 0;
    *IO_VDC_DATA_LO = world & 63;*IO_VDC_DATA_HI = 0;
    *(volatile uint8_t *)0x20f7 = 2;
    *IO_VDC_INDEX = 2;
    for (uint8_t y = 0; y < 30; ++y) {
        *IO_VDC_DATA_LO = cb[y*3];
        *IO_VDC_DATA_HI = cb[y*3+1];
    }
    /* Drain the final queued VRAM write before changing CR's increment.
     * A data-port read stalls for VRAM access without acknowledging IRQs. */
    (void)*IO_VDC_DATA_LO;
    *(volatile uint8_t *)0x20f7 = 5;
    *IO_VDC_INDEX = 5;
    *IO_VDC_DATA_LO = control;
    *IO_VDC_DATA_HI = control>>8;
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;*IO_VDC_INDEX=2;
    pce_cpu_irq_enable();
    return true;
}
void video_restore(void) { first_column=last_column=0xffff; }
PCE_RENDER bool video_background(uint16_t camera) {
    uint16_t column = camera >> 3;
    held_free();
    if (first_column == 0xffff || column + 32 < first_column || column > last_column) {
        memset(cache_refs, 0, sizeof cache_refs);held_count=0;
        memset(columns, 0xff, sizeof columns);
        first_column = column; last_column = column;
        for (uint8_t k = 0; k < 33; ++k) if (!column_load(column + k)) return false;
        last_column = column + 32;
    } else {
        while (column < first_column) {
            column_release(columns[last_column % 33]);
            --first_column; --last_column;
            if (!column_load(first_column)) return false;
        }
        while (first_column < column) {
            column_release(columns[first_column % 33]);
            ++first_column; ++last_column;
            if (!column_load(last_column)) return false;
        }
    }
    /* Keep the displayed shake until the matching SAT has been submitted. */
    video_scroll(camera, pce_scroll_y);
    return true;
}
PCE_RENDER void video_text(uint8_t x, uint8_t y, const char *text) {
    if(pce_raster_enabled)x*=2;
    uint16_t dest = pce_raster_enabled ? (uint16_t)(48+y)*128+x :
        (uint16_t)y*64+(((pce_scroll_x>>3)+x)&63);
    while (*text) {
        uint8_t c = *text++;
        if (c < 32 || c > 127) c = '?';
        video_vdc(0, dest);
        dest=pce_raster_enabled?dest+1:(dest&~63U)|((dest+1)&63);
        if(pce_raster_enabled) {
            uint16_t glyph=0xe420+(uint16_t)(c-32)*2;
            video_vdc(2,glyph);video_vdc(2,glyph+1);++dest;
        } else video_vdc(2, 0xf000 | ((PCE_FONT_WORD >> 4) + c - 32));
    }
}
__attribute__((noinline,minsize,section(".ram_bank110.text"))) void video_number(uint8_t x, uint8_t y, uint16_t n) {
    char text[6];
    for (uint8_t k = 0; k < 5; ++k) { text[4-k] = '0' + n % 10; n /= 10; }
    text[5] = 0; video_text(x, y, text);
}
PCE_FLOW static void sat_begin_body(void) {
    ++sprite_epoch;
    sat_count = clipped_count = 0;front_start=64;front_keep=0;
    sprite_lines_clear();
    sprite_cache_begin();
}
PCE_RENDER void video_sat_begin(void) {overlay_call(0x6e,sat_begin_body);}
PCE_RENDER bool video_sprite(uint16_t id, int16_t x, int16_t y, bool flip, uint8_t scale) {
    bool fast=scale==16;
    if(fast) {
        sprite_emit_id=id;sprite_emit_x=x;sprite_emit_y=y;sprite_emit_flip=flip?8:0;
        overlay_call(0x74,sprite_fast);
        if(!sprite_fast_miss) {
            if(!sprite_emit_ok&&!sprite_optional)++pce_metrics.essential_overflow;
            return sprite_emit_ok;
        }
    }
    if (id >= scene->nsprites) return false;
    uint8_t slot=sprite_slot_of[id];
    uint8_t count;
    if(slot<48&&sprite_ids[slot]==id)count=sprite_count[slot];
    else {
        uint8_t entry[16],colors[32];
        arcade_read(2, scene->sprites + (uint32_t)id * 16, entry, 16);
        count = entry[12];
        if (!count || count > 32 || entry[13]) return false;
        slot=sprite_slot(id,count);
        if(slot==48){if(!sprite_optional)++pce_metrics.essential_overflow;return false;}
        if (sprite_ids[slot] != id) {
            uint32_t pat = (uint32_t)entry[0] | (uint32_t)entry[1]<<8 | (uint32_t)entry[2]<<16 | (uint32_t)entry[3]<<24;
            uint32_t pal = (uint32_t)entry[8] | (uint32_t)entry[9]<<8 | (uint32_t)entry[10]<<16 | (uint32_t)entry[11]<<24;
            pce_vdc_index = 2; *(volatile uint8_t *)0x20f7 = 2;
            arcade_vram(pat, sprite_words[slot], count * 128);
            arcade_read(2, pal, colors, 32);
            pce_vce_copy_palette(16 + (slot<15?slot:15), colors, 1);
            sprite_ids[slot] = id;
            pce_metrics.uploads += count * 128;
        }
        sprite_count[slot]=count;sprite_slot_of[id]=slot;
        sprite_p0[slot]=entry[4];sprite_p1[slot]=entry[5];sprite_p2[slot]=entry[6];
        sprite_len[slot]=count*6;
        sprite_pb_lo[slot]=(sprite_words[slot]>>5);sprite_pb_hi[slot]=(sprite_words[slot]>>13);
        sprite_attr[slot]=VDC_SPRITE_FG|(slot<15?slot:15);
    }
    if(fast) {
        overlay_call(0x74,sprite_fast);
        if(sprite_fast_miss)return false;
        if(!sprite_emit_ok&&!sprite_optional)++pce_metrics.essential_overflow;
        return sprite_emit_ok;
    }
    sprite_used[slot] = 1;
    arcade_read(2, (uint32_t)sprite_p0[slot] | (uint32_t)sprite_p1[slot]<<8 | (uint32_t)sprite_p2[slot]<<16, descriptor, count * 6);
    generic_id=id;generic_x=x;generic_y=y;generic_flip=flip;generic_scale=scale;generic_count=count;generic_slot=slot;
    overlay_call(0x71,sprite_generic);
    return generic_ok;
}
void video_front_mark(void) {front_keep=sat_count;}
void video_front_begin(void) {front_start=sat_count;}
extern volatile uint16_t sat_copy_word,sat_copy_src,sat_copy_len;
extern void sat_copy(void);
__attribute__((noinline)) static void sat_transfer(uint16_t word,const void *src,uint16_t bytes) {
    sat_copy_word=word;sat_copy_src=(uint16_t)src;sat_copy_len=bytes;
    overlay_call(0x6e,sat_copy);
}
/* HUD, then foreground occluders, then actors: lower SAT slots win. Segments
 * are uploaded in that order straight from the build order, without moving
 * entries in RAM. Slots left from the previous frame are hidden by Y=0. */
PCE_RENDER void video_sat_end(void) {
    vdc_sprite_t *s = sat[sat_page];
    if(!(pce_metrics.frames&31)){uint8_t peak=0;for(uint8_t l=0,n=sprite_exact?224:30;l<n;++l)if(sprite_occupancy[l]>peak)peak=sprite_occupancy[l];
        if(peak>pce_metrics.max_units)pce_metrics.max_units=peak;}
    for(uint8_t k=sat_count;k<sat_previous;++k)s[k].y=0;
    pce_vdc_index = 2; *(volatile uint8_t *)0x20f7 = 2;
    /* Atomic address/index setup, then <=64-byte transfers with IRQ service
     * between them. HBlank/VBlank restore index 2 and leave MAWR untouched. */
    if(front_start<sat_count) {
        uint8_t fg=sat_count-front_start;
        if(front_keep)sat_transfer(PCE_SAT_WORD,s,(uint16_t)front_keep*8);
        sat_transfer(PCE_SAT_WORD+(uint16_t)front_keep*4,s+front_start,(uint16_t)fg*8);
        if(front_start>front_keep)
            sat_transfer(PCE_SAT_WORD+(uint16_t)(front_keep+fg)*4,s+front_keep,(uint16_t)(front_start-front_keep)*8);
    } else if(sat_count)sat_transfer(PCE_SAT_WORD,s,(uint16_t)sat_count*8);
    if(sat_previous>sat_count)
        sat_transfer(PCE_SAT_WORD+(uint16_t)sat_count*4,s+sat_count,(uint16_t)(sat_previous-sat_count)*8);
    sat_previous=sat_count;
    video_vdc(VDC_REG_SATB_START, PCE_SAT_WORD);
    pce_scroll_hold=0;   /* the new SAT is queued: from the next VBlank both it and the scroll apply */
    pce_metrics.sat_count = sat_count;
    ++pce_draws;
}
PCE_RENDER void video_race_init(void) {
    pce_raster_enabled = 1;
    sprite_screen_height=224;
    timing(true);
    video_vdc(VDC_REG_MEMORY, VDC_BG_SIZE_128_64);
    video_display(true);
}
__attribute__((noinline,section(".ram_bank109.text"))) static void race_sky_load(void) {   /* beside the road code that calls it */
    uint16_t used=0;
    memset(cache_ids,0xff,sizeof cache_ids);
    for(uint8_t x=0;x<128;++x) {
        arcade_read(1,scene->map+(uint32_t)(x&63)*90,buffer,90);
        for(uint8_t y=0;y<16;++y) {   /* 16 rows: the sky reaches scanline 127, where the road's first scanline is (irq.S) */
            uint16_t id=buffer[y*3]|(uint16_t)buffer[y*3+1]<<8;
            uint16_t slot;
            for(slot=0;slot<used&&cache_ids[slot]!=id;++slot) {}
            if(slot==used) {
                if(used>=256) {pce_control.ok=0;return;}
                cache_ids[used++]=id;
                uint16_t word=0x3000+slot*16;
                pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
                arcade_vram(scene->tiles+(uint32_t)id*32,word,32);
            }
            uint16_t word=0x3000+slot*16;
            video_vdc(0,(uint16_t)(48+y)*128+x);
            video_vdc(2,(word>>4)|((uint16_t)buffer[y*3+2]<<12));
        }
    }
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    /* The road: 256 characters at $2000, its BAT rows (two copies of 12 rows from row 0, the wrap variants of the lower rows at 24) and eight palettes. */
    arcade_vram(PCE_RACE_ROAD_TILES,0x2000,8192);
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    arcade_vram(PCE_RACE_ROAD_BAT,0,6144);
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    arcade_vram(PCE_RACE_ROAD_BAT+6656,24*128,6144);   /* the lower rows' wrap variants (BAT rows 24-47) */
    arcade_read(2,PCE_RACE_ROAD_PALETTE,buffer,256);
    pce_vce_copy_palette(1,buffer,8);
    pce_vce_set_color(255,0x1ff);   /* palette 15's white: the text glyphs' ink */
    pce_control.ok=1;
}

bool video_race_sky(void) {overlay_call(0x6d,race_sky_load);return pce_control.ok;}
