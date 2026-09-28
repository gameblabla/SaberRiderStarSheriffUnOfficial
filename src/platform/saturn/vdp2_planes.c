/* A level's tile layers as VDP2 scroll planes (plan 4.2 / 4.3), from the blocks tools/saturn/layers.py bakes into
 * stage.pck: "SPL1" (planes, bands, palettes, the names in LZ40S column chunks) and the cells (32 KB blocks).
 *
 * The core asks for every tile layer through r_layer (level.c); a layer a plane holds is shown by that plane and the
 * core draws nothing for it. The first such call for a level loads its planes: every cell into video memory (they
 * all fit: level 1 is 368 KB of 4bpp cells), the palettes into colour RAM (taken from VDP1's palette banks), the name
 * tables cleared. Each frame a plane was asked for, the columns that came into view are written into its name
 * table (one 64x64 page, used as a ring), and the scroll registers / NBG0-1's line scroll table give each band its
 * rate. A frame without any plane (menus) turns them off and gives the colour RAM back.
 *
 * Video memory: the cells from 0 (banks A0, A1, B0), a 16 KB name page per NBG in B1 at 0x60000 + nbg * 0x4000,
 * the line scroll tables at 0x70000 (NBG0) and 0x70400 (NBG1), twice (0x70800 on: one shown, one written). The access
 * cycle patterns follow the layout and the console's rules (set_cycle_patterns). While a level plays, video memory and
 * colour RAM are written in the vertical blank only: the name table columns and line scroll tables from here
 * (sat_planes_shown), the palettes through render_sat.c's copy of colour RAM. */
#include "../render.h"
#include "../../pack.h"
#include "lz40s.h"
#include "sat_internal.h"
#include <yaul.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SPL_XOR      0x53504C00u   /* tools/saturn/build_disc.py */
#define SPC_XOR      0x53504300u
#define CHUNK_COLS   16            /* layers.py */
#define CELL_CHUNK   1024
#define MAX_PLANES   4
#define MAX_BANDS    8
#define MAX_ROWS     32
#define CELLS_END    0x5c400u
#define PAGE(nbg)    (0x60000u + (uint32_t)(nbg) * 0x4000u)
#define LS_TABLE(n, k) (0x70000u + (uint32_t)(k) * 0x800u + (uint32_t)(n) * 0x400u)
#define VRAM(off)    ((volatile uint32_t *)(0x25E00000u + (off)))

typedef struct { char magic[4]; uint16_t nplanes, nbands, npal, nbackdrops; uint32_t ncells, bands_off, names_off, cellpal_off, backdrops_off, layers; uint8_t depth[32]; } SplHead;
typedef struct { uint8_t nbg, prio, first_band, nbands; uint32_t first_cell, ncells; uint8_t line_scroll, pad[3]; } SplPlane;
typedef struct { uint8_t plane, row0, row1, flags; int32_t rate; uint32_t cols, wrap, chunk_first, nchunks, pad[2]; } SplBand;
typedef struct { uint32_t tex; int16_t x, y; uint8_t prio, pad[3]; } SplBackdrop;

typedef struct {
    const SplBand *b;
    int lo, hi;                 /* the columns in the name page: [lo, hi), -1 none */
    int scroll;                 /* this frame's */
    int chunk;                  /* the decoded chunk, -1 none */
    uint16_t names[CHUNK_COLS * MAX_ROWS];
} BandRt;

static struct {
    uint32_t level;             /* the level whose planes are loaded (0 none) */
    uint32_t tried;             /* a level with no planes (don't look again) */
    const uint8_t *spl; uint32_t spl_id;   /* the planes' block */
    const SplHead *h;
    const SplPlane *planes;
    const uint8_t *cellpal;
    const uint32_t *chunks;     /* per chunk: offset, stored length | 0x80000000 LZ40S */
    BandRt band[MAX_BANDS];
    RTex *backdrop[4]; int nbackdrops;
    uint8_t depth_reg[32];      /* per level layer: the sprite priority register its palette sprites take (0 the front) */
    bool asked, shown, palettes_in;
    fx cam_x, cam_y;
} P;

/* The scroll of a frame's planes goes on screen with that frame's sprites (the store's camera prop on its wall, in the
 * shake). VDP1 changes framebuffers at the first vblank after it has drawn the list (variable mode), a field after
 * VDP2's registers would take a value set with the list when the list runs past its field. sat_planes_frame leaves
 * the scroll here, the line scroll tables in ls_ram and the new columns in the queue; sat_planes_shown puts them in
 * video memory (the line scroll table not shown) and the registers at the vblank VDP1's frame changes. */
static struct { fix16_t x[4], y[4]; bool ls[4]; } latch;
static volatile bool latch_ready;
static int ls_shown;            /* the line scroll tables on screen (0, 1) */

static uint32_t be32(const void *p) { const uint8_t *b = p; return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

static vdp2_scrn_t scrn_of(int nbg) { return (vdp2_scrn_t)(VDP2_SCRN_NBG0 << nbg); }

/* ---------------------------------------------------------------- loading */
/* the cells: 32 KB blocks (level ^ (SPC_XOR + n)), each read, copied into video memory and released */
static bool load_cells(uint32_t level, uint32_t ncells)
{
    if (ncells * 32u > CELLS_END) { printf("planes %08X: %u KB of cells, more than fits\n", (unsigned)level, (unsigned)(ncells / 32)); return false; }
    for (uint32_t k = 0; k * CELL_CHUNK < ncells; k++) {
        uint32_t id = level ^ (SPC_XOR + k), want = (ncells - k * CELL_CHUNK < CELL_CHUNK ? ncells - k * CELL_CHUNK : CELL_CHUNK) * 32;
        const PackEntry *e = packs_find_type(id, RES_DATA);
        if (!e || e->size < want) { printf("planes %08X: cell block %u missing\n", (unsigned)level, (unsigned)k); return false; }
        volatile uint32_t *d = VRAM(k * CELL_CHUNK * 32u);
        const uint32_t *s = (const uint32_t *)e->data;   /* big-endian on disc: the SH-2's order */
        for (uint32_t i = 0; i < want / 4; i++) d[i] = s[i];
        packs_release_type(id, RES_DATA);
    }
    return true;
}

/* The access cycle patterns follow the VDP2 manual (ST-058-R2 3.3, "Read/Write Access by the CPU") and Sega's bulletin
 * on them (SOA #6, Sattechs.pdf): VRAM is split in two halves per chip, and a CPU slot must be a CPU slot in both
 * halves of the chip (A0 and A1, B0 and B1), with "no access" in both at the slot before a run of CPU slots; a slot
 * one half can't give the CPU is "no access", not CPU. Mednafen and Ymir take any pattern. The patterns this file had
 * gave the CPU T4-T7 after a name / character read, and a CPU slot in one half across a read in the other: on the
 * console level 1's far plane (NBG0, its characters read in A0 at T0 against A1's CPU slot) came out black, and name
 * table writes made while the screen was drawn went missing (cells of the rocks black or showing another tile).
 *   loading (nothing shown):  every bank F E E E E E E E (the bulletin's pattern for a bank no screen reads)
 *   shown:  T0-T3 NBGn's reads at Tn (names in B1, characters in each bank its cells are in; a character read at its
 *           name read's slot is always a legal one), else F; T4 F; T5-T7 CPU */
static void put_cycle_patterns(const vdp2_vram_cycp_t *c)
{
    vdp2_vram_cycp_set(c);
    /* in the registers now too (libyaul's copy goes at the next vblank's commit): cells and names are written next */
    volatile uint32_t *cyc = (volatile uint32_t *)(VDP2_IOREG_BASE + 0x10u);
    for (int b = 0; b < 4; b++) cyc[b] = c->pt[b].raw;
}

static void set_upload_patterns(void)
{
    vdp2_vram_cycp_t c;
    for (int b = 0; b < 4; b++) c.pt[b].raw = 0xFEEEEEEEu;
    put_cycle_patterns(&c);
}

static void set_cycle_patterns(void)
{
    vdp2_vram_cycp_t c;
    for (int b = 0; b < 4; b++) c.pt[b].raw = 0xFFFFFEEEu;
    uint8_t *slot[4];
    for (int b = 0; b < 4; b++) slot[b] = (uint8_t *)&c.pt[b].raw;   /* big-endian: T0 in the top nibble */
#define SET(bank, t, v) (slot[bank][(t) >> 1] = (uint8_t)(((t) & 1) ? (slot[bank][(t) >> 1] & 0xF0) | (v) : (slot[bank][(t) >> 1] & 0x0F) | (v) << 4))
    for (int i = 0; i < P.h->nplanes; i++) {
        const SplPlane *pl = &P.planes[i];
        int n = pl->nbg;
        SET(3, n, VDP2_VRAM_CYCP_PNDR(n));   /* names: B1 */
        uint32_t a = pl->first_cell * 32u, z = (pl->first_cell + pl->ncells) * 32u;
        for (int b = (int)(a >> 17); b <= (int)((z - 1) >> 17) && b < 3; b++) SET(b, n, VDP2_VRAM_CYCP_CHPNDR(n));
    }
#undef SET
    put_cycle_patterns(&c);
}

/* ---------------------------------------------------------------- name table writes, in the vertical blank
 * The columns that come into view go into this queue (sat_planes_frame, while the screen is drawn) and into the name
 * tables at the vblank that shows their frame (sat_planes_shown), before the scroll that shows them. A frame that
 * writes more (a level's first frame: the whole view) writes the queue itself in the vblanks as it fills. */
enum { NQ_MAX = 2048 };
static uint32_t nq_val[NQ_MAX];
static uint16_t nq_at[NQ_MAX];     /* the entry's long word from PAGE(0) (NBGn's page at n * 4096) */
static int nq_n;
static uint32_t ls_ram[2][SAT_SCREEN_H];   /* NBG0-1's line scroll table of the frame, for the vblank */

#define TVSTAT_VBLANK() ((*(volatile uint16_t *)0x25F80004u & 0x0008u) != 0)

/* the queue into video memory now (the vblank-in handler, or the main loop in a vblank) */
static void nq_write(void)
{
    volatile uint32_t *pg = VRAM(PAGE(0));
    for (int i = 0; i < nq_n; i++) pg[nq_at[i]] = nq_val[i];
    nq_n = 0;
}

/* a full queue from the main loop (latch_ready is off: the handler leaves the queue alone) */
static void nq_drain(void)
{
    volatile uint32_t *pg = VRAM(PAGE(0));
    int i = 0;
    while (i < nq_n) {
        for (uint32_t spins = 0; !TVSTAT_VBLANK() && spins < 400000u; spins++) { }   /* (~20 ms at most) */
        do pg[nq_at[i]] = nq_val[i]; while (++i < nq_n && TVSTAT_VBLANK());
    }
    nq_n = 0;
}

static void nq_put(int nbg, int k, uint32_t v)
{
    if (nq_n == NQ_MAX) nq_drain();
    nq_at[nq_n] = (uint16_t)(nbg * 4096 + k); nq_val[nq_n++] = v;
}

static void setup_screens(void)
{
    const vdp2_vram_ctl_t ctl = { .coeff_table = VDP2_VRAM_CTL_COEFF_TABLE_VRAM, .vram_mode = VDP2_VRAM_CTL_MODE_PART_BANK_BOTH };
    uint16_t floor_ramctl = sat_floor_visible() ? (uint16_t)(vdp2_regs_get()->ramctl & 0x00FFu) : 0;
    vdp2_vram_control_set(&ctl);
    vdp2_cram_mode_set(1);
    vdp2_ioregs_t *regs = vdp2_regs_get();
    regs->ramctl = (uint16_t)((regs->ramctl & (uint16_t)~0x00FFu) | floor_ramctl);
    /* NBG0-1's line scroll off: vdp2_scrn_ls_set only turns it on, and a plane without it that follows one with it
     * (level 1's NBG0, then level 3's) read the old level's table: its page scrolled wrong, the ring's empty columns
     * a hole in the far mountains that crept across the screen */
    regs->scrctl = (uint16_t)(regs->scrctl & ~0x3E3Eu);
    nq_n = 0;   /* (the last level's columns, never shown) */
    for (int i = 0; i < P.h->nplanes; i++) {
        const SplPlane *pl = &P.planes[i];
        vdp2_scrn_cell_format_t f = {
            .scroll_screen = scrn_of(pl->nbg), .ccc = VDP2_SCRN_CCC_PALETTE_16, .char_size = VDP2_SCRN_CHAR_SIZE_1X1,
            .pnd_size = 2, .aux_mode = VDP2_SCRN_AUX_MODE_0, .plane_size = VDP2_SCRN_PLANE_SIZE_1X1,
            .cpd_base = VDP2_VRAM_ADDR(0, 0), .palette_base = VDP2_CRAM_ADDR(0) };
        uint32_t page = VDP2_VRAM_ADDR(0, PAGE(pl->nbg));
        const vdp2_scrn_normal_map_t map = { .plane_a = page, .plane_b = page, .plane_c = page, .plane_d = page };
        vdp2_scrn_cell_format_set(&f, &map);
        vdp2_scrn_priority_set(scrn_of(pl->nbg), pl->prio);
        if (pl->line_scroll && pl->nbg < 2) {
            const vdp2_scrn_ls_format_t ls = { .scroll_screen = scrn_of(pl->nbg), .table_base = VDP2_VRAM_ADDR(0, LS_TABLE(pl->nbg, ls_shown)),
                                               .interval = 0, .type = VDP2_SCRN_LS_TYPE_HORZ };
            vdp2_scrn_ls_set(&ls);
        }
        volatile uint32_t *pg = VRAM(PAGE(pl->nbg));
        for (int k = 0; k < 64 * 64; k++) pg[k] = 0;   /* char 0 = plane 0's empty cell: transparent (the upload patterns) */
    }
    for (int i = 0; i < P.h->nbands && i < MAX_BANDS; i++) { P.band[i].lo = P.band[i].hi = -1; P.band[i].chunk = -1; }
}

static void load_palettes(void)
{
    rsat_cram_reserve(P.h->npal * 16);
    const uint8_t *pal = P.spl + P.h->bands_off + sizeof(SplBand) * P.h->nbands;
    rsat_cram_put_be(0, pal, P.h->npal * 16);   /* at the next vblank (a write while the screen is drawn is lost) */
    P.palettes_in = true;
}

/* a level's planes let go: the backdrops, the block (its names), the display at the next frame (sat_planes_frame) */
static void unload(void)
{
    if (P.shown) { rsat_set_backdrops(NULL, NULL, NULL, 0, false); P.shown = false; }
    for (int i = 0; i < P.nbackdrops; i++) rtex_destroy(P.backdrop[i]);
    P.nbackdrops = 0;
    if (P.spl) packs_release_type(P.spl_id, RES_DATA);
    P.level = 0; P.spl = NULL; P.h = NULL; P.asked = false;
}

static bool load(uint32_t level)
{
    const PackEntry *e = packs_find_type(level ^ SPL_XOR, RES_DATA);
    if (!e || e->size < sizeof(SplHead) || memcmp(e->data, "SPL1", 4)) return false;
    const SplHead *h = (const SplHead *)e->data;
    if (h->nplanes > MAX_PLANES || h->nbands > MAX_BANDS) { printf("planes %08X: too many planes / bands\n", (unsigned)level); return false; }
    for (int i = 0; i < h->nbands; i++) {
        const SplBand *b = (const SplBand *)(e->data + h->bands_off) + i;
        if (b->row1 - b->row0 > MAX_ROWS) { printf("planes %08X: a band of %d rows\n", (unsigned)level, b->row1 - b->row0); return false; }
    }
    P.spl = e->data; P.spl_id = level ^ SPL_XOR; P.h = h;
    P.planes = (const SplPlane *)(e->data + sizeof(SplHead));
    P.cellpal = e->data + h->cellpal_off;
    P.chunks = (const uint32_t *)(e->data + h->names_off);
    for (int i = 0; i < h->nbands; i++) P.band[i].b = (const SplBand *)(e->data + h->bands_off) + i;
    /* the planes off (in the register too: the display may still show the last level's) while their video memory is
     * written through the loading patterns, then the patterns that show them */
    vdp2_scrn_display_set(sat_floor_visible() ? VDP2_SCRN_DISPTP_RBG0 : VDP2_SCRN_DISP_NONE);
    *(volatile uint16_t *)(VDP2_IOREG_BASE + 0x20u) = vdp2_regs_get()->bgon;
    set_upload_patterns();
    if (!load_cells(level, h->ncells)) { set_cycle_patterns(); unload(); return false; }
    setup_screens();
    set_cycle_patterns();
    load_palettes();
    const SplBackdrop *bd = (const SplBackdrop *)(e->data + h->backdrops_off);
    P.nbackdrops = 0;
    for (int i = 0; i < h->nbackdrops && i < 4; i++) {
        const PackEntry *t = packs_find_type(bd[i].tex, RES_TEX);
        uint8_t *owned = t ? packs_take_type(bd[i].tex, RES_TEX) : NULL;
        RTex *tex = owned ? rtex_create_baked(rsat_renderer(), owned, t->size) : NULL;
        if (!tex) { printf("planes %08X: backdrop %08X missing\n", (unsigned)level, (unsigned)bd[i].tex); continue; }
        rsat_tex_priority(tex, bd[i].prio);
        P.backdrop[P.nbackdrops++] = tex;
    }
    vdp2_sprite_priority_set(1, 1);   /* sprite register 1: the backdrops, under every plane */
    /* registers 2..7 for the priorities of the sprite layers between planes (layers.py depth_table) */
    int nreg = 2; uint8_t reg_of[8] = { 0 };
    memset(P.depth_reg, 0, sizeof P.depth_reg);
    for (int i = 0; i < 32; i++) {
        uint8_t v = h->depth[i];
        if (!v || v > 7) continue;
        if (!reg_of[v] && nreg < 8) { reg_of[v] = (uint8_t)nreg; vdp2_sprite_priority_set((vdp2_sprite_register_t)nreg, v); nreg++; }
        P.depth_reg[i] = reg_of[v];
    }
    P.level = level;
    printf("planes %08X: %d planes, %u cells, %d palettes\n", (unsigned)level, h->nplanes, (unsigned)h->ncells, h->npal);
    return true;
}

/* a plane's vertical scroll for the camera's y (the shake: cam_y 0..3 in the platform stages): level.c's cam_y x the
 * layer's parallax, rounded up as it rounds a cell's position; a plane with a static band (the sky) stays put, as the
 * PC's wrapping sky layer does, and so do the backdrops under it */
static int plane_oy(const SplPlane *pl, fx cam_y)
{
    if (cam_y <= 0) return 0;
    int oy = 0;
    for (int k = 0; k < pl->nbands; k++) {
        const SplBand *b = P.band[pl->first_band + k].b;
        if (b->flags & 1) return 0;
        int v = fx_ceil(fx_mul(cam_y, (fx)b->rate));
        if (v > oy) oy = v;
    }
    return oy;
}

/* ---------------------------------------------------------------- the core's question */
bool r_layer(Ren *r, uint32_t level, int layer, fx cam_x, fx cam_y)
{
    (void)r;
    if (level != P.level) {   /* another level's (or level 0: a level left for good, render.h) */
        if (P.level) unload();
        if (!level || level == P.tried) return false;
        if (!load(level)) { P.tried = level; return false; }
    }
    if (layer < 0 || layer >= 32 || !(P.h->layers & (1u << layer))) return false;
    if (!P.asked) {   /* the backdrops (under every plane) move with the plane that moves least */
        int dy = 1 << 30;
        for (int i = 0; i < P.h->nplanes; i++) { int v = plane_oy(&P.planes[i], cam_y); if (v < dy) dy = v; }
        rsat_backdrops_dy(P.h->nplanes ? dy : 0);
    }
    P.asked = true; P.cam_x = cam_x; P.cam_y = cam_y;
    return true;
}

bool r_layer_held(Ren *r, uint32_t level, int layer)
{
    (void)r;
    if (layer < 0 || layer >= 32) return false;
    const PackEntry *e = packs_find_type(level ^ SPL_XOR, RES_DATA);
    return e && e->size >= sizeof(SplHead) && !memcmp(e->data, "SPL1", 4) && (((const SplHead *)e->data)->layers & (1u << layer));
}

/* the sprite priority register of level layer `layer`'s sprites (0: the default one, in front of the planes) */
uint32_t sat_planes_level(void) { return P.level; }

int sat_planes_depth_reg(uint32_t level, int layer)
{
    return level == P.level && layer >= 0 && layer < 32 ? P.depth_reg[layer] : 0;
}

/* ---------------------------------------------------------------- per frame */
static const uint16_t *column(BandRt *br, int col)
{
    const SplBand *b = br->b;
    int rows = b->row1 - b->row0, k = col / CHUNK_COLS;
    if (k != br->chunk) {
        uint32_t off = be32(&P.chunks[(b->chunk_first + (uint32_t)k) * 2]), len = be32(&P.chunks[(b->chunk_first + (uint32_t)k) * 2 + 1]);
        int want = (b->cols - (uint32_t)k * CHUNK_COLS < CHUNK_COLS ? (int)(b->cols - (uint32_t)k * CHUNK_COLS) : CHUNK_COLS) * rows * 2;
        if (len & 0x80000000u) {
            if (lz40s_decode(P.spl + off, (int)(len & 0x7FFFFFFF), (uint8_t *)br->names, want) != want) memset(br->names, 0, sizeof br->names);
        } else memcpy(br->names, P.spl + off, (size_t)want);
        br->chunk = k;
    }
    return br->names + (col % CHUNK_COLS) * rows;
}

static void write_column(BandRt *br, int col)
{
    const SplBand *b = br->b;
    const SplPlane *pl = &P.planes[b->plane];
    int rows = b->row1 - b->row0, pc = col & 63;
    int src = b->wrap ? col % (int)b->wrap : col;
    if (src < 0 || src >= (int)b->cols) {   /* beyond the band: empty */
        for (int r = 0; r < rows; r++) nq_put(pl->nbg, (b->row0 + r) * 64 + pc, 0);
        return;
    }
    const uint16_t *n = column(br, src);
    for (int r = 0; r < rows; r++) {
        uint32_t cell = pl->first_cell + (n[r] & 0x3FFFu);
        nq_put(pl->nbg, (b->row0 + r) * 64 + pc, (uint32_t)(n[r] & 0xC000u) << 16 | (uint32_t)P.cellpal[cell] << 16 | cell);
    }
}

static void update_band(BandRt *br, int sw)
{
    const SplBand *b = br->b;
    fx ox = (b->flags & 1) ? 0 : fx_mul(P.cam_x, (fx)b->rate);   /* level.c's offset: the camera times the layer's parallax */
    if (ox < 0) ox = 0;
    int scroll = fx_ceil(ox);   /* level.c: a cell at cx * tw + floor(-ox) */
    br->scroll = scroll;
    int c0 = scroll >> 3, c1 = (scroll + sw + 7) >> 3;   /* [c0, c1) */
    if (br->lo < 0 || c0 >= br->hi || c1 <= br->lo || c1 - c0 > 64) {
        for (int c = c0; c < c1; c++) write_column(br, c);
    } else {
        for (int c = c0; c < br->lo; c++) write_column(br, c);
        for (int c = br->hi; c < c1; c++) write_column(br, c);
    }
    br->lo = c0; br->hi = c1;
}

/* the planes of the frame whose list goes to VDP1 now: the last one drawn (render_sat.c submits it at the start of the
 * next, before the core asks for the next frame's layers) */
void sat_planes_frame(int sw)
{
    latch_ready = false;
    if (!P.asked || !P.h) {
        vdp2_scrn_display_set(sat_floor_visible() ? VDP2_SCRN_DISPTP_RBG0 : VDP2_SCRN_DISP_NONE);
        if (P.shown) { rsat_set_backdrops(NULL, NULL, NULL, 0, false); P.shown = false; }
        if (P.palettes_in) { rsat_cram_reserve(0); P.palettes_in = false; }
        return;
    }
    P.asked = false;
    if (!P.palettes_in) load_palettes();
    for (int i = 0; i < P.h->nbands; i++) update_band(&P.band[i], sw);
    vdp2_scrn_disp_t disp = sat_floor_visible() ? VDP2_SCRN_DISPTP_RBG0 : VDP2_SCRN_DISP_NONE;
    for (int i = 0; i < P.h->nplanes; i++) {
        const SplPlane *pl = &P.planes[i];
        disp |= (vdp2_scrn_disp_t)(VDP2_SCRN_DISPTP_NBG0 << pl->nbg);   /* DISPTP: colour 0 transparent (DISP_ also sets TPON) */
        int oy = plane_oy(pl, P.cam_y);   /* the camera shake */
        latch.y[pl->nbg] = (fix16_t)(oy << 16);
        latch.ls[pl->nbg] = false;
        if (pl->nbands == 1 || !(pl->line_scroll && pl->nbg < 2)) {
            latch.x[pl->nbg] = (fix16_t)((P.band[pl->first_band].scroll & 511) << 16);
            continue;
        }
        latch.x[pl->nbg] = 0;
        latch.ls[pl->nbg] = true;
        uint32_t *t = ls_ram[pl->nbg];   /* per screen line: the scroll of the band at that plane row (the vblank writes it) */
        for (int y = 0; y < SAT_SCREEN_H; y++) {
            int row = (y + oy) >> 3, sc = 0;
            for (int k = 0; k < pl->nbands; k++) {
                const SplBand *b = P.band[pl->first_band + k].b;
                if (row >= b->row0 && row < b->row1) { sc = P.band[pl->first_band + k].scroll; break; }
            }
            t[y] = (uint32_t)(sc & 511) << 16;
        }
    }
    latch_ready = true;
    if (!P.shown) {
        vdp2_scrn_display_set(disp);
        int x[4], y[4];
        const SplBackdrop *bd = (const SplBackdrop *)(P.spl + P.h->backdrops_off);
        for (int i = 0; i < P.nbackdrops; i++) { x[i] = bd[i].x; y[i] = bd[i].y; }
        rsat_set_backdrops(P.backdrop, x, y, P.nbackdrops, true);
        P.shown = true;
    }
}

/* VDP1's frame changes at this vblank (vdp1_sync_render_set: libyaul calls it from the vblank-in handler, once the
 * list is drawn): the planes' scroll of that frame, in the registers (and in libyaul's copy, which it commits after) */
void sat_planes_shown(void)
{
    if (!latch_ready || !P.h) return;
    latch_ready = false;
    nq_write();   /* the columns this frame's scroll brings into view */
    vdp2_ioregs_t *sh = vdp2_regs_get();
    volatile vdp2_ioregs_t *hw = (volatile vdp2_ioregs_t *)VDP2_IOREG_BASE;
    bool flip = false;
    for (int i = 0; i < P.h->nplanes; i++) {
        int n = P.planes[i].nbg;
        vdp2_scrn_scroll_x_set(scrn_of(n), latch.x[n]);
        vdp2_scrn_scroll_y_set(scrn_of(n), latch.y[n]);
        if (latch.ls[n]) {
            flip = true;
            volatile uint32_t *t = VRAM(LS_TABLE(n, ls_shown ^ 1));
            for (int y = 0; y < SAT_SCREEN_H; y++) t[y] = ls_ram[n][y];
            uint32_t a = VDP2_VRAM_ADDR(0, LS_TABLE(n, ls_shown ^ 1));
            uint16_t u = (uint16_t)VDP2_VRAM_BANK(a), l = (uint16_t)((a >> 1) & 0xFFFF);
            if (n == 0) { sh->lsta0u = u; sh->lsta0l = l; hw->lsta0u = u; hw->lsta0l = l; }
            else        { sh->lsta1u = u; sh->lsta1l = l; hw->lsta1u = u; hw->lsta1l = l; }
        }
    }
    const volatile uint16_t *from = (const uint16_t *)&sh->sc0;   /* SCXIN0 .. SCYN3: 16-bit registers */
    volatile uint16_t *to = (volatile uint16_t *)&hw->sc0;
    for (unsigned k = 0; k < (unsigned)((const uint8_t *)&sh->scn3 + sizeof sh->scn3 - (const uint8_t *)&sh->sc0) / 2; k++) to[k] = from[k];
    if (flip) ls_shown ^= 1;
}
