#include "video_pce.h"
#include "arcade_pce.h"
#include "mask_pce.h"
#include <string.h>

volatile PceTelemetry pce_metrics;
volatile uint8_t pce_ticks, pce_raster_enabled, pce_floor_page, pce_raster_row;
volatile uint8_t pce_vdc_index;
volatile uint16_t pce_scroll_x, pce_scroll_y;
volatile uint8_t pce_floor_pending;
static const PceScene *scene;
static uint16_t cache_ids[PCE_BG_MAX_TILES] PCE_WORK;
static uint16_t cache_refs[PCE_BG_MAX_TILES] PCE_WORK;
static uint16_t columns[33][28] PCE_WORK;
static uint16_t first_column, last_column;
static uint16_t free_cursor;
static uint8_t buffer[2048] PCE_STAGE;
static uint8_t occupancy[224], trial[224];
static uint8_t clipped_pattern[128];
static uint8_t clipped_count;
static vdc_sprite_t sat[2][64];
static uint8_t sat_page, sat_count;
static uint16_t sprite_ids[12],sprite_words[12];
static uint8_t sprite_used[12],sprite_pinned[12],pattern_owner[48];
static uint8_t descriptor[384] PCE_STAGE;

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
PCE_RENDER void video_init(void) {
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
}
PCE_RENDER void video_display(bool enable) {
    video_vdc(VDC_REG_CONTROL, VDC_CONTROL_IRQ_VBLANK | VDC_CONTROL_IRQ_SCANLINE |
              (enable ? VDC_CONTROL_ENABLE_BG | VDC_CONTROL_ENABLE_SPRITE : 0));
}
PCE_RENDER void video_scroll(uint16_t x, uint16_t y) { pce_scroll_x = x; pce_scroll_y = y; }
PCE_RENDER void video_scene(const PceScene *s) {
    scene = s;
    timing(false);
    memset(cache_refs, 0, sizeof cache_refs);
    memset(cache_ids, 0xff, sizeof cache_ids);
    memset(columns, 0xff, sizeof columns);
    memset(sprite_ids, 0xff, sizeof sprite_ids);
    memset(sprite_used,0,sizeof sprite_used);
    memset(sprite_pinned,0,sizeof sprite_pinned);
    memset(pattern_owner,0,sizeof pattern_owner);
    first_column = last_column = 0xffff;
    free_cursor=0;
    for(uint8_t page=0;page<32;++page)arcade_fill(0x1e0000UL+(uint32_t)page*4096,0xff,4096);
    arcade_read(2, s->pal, buffer, 512);
    pce_vce_copy_palette(0, buffer, 16);
    pce_vce_set_color(255, 0x1ff);
    pce_vdc_index = 0;
}
PCE_RENDER static bool column_load(uint16_t world) {
    if (!arcade_read(1, scene->map + (uint32_t)(world % scene->cols) * 84, buffer, 84)) return false;
    uint16_t *refs = columns[world % 33];
    for (uint8_t y = 0; y < 28; ++y) {
        uint16_t id = buffer[y * 3] | (uint16_t)buffer[y * 3 + 1] << 8;
        uint16_t slot;
        arcade_read(3,0x1e0000UL+(uint32_t)id*2,&slot,2);
        if (slot == 0xffff) {
            slot=free_cursor;
            uint16_t checked=0;
            while(cache_refs[slot]) {
                if(++checked==PCE_BG_MAX_TILES)return false;
                if(++slot==PCE_BG_MAX_TILES)slot=0;
            }
            free_cursor=slot+1;if(free_cursor==PCE_BG_MAX_TILES)free_cursor=0;
            if(cache_ids[slot]!=0xffff) {
                uint16_t invalid=0xffff;
                arcade_write(3,0x1e0000UL+(uint32_t)cache_ids[slot]*2,&invalid,2);
            }
            pce_vdc_index = 2; *(volatile uint8_t *)0x20f7 = 2;
            if (!arcade_vram(scene->tiles + (uint32_t)id * 32,
                             PCE_BG_WORD + slot * 16, 32)) return false;
            cache_ids[slot] = id;
            arcade_write(3,0x1e0000UL+(uint32_t)id*2,&slot,2);
            pce_metrics.uploads += 32;
        }
        ++cache_refs[slot]; refs[y] = slot;
        video_vdc(0, (uint16_t)y * 64 + (world & 63));
        video_vdc(2, (PCE_BG_WORD >> 4) + slot + ((uint16_t)buffer[y * 3 + 2] << 12));
    }
    return true;
}
void video_restore(void) { first_column=last_column=0xffff; }
PCE_RENDER bool video_background(uint16_t camera) {
    uint16_t column = camera >> 3;
    if (first_column == 0xffff || column + 32 < first_column || column > last_column) {
        memset(cache_refs, 0, sizeof cache_refs);
        memset(columns, 0xff, sizeof columns);
        first_column = column; last_column = column;
        for (uint8_t k = 0; k < 33; ++k) if (!column_load(column + k)) return false;
        last_column = column + 32;
    } else {
        while (column < first_column) {
            uint16_t *refs = columns[last_column % 33];
            for (uint8_t y = 0; y < 28; ++y) if (refs[y] != 0xffff) --cache_refs[refs[y]];
            --first_column; --last_column;
            if (!column_load(first_column)) return false;
        }
        while (first_column < column) {
            uint16_t *refs = columns[first_column % 33];
            for (uint8_t y = 0; y < 28; ++y) if (refs[y] != 0xffff) --cache_refs[refs[y]];
            ++first_column; ++last_column;
            if (!column_load(last_column)) return false;
        }
    }
    video_scroll(camera, 0);
    return true;
}
PCE_RENDER void video_text(uint8_t x, uint8_t y, const char *text) {
    uint16_t dest = pce_raster_enabled ? (uint16_t)(48+y)*128+x :
        y?(uint16_t)y*64+(((pce_scroll_x>>3)+x)&63):28*64+x;
    while (*text) {
        uint8_t c = *text++;
        if (c < 32 || c > 127) c = '?';
        video_vdc(0, dest++);
        video_vdc(2, 0xf000 | ((PCE_FONT_WORD >> 4) + c - 32));
    }
}
__attribute__((noinline)) void video_number(uint8_t x, uint8_t y, uint16_t n) {
    char text[6];
    for (uint8_t k = 0; k < 5; ++k) { text[4-k] = '0' + n % 10; n /= 10; }
    text[5] = 0; video_text(x, y, text);
}
PCE_RENDER void video_sat_begin(void) {
    sat_page ^= 1; sat_count = clipped_count = 0;
    memset(sat[sat_page], 0, sizeof sat[0]);
    memset(occupancy, 0, sizeof occupancy);
    for(uint8_t i=0;i<12;++i) {
        if(sprite_used[i])sprite_pinned[i]=2;
        else if(sprite_pinned[i])--sprite_pinned[i];
        sprite_used[i]=0;
    }
}
/* Resident allocator. A 512-byte page holds four 16x16 patterns. Keep the
 * last two displayed generations pinned through SAT DMA, including palettes.
 * Canonical left/right frames share their cache ID and patterns. */
__attribute__((noinline)) static uint8_t sprite_slot(uint16_t id,uint8_t count) {
    for(uint8_t i=0;i<12;++i)if(sprite_ids[i]==id)return i;
    uint8_t slot=0;
    while(slot<12&&(sprite_used[slot]||sprite_pinned[slot]))++slot;
    if(slot==12)return 12;
    uint8_t pages=(count+3)>>2;
    for(uint8_t base=0;base<=48-pages;++base) {
        bool available=true;
        for(uint8_t p=base;p<base+pages;++p) {
            uint8_t owner=pattern_owner[p];
            if(owner&&(sprite_used[owner-1]||sprite_pinned[owner-1])){available=false;break;}
        }
        if(!available)continue;
        for(uint8_t p=base;p<base+pages;++p) {
            uint8_t owner=pattern_owner[p];
            if(owner) {
                sprite_ids[owner-1]=0xffff;
                for(uint8_t q=0;q<48;++q)if(pattern_owner[q]==owner)pattern_owner[q]=0;
            }
        }
        for(uint8_t q=0;q<48;++q)if(pattern_owner[q]==slot+1)pattern_owner[q]=0;
        for(uint8_t p=base;p<base+pages;++p)pattern_owner[p]=slot+1;
        sprite_words[slot]=PCE_SPR_WORD+(uint16_t)base*256;
        return slot;
    }
    return 12;
}
PCE_RENDER bool video_sprite(uint16_t id, int16_t x, int16_t y, bool flip, uint8_t scale) {
    if (id >= scene->nsprites) return false;
    uint8_t entry[32];
    arcade_read(2, scene->sprites + (uint32_t)id * 16, entry, 16);
    uint32_t pat = (uint32_t)entry[0] | (uint32_t)entry[1]<<8 | (uint32_t)entry[2]<<16 | (uint32_t)entry[3]<<24;
    uint32_t parts = (uint32_t)entry[4] | (uint32_t)entry[5]<<8 | (uint32_t)entry[6]<<16 | (uint32_t)entry[7]<<24;
    uint32_t pal = (uint32_t)entry[8] | (uint32_t)entry[9]<<8 | (uint32_t)entry[10]<<16 | (uint32_t)entry[11]<<24;
    uint16_t count = entry[12] | (uint16_t)entry[13]<<8;
    if (!count || count > 32) return false;
    uint8_t slot=sprite_slot(id,count);
    if(slot==12){++pce_metrics.essential_overflow;return false;}
    if (sprite_ids[slot] != id) {
        pce_vdc_index = 2; *(volatile uint8_t *)0x20f7 = 2;
        arcade_vram(pat, sprite_words[slot], count * 128);
        arcade_read(2, pal, entry, 32);
        pce_vce_copy_palette(16 + slot, entry, 1);
        sprite_ids[slot] = id;
        pce_metrics.uploads += count * 128;
    }
    sprite_used[slot] = 1;
    arcade_read(2, parts, descriptor, count * 6);
    for (uint8_t pass = 0; pass < 2; ++pass) {
    uint8_t admitted = 0,clipped_admitted=0;
    if (!pass) memcpy(trial, occupancy, sizeof trial);
    for (uint8_t k = 0; k < count; ++k) {
        const uint8_t *d = descriptor + k * 6;
        int16_t dx = (int16_t)(d[0] | (uint16_t)d[1]<<8);
        int16_t dy = (int16_t)(d[2] | (uint16_t)d[3]<<8);
        if (flip) dx = -dx - 16;
        if (scale != 16) { dx = (int32_t)dx * scale / 16; dy = (int32_t)dy * scale / 16; }
        int16_t px = x + dx, py = y + dy;
        if (px <= -16 || px >= (pce_raster_enabled ? 512 : 256) || py <= -16 || py >= 224) continue;
        /* Cockpit viewing window: never draw a partial slice across its rim. */
        if (pce_metrics.stage == 6 && id<99 && (px + 16 <= 16 || px >= 240 || py + 16 <= 20 || py >= 180)) continue;
        int16_t lo = py < 0 ? 0 : py, hi = py + 16 > 224 ? 224 : py + 16;
        if (!pass) {
            bool fits = sat_count + ++admitted <= 64;
            if(pce_metrics.stage==6&&id<99&&(px<16||px+16>240||py<20||py+16>180))
                if(clipped_count+ ++clipped_admitted>28)fits=false;
            if(scene->occlusion&&clipped_count+ ++clipped_admitted>28)fits=false;
            for (int16_t line=lo; line<hi; ++line) if (++trial[line]>16) fits=false;
            if (!fits) { ++pce_metrics.essential_overflow; return false; }
            continue;
        }
        for (int16_t line=lo;line<hi;++line) {
            uint8_t n=++occupancy[line];
            if(n>pce_metrics.max_units) pce_metrics.max_units=n;
        }
        uint16_t pattern = d[4] | (uint16_t)d[5]<<8;
        uint16_t vram_pattern=(sprite_words[slot]>>5)+pattern*2;
        if (pce_metrics.stage==6 && id<99 && (px<16 || px+16>240 || py<20 || py+16>180)) {
            if (clipped_count>=28) { ++pce_metrics.essential_overflow; return false; }
            uint8_t left=px<16?16-px:0, right=px+16>240?240-px:16;
            uint8_t top=py<20?20-py:0, bottom=py+16>180?180-py:16;
            if(flip) { uint8_t t=left;left=16-right;right=16-t; }
            uint16_t mask=(0xffffU>>left)&(0xffffU<<(16-right));
            arcade_read(2,pat+(uint32_t)pattern*128,clipped_pattern,128);
            for(uint8_t plane=0;plane<4;++plane) for(uint8_t row=0;row<16;++row) {
                uint16_t bits=(row>=top&&row<bottom)?mask:0;
                clipped_pattern[plane*32+row*2]&=bits;
                clipped_pattern[plane*32+row*2+1]&=bits>>8;
            }
            uint16_t word=0x7800+(uint16_t)clipped_count++*64;
            pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
            pce_vdc_copy_to_vram(word,clipped_pattern,128);
            pce_metrics.uploads+=128;vram_pattern=word>>5;
        }
        if(scene->occlusion&&mask_pattern(pat+(uint32_t)pattern*128,px,py,flip,clipped_pattern)) {
            uint16_t word=0x7800+(uint16_t)clipped_count++*64;
            pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
            pce_vdc_copy_to_vram(word,clipped_pattern,128);
            pce_metrics.uploads+=128;vram_pattern=word>>5;
        }
        sat[sat_page][sat_count++] = (vdc_sprite_t){py + 64, px + 32,
            vram_pattern,
            VDC_SPRITE_FG | slot | (flip ? VDC_SPRITE_FLIP_X : 0)};
    }
    }
    return true;
}
PCE_RENDER void video_sat_end(void) {
    pce_vdc_index = 2; *(volatile uint8_t *)0x20f7 = 2;
    pce_vdc_copy_to_vram(PCE_SAT_WORD, sat[sat_page], 512);
    video_vdc(VDC_REG_SATB_START, PCE_SAT_WORD);
    pce_metrics.sat_count = sat_count;
}
PCE_RENDER void video_race_init(void) {
    pce_raster_enabled = 1;
    timing(true);
    video_vdc(VDC_REG_MEMORY, VDC_BG_SIZE_128_64);
    video_display(true);
}
PCE_RENDER void video_floor_row(uint8_t page, uint8_t row, const uint8_t *pairs) {
    video_vdc(0, (uint16_t)row * 128 + (page ? 64 : 0));
    pce_cpu_irq_disable();
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;*IO_VDC_INDEX=2;
    pce_cpu_irq_enable();
    /* IRQ handlers restore index 2 and never change the VRAM write address.
     * Stream a whole row without a banked call and SEI/CLI per BAT entry. */
    for (uint8_t x = 0; x < 64; ++x) {
        *IO_VDC_DATA_LO=pairs[x];*IO_VDC_DATA_HI=0xf2;
    }
}
PCE_RENDER bool video_race_sky(void) {
    uint16_t used=0;
    memset(cache_ids,0xff,sizeof cache_ids);
    for(uint8_t x=0;x<64;++x) {
        arcade_read(1,scene->map+(uint32_t)x*84,buffer,84);
        for(uint8_t y=0;y<16;++y) {
            uint16_t id=buffer[y*3]|(uint16_t)buffer[y*3+1]<<8;
            uint16_t slot;
            for(slot=0;slot<used&&cache_ids[slot]!=id;++slot) {}
            if(slot==used) {
                if(used>=288) return false;
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
    arcade_vram(PCE_RACE_PAIR_CHARACTERS,0x2000,8192);
    arcade_read(2,PCE_RACE_FLOOR_PALETTE,buffer,32);
    pce_vce_copy_palette(15,buffer,1);
    return true;
}
