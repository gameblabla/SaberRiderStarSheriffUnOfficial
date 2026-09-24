/* A level's tile layers as VDP2 scroll planes (plan 4.2 / 4.3), from the blocks tools/saturn/layers.py bakes into
 * stage.pck: "SPL1" (planes, bands, palettes, the names in LZ4 column chunks) and the cells (32 KB blocks).
 *
 * The core asks for every tile layer through r_layer (level.c); a layer a plane holds is shown by that plane and the
 * core draws nothing for it. The first such call for a level loads its planes: every cell into video memory (they
 * all fit: level 1 is 368 KB of 4bpp cells), the palettes into colour RAM (taken from VDP1's palette banks), the name
 * tables cleared. Each frame a plane was asked for, the columns that came into view are written into its name
 * table (one 64x64 page, used as a ring), and the scroll registers / NBG0-1's line scroll table give each band its
 * rate. A frame without any plane (menus) turns them off and gives the colour RAM back.
 *
 * Video memory: the cells from 0 (banks A0, A1, B0), a 16 KB name page per NBG in B1 at 0x60000 + nbg * 0x4000,
 * the line scroll tables (x, y per line) at 0x70000 (NBG0) and 0x70800 (NBG1). The access cycle patterns follow the layout: the
 * name reads in B1 (NBGn at Tn), each plane's character reads at the same slot in every bank its cells are in, the
 * other slots CPU. mednafen only checks that a bank has a slot for what it reads; real hardware has placement rules
 * too (plan 11: check on a console). */
#include "../render.h"
#include "../../pack.h"
#include "../dreamcast/dcfmv/lz4_mini.h"
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
#define CELLS_END    0x60000u      /* the cells may use A0, A1, B0 */
#define PAGE(nbg)    (0x60000u + (uint32_t)(nbg) * 0x4000u)
#define LS_TABLE(n)  (0x70000u + (uint32_t)(n) * 0x800u)   /* 224 lines x (x, y) */
#define VRAM(off)    ((volatile uint32_t *)(0x25E00000u + (off)))

typedef struct { char magic[4]; uint16_t nplanes, nbands, npal, nbackdrops; uint32_t ncells, bands_off, names_off, cellpal_off, backdrops_off, layers; } SplHead;
typedef struct { uint8_t nbg, prio, first_band, nbands; uint32_t first_cell, ncells; uint8_t line_scroll, pad[3]; } SplPlane;
typedef struct { uint8_t plane, row0, row1, flags; int32_t rate; uint32_t cols, wrap, chunk_first, nchunks, pad[2]; } SplBand;
typedef struct { uint32_t tex; int16_t x, y; uint8_t prio, pad[3]; } SplBackdrop;

typedef struct {
    const SplBand *b;
    int lo, hi;                 /* the columns in the name page: [lo, hi), -1 none */
    int scroll, yscroll;        /* this frame's (level.c: a layer scrolls by the camera times its rate, both ways) */
    int chunk;                  /* the decoded chunk, -1 none */
    uint16_t names[CHUNK_COLS * MAX_ROWS];
} BandRt;

static struct {
    uint32_t level;             /* the level whose planes are loaded (0 none) */
    uint32_t tried;             /* a level with no planes (don't look again) */
    const uint8_t *spl;
    const SplHead *h;
    const SplPlane *planes;
    const uint8_t *cellpal;
    const uint32_t *chunks;     /* per chunk: offset, stored length | 0x80000000 LZ4 */
    BandRt band[MAX_BANDS];
    RTex *backdrop[4]; int nbackdrops;
    bool asked, shown, palettes_in;
    float cam_x, cam_y;
} P;

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

static void set_cycle_patterns(void)
{
    vdp2_vram_cycp_t c;
    for (int b = 0; b < 4; b++) c.pt[b].raw = 0xEEEEEEEEu;   /* CPU everywhere */
    uint8_t *slot[4];
    for (int b = 0; b < 4; b++) slot[b] = (uint8_t *)&c.pt[b].raw;
#define SET(bank, t, v) (slot[bank][(t) >> 1] = (uint8_t)(((t) & 1) ? (slot[bank][(t) >> 1] & 0xF0) | (v) : (slot[bank][(t) >> 1] & 0x0F) | (v) << 4))
    for (int i = 0; i < P.h->nplanes; i++) {
        const SplPlane *pl = &P.planes[i];
        int n = pl->nbg;
        SET(3, n, VDP2_VRAM_CYCP_PNDR(n));   /* names: B1 */
        uint32_t a = pl->first_cell * 32u, z = (pl->first_cell + pl->ncells) * 32u;
        for (int b = (int)(a >> 17); b <= (int)((z - 1) >> 17) && b < 3; b++) SET(b, n, VDP2_VRAM_CYCP_CHPNDR(n));
    }
#undef SET
    vdp2_vram_cycp_set(&c);
}

static void setup_screens(void)
{
    const vdp2_vram_ctl_t ctl = { .coeff_table = VDP2_VRAM_CTL_COEFF_TABLE_VRAM, .vram_mode = VDP2_VRAM_CTL_MODE_PART_BANK_BOTH };
    vdp2_vram_control_set(&ctl);
    /* libyaul's vdp2_vram_control_set clears RAMCTL bit 12 (CRAM mode 1 -> 0: 1024 colours, mirrored) and sets bit 0
     * (a rotation data bank select): back to CRAM mode 1, no bank for RBG0 */
    vdp2_regs_get()->ramctl &= (uint16_t)~0x00FFu;
    vdp2_cram_mode_set(1);
    set_cycle_patterns();
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
            const vdp2_scrn_ls_format_t ls = { .scroll_screen = scrn_of(pl->nbg), .table_base = VDP2_VRAM_ADDR(0, LS_TABLE(pl->nbg)),
                                               .interval = 0, .type = VDP2_SCRN_LS_TYPE_HORZ | VDP2_SCRN_LS_TYPE_VERT };
            vdp2_scrn_ls_set(&ls);
        }
        volatile uint32_t *pg = VRAM(PAGE(pl->nbg));
        for (int k = 0; k < 64 * 64; k++) pg[k] = 0;   /* char 0 = plane 0's empty cell: transparent */
    }
    for (int i = 0; i < P.h->nbands && i < MAX_BANDS; i++) { P.band[i].lo = P.band[i].hi = -1; P.band[i].chunk = -1; }
}

static void load_palettes(void)
{
    rsat_cram_reserve(P.h->npal * 16);
    const uint8_t *pal = P.spl + P.h->bands_off + sizeof(SplBand) * P.h->nbands;
    volatile uint16_t *c = (volatile uint16_t *)VDP2_CRAM_ADDR(0);
    for (int i = 0; i < P.h->npal * 16; i++) c[i] = (uint16_t)(pal[i * 2] << 8 | pal[i * 2 + 1]);
    P.palettes_in = true;
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
    P.spl = e->data; P.h = h;
    P.planes = (const SplPlane *)(e->data + sizeof(SplHead));
    P.cellpal = e->data + h->cellpal_off;
    P.chunks = (const uint32_t *)(e->data + h->names_off);
    for (int i = 0; i < h->nbands; i++) P.band[i].b = (const SplBand *)(e->data + h->bands_off) + i;
    vdp2_scrn_display_set(VDP2_SCRN_DISP_NONE);
    if (!load_cells(level, h->ncells)) return false;
    setup_screens();
    load_palettes();
    const SplBackdrop *bd = (const SplBackdrop *)(e->data + h->backdrops_off);
    P.nbackdrops = 0;
    for (int i = 0; i < h->nbackdrops && i < 4; i++) {
        const PackEntry *t = packs_find_type(bd[i].tex, RES_TEX);
        RTex *tex = t ? rtex_create_baked(rsat_renderer(), (uint8_t *)t->data, t->size) : NULL;
        if (t) packs_release_type(bd[i].tex, RES_TEX);
        if (!tex) { printf("planes %08X: backdrop %08X missing\n", (unsigned)level, (unsigned)bd[i].tex); continue; }
        rsat_tex_priority(tex, bd[i].prio);
        P.backdrop[P.nbackdrops++] = tex;
    }
    vdp2_sprite_priority_set(1, 1);   /* sprite register 1: the backdrops, under every plane */
    P.level = level;
    printf("planes %08X: %d planes, %u cells, %d palettes\n", (unsigned)level, h->nplanes, (unsigned)h->ncells, h->npal);
    return true;
}

/* ---------------------------------------------------------------- the core's question */
bool r_layer(Ren *r, uint32_t level, int layer, float cam_x, float cam_y)
{
    (void)r;
    if (level != P.level) {
        if (level == P.tried) return false;
        for (int i = 0; i < P.nbackdrops; i++) rtex_destroy(P.backdrop[i]);
        P.nbackdrops = 0; P.level = 0; P.spl = NULL;
        if (!load(level)) { P.tried = level; return false; }
    }
    if (layer < 0 || layer >= 32 || !(P.h->layers & (1u << layer))) return false;
    P.asked = true; P.cam_x = cam_x; P.cam_y = cam_y;
    return true;
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
            if (lz4_mini_decode(P.spl + off, (int)(len & 0x7FFFFFFF), (uint8_t *)br->names, (int)sizeof br->names, want) != want) memset(br->names, 0, sizeof br->names);
        } else memcpy(br->names, P.spl + off, (size_t)want);
        br->chunk = k;
    }
    return br->names + (col % CHUNK_COLS) * rows;
}

static void write_column(BandRt *br, int col)
{
    const SplBand *b = br->b;
    const SplPlane *pl = &P.planes[b->plane];
    volatile uint32_t *pg = VRAM(PAGE(pl->nbg));
    int rows = b->row1 - b->row0, pc = col & 63;
    int src = b->wrap ? col % (int)b->wrap : col;
    if (src < 0 || src >= (int)b->cols) {   /* beyond the band: empty */
        for (int r = 0; r < rows; r++) pg[(b->row0 + r) * 64 + pc] = 0;
        return;
    }
    const uint16_t *n = column(br, src);
    for (int r = 0; r < rows; r++) {
        uint32_t cell = pl->first_cell + (n[r] & 0x3FFFu);
        pg[(b->row0 + r) * 64 + pc] = (uint32_t)(n[r] & 0xC000u) << 16 | (uint32_t)P.cellpal[cell] << 16 | cell;
    }
}

static void update_band(BandRt *br, int sw)
{
    const SplBand *b = br->b;
    float ox = (b->flags & 1) ? 0.0f : P.cam_x * ((float)b->rate / 65536.0f);
    if (ox < 0) ox = 0;
    int scroll = (int)ceilf(ox);   /* level.c: a cell at cx * tw + floor(-ox) */
    br->scroll = scroll;
    float oy = (b->flags & 1) ? 0.0f : P.cam_y * ((float)b->rate / 65536.0f);
    br->yscroll = oy > 0 ? (int)ceilf(oy) : 0;
    int c0 = scroll >> 3, c1 = (scroll + sw + 7) >> 3;   /* [c0, c1) */
    if (br->lo < 0 || c0 >= br->hi || c1 <= br->lo || c1 - c0 > 64) {
        for (int c = c0; c < c1; c++) write_column(br, c);
    } else {
        for (int c = c0; c < br->lo; c++) write_column(br, c);
        for (int c = br->hi; c < c1; c++) write_column(br, c);
    }
    br->lo = c0; br->hi = c1;
}

void sat_planes_frame(int sw, bool delayed)
{
    /* delayed: the list going to VDP1 is the frame before's (the slave replayed it): so are the planes */
    static bool prev_asked; static float prev_x, prev_y;
    bool asked = P.asked; float cx = P.cam_x, cy = P.cam_y;
    if (delayed) { P.asked = prev_asked; P.cam_x = prev_x; P.cam_y = prev_y; prev_asked = asked; prev_x = cx; prev_y = cy; }
    if (!P.asked) {   /* no level on screen: planes off, colour RAM back to VDP1 */
        if (P.shown) { vdp2_scrn_display_set(VDP2_SCRN_DISP_NONE); rsat_set_backdrops(NULL, NULL, NULL, 0, false); P.shown = false; }
        if (P.palettes_in) { rsat_cram_reserve(0); P.palettes_in = false; }
        return;
    }
    P.asked = false;
    if (!P.palettes_in) load_palettes();
    for (int i = 0; i < P.h->nbands; i++) update_band(&P.band[i], sw);
    vdp2_scrn_disp_t disp = VDP2_SCRN_DISP_NONE;
    for (int i = 0; i < P.h->nplanes; i++) {
        const SplPlane *pl = &P.planes[i];
        vdp2_scrn_t s = scrn_of(pl->nbg);
        disp |= (vdp2_scrn_disp_t)(VDP2_SCRN_DISPTP_NBG0 << pl->nbg);   /* DISPTP: colour 0 transparent (DISP_ also sets TPON) */
        if (pl->nbands == 1 || !(pl->line_scroll && pl->nbg < 2)) {
            vdp2_scrn_scroll_x_set(s, (fix16_t)((P.band[pl->first_band].scroll & 511) << 16));
            vdp2_scrn_scroll_y_set(s, (fix16_t)((P.band[pl->first_band].yscroll & 511) << 16));
            continue;
        }
        vdp2_scrn_scroll_x_set(s, 0);
        vdp2_scrn_scroll_y_set(s, 0);
        /* per screen line (x, y): the band whose rows that line shows (the lower one where two do: the moving band's
         * first lines over the backdrop) */
        volatile uint32_t *t = VRAM(LS_TABLE(pl->nbg));
        for (int y = 0; y < SAT_SCREEN_H; y++) {
            int sx = 0, sy = 0;
            for (int k = 0; k < pl->nbands; k++) {
                const BandRt *br = &P.band[pl->first_band + k];
                int prow = y + br->yscroll;
                if (prow >= br->b->row0 * 8 && prow < br->b->row1 * 8) { sx = br->scroll; sy = br->yscroll; }
            }
            /* with vertical line scroll the table gives the line's plane row itself (not an offset added to it) */
            t[y * 2] = (uint32_t)(sx & 511) << 16; t[y * 2 + 1] = (uint32_t)((y + sy) & 511) << 16;
        }
    }
    if (!P.shown) {
        vdp2_scrn_display_set(disp);
        int x[4], y[4];
        const SplBackdrop *bd = (const SplBackdrop *)(P.spl + P.h->backdrops_off);
        for (int i = 0; i < P.nbackdrops; i++) { x[i] = bd[i].x; y[i] = bd[i].y; }
        rsat_set_backdrops(P.backdrop, x, y, P.nbackdrops, true);
        P.shown = true;
    }
}
