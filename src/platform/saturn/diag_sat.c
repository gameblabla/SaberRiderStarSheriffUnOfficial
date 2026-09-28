/* The diagnostic build's boot log on screen (make -f Makefile.saturn DIAG=1: SAT_DIAG), for a console without a
 * debugger: from the power-on until the main loop's first frame, and whenever the main loop then stops for more than
 * 4 s, the vblank-in handler shows the tail of the log ring (log_sat.c: every printf) as text on NBG0, over
 * everything else. It writes VDP2's registers itself (libyaul's shadow copy comes back with the game's next
 * vdp2_sync), so it still shows a hang in the middle of a frame; a hang with the interrupts off leaves the last
 * screen it drew.
 * Under the text, the whole screen shows the boot stage reached (sat_diag_stage) as VDP2's back screen colour, which
 * needs no tiles, font or VRAM timing: blue is the IP (tools/saturn/diag_ip.py), then red, orange, yellow, green, cyan,
 * white (DIAG_STAGES below); the text rows have a dark blue strip, the rest shows the colour.
 * Video memory: the 8x8 font (libyaul's dbgio font, 4bpp) at 0x7A000, the name page at 0x7C000 (bank B1, above
 * everything vdp2_planes.c / the floor use), the back screen word at 0x7FFFE (the renderer's own, set again every
 * frame). Colour RAM: palettes 0x7E / 0x7F (entries 0x7E0-0x7FF, saved and put back when the game runs again). */
#include "sat_internal.h"

#ifdef SAT_DIAG
#include <yaul.h>
#include <string.h>
#include <stdio.h>

struct SaberLog { char magic[8]; uint32_t size; volatile uint32_t head; char buf[]; };
extern struct SaberLog saber_log;

typedef struct { const uint8_t *cg; size_t cg_size; const uint16_t *pal; size_t pal_size; uint8_t fg_pal, bg_pal; } DiagFont;
extern const DiagFont __dbgio_default_font;   /* libyaul (kernel/dbgio/font/default_font.c): 256 chars, 1bpp 8x8 */

#define REG(o)      (*(volatile uint16_t *)(0x25F80000u + (o)))
#define VRAM16(o)   ((volatile uint16_t *)(0x25E00000u + (o)))
#define CRAM16(i)   (*(volatile uint16_t *)(0x25F00000u + (i) * 2u))
#define CHR_OFF     0x7A000u
#define MAP_OFF     0x7C000u   /* map number 62 (8 KB pages) */
#define PAL_ENTRY   0x7E0u
#define COLS        40
#define ROWS        28
#define STALL_VB    240u       /* 4 s of fields at 60 Hz (4.8 at 50) */
#define BACK_WORD   (*(volatile uint16_t *)(0x25E7FFFEu))

/* the back screen colour a stage shows (0bbbbbgggggrrrrr); a stall afterwards: dark blue */
static const uint16_t stage_colour[] = {
    0x2400,   /* 0: none yet / a stall */
    0x7C00,   /* 1: blue - the IP ran and the BIOS loaded 0.BIN (diag_ip.py; never set here) */
    0x001F,   /* 2: red - libyaul's start-up done, user_init entered */
    0x021F,   /* 3: orange - cd_block_init returned */
    0x03FF,   /* 4: yellow - smpc_peripheral_init returned: main() next */
    0x03E0,   /* 5: green - game_main: the timer is up, cd_sat_init (the file list) next */
    0x7FE0,   /* 6: cyan - cd_sat_init done, rsat_init (VDP1 / VDP2, the slave SH-2, the sound driver) next */
    0x7FFF,   /* 7: white - rsat_init done, app_init (the packs, the game) next */
};
static volatile unsigned stage;

static bool shown, font_ready, cram_saved, ever_alive;
static uint32_t alive_vb, drawn_head = 0xFFFFFFFFu, drawn_vb;
static uint16_t cram_keep[32];
static char screen[ROWS][COLS];

void sat_diag_alive(void) { ever_alive = true; alive_vb = sat_vblanks(); }

/* VDP2's memory (the font, the palettes, the back screen word) is written in the blank: the console loses a colour RAM
 * write made while the screen is drawn (and a video memory one where the access pattern gives the CPU no slot - libyaul's
 * all "no access" until the game sets its own). From the vblank-in handler it is; sat_diag_stage waits for it. */
static void wait_blank(void)
{
    for (uint32_t spins = 0; !(REG(0x004) & 0x0008u) && spins < 400000u; spins++) { }   /* TVSTAT VBLANK (~20 ms at most) */
}

static void setup(void)
{
    REG(0x00E) = 0x1300;                        /* RAMCTL: colour RAM mode 1, banks A and B split */
    /* the access patterns by the console's rules (vdp2_planes.c set_cycle_patterns): a CPU slot in both halves of a
     * chip, "no access" before a run of them */
    REG(0x010) = REG(0x014) = 0xFEEE; REG(0x012) = REG(0x016) = 0xEEEE;   /* A0, A1: CPU from T1 */
    REG(0x018) = 0xFFFE; REG(0x01A) = 0xEEEE;   /* B0: CPU from T3 */
    REG(0x01C) = 0x04FE; REG(0x01E) = 0xEEEE;   /* B1: NBG0's names at T0, characters at T1, CPU from T3 */
    REG(0x020) = 0x0001;                        /* BGON: NBG0 alone, colour 0 transparent */
    REG(0x022) = 0;                             /* no mosaic */
    REG(0x028) = 0;                             /* CHCTLA: NBG0 cells, 16 colours, 1x1 */
    REG(0x030) = 0x80EF;                        /* PNCN0: 1 word, palette bits 6-4 = 7, character bits 14-10 = 0xF */
    REG(0x03A) = 0;                             /* PLSZ: 1x1 planes */
    REG(0x03C) = 0;                             /* MPOFN */
    REG(0x040) = 0x3E3E; REG(0x042) = 0x3E3E;   /* MPABN0 / MPCDN0: page 62 */
    REG(0x070) = REG(0x072) = REG(0x074) = REG(0x076) = 0;                 /* scroll 0 */
    REG(0x078) = 1; REG(0x07A) = 0; REG(0x07C) = 1; REG(0x07E) = 0;        /* zoom 1.0 */
    REG(0x098) = 0; REG(0x09A) = 0;             /* ZMCTL, SCRCTL: no reduction, no line / cell scroll */
    REG(0x0D0) = 0;                             /* no window on NBG0 */
    REG(0x0E0) = 0; REG(0x0E2) = 0; REG(0x0E4) = 0; REG(0x0E6) = 0;       /* no shadow, CRAM offsets 0, no line colour */
    REG(0x0E8) = 0; REG(0x0EC) = 0;             /* no special priority, no colour calculation */
    REG(0x0F0) = REG(0x0F2) = REG(0x0F4) = REG(0x0F6) = 0;               /* sprites at priority 0: hidden */
    REG(0x0F8) = 7;                             /* PRINA: NBG0 on top */
    REG(0x110) = 0;                             /* no colour offset (a fade left dark) */
    REG(0x0AC) = 0x0003; REG(0x0AE) = 0xFFFF;   /* BKTAU / BKTAL: one back screen colour, at VRAM 0x7FFFE */
    if (!font_ready) {   /* chars 1-127: each 1bpp row byte (bit 0 the left pixel) to four 4bpp bytes, colour 1 on 2;
                          * char 0 all colour 0: transparent (the stage colour shows) */
        const uint8_t *cg = __dbgio_default_font.cg;
        volatile uint16_t *d = VRAM16(CHR_OFF);
        for (int c = 0; c < 128; c++)
            for (int y = 0; y < 8; y++) {
                uint8_t b = cg[c * 8 + y];
                for (int x = 0; x < 8; x += 4) {
                    uint16_t w = 0;
                    for (int k = 0; k < 4; k++) w = (uint16_t)(w << 4 | (c == 0 ? 0 : (b >> (x + k)) & 1 ? 1 : 2));
                    *d++ = w;
                }
            }
        font_ready = true;
    }
    if (!cram_saved) {
        for (int i = 0; i < 32; i++) cram_keep[i] = CRAM16(PAL_ENTRY + i);
        cram_saved = true;
    }
    CRAM16(PAL_ENTRY + 1) = 0x03FF; CRAM16(PAL_ENTRY + 2) = 0x2400;          /* palette 0x7E: yellow on dark blue */
    CRAM16(PAL_ENTRY + 17) = 0x7FFF; CRAM16(PAL_ENTRY + 18) = 0x2400;        /* palette 0x7F: white on dark blue */
    BACK_WORD = stage_colour[ever_alive ? 0 : stage];
    REG(0x000) = (uint16_t)(vdp2_regs_get()->tvmd | 0x8000);   /* TVMD: the display on, libyaul's mode */
}

/* the log's last lines, wrapped at COLS, into screen[1..]; screen[0] the status line */
static void compose(uint32_t vb)
{
    memset(screen, 0, sizeof screen);   /* transparent where there is no text */
    uint32_t head = saber_log.head, size = saber_log.size, n = head < size ? head : size;
    if (n > 4096) n = 4096;
    static char tail[4097];
    for (uint32_t i = 0; i < n; i++) tail[i] = saber_log.buf[(head - n + i) % size];
    /* the rows, bottom up: split the text into lines, each wrapped into rows of COLS */
    int row = ROWS - 1;
    int end = (int)n;
    if (end && tail[end - 1] == '\n') end--;
    while (row >= 1 && end > 0) {
        int start = end;
        while (start > 0 && tail[start - 1] != '\n') start--;
        int len = end - start, nrows = len ? (len + COLS - 1) / COLS : 1;
        for (int r = nrows - 1; r >= 0 && row >= 1; r--, row--) {
            int off = start + r * COLS, k = end - off < COLS ? end - off : COLS;
            for (int c = 0; c < k; c++) { char ch = tail[off + c]; screen[row][c] = (ch >= 32 && ch < 127) ? ch : '?'; }
        }
        end = start - 1;
    }
    char st[COLS + 1];
    if (!ever_alive) snprintf(st, sizeof st, "SABER DIAG  boot stage %u  field %u", stage, (unsigned)vb);
    else snprintf(st, sizeof st, "SABER DIAG  STALL %us  field %u", (unsigned)((vb - alive_vb) / 60u), (unsigned)vb);
    memcpy(screen[0], st, strlen(st));
}

static void draw(uint32_t vb)
{
    drawn_head = saber_log.head; drawn_vb = vb;
    compose(vb);
    volatile uint16_t *map = VRAM16(MAP_OFF);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < 64; c++) {
            char ch = c < COLS ? screen[r][c] : 0;
            map[r * 64 + c] = (uint16_t)((r ? 0xF000u : 0xE000u) | (0x100u + (uint8_t)ch));
        }
}

void sat_diag_vblank(void)
{
    uint32_t vb = sat_vblanks();
    bool want = !ever_alive || vb - alive_vb > STALL_VB;
    if (!want) {
        if (shown) {   /* the game draws again (its vdp2_sync has put its registers back): its colours too */
            for (int i = 0; i < 32; i++) CRAM16(PAL_ENTRY + i) = cram_keep[i];
            BACK_WORD = 0x8000;   /* black until the renderer's next frame sets its own */
            cram_saved = false; shown = false; drawn_head = 0xFFFFFFFFu;
        }
        return;
    }
    setup();
    shown = true;
    if (saber_log.head == drawn_head && vb - drawn_vb < 30) return;
    draw(vb);
}

/* a boot stage reached: its colour at once (the vblank handler keeps it up; before user_init sets that handler, and
 * with the interrupts off, this write is all there is) */
void sat_diag_stage(unsigned s)
{
    stage = s < sizeof stage_colour / sizeof stage_colour[0] ? s : 0;
    if (ever_alive) return;
    wait_blank();
    setup();
    shown = true;
    draw(sat_vblanks());
}
#endif
