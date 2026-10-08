#include "sgx_pce.h"

#ifdef PCE_SGX
#include "arcade_pce.h"
#include "assets.h"
#include "pce_config.h"
#include "overlay_pce.h"
#include <pce/hardware.h>
#include <pce/vdc.h>
#include "video_pce.h"

#define SGX_CODE __attribute__((noinline, minsize, section(".ram_bank120.text")))
#define SGX_UI_CODE __attribute__((noinline, minsize, section(".ram_bank124.text")))
#define SGX_GAME_CODE __attribute__((noinline, minsize, section(".ram_bank120.text")))
#define SGX_MODE_CODE __attribute__((noinline, minsize, section(".ram_bank114.text")))
#define SGX_AUX_CODE __attribute__((noinline, minsize, section(".ram_bank110.text")))
#define SGX_VDC2_INDEX 0x0010
#define SGX_VDC2_DATA_LO 0x0012
#define SGX_VDC2_DATA_HI 0x0013
#define SGX_UI_SCREEN_FIRST 5
#define SGX_UI_SCREEN_LAST 23
#define SGX_UI_TILE_WORD 0x0800
#define SGX_UI_SPRITE_WORD 0x6800
#define SGX_UI_MAX_TILES ((SGX_UI_SPRITE_WORD - SGX_UI_TILE_WORD) / 16)
typedef struct __attribute__((packed)) {
    uint32_t tiles,map;
    uint16_t bytes;
    uint8_t bat_cols;
} SgxSkyRecord;

volatile PceSgxTelemetry pce_sgx_metrics;
/* The IRQ publishes VDC1's SAT at the same boundary as VDC0's. */
volatile uint8_t pce_sgx_sat1_alt PCE_WORK;
volatile uint8_t pce_sgx_sat1_pending PCE_WORK;
volatile uint8_t pce_sgx_arena_hidden PCE_WORK;
volatile uint8_t pce_sgx_arena_bg_pending_page PCE_WORK;
volatile uint16_t pce_sgx_sprite_id PCE_WORK;
volatile uint8_t pce_sgx_sprite_slot PCE_WORK, pce_sgx_sprite_upload_ok PCE_WORK;
extern uint8_t buffer[2048];
extern vdc_sprite_t sat[2][64];
extern uint16_t pce_panel_column;
extern volatile uint8_t pce_display_on;
extern volatile uint8_t pce_sat_pending;
extern volatile uint16_t pce_sgx_sky_scroll_x;
extern volatile uint16_t pce_scroll_x;
extern volatile uint8_t sat_copy_vdc, hud_copy_opcode;
extern volatile uint16_t sat_copy_word, sat_copy_src, sat_copy_len;
extern uint16_t sprite_words[48];
extern uint8_t sprite_count[48];
void sat_copy(void);

static SGX_CODE void vdc2_write(uint8_t reg, uint16_t value) {
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p", "memory");
    PCE_SGX_RECORD_VDC2_INDEX(reg);
    *(volatile uint8_t *)SGX_VDC2_INDEX = reg;
    *(volatile uint8_t *)SGX_VDC2_DATA_LO = (uint8_t)value;
    *(volatile uint8_t *)SGX_VDC2_DATA_HI = (uint8_t)(value >> 8);
    __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
}

static SGX_CODE void vdc2_timing(void) {
    vdc2_write(VDC_REG_CONTROL, 0);
    vdc2_write(VDC_REG_MEMORY, 0x0010);
    vdc2_write(VDC_REG_TIMING_HSYNC, 0x0503);
    vdc2_write(VDC_REG_TIMING_HDISP, 0x0627);
    vdc2_write(VDC_REG_TIMING_VSYNC, 0x1702);
    vdc2_write(VDC_REG_TIMING_VDISP, 223);
    vdc2_write(VDC_REG_TIMING_VDISPEND, 12);
    vdc2_write(VDC_REG_DMA_CONTROL, 0);
}

SGX_CODE void pce_sgx_detect_init(void) {
    pce_sgx_metrics.magic[0] = 'S'; pce_sgx_metrics.magic[1] = 'G';
    pce_sgx_metrics.magic[2] = 'X'; pce_sgx_metrics.magic[3] = '1';
    pce_sgx_metrics.flags = pce_sgx_detect() ?
        PCE_SGX_ACTIVE | PCE_SGX_PRIORITY : 0;
    if (!pce_sgx_active) return;
    /* Keep VDC2 at the menu's 352-pixel timing. The paired stills only run
       in menus; gameplay leaves VDC2 disabled, so it needs no mode switch. */
    vdc2_timing();
    /* Enable both VDCs in every window and select VPC mode 1 (0x7 per
       nibble): BG0 wins over BG1, with transparent pixels falling through. */
    *IO_VPC_CONTROL_LO = 0x77;
    *IO_VPC_CONTROL_HI = 0x77;
    *IO_VPC_WINDOW_1 = 0;
    *IO_VPC_WINDOW_2 = 0;
}

static SGX_MODE_CODE void vdc2_write_mode(uint8_t reg, uint16_t value) {
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p", "memory");
    PCE_SGX_RECORD_VDC2_INDEX(reg);
    *IO_VDC2_INDEX = reg;
    *IO_VDC2_DATA_LO = (uint8_t)value;
    *IO_VDC2_DATA_HI = (uint8_t)(value >> 8);
    __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
}

SGX_MODE_CODE void pce_sgx_display_on_body(void) {
    if (pce_sgx_active) {
        uint16_t control = 0;
        if (pce_display_on) {
            if (pce_sgx_arena_sprites()) {
                if (pce_sgx_metrics.paired_screen & PCE_SGX_ARENA_BG_READY)
                    control |= VDC_CONTROL_ENABLE_BG;
                if (!pce_sgx_arena_hidden)
                    control |= VDC_CONTROL_ENABLE_SPRITE;
            }
            else if ((pce_sgx_metrics.flags & PCE_SGX_PAIR_ACTIVE) || pce_sgx_gameplay())
                control = VDC_CONTROL_ENABLE_BG;
        } else if (pce_sgx_arena_sprites()) {
            pce_sgx_arena_hidden = 1;
            pce_sgx_sat1_pending = 0;
        }
        vdc2_write_mode(VDC_REG_CONTROL, control);
    }
}

static SGX_GAME_CODE void vdc1_set_index(uint8_t reg) {
    PCE_SGX_RECORD_VDC2_INDEX(reg);
    *IO_VDC2_INDEX = reg;
}

static SGX_GAME_CODE void vdc1_write(uint8_t reg, uint16_t value) {
    /* HBlank and VBlank both select VDC1 registers. Keep the index and its
       two data bytes together so an IRQ cannot split a register write. */
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p", "memory");
    vdc1_set_index(reg);
    *IO_VDC2_DATA_LO = (uint8_t)value;
    *IO_VDC2_DATA_HI = (uint8_t)(value >> 8);
    __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
}

static SGX_AUX_CODE void vdc1_set_index_aux(uint8_t reg) {
    PCE_SGX_RECORD_VDC2_INDEX(reg);
    *IO_VDC2_INDEX = reg;
}

static SGX_AUX_CODE void vdc1_write_aux(uint8_t reg, uint16_t value) {
    /* The raster handlers also touch VDC1's scroll registers. Preserve the
       caller's interrupt state around the index/data register transaction. */
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p", "memory");
    vdc1_set_index_aux(reg);
    *IO_VDC2_DATA_LO = (uint8_t)value;
    *IO_VDC2_DATA_HI = (uint8_t)(value >> 8);
    __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
}

SGX_AUX_CODE void pce_sgx_sky_load_body(void) {
    const PceScene *scene = &pce_scenes[pce_metrics.stage - 1];
    SgxSkyRecord sky={0};
    uint8_t sky_read=scene->occlusion && arcade_read(2,scene->occlusion,&sky,sizeof sky);
    uint16_t map_bytes = (uint16_t)sky.bat_cols * 64;
    uint16_t pattern_word=sky.bat_cols == 128 ? 0x1000 : PCE_BG_WORD;
    pce_sgx_metrics.paired_screen |= PCE_SGX_STATIC_SKY;
    vdc1_write_aux(VDC_REG_CONTROL,0);
    vdc1_write_aux(VDC_REG_MEMORY,sky.bat_cols == 128 ? VDC_BG_SIZE_128_32 : VDC_BG_SIZE_64_32);
    if (!sky_read || !sky.tiles || !sky.map || !sky.bytes || !sky.bat_cols ||
        !arcade_vram_to(1,sky.tiles,pattern_word,sky.bytes) ||
        !arcade_vram_to(1,sky.map,PCE_BAT_WORD,map_bytes)) {
        ++pce_sgx_metrics.failures;
        pce_sgx_metrics.paired_screen = 0;
        vdc1_write_aux(VDC_REG_CONTROL,0);
        arcade_read(2,scene->pal,buffer,512);
        pce_vce_copy_palette(0,buffer,16);
        return;
    }
    vdc1_write_aux(VDC_REG_CONTROL,pce_display_on?VDC_CONTROL_ENABLE_BG:0);
}

/* Most SGX stages stream the native map on VDC1. Stages 1 and 3 keep their
   scrolling playfield on VDC0 and use VDC1 for the source's static sky. */
SGX_GAME_CODE void pce_sgx_gameplay_begin_body(void) {
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p", "memory");
    pce_sgx_metrics.flags &= (uint8_t)~PCE_SGX_PAIR_ACTIVE;
    pce_sgx_sky_scroll_x = 0;
    if (!pce_sgx_active || pce_metrics.stage == 2) {
        pce_sgx_metrics.paired_screen = 0;
        pce_sgx_arena_hidden = 1;
        pce_sgx_sat1_pending = 0;
        pce_sgx_arena_bg_pending_page = 0xff;
        if (pce_sgx_active) vdc1_write(VDC_REG_CONTROL, 0);
        __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
        return;
    }

    pce_sgx_metrics.paired_screen =
        (pce_metrics.stage == 6 ? PCE_SGX_GAMEPLAY | PCE_SGX_ARENA_SPRITES :
                                  PCE_SGX_GAMEPLAY) | pce_metrics.stage;
    vdc1_write(VDC_REG_CONTROL, 0);
    vdc1_write(VDC_REG_MEMORY, 0x0010);
    vdc1_write(VDC_REG_TIMING_HSYNC, 0x0202);
    vdc1_write(VDC_REG_TIMING_HDISP, 0x041f);
    vdc1_write(VDC_REG_TIMING_VSYNC, 0x1702);
    vdc1_write(VDC_REG_TIMING_VDISP, 223);
    vdc1_write(VDC_REG_TIMING_VDISPEND, 12);
    vdc1_write(VDC_REG_DMA_CONTROL, 0);
    vdc1_write(VDC_REG_BG_SCROLL_X, 0);
    vdc1_write(VDC_REG_BG_SCROLL_Y, 0);

    if (pce_metrics.stage == 6) {
        /* VDC1 owns the arena panorama/floor. VDC0's zero tile and BAT let
           transparent BG0 pixels fall through while its hardware sprites run. */
        overlay_call(0x74,video_arena_bg_clear_body);
        pce_sgx_arena_hidden = 1;
        pce_sgx_arena_bg_pending_page = 0xff;
        pce_sgx_sat1_alt = 0;
        pce_sgx_sat1_pending = 0;
        vdc1_write(VDC_REG_SATB_START, PCE_SAT_WORD);
        __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
        return;
    }

    if (pce_metrics.stage == 1 || pce_metrics.stage == 3 ||
        pce_metrics.stage == 4 || pce_metrics.stage == 5) {
        overlay_call(0x6e,pce_sgx_sky_load_body);
        __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
        return;
    }

    /* The loaded font begins with its transparent, zero-filled space tile. */
    uint16_t control = *(volatile uint16_t *)0x20f3;
    pce_vdc_index = VDC_REG_CONTROL;
    *(volatile uint8_t *)0x20f7 = VDC_REG_CONTROL;
    *IO_VDC_INDEX = VDC_REG_CONTROL;
    *IO_VDC_DATA_LO = (uint8_t)control;
    *IO_VDC_DATA_HI = (uint8_t)(control >> 8) | 0x10;
    for (uint8_t row = 0; row < 32; ++row) {
        uint16_t address = (uint16_t)row * 64;
        pce_vdc_index = VDC_REG_VRAM_WRITE_ADDR;
        *(volatile uint8_t *)0x20f7 = VDC_REG_VRAM_WRITE_ADDR;
        *IO_VDC_INDEX = VDC_REG_VRAM_WRITE_ADDR;
        *IO_VDC_DATA_LO = (uint8_t)address;
        *IO_VDC_DATA_HI = (uint8_t)(address >> 8);
        pce_vdc_index = VDC_REG_VRAM_DATA;
        *(volatile uint8_t *)0x20f7 = VDC_REG_VRAM_DATA;
        *IO_VDC_INDEX = VDC_REG_VRAM_DATA;
        for (uint8_t col = 0; col < 64; ++col) {
            uint16_t word = PCE_FONT_WORD >> 4;
            *IO_VDC_DATA_LO = (uint8_t)word;
            *IO_VDC_DATA_HI = (uint8_t)(word >> 8);
        }
    }
    (void)*IO_VDC_DATA_LO;
    pce_vdc_index = VDC_REG_CONTROL;
    *(volatile uint8_t *)0x20f7 = VDC_REG_CONTROL;
    *IO_VDC_INDEX = VDC_REG_CONTROL;
    *IO_VDC_DATA_LO = (uint8_t)control;
    *IO_VDC_DATA_HI = (uint8_t)(control >> 8);
    vdc1_write(VDC_REG_CONTROL, pce_display_on ? VDC_CONTROL_ENABLE_BG : 0);
    __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
}

SGX_GAME_CODE void pce_sgx_gameplay_end_body(void) {
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p", "memory");
    pce_sgx_metrics.paired_screen = 0;
    pce_sgx_metrics.flags &= (uint8_t)~PCE_SGX_PAIR_ACTIVE;
    pce_sgx_arena_hidden = 1;
    pce_sgx_sat1_pending = 0;
    if (pce_sgx_active) vdc1_write(VDC_REG_CONTROL, 0);
    __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
}

SGX_GAME_CODE void pce_sgx_sky_scroll_body(void) {
    uint16_t camera=pce_scroll_x;
    if(pce_metrics.stage==4)pce_sgx_sky_scroll_x=(uint16_t)((camera*3)/100);
    else if(pce_metrics.stage==5)pce_sgx_sky_scroll_x=camera/5;
    else pce_sgx_sky_scroll_x=0;
}

SGX_GAME_CODE void pce_sgx_arena_sat_upload_body(void) {
    while (pce_sat_pending) {}
    uint16_t word = pce_sgx_sat1_alt ? PCE_SAT_WORD : PCE_SAT_ALT_WORD;
    sat_copy_word = word;
    sat_copy_src = (uint16_t)sat[1];
    sat_copy_len = sizeof sat[1];
    sat_copy_vdc = 1;
    hud_copy_opcode = 0xe3;
    overlay_call(0x72, sat_copy);
    pce_sgx_sat1_alt ^= 1;
    pce_sgx_sat1_pending = 1;
}

SGX_GAME_CODE void pce_sgx_sprite_upload_body(void) {
    uint8_t slot=pce_sgx_sprite_slot, entry[16];
    const PceScene *scene=&pce_scenes[pce_metrics.stage-1];
    uint32_t record=scene->sprites+(uint32_t)pce_sgx_sprite_id*16;
    pce_sgx_sprite_upload_ok=0;
    if(!arcade_read(2,record,entry,sizeof entry)) {
        ++pce_sgx_metrics.failures;
        return;
    }
    uint32_t patterns=(uint32_t)entry[0]|(uint32_t)entry[1]<<8|
                      (uint32_t)entry[2]<<16|(uint32_t)entry[3]<<24;
    pce_sgx_sprite_upload_ok=arcade_vram_to(
        1,patterns,sprite_words[slot],(uint16_t)sprite_count[slot]*128);
    if(!pce_sgx_sprite_upload_ok)++pce_sgx_metrics.failures;
}

SGX_GAME_CODE void pce_sgx_arena_hide_body(void) {
    pce_sgx_arena_hidden = 1;
    pce_sgx_sat1_pending = 0;
    vdc1_write(VDC_REG_CONTROL,
        pce_display_on &&
        (pce_sgx_metrics.paired_screen & PCE_SGX_ARENA_BG_READY) ?
        VDC_CONTROL_ENABLE_BG : 0);
}

/* The renderer prepares a transformed BAT column in buffer[1920..2010]. */
SGX_AUX_CODE void pce_sgx_column_write_body(void) {
    uint8_t *cells = buffer + 1920;
    uint16_t control = pce_display_on &&
        (!pce_sgx_arena_sprites() ||
         (pce_sgx_metrics.paired_screen & PCE_SGX_ARENA_BG_READY)) ?
        VDC_CONTROL_ENABLE_BG : 0;
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p", "memory");
    vdc1_write_aux(VDC_REG_CONTROL, control | 0x1000);
    vdc1_write_aux(VDC_REG_VRAM_WRITE_ADDR, pce_panel_column & 63);
    vdc1_set_index_aux(VDC_REG_VRAM_DATA);
    for (uint8_t y = 0; y < 30; ++y) {
        uint16_t word = cells[y * 3] | (uint16_t)cells[y * 3 + 1] << 8;
        *IO_VDC2_DATA_LO = (uint8_t)word;
        *IO_VDC2_DATA_HI = (uint8_t)(word >> 8);
    }
    (void)*IO_VDC2_DATA_LO;
    vdc1_write_aux(VDC_REG_CONTROL, control);
    vdc1_set_index_aux(VDC_REG_VRAM_DATA);
    __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
}

SGX_MODE_CODE void pce_sgx_ui_mode_body(void) {
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p", "memory");
    pce_sgx_metrics.paired_screen = 0;
    pce_sgx_metrics.flags &= (uint8_t)~PCE_SGX_PAIR_ACTIVE;
    if (pce_sgx_active) {
        vdc2_write_mode(VDC_REG_CONTROL, 0);
        vdc2_write_mode(VDC_REG_MEMORY, 0x0010);
        vdc2_write_mode(VDC_REG_TIMING_HSYNC, 0x0503);
        vdc2_write_mode(VDC_REG_TIMING_HDISP, 0x0627);
        vdc2_write_mode(VDC_REG_TIMING_VSYNC, 0x1702);
        vdc2_write_mode(VDC_REG_TIMING_VDISP, 223);
        vdc2_write_mode(VDC_REG_TIMING_VDISPEND, 12);
        vdc2_write_mode(VDC_REG_DMA_CONTROL, 0);
    }
    __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
}

static SGX_UI_CODE void vdc2_write_ui(uint8_t reg, uint16_t value);

SGX_UI_CODE static void pair_clear(void) {
    pce_sgx_metrics.flags &= (uint8_t)~PCE_SGX_PAIR_ACTIVE;
    pce_sgx_metrics.paired_screen = 0;
    pce_sgx_metrics.colors = 0;
    if (pce_sgx_active) vdc2_write_ui(VDC_REG_CONTROL, 0);
}

/* UI loading runs from bank $7c. Keep a tiny local register writer here so
   this overlay never calls back into the bank $78 startup code. */
SGX_UI_CODE static void vdc2_write_ui(uint8_t reg, uint16_t value) {
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p", "memory");
    PCE_SGX_RECORD_VDC2_INDEX(reg);
    *(volatile uint8_t *)SGX_VDC2_INDEX = reg;
    *(volatile uint8_t *)SGX_VDC2_DATA_LO = (uint8_t)value;
    *(volatile uint8_t *)SGX_VDC2_DATA_HI = (uint8_t)(value >> 8);
    __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
}

static SGX_UI_CODE bool upload_map(uint8_t vdc, uint32_t offset) {
    for (uint8_t row = 0; row < 28; ++row) {
        if (!arcade_vram_to(vdc, offset + (uint32_t)row * 80,
                            (uint16_t)row * 64, 80)) return false;
    }
    return true;
}

static SGX_UI_CODE bool load_pair(const PceSgxUiPair *r) {
    pce_sgx_metrics.flags &= (uint8_t)~PCE_SGX_PAIR_ACTIVE;
    if (!pce_sgx_active) return false;
    vdc2_write_ui(VDC_REG_CONTROL, 0);
    if (r->ntiles0 == 0 || r->ntiles1 == 0 ||
        r->ntiles0 > SGX_UI_MAX_TILES || r->ntiles1 > SGX_UI_MAX_TILES ||
        r->colors == 0) {
        ++pce_sgx_metrics.failures;
        return false;
    }

    if (!arcade_read(2, r->palette, buffer, 512)) goto failed;
    pce_vce_copy_palette(0, buffer, 16);
    if (!arcade_vram_to(0, r->tiles0, SGX_UI_TILE_WORD,
                        (uint16_t)((uint32_t)r->ntiles0 * 32UL)) ||
        !arcade_vram_to(1, r->tiles1, SGX_UI_TILE_WORD,
                        (uint16_t)((uint32_t)r->ntiles1 * 32UL)) ||
        !upload_map(0, r->map0) || !upload_map(1, r->map1)) goto failed;

    vdc2_write_ui(VDC_REG_BG_SCROLL_X, 0);
    vdc2_write_ui(VDC_REG_BG_SCROLL_Y, 0);
    pce_sgx_metrics.flags |= PCE_SGX_PAIR_ACTIVE;
    pce_sgx_metrics.colors = (uint8_t)r->colors;
    return true;

failed:
    ++pce_sgx_metrics.failures;
    vdc2_write_ui(VDC_REG_CONTROL, 0);
    return false;
}

SGX_UI_CODE void pce_sgx_ui_load_body(void) {
    uint8_t id = pce_sgx_metrics.paired_screen;
    if (id >= 24) id = 0;
    const PceUiScreen *s = &pce_ui[id];
    pce_sgx_metrics.flags &= (uint8_t)~PCE_SGX_PAIR_ACTIVE;
    if (id == 0 || (id >= SGX_UI_SCREEN_FIRST && id <= SGX_UI_SCREEN_LAST)) {
        const PceSgxUiPair *pair = id == 0 ? &pce_sgx_ui_pair[19] :
            &pce_sgx_ui_pair[id - SGX_UI_SCREEN_FIRST];
        if (!load_pair(pair)) {
            pce_sgx_metrics.paired_screen = 0;
            pce_sgx_metrics.flags &= (uint8_t)~PCE_SGX_PAIR_ACTIVE;
            if (pce_sgx_active) vdc2_write_ui(VDC_REG_CONTROL, 0);
        }
    } else {
        pair_clear();
    }

    if (!(pce_sgx_metrics.flags & PCE_SGX_PAIR_ACTIVE)) {
        arcade_read(2, s->pal, buffer, 512);
        pce_vce_copy_palette(0, buffer, 16);
        arcade_vram(s->tiles, SGX_UI_TILE_WORD, (uint16_t)((uint32_t)s->ntiles * 32UL));
        for (uint8_t row = 0; row < 28; ++row)
            arcade_vram(s->map + (uint32_t)row * 80, (uint16_t)row * 64, 80);
    }
    arcade_read(2, s->sprpal, buffer, 512);
    pce_vce_copy_palette(16, buffer, 16);
    if (s->nsprpat)
        arcade_vram(s->sprpat, SGX_UI_SPRITE_WORD, (uint16_t)((uint32_t)s->nsprpat * 128UL));
}

SGX_UI_CODE void pce_sgx_ui_end_body(void) {
    pair_clear();
}
#endif
