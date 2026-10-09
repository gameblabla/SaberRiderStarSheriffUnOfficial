"""PC Engine front end: title, options/credits, and hero selection screens.

All screens are 320x224 (40x28 BG characters). Art comes from the same source
assets as the Saturn menus. Animation is palette driven so it costs almost no
CPU: the tunnel backdrop cycles its palettes, and the hero panels switch
between selected/unselected palettes. Text is drawn at runtime with BG font
characters on flat panel colours, so no glyph ever sits on a black box.
"""
import math
import json
import struct
import numpy as np
from PIL import Image
from formats import Archive, planar_tile, planar_sprite, vce_colors, vce_rgb, palette_for, indexed
import palfit

W, H = 320, 224
COLS, ROWS = W // 8, H // 8
UI_TILE_WORD = 0x0800          # BG character n is at VRAM word UI_TILE_WORD + 16 * n
UI_SPRITE_WORD = 0x6800        # sprite patterns, 64 words each (92 fit below the SAT)
UI_SCREENS = ('title', 'select', 'options', 'panel', 'gameover')

def vc(rgb):
    """Round an RGB triple to the hardware's 9-bit lattice."""
    return tuple(int(v) for v in vce_rgb(vce_colors(np.asarray(rgb, np.uint8))))

def word(rgb):
    return int(vce_colors(np.asarray(rgb, np.uint8)))

def blank_palettes():
    return np.zeros((16, 16), '<u2')

class Screen:
    """Indexed 320x224 picture plus its per-character palette map."""
    def __init__(self):
        self.index = np.zeros((H, W), np.uint8)
        self.pal = np.zeros((ROWS, COLS), np.uint8)
        self.palettes = blank_palettes()
        self.sprite_patterns = []
        self.sprite_palettes = blank_palettes()

    def preview(self, path, shift=0):
        rgb = np.zeros((H, W, 3), np.uint8)
        for cy in range(ROWS):
            for cx in range(COLS):
                p = self.palettes[self.pal[cy, cx]]
                tile = self.index[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8]
                rgb[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8] = vce_rgb(p)[tile]
        Image.fromarray(rgb).resize((W * 3, H * 3), Image.NEAREST).save(path)

    def emit(self, archive, name, reserved=()):
        """Write palettes, characters, and the BAT; return the C record.
        `reserved` characters come first so the runtime can address them."""
        tiles, lookup, words = [], {}, []
        for data in reserved:
            if data not in lookup: lookup[data] = len(tiles)
            tiles.append(data)
        for cy in range(ROWS):
            for cx in range(COLS):
                data = planar_tile(self.index[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8])
                if data not in lookup:
                    lookup[data] = len(tiles); tiles.append(data)
                words.append((int(self.pal[cy, cx]) << 12) | ((UI_TILE_WORD >> 4) + lookup[data]))
        if UI_TILE_WORD // 16 + len(tiles) > UI_SPRITE_WORD // 16:
            raise ValueError(f'{name}: {len(tiles)} characters exceed the UI tile budget')
        rec = dict(
            pal=archive.add(f'ui_{name}_palette', self.palettes.astype('<u2').tobytes()),
            tiles=archive.add(f'ui_{name}_tiles', b''.join(tiles)),
            map=archive.add(f'ui_{name}_map', np.asarray(words, '<u2').tobytes()),
            sprpal=archive.add(f'ui_{name}_sprite_palette', self.sprite_palettes.astype('<u2').tobytes()),
            sprpat=archive.add(f'ui_{name}_sprite_patterns', b''.join(self.sprite_patterns) or bytes(2)),
            ntiles=len(tiles), nsprpat=len(self.sprite_patterns))
        return rec

def kmeans_groups(means, k, seed=1):
    """Cluster characters by mean colour."""
    rng = np.random.default_rng(seed)
    centers = means[np.linspace(0, len(means) - 1, k, dtype=int)].astype(np.float64)
    order = np.argsort(means.sum(1))
    centers = means[order[np.linspace(0, len(means) - 1, k, dtype=int)]].astype(np.float64)
    for _ in range(20):
        g = ((means[:, None] - centers[None]) ** 2).sum(-1).argmin(1)
        for i in range(k):
            if (g == i).any(): centers[i] = means[g == i].mean(0)
    return g

def paint_cells(screen, rgba, palette_slots, colors=15, skip=()):
    """Quantize a full-colour picture into `palette_slots` palettes (cells in `skip` are left alone)."""
    a = np.asarray(rgba.convert('RGBA'))
    cells = a.reshape(ROWS, 8, COLS, 8, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 8, 8, 4)
    means = cells[..., :3].reshape(len(cells), -1, 3).mean(1)
    keep = np.array([i not in skip for i in range(len(cells))])
    groups = np.full(len(cells), -1)
    groups[keep] = kmeans_groups(means[keep], len(palette_slots))
    for gi, slot in enumerate(palette_slots):
        members = np.nonzero(groups == gi)[0]
        if not len(members): continue
        pal = palette_for([Image.fromarray(cells[members].reshape(-1, 8, 4))], colors)
        screen.palettes[slot] = pal
        for m in members:
            cy, cx = divmod(m, COLS)
            screen.pal[cy, cx] = slot
            screen.index[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8] = indexed(Image.fromarray(cells[m]), pal)

def two_colour_sprites(image, palette_a, palette_b=None):
    """16x16 patterns for an image with two opaque colours (1 = ink, 2 = outline)."""
    a = np.asarray(image.convert('RGBA'))
    h, w = a.shape[:2]
    patterns = []
    ink = a[..., :3].sum(-1) > 600
    idx = np.where(a[..., 3] > 0, np.where(ink, 1, 2), 0).astype(np.uint8)
    idx = np.pad(idx, ((0, -h % 16), (0, -w % 16)))
    for y in range(0, idx.shape[0], 16):
        for x in range(0, idx.shape[1], 16):
            patterns.append(planar_sprite(idx[y:y + 16, x:x + 16]))
    return patterns, idx.shape[1] // 16, idx.shape[0] // 16

# ---------------------------------------------------------------- tunnel
RAMP = 12                       # cycled colours per ring palette (indices 1..12)
RINGS = 4                       # palette slots 0..3 hold the brightness rings
STATIC_WHITE, STATIC_LIGHT, STATIC_DARK = 13, 14, 15
RIB = (8, 8, 16)                # backdrop / rib colour (palette 0 entry 0)

def ramp_colors(brightness):
    """One full cycle of the arm colours, navy -> purple -> red -> navy."""
    out = []
    for i in range(RAMP):
        t = 0.5 - 0.5 * math.cos(2 * math.pi * i / RAMP)
        a, b = np.array((22, 26, 104.0)), np.array((176, 6, 34.0))
        c = a + (b - a) * t
        out.append(vc(np.clip(c * brightness, 0, 255)))
    return out

RING_BRIGHT_FULL = (0.50, 0.68, 0.86, 1.00)
RING_BRIGHT_DIM = (0.28, 0.38, 0.48, 0.58)

def tunnel(screen, bright, arms=8):
    """Fill the screen with the spiral tunnel (index 0 = static ribs)."""
    yy, xx = np.mgrid[0:H, 0:W]
    dx, dy = xx + 0.5 - W / 2, yy + 0.5 - H / 2
    r = np.hypot(dx, dy) + 1.0
    theta = np.arctan2(dy, dx)
    lr = np.log(r + 3)
    arm = theta / (2 * math.pi) * arms + 1.25 * lr
    phase = arm % 1.0
    idx = 1 + np.floor(phase * RAMP).astype(np.uint8)
    # Each tentacle's ribs are staggered against its neighbours.
    rib = ((lr * 9.0 + 0.5 * np.floor(arm)) % 1.0) < 0.17
    idx = np.where(rib, 0, idx).astype(np.uint8)
    idx[r < 14] = 0
    screen.index[:] = idx
    # Ring palette per character, by mean radius.
    cell_r = r.reshape(ROWS, 8, COLS, 8).mean((1, 3))
    edges = (62, 108, 154)
    ring = np.digitize(cell_r, edges).astype(np.uint8)
    screen.pal[:] = ring
    for k in range(RINGS):
        ramp = ramp_colors(bright[k])
        pal = screen.palettes[k]
        pal[0] = word(RIB)
        for i in range(RAMP): pal[1 + i] = word(ramp[i])
        pal[STATIC_WHITE] = word((255, 255, 255))
        pal[STATIC_LIGHT] = word((189, 189, 189))
        pal[STATIC_DARK] = word((16, 16, 8))
    return ring

def ramp_table(bright):
    return b''.join(np.asarray([word(c) for c in ramp_colors(bright[k])], '<u2').tobytes() for k in range(RINGS))

def static_text(screen, image, x, y):
    """Place a white/light/dark picture on the tunnel using the static indices."""
    a = np.asarray(image.convert('RGBA'))
    h, w = a.shape[:2]
    lum = a[..., :3].sum(-1)
    cls = np.where(lum > 700, STATIC_WHITE, np.where(lum > 400, STATIC_LIGHT, STATIC_DARK))
    region = screen.index[y:y + h, x:x + w]
    region[:] = np.where(a[..., 3] >= 128, cls, region)
    cy0, cy1 = y // 8, (y + h - 1) // 8
    cx0, cx1 = x // 8, (x + w - 1) // 8
    return (cy0, cy1, cx0, cx1)

# ---------------------------------------------------------------- title
BAYER = (np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) + 0.5) / 16

def dithered(rgb, strength=1.0):
    """Ordered-dither a float RGB picture onto the 9-bit lattice (repeating, so cheap in tiles)."""
    h, w = rgb.shape[:2]
    thr = np.tile(BAYER, (h // 4 + 1, w // 4 + 1))[:h, :w, None]
    level = np.floor(np.clip(rgb, 0, 255) / 255 * 7 + 0.5 + (thr-0.5)*strength).clip(0, 7)
    return (level * 255 / 7).round().astype(np.uint8)

def title_backdrop():
    """The Saturn's blue field, rebuilt as a smooth glow so it quantizes cleanly."""
    yy, xx = np.mgrid[0:H, 0:W]
    d = np.hypot((xx - W / 2) / (W / 2), (yy - 100) / (H * 0.62)).clip(0, 1)
    inner, outer = np.array((10, 66, 130.0)), np.array((0, 18, 70.0))
    rgb = inner + (outer - inner) * (d ** 1.15)[..., None]
    img = np.dstack((dithered(rgb), np.full((H, W), 255, np.uint8)))
    return Image.fromarray(img)

def title_screen(get, sgx=False):
    s = Screen()
    canvas = title_backdrop()
    if sgx:
        canvas = Image.new('RGBA', (W,H), (0,0,0,255))
        bg = get(0xD7DEBAC0).convert('RGBA')
        canvas.alpha_composite(bg, (0,0))
        canvas.alpha_composite(bg.transpose(Image.Transpose.FLIP_LEFT_RIGHT), (W-bg.width,0))
    shadow = get(0x989121EC); sa = np.asarray(shadow).copy(); sa[..., 3] = (sa[..., 3].astype(np.uint16) * 80 // 255).astype(np.uint8)
    canvas.alpha_composite(Image.fromarray(sa), (8, 32))
    canvas.alpha_composite(get(0xB04BAC5F), (8, 32))
    paint_cells(s, canvas, list(range(16)))
    s.preview_source = canvas
    # Menu items are sprites so the highlight is a palette swap.
    start, opt = get(0x01E9B701), get(0x8FF0AB30)
    # Original menu art is singular OPTION. Match the requested OPTIONS.
    # Include the complete S outline (x=39..46), without the next T at x=47.
    suffix=start.crop((38,0,47,16))
    # Place S after the visible N, excluding the source image's trailing padding.
    suffix_bounds=suffix.getchannel('A').getbbox()
    suffix=suffix.crop((suffix_bounds[0],0,suffix_bounds[2],suffix.height))
    # The right bound is exclusive, so S starts without overlapping N.
    suffix_x=opt.getchannel('A').getbbox()[2]
    plural=Image.new('RGBA',(suffix_x+suffix.width,opt.height))
    plural.alpha_composite(opt,(0,0));plural.alpha_composite(suffix,(suffix_x,0))
    opt=plural
    p0, n0, _ = two_colour_sprites(start, None)
    p1, n1, _ = two_colour_sprites(opt, None)
    s.sprite_patterns = p0 + p1
    ink_outline = lambda ink: [0, word(ink), word((0, 33, 66))]
    for slot, ink in enumerate(((255, 255, 255), (255, 182, 0))):
        s.sprite_palettes[slot][1:3] = ink_outline(ink)[1:]
    s.items = dict(start=(0, n0), option=(len(p0), n1))
    if not sgx: sprite_patches(s, canvas, first_pattern=len(s.sprite_patterns), first_palette=2, max_pieces=44, max_units=11)
    if sgx: s.patches = []
    return s

def sprite_patches(s, source, first_pattern, first_palette, max_pieces, max_units):
    """Cover the worst-quantized 16x16 blocks with sprites, which carry their own palettes.
    Chosen blocks are fully opaque, so the BG beneath them is hidden."""
    src = np.asarray(source.convert('RGB'))
    src = vce_rgb(vce_colors(src)).astype(np.int32)
    rgb = np.zeros((H, W, 3), np.int32)
    for cy in range(ROWS):
        for cx in range(COLS):
            rgb[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8] = vce_rgb(s.palettes[s.pal[cy, cx]])[s.index[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8]]
    err = ((src - rgb) ** 2).sum(-1)
    blocks = []
    for by in range(0, H, 16):
        for bx in range(0, W, 16):
            blocks.append((int(err[by:by + 16, bx:bx + 16].sum()), bx, by))
    blocks.sort(reverse=True)
    units = np.zeros(H, np.int32)
    chosen = []
    for e, bx, by in blocks:
        if len(chosen) >= max_pieces or e == 0: break
        if units[by:by + 16].max() >= max_units: continue
        units[by:by + 16] += 1
        chosen.append((bx, by))
    palettes = list(range(first_palette, 16))
    means = np.array([src[by:by + 16, bx:bx + 16].reshape(-1, 3).mean(0) for bx, by in chosen])
    groups = kmeans_groups(means, len(palettes))
    for gi, slot in enumerate(palettes):
        members = [c for c, g in zip(chosen, groups) if g == gi]
        if not members: continue
        px = np.concatenate([src[by:by + 16, bx:bx + 16].reshape(-1, 3) for bx, by in members]).astype(np.uint8)
        img = Image.fromarray(np.concatenate((px, np.full((len(px), 1), 255, np.uint8)), 1).reshape(1, -1, 4))
        s.sprite_palettes[slot] = palette_for([img], 15)
    s.patches = []
    for (bx, by), g in zip(chosen, groups):
        cell = Image.fromarray(np.dstack((src[by:by + 16, bx:bx + 16].astype(np.uint8), np.full((16, 16), 255, np.uint8))))
        s.sprite_patterns.append(planar_sprite(indexed(cell, s.sprite_palettes[palettes[g]])))
        s.patches.append((bx, by, first_pattern + len(s.patches), palettes[g]))
    s.patch_error = (err.sum(), len(chosen))

# ---------------------------------------------------------------- hero select
PANEL_X = (0x10, 0x60, 0xa0, 0xe0)
PANEL_Y = 0x30
CS_FRAME = (0xC2EBBACE, 0x13D53116, 0xCDC8A9CC, 0x957325FD)
CS_NAME_ON = (0xB48828F4, 0x699DC4C3, 0xA9AB3BF0, 0xF6CBB2F4)
CS_PORT_ON = (0x67C9A3D9, 0x74100546, 0x72A6B0FB, 0xEEE2331F)
CS_PORT_OFF = (0x3459994C, 0xAA051172, 0x3F368A4E, 0x217B03F1)
SELECT_STATES = ('dim', 'on', 'glow')
HERO_PALETTE0 = RINGS            # BG slots 4..7: frame colours 1..5, silhouette 6
PORTRAIT_PALETTES = tuple(range(3, 16))      # sprite palettes for the selected portrait

def frame_colors(frame):
    """Up to five distinct frame colours, sorted by luminance; white border last-but-one."""
    a = np.asarray(frame)
    px = a[a[..., 3] >= 128][:, :3]
    u, c = np.unique(px, axis=0, return_counts=True)
    top = u[np.argsort(-c)[:5]]
    return sorted((tuple(int(v) for v in t) for t in top), key=lambda t: sum(t))

def hero_state_palette(colors, state):
    pal = np.zeros(16, '<u2')
    for i, c in enumerate(colors):
        v = np.array(c, float)
        if state == 'dim': v = v * 0.42
        elif state == 'glow' and sum(c) > 700: v = np.array((255, 215, 60.0))
        pal[1 + i] = word(vc(np.clip(v, 0, 255)))
    pal[6] = word((0, 0, 0))
    return pal

def select_screen(get):
    s = Screen()
    tunnel(s, RING_BRIGHT_FULL)
    static_text(s, get(0x4813ED48), 0x20, 0x10)              # CHARACTER SELECT
    states = {st: [] for st in SELECT_STATES}
    portraits = []
    sprites = []
    sprite_pieces = []
    for hero in range(4):
        px = PANEL_X[hero]
        frame = get(CS_FRAME[hero]); port = get(CS_PORT_ON[hero])
        fa = np.zeros((160, 80, 4), np.uint8); f0 = np.asarray(frame); fa[:f0.shape[0], :f0.shape[1]] = f0
        pa = np.zeros((160, 80, 4), np.uint8); p0 = np.asarray(port); pa[:p0.shape[0], :p0.shape[1]] = p0
        colors = frame_colors(frame)
        lattice = np.asarray(colors, np.int32)
        mask_frame = fa[..., 3] >= 128
        mask_port = pa[..., 3] >= 128
        off = get(CS_PORT_OFF[hero]); oa = np.zeros((160, 80, 4), np.uint8); o0 = np.asarray(off); oa[:o0.shape[0], :o0.shape[1]] = o0
        mask_off = oa[..., 3] >= 128
        cls = np.zeros((160, 80), np.uint8)
        d = ((fa[mask_frame][:, None, :3].astype(np.int32) - lattice[None]) ** 2).sum(-1)
        cls[mask_frame] = d.argmin(1).astype(np.uint8) + 1
        cls[mask_off] = 6                                    # silhouette; the selected hero's sprites cover it
        slot = HERO_PALETTE0 + hero
        for y in range(160 // 8):
            for x in range(80 // 8):
                tile = cls[y * 8:y * 8 + 8, x * 8:x * 8 + 8]
                if not tile.any(): continue
                gy, gx = (PANEL_Y // 8) + y, px // 8 + x
                s.index[gy * 8:gy * 8 + 8, gx * 8:gx * 8 + 8] = tile
                s.pal[gy, gx] = slot
        for st in SELECT_STATES: states[st].append(hero_state_palette(colors, st))
        s.palettes[slot] = states['on'][-1]
        # Selected portrait: 16x16 sprite pieces, each with a free palette.
        pieces = []
        for cy in range(10):
            for cx in range(5):
                if mask_port[cy * 16:cy * 16 + 16, cx * 16:cx * 16 + 16].sum() >= 4: pieces.append((cx, cy))
        means = np.array([pa[cy * 16:cy * 16 + 16, cx * 16:cx * 16 + 16][mask_port[cy * 16:cy * 16 + 16, cx * 16:cx * 16 + 16]][:, :3].mean(0) for cx, cy in pieces])
        groups = kmeans_groups(means, len(PORTRAIT_PALETTES))
        pals = np.zeros((16, 16), '<u2')
        for gi, slot_s in enumerate(PORTRAIT_PALETTES):
            members = [p for p, g in zip(pieces, groups) if g == gi]
            if not members: continue
            px_ = np.concatenate([pa[cy * 16:cy * 16 + 16, cx * 16:cx * 16 + 16][mask_port[cy * 16:cy * 16 + 16, cx * 16:cx * 16 + 16]][:, :3] for cx, cy in members])
            img = Image.fromarray(np.concatenate((px_, np.full((len(px_), 1), 255, np.uint8)), 1).reshape(1, -1, 4))
            pals[slot_s] = palette_for([img], 15)
        pats, table = [], []
        for (cx, cy), g in zip(pieces, groups):
            cell = Image.fromarray(np.where(mask_port[cy * 16:cy * 16 + 16, cx * 16:cx * 16 + 16, None], pa[cy * 16:cy * 16 + 16, cx * 16:cx * 16 + 16], 0).astype(np.uint8))
            idx = indexed(cell, pals[PORTRAIT_PALETTES[g]])
            pats.append(planar_sprite(idx))
            table.append((cx * 16, cy * 16, len(pats) - 1, PORTRAIT_PALETTES[g]))
        portraits.append(dict(patterns=pats, palettes=pals, pieces=table))
    s.state_palettes = [np.asarray(states[st], '<u2') for st in SELECT_STATES]
    s.portraits = portraits
    # Names (on/off palettes) and arrows: resident sprites.
    info = {}
    for hero in range(4):
        pats, w16, h16 = two_colour_sprites(get(CS_NAME_ON[hero]), None)
        if len(sprites) % 2: sprites.append(bytes(128))          # a 32-wide sprite starts on an even pattern
        info[f'name{hero}'] = (len(sprites), w16); sprites += pats
    for key, rid in (('left', 0xAE16B01D), ('right', 0xE34D3083)):
        im = get(rid); box = im.getchannel('A').getbbox(); im = im.crop(box)
        a = np.asarray(im)
        idx = np.zeros(a.shape[:2], np.uint8)
        lum = a[..., :3].astype(int)
        idx[(a[..., 3] > 0) & (lum[..., 0] > 150) & (lum[..., 1] < 100)] = 1
        idx[(a[..., 3] > 0) & (lum.sum(-1) > 600)] = 2
        idx[(a[..., 3] > 0) & (idx == 0)] = 3
        idx = np.pad(idx, ((0, -idx.shape[0] % 16), (0, -idx.shape[1] % 16)))
        pats = [planar_sprite(idx[y:y + 16, x:x + 16]) for y in range(0, idx.shape[0], 16) for x in range(0, idx.shape[1], 16)]
        info[key] = (len(sprites), idx.shape[1] // 16, idx.shape[0] // 16, box); sprites += pats
    s.sprite_patterns = sprites
    s.info = info
    sp = s.sprite_palettes
    sp[0][1:3] = [word((255, 255, 255)), word((16, 16, 8))]
    sp[1][1:3] = [word((118, 118, 118)), word((16, 16, 8))]
    sp[2][1:4] = [word((206, 0, 0)), word((239, 239, 239)), word((0, 0, 0))]
    return s

# ---------------------------------------------------------------- options / credits panel
TEXT_SLOTS = (12, 13, 14, 15)       # white, gold, grey, cyan
PANEL_RGB = (14, 18, 52)
PANEL_BOX = (3, 6, 34, 20)           # first column, first row, columns, rows
TEXT_INK = ((255, 255, 255), (255, 190, 20), (150, 150, 165), (120, 225, 255))

def font_tiles(get):
    """95 BG characters: blank, then glyphs for 0x21..0x7e (panel = 1, ink = 2, outline = 3, shade = 4)."""
    tiles = [planar_tile(np.ones((8, 8), np.uint8))]
    for n in range(94):
        a = np.asarray(get(0x4058897F, n).convert('RGBA'))[:8, :8]
        lum = a[..., :3].astype(int).sum(-1)
        idx = np.where(a[..., 3] < 128, 1, np.where(lum < 150, 3, np.where(lum > 690, 2, 4))).astype(np.uint8)
        full = np.ones((8, 8), np.uint8); full[:idx.shape[0], :idx.shape[1]] = idx
        tiles.append(planar_tile(full))
    return tiles

def options_screen(get, header=True):
    s = Screen()
    tunnel(s, RING_BRIGHT_DIM)
    if header: static_text(s, get(0xF8F9017C), 80, 0x10)     # OPTION MODE header art
    x0, y0, w, h = PANEL_BOX
    for r in range(h):
        for c in range(w):
            tile = np.ones((8, 8), np.uint8)
            if r == 0: tile[0:2, :] = (5, 5, 5, 5, 5, 5, 5, 5)[0]; tile[2, :] = 6
            if r == h - 1: tile[6:8, :] = 5; tile[5, :] = 6
            if c == 0: tile[:, 0:2] = 5; tile[:, 2] = np.where(tile[:, 2] == 1, 6, tile[:, 2])
            if c == w - 1: tile[:, 6:8] = 5; tile[:, 5] = np.where(tile[:, 5] == 1, 6, tile[:, 5])
            gy, gx = y0 + r, x0 + c
            s.index[gy * 8:gy * 8 + 8, gx * 8:gx * 8 + 8] = tile
            s.pal[gy, gx] = TEXT_SLOTS[0]
    for k, slot in enumerate(TEXT_SLOTS):
        pal = s.palettes[slot]
        ink = TEXT_INK[k]
        pal[1] = word(PANEL_RGB)
        pal[2] = word(ink)
        pal[3] = word(tuple(int(v * 0.12) for v in ink))
        pal[4] = word(tuple(int(v * 0.62) for v in ink))
        pal[5] = word((176, 186, 240))
        pal[6] = word((52, 62, 128))
    s.font = font_tiles(get)
    return s

# ---------------------------------------------------------------- game over
GAMEOVER_ART, GAMEOVER_TEXT = 0x64981FC5, 0x24138418       # the Nemesis painting and GAME OVER lettering, as on the Saturn
GAMEOVER_TEXT_Y = 168
GAMEOVER_GLOW_SLOTS = (12, 13, 14, 15)    # BG palettes of the cells under the lettering
GAMEOVER_PULSE = (1.0, 0.84, 0.67, 0.51)  # glow strength per pulse step (step 0 = the palettes as loaded)
GAMEOVER_GLOW = (0, 0.28, 0.62, 1.0)      # additive light of each text level (0 = none)

def gameover_screen(get, artwork=None):
    """The supplied painting filling the 320x224 screen with the lettering added on top as light."""
    art = (artwork if artwork is not None else get(GAMEOVER_ART)).convert('RGBA')
    w = round(art.width * H / art.height)
    canvas = art.resize((w, H), Image.Resampling.LANCZOS).crop(((w - W) // 2, 0, (w - W) // 2 + W, H)).convert('RGBA')
    text = get(GAMEOVER_TEXT).convert('RGBA')
    return glow_screen(canvas, [(text, (W - text.width) // 2, GAMEOVER_TEXT_Y)])

def victory_screen(canvas, layers):
    """A 320x224 painting with opaque lettering on top: the painting uses all sixteen BG palettes and the lettering is
    16x16 sprites (one shared sprite palette), which sit over the background with no blending. `layers` are
    (RGBA lettering, x, y, 'top'|'bottom'): the two 16-line sprite rows start at the glyph top, or end at its
    bottom, so the rows of the stacked lines of text never share a scanline (16 sprites per line at most)."""
    s = Screen()
    s.preview_source = canvas
    paint_cells(s, canvas, list(range(16)))
    cuts = []
    for image, tx, ty, align in layers:
        a = np.asarray(image.convert('RGBA'))
        ys, xs = np.nonzero(a[..., 3] >= 128)
        x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
        top = ty + y0
        start = top if align == 'top' else top + (y1 - y0) - 32
        for sy in (start, start + 16):
            for sx in range(tx + x0, tx + x1, 16):
                piece = Image.new('RGBA', (16, 16))
                for y in range(16):
                    for x in range(16):
                        px, py = sx + x - tx, sy + y - ty
                        if 0 <= px < a.shape[1] and 0 <= py < a.shape[0] and a[py, px, 3] >= 128:
                            piece.putpixel((x, y), tuple(int(v) for v in a[py, px]))
                if piece.getbbox(): cuts.append((sx, sy, piece))
    pal = palette_for([c[2] for c in cuts], 15)
    s.sprite_palettes[0] = pal
    s.pieces = []
    for sx, sy, piece in cuts:
        s.pieces.append((sx, sy, len(s.sprite_patterns), 0))
        s.sprite_patterns.append(planar_sprite(indexed(piece, pal)))
    # preview: the sprites over the background
    prev = np.zeros((H, W, 3), np.uint8)
    for cy in range(ROWS):
        for cx in range(COLS):
            prev[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8] = vce_rgb(s.palettes[s.pal[cy, cx]])[s.index[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8]]
    for (sx, sy, piece) in cuts:
        arr = np.asarray(piece)
        idx = indexed(piece, pal)
        for y in range(16):
            for x in range(16):
                if idx[y, x] and 0 <= sy + y < H and 0 <= sx + x < W: prev[sy + y, sx + x] = vce_rgb(pal)[idx[y, x]]
    s.sprite_preview = prev
    return s

def sgx_paired_background(image, archive, name, previews, glow=None):
    """Encode one static painting across the two SGX BG planes.

    Each 8x8 tile pair chooses two palettes from the shared VCE allocation.
    Every opaque pixel is assigned to exactly one plane; palette index zero
    remains transparent so the other VDC can show through. This is a fixed
    4bpp-per-plane image, not an 8bpp tile mode.
    """
    palette_count = 12 if glow is not None else 16
    rgba = np.asarray(image.convert('RGBA'))
    if rgba.shape[:2] != (H, W):
        raise ValueError(f'{name}: paired BG must be {W}x{H}, got {rgba.shape[1]}x{rgba.shape[0]}')
    encoded = rgba.copy()
    if name == 'title':
        encoded[..., :3] = dithered(rgba[..., :3].astype(np.float32),0.15)
    cells = encoded.reshape(ROWS, 8, COLS, 8, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 8, 8, 4)
    codes = vce_colors(cells[..., :3]).astype(np.int64).reshape(-1, 64)
    opaque = (cells[..., 3].reshape(-1, 64) >= 128)

    # Fit a common 16 x 15 color pool, then iteratively refit it to pixels
    # selected between the two best palettes for each aligned tile pair.
    palettes, _ = palfit.fit_palettes(cells, palette_count, iterations=6, verbose=False)
    safe_codes = np.where(opaque, codes, 0)

    def error_tables(pals):
        return np.asarray([palfit.error_table((p[1:] & 0x1ff).astype(np.int64)) for p in pals], np.float32)

    def pair_costs(tables):
        cost = np.empty((len(codes), palette_count), np.float32)
        for p in range(palette_count):
            cost[:, p] = (tables[p][safe_codes] * opaque).sum(1)
        return cost

    tables = error_tables(palettes)
    pairs = np.argsort(pair_costs(tables), axis=1)[:, :2].astype(np.uint8)
    for _ in range(4):
        g0, g1 = pairs[:, 0], pairs[:, 1]
        d0 = tables[g0[:, None], safe_codes]
        d1 = tables[g1[:, None], safe_codes]
        choose0 = d0 <= d1
        group0 = np.broadcast_to(g0[:, None], codes.shape).astype(np.int64)
        group1 = np.broadcast_to(g1[:, None], codes.shape).astype(np.int64)
        hist = np.bincount(
            (group0[choose0 & opaque] * 512 + codes[choose0 & opaque]),
            minlength=palette_count * 512,
        )
        hist += np.bincount(
            (group1[~choose0 & opaque] * 512 + codes[~choose0 & opaque]),
            minlength=palette_count * 512,
        )
        hist = hist.reshape(palette_count, 512)
        fitted = np.zeros((palette_count, 16), '<u2')
        for p in range(palette_count):
            colors = palfit.fit_colors(hist[p], 15, iterations=6)
            if len(colors):
                fitted[p, 1:] = int(colors[0])
                fitted[p, 1:len(colors) + 1] = colors
        palettes = fitted
        tables = error_tables(palettes)
        pairs = np.argsort(pair_costs(tables), axis=1)[:, :2].astype(np.uint8)

    g0, g1 = pairs[:, 0], pairs[:, 1]
    d0, d1 = tables[g0[:, None], safe_codes], tables[g1[:, None], safe_codes]
    choose0 = d0 <= d1
    indexes = np.empty((palette_count, 512), np.uint8)
    lattice = palfit.LATTICE
    for p in range(palette_count):
        colors = (palettes[p, 1:] & 0x1ff).astype(np.int64)
        indexes[p] = ((lattice[:, None, :] - lattice[colors][None, :, :]) ** 2).sum(-1).argmin(1) + 1

    plane_pixels = [np.zeros((H, W), np.uint8), np.zeros((H, W), np.uint8)]
    plane_groups = [g0, g1]
    rendered = np.zeros((H, W, 3), np.uint16)
    actual_colors = set()
    total_error = 0.0
    opaque_count = int(opaque.sum())
    for cell in range(len(cells)):
        cy, cx = divmod(cell, COLS)
        m0 = (choose0[cell] & opaque[cell]).reshape(8, 8)
        m1 = (~choose0[cell] & opaque[cell]).reshape(8, 8)
        for layer, mask in ((0, m0), (1, m1)):
            idx = np.where(mask, indexes[int(plane_groups[layer][cell])][codes[cell]].reshape(8, 8), 0).astype(np.uint8)
            plane_pixels[layer][cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8] = idx
            pal = palettes[int(plane_groups[layer][cell])]
            if mask.any():
                actual_colors.update(int(c) for c in (pal[idx[mask]] & 0x1ff))
                rendered[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8][mask] = vce_rgb(pal)[idx[mask]]
        total_error += float(np.minimum(d0[cell], d1[cell])[opaque[cell]].sum())

    if glow is not None:
        # Keep the additive bitplane letters and their four pulse palettes.
        # Both BATs clear the underlying cell; VDC0 supplies its painting+ink.
        palettes = np.concatenate((palettes, glow.palettes[12:16]))

    def emit_plane(layer):
        pixels = plane_pixels[layer]
        tiles = [planar_tile(np.zeros((8, 8), np.uint8))]
        lookup = {tiles[0]: 0}
        words = np.empty((ROWS, COLS), '<u2')
        groups = plane_groups[layer]
        for cy in range(ROWS):
            for cx in range(COLS):
                idx = pixels[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8]
                group = int(groups[cy * COLS + cx])
                if glow is not None and int(glow.pal[cy, cx]) >= 12:
                    idx = glow.index[cy*8:cy*8+8, cx*8:cx*8+8] if layer == 0 else np.zeros((8,8),np.uint8)
                    group = int(glow.pal[cy, cx])
                key = planar_tile(idx)
                tile = lookup.get(key)
                if tile is None:
                    tile = len(tiles)
                    lookup[key] = tile
                    tiles.append(key)
                words[cy, cx] = (group << 12) | ((UI_TILE_WORD >> 4) + tile)
        limit = (UI_SPRITE_WORD - UI_TILE_WORD) // 16
        if len(tiles) > limit:
            raise ValueError(f'{name}: paired BG{layer} needs {len(tiles)} tiles; limit is {limit}')
        tile_offset = archive.add(f'sgx_{name}_bg{layer}_tiles', b''.join(tiles))
        map_offset = archive.add(f'sgx_{name}_bg{layer}_map', words.tobytes())
        return tile_offset, map_offset, len(tiles)

    tiles0, map0, count0 = emit_plane(0)
    tiles1, map1, count1 = emit_plane(1)
    palette_offset = archive.add(f'sgx_{name}_bg_palette', palettes.astype('<u2').tobytes())
    preview = np.clip(rendered, 0, 255).astype(np.uint8)
    source_rgb = rgba[..., :3].astype(np.float64)
    rgb_error = float(((source_rgb-preview.astype(np.float64))**2)[rgba[...,3]>=128].mean())
    image.save(previews / f'sgx_{name}_source.png')
    for label, art in (('source',rgba[..., :3]),('pair',preview)):
        Image.fromarray(art[32:176,8:312]).save(previews / f'sgx_{name}_{label}_detail.png')
    Image.fromarray(preview).resize((W * 3, H * 3), Image.Resampling.NEAREST).save(previews / f'sgx_{name}_paired.png')
    return dict(sgx_palette=palette_offset, sgx_tiles0=tiles0, sgx_map0=map0,
                sgx_tiles1=tiles1, sgx_map1=map1, sgx_ntiles0=count0,
                sgx_ntiles1=count1, sgx_colors=len(actual_colors),
                sgx_mse=total_error / max(opaque_count, 1), sgx_rgb_mse=rgb_error,
                sgx_source_colors=len(np.unique(rgba[..., :3].reshape(-1,3),axis=0)),
                sgx_plane_coverage=[int(np.count_nonzero(px)) for px in plane_pixels])

def glow_screen(canvas, layers, dim=1.0):
    """A 320x224 painting with lettering added on top as light.

    The translucency is done with the bit planes (PCE_ISSUES / Transparency notes): the cells under the lettering
    keep the painting in the low two bit planes (black + 3 colours of its own) and the lettering sits in the high
    two. Each of their palettes has four sets of four colours: the painting's, and the same three colours plus the
    light of text level 1, 2 and 3 - so the hardware's index OR is an additive blend, with no masking. Pulsing the
    lettering is rewriting the tail of those few palettes (`s.glow` holds every step). `layers` are (RGBA lettering,
    x, y); `dim` darkens the painting under the lettering so that the added light keeps its contrast."""
    s = Screen()
    s.preview_source = canvas
    # text levels: ink 3, light edge 2, dark outline 1 (a faint halo)
    level = np.zeros((H, W), np.uint8)
    for image, tx, ty in layers:
        text = np.asarray(image.convert('RGBA')).astype(int)
        th, tw = text.shape[:2]
        lum = text[..., :3].sum(-1)
        lv = np.where(text[..., 3] < 128, 0, np.where(lum > 700, 3, np.where(lum > 450, 2, 1)))
        level[ty:ty + th, tx:tx + tw] = np.maximum(level[ty:ty + th, tx:tx + tw], lv)
    cell_has_text = level.reshape(ROWS, 8, COLS, 8).max(axis=(1, 3)) > 0
    text_cells = [cy * COLS + cx for cy, cx in zip(*np.nonzero(cell_has_text))]
    free_slots = [k for k in range(16) if k not in GAMEOVER_GLOW_SLOTS]
    paint_cells(s, canvas, free_slots, skip=set(text_cells))
    # the painting under the lettering: black + three colours per palette, four palettes
    rgb = (np.asarray(canvas)[..., :3] * dim).astype(np.uint8)
    means = np.array([rgb[(c // COLS) * 8:(c // COLS) * 8 + 8, (c % COLS) * 8:(c % COLS) * 8 + 8].reshape(-1, 3).mean(0) for c in text_cells])
    groups = kmeans_groups(means, len(GAMEOVER_GLOW_SLOTS))
    bases = []
    for gi, slot in enumerate(GAMEOVER_GLOW_SLOTS):
        members = [c for c, g in zip(text_cells, groups) if g == gi]
        px = np.concatenate([rgb[(c // COLS) * 8:(c // COLS) * 8 + 8, (c % COLS) * 8:(c % COLS) * 8 + 8].reshape(-1, 3) for c in members]) if members else np.zeros((1, 3), np.uint8)
        px = px[px.sum(1) > 60]                       # colour 0 is the black backdrop
        q = Image.fromarray(vce_rgb(vce_colors(px)).astype(np.uint8).reshape(1, -1, 3)).quantize(colors=3, method=Image.Quantize.MEDIANCUT) if len(px) else None
        base = [(0, 0, 0)] + ([tuple(int(v) for v in c) for c in np.asarray(q.getpalette(), np.uint8).reshape(-1, 3)[:3]] if q else [])
        base += [(0, 0, 0)] * (4 - len(base))
        bases.append(np.array(base, int))
        lut = np.array(base, int)
        for c in members:
            cy, cx = divmod(c, COLS)
            tile = rgb[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8].astype(int)
            near = ((tile[:, :, None, :] - lut[None, None]) ** 2).sum(-1).argmin(-1)
            s.index[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8] = level[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8] * 4 + near
            s.pal[cy, cx] = slot
    def glow_palette(base, strength):
        pal = np.zeros(16, '<u2')
        for t in range(4):
            for b in range(4):
                if t == 0 and b == 0: continue            # the backdrop
                pal[4 * t + b] = word(np.clip(base[b] + GAMEOVER_GLOW[t] * strength * 255, 0, 255))
        return pal
    for gi, slot in enumerate(GAMEOVER_GLOW_SLOTS): s.palettes[slot] = glow_palette(bases[gi], GAMEOVER_PULSE[0])
    s.glow = b''.join(glow_palette(bases[gi], strength).tobytes() for strength in GAMEOVER_PULSE for gi in range(len(GAMEOVER_GLOW_SLOTS)))
    return s

# ---------------------------------------------------------------- credits text
CREDITS_PREFIX = ['MAIN PROGRAMMER\nGameblabla', 'ADDITIONAL GRAPHICS\nGameblabla']

def credits_pages(work):
    import re, unicodedata
    raw = (work / '7E11BC19.levl').read_bytes().decode('utf-8', 'replace')
    pages = list(CREDITS_PREFIX)
    def clean(t):
        t = re.sub(r'<[^>]*>', '', t)
        t = unicodedata.normalize('NFKD', t).encode('ascii', 'ignore').decode()
        return [fit(l.strip()) for l in t.splitlines() if l.strip()]
    def fit(l):   # the panel's text area is 32 columns: a longer line is shortened (no bracketed note, no 'awesome') instead of running out of the box
        if len(l) > 32: l = re.sub(r'\s*\([^)]*\)', '', l)
        if len(l) > 32: l = re.sub(r'(?i)\bawesome ', '', l)
        if len(l) > 32: l = l[:33].rsplit(' ', 1)[0]
        return l
    # [fade] splits pages; a [roll] section is a long list of names.
    for chunk in re.split(r'\[fade\]', raw):
        if 'See YOUR NAME' in chunk: continue
        parts = re.split(r'\[roll\]', chunk)
        head = clean(parts[0])
        if head: pages.append('\n'.join(head))
        for roll in parts[1:]:
            names = clean(roll)
            page = []   # at most ten lines and 150 characters a page (the player reads 160 bytes of it)
            for n in names:
                if page and (len(page) == 10 or len('\n'.join(page + [n])) > 150): pages.append('\n'.join(page)); page = []
                page.append(n)
            if page: pages.append('\n'.join(page))
    texts = [p.upper().encode('ascii', 'replace') + b'\0' for p in pages]
    head = 2 + 2 * len(texts)
    offsets, pos = [], head
    for t in texts: offsets.append(pos); pos += len(t)
    return struct.pack('<H', len(texts)) + struct.pack(f'<{len(texts)}H', *offsets) + b''.join(texts), len(texts)

# ---------------------------------------------------------------- bake
def sgx_selection(get, archive, previews):
    """Six backdrop palettes, ten immutable foreground palettes, four resident maps.

    Two 64x32 BAT pages precede patterns at word $1000. Selection changes
    upload only an inactive map, then publish its Y scroll at VBlank.
    """
    spiral = get(0x92702CF3).convert('RGBA')
    spiral = spiral.resize((round(spiral.width*W/420), round(spiral.height*W/420)), Image.Resampling.LANCZOS)
    moon = get(0x2178AD91).convert('RGBA')
    def background(angle):
        bg = Image.new('RGBA', (W,H), (0,0,0,255))
        # PIL's positive angles turn anticlockwise, opposite the PC renderer.
        for art, degrees in ((spiral, angle), (moon, -angle)):
            rotated = art.rotate(degrees, Image.Resampling.BICUBIC, expand=True)
            bg.alpha_composite(rotated, ((W-rotated.width)//2,(H-rotated.height)//2))
        return bg
    backdrop = background(0)
    foregrounds = []
    for chosen in range(4):
        fg = Image.new('RGBA', (W,H))
        fg.alpha_composite(get(0x4813ED48).convert('RGBA'), (32,16))
        for hero in range(4):
            frame = get(CS_FRAME[hero]).convert('RGBA')
            if hero != chosen:
                a = np.asarray(frame).copy(); a[..., :3] = (a[..., :3].astype(np.uint16)*128//255).astype(np.uint8)
                frame = Image.fromarray(a)
            fg.alpha_composite(frame, (PANEL_X[hero], PANEL_Y))
            fg.alpha_composite(get((CS_PORT_ON if hero==chosen else CS_PORT_OFF)[hero]).convert('RGBA'), (PANEL_X[hero], PANEL_Y))
        foregrounds.append(fg)
    def cells(image):
        return np.asarray(image).reshape(ROWS,8,COLS,8,4).transpose(0,2,1,3,4).reshape(-1,8,8,4)
    # Reuse Continue's resident palette-cycled tunnel. Full rotated bitmaps
    # required a 1.46 MB tail before selection could appear. Keep the complete
    # SGX portrait plane and its ten palettes unchanged.
    backdrop_screen = Screen()
    tunnel(backdrop_screen, RING_BRIGHT_FULL)
    bp = np.zeros((6,16), '<u2')
    bp[:4] = backdrop_screen.palettes[:4]
    fc = np.concatenate([cells(fg) for fg in foregrounds])
    fp,fg = palfit.fit_palettes(fc,10,verbose=False)
    palette = np.concatenate([bp,fp])
    def emit(cs, pals, groups, slot, name, tilebase=0x100):
        indexes = palfit.index_cells(cs,pals,groups)
        tiles = [bytes(32)]; lookup = {bytes(32):0}; maps = []
        rendered = []
        for start in range(0,len(cs),ROWS*COLS):
            words=[]; rgb=[]
            for i in range(start,start+ROWS*COLS):
                key=planar_tile(indexes[i])
                if key not in lookup: lookup[key]=len(tiles);tiles.append(key)
                words.append(((int(groups[i])+slot)<<12)|(tilebase+lookup[key]))
                rgb.append(vce_rgb(pals[groups[i]])[indexes[i]])
            maps.append(archive.add(f'sgx_select_{name}_map{len(maps)}',np.asarray(words,'<u2').tobytes()))
            rendered.append(np.asarray(rgb).reshape(ROWS,COLS,8,8,3).transpose(0,2,1,3,4).reshape(H,W,3).astype(np.uint8))
        if len(tiles)>1408: raise ValueError(f'SGX select {name}: {len(tiles)} exceeds resident VRAM')
        return archive.add(f'sgx_select_{name}_tiles',b''.join(tiles)),maps,len(tiles),rendered
    tiles=[bytes(32)]; lookup={bytes(32):0}; words=[]
    for cy in range(ROWS):
        for cx in range(COLS):
            key=planar_tile(backdrop_screen.index[cy*8:cy*8+8,cx*8:cx*8+8])
            if key not in lookup:lookup[key]=len(tiles);tiles.append(key)
            words.append((int(backdrop_screen.pal[cy,cx])<<12)|(0x100+lookup[key]))
    bt=archive.add('sgx_select_backdrop_tiles',b''.join(tiles))
    bm=[archive.add('sgx_select_backdrop_map',np.asarray(words,'<u2').tobytes())]
    bn=len(tiles)
    br=[vce_rgb(bp[backdrop_screen.pal.repeat(8,0).repeat(8,1),backdrop_screen.index]).astype(np.uint8)]
    def finish_animation(result):
        pass  # Everything is resident in the core UI extent.
    ft,fm,fn,fr=emit(fc,fp,fg,6,'foreground')
    for hero in range(4):
        mask=np.asarray(foregrounds[hero])[...,3]>=128
        preview=np.where(mask[...,None],fr[hero],br[0])
        Image.fromarray(preview).save(previews/f'sgx_select_{hero}.png')
        foregrounds[hero].save(previews/f'sgx_select_source_{hero}.png')
    Image.fromarray(br[0]).save(previews/'sgx_select_backdrop_source.png')
    phases=[]
    for shift in range(RAMP):
        phase=bp.copy()
        phase[:4,1:13]=np.roll(bp[:4,1:13],-shift,axis=1)
        phases.append(phase.astype('<u2').tobytes())
    cycle=archive.add('sgx_select_cycle',b''.join(phases))
    po=archive.add('sgx_select_palette',palette.astype('<u2').tobytes())
    return dict(palette=po,tiles0=ft,maps=fm,tiles1=bt,map1=bm[0],ntiles0=fn,ntiles1=bn,cycle=cycle,animation=0,animation_frames=1,finish_animation=finish_animation,
        report=dict(name='selection',bg0_tiles=fn,bg1_tiles=bn,backdrop_palettes=[0,6],foreground_palettes=[6,16],
                    map_switch_bytes=2240,pattern_switch_bytes=0,cycle_bytes=192,
                    rotation='Continue tunnel palette cycle, resident backdrop',
                    animation_frames=1,max_frame_patterns=bn,animation_bytes=0))

def bake(root, work, out, previews, cblock_frame, sgx=False):
    def get(rid, n=0):
        return cblock_frame(work / 'srgb' / f'{rid:08X}.srgb', n)
    archive = Archive()
    title = title_screen(get, sgx=sgx); select = select_screen(get); options = options_screen(get)
    panel = options_screen(get, header=False)
    gameover = gameover_screen(get, Image.open(root / 'assets/pce/gameover.png'))
    for name, scr in (('title', title), ('select', select), ('options', options), ('panel', panel),
                      ('gameover', gameover)):
        scr.preview(previews / f'ui_{name}.png')
    recs = [title.emit(archive, 'title'), select.emit(archive, 'select'), options.emit(archive, 'options', options.font),
            panel.emit(archive, 'panel', panel.font), gameover.emit(archive, 'gameover')]
    # Original per-stage/per-hero victory paintings (426x240): the centred 320x224 of each, lettering added as light like GAME OVER.
    victory = []
    sgx_report = []
    sgx_title_pair = None
    if sgx:
        sgx_title_pair = sgx_paired_background(title.preview_source, archive, 'title', previews)
        # Fit the painting into twelve palettes; the shared additive panel
        # owns the remaining four, so its runtime pulse works on both targets.
        sgx_gameover_pair=sgx_paired_background(gameover.preview_source,archive,'gameover',previews,glow=gameover)
        sgx_report.append(dict(name='gameover',colors=sgx_gameover_pair['sgx_colors'],rgb_mse=sgx_gameover_pair['sgx_rgb_mse']))
        sgx_select = sgx_selection(get, archive, previews)
        sgx_report.append(sgx_select['report'])
        baseline = title_screen(get)
        baseline.preview(previews/'sgx_title_pce_baseline.png')
        # Reconstruct its patches as well as the single-plane baseline.
        base = np.asarray(Image.open(previews/'sgx_title_pce_baseline.png').resize((W,H),Image.Resampling.NEAREST)).copy()
        for x,y,pat,slot in baseline.patches:
            data = baseline.sprite_patterns[pat]; px = np.zeros((16,16),np.uint8)
            for plane in range(4):
                for row in range(16):
                    bits=int.from_bytes(data[plane*32+row*2:plane*32+row*2+2],'little')
                    for col in range(16): px[row,col]|=((bits>>(15-col))&1)<<plane
            base[y:y+16,x:x+16]=vce_rgb(baseline.sprite_palettes[slot])[px]
        Image.fromarray(base).save(previews/'sgx_title_pce_final.png')
        source=np.asarray(title.preview_source.convert('RGB'),np.float64)
        sgx_report.append(dict(name='title', source_colors=sgx_title_pair['sgx_source_colors'],
                               rgb_mse=sgx_title_pair['sgx_rgb_mse'],
                               pce_final_rgb_mse=float(((source-base)**2).mean()),
                               pce_final_colors=len(np.unique(base.reshape(-1,3),axis=0)),
                               final_rgb_mse=sgx_title_pair['sgx_rgb_mse'], patches=0,
                               plane_coverage=sgx_title_pair['sgx_plane_coverage'], colors=sgx_title_pair['sgx_colors'],
                               mean_squared_error=sgx_title_pair['sgx_mse'],
                               bg0_tiles=sgx_title_pair['sgx_ntiles0'],
                               bg1_tiles=sgx_title_pair['sgx_ntiles1']))
    for stage in range(1,8):
        heroes=('saber','fireball','april','colt') if stage in (1,3,4,5) else (None,)
        for hero in heroes:
            name=f'stage{stage}'+(f'_{hero}' if hero else '')
            art=Image.open(root/'assets/victory'/f'{name}.png').convert('RGBA')
            x0,y0=(art.width-W)//2,(art.height-H)//2
            canvas=art.crop((x0,y0,x0+W,y0+H))
            layers=[]
            for rid,cy,align in ((0xF6172502,160,'bottom'),(0xF629241D,184,'top')):
                label=get(rid).convert('RGBA')
                layers.append((label,(W-label.width)//2,cy-label.height//2,align))
            scr=victory_screen(canvas,layers)
            Image.fromarray(scr.sprite_preview).resize((W*3,H*3),Image.NEAREST).save(previews/f'ui_victory_{name}.png')
            # each painting is its own small extent of victory.bin (about 45 KB): the disc read at a stage's end is not the whole UI
            own=Archive()
            rec=scr.emit(own,f'victory_{name}')
            if sgx:
                pair=sgx_paired_background(canvas,own,f'victory_{name}',previews)
                rec.update(pair)
                sgx_report.append(dict(name=name,colors=pair['sgx_colors'],mean_squared_error=pair['sgx_mse'],
                                       bg0_tiles=pair['sgx_ntiles0'],bg1_tiles=pair['sgx_ntiles1']))
            rec['extra']=own.add('ui_victory_extra',ramp_table(RING_BRIGHT_DIM)+struct.pack('<H',len(scr.pieces))+b''.join(struct.pack('<HHHH',*p) for p in scr.pieces))
            victory.append((rec,own.finish()))
    # Per-screen extra blobs.
    patches = b''.join(struct.pack('<HHHH', *p) for p in title.patches)
    recs[0]['extra'] = archive.add('ui_title_patches', struct.pack('<H', len(title.patches)) + patches)
    ramp = ramp_table(RING_BRIGHT_FULL)
    states = b''.join(np.asarray(st, '<u2').tobytes() for st in select.state_palettes)
    recs[1]['extra'] = archive.add('ui_select_extra', ramp + states)
    recs[2]['extra'] = archive.add('ui_options_extra', ramp_table(RING_BRIGHT_DIM))
    for rec in recs[3:]: rec['extra'] = recs[2]['extra']     # only the panel cycles; the rest just need something to read
    # a victory screen's extra blob is the ramp the loader reads, then the pulse palettes of its lettering
    recs += [rec for rec, _ in victory]
    victory_bin = b''.join(data for _, data in victory)
    (out / 'victory.bin').write_bytes(victory_bin)
    if sgx:
        (out / 'sgx_static_report.json').write_text(json.dumps({'edition':'SuperGrafx','paired_static_screens':sgx_report},indent=2)+'\n')
    sectors = np.cumsum([0] + [len(d) // 2048 for _, d in victory])[:-1]
    glow_off = archive.add('ui_gameover_glow', gameover.glow)
    portraits = []
    for hero, p in enumerate(select.portraits):
        pat = archive.add(f'ui_portrait{hero}_patterns', b''.join(p['patterns']))
        pal = archive.add(f'ui_portrait{hero}_palette', np.asarray(p['palettes'], '<u2').tobytes())
        table = archive.add(f'ui_portrait{hero}_pieces', b''.join(struct.pack('<HHHH', *t) for t in p['pieces']))
        portraits.append((pat, pal, table, len(p['pieces'])))
    credits, ncredits = credits_pages(work)
    credits_off = archive.add('ui_credits', credits)
    if sgx:
        core_bytes=archive.add('ui_animation_boundary',b'',alignment=16384)
        sgx_select['finish_animation'](sgx_select)
        (out / 'sgx_static_report.json').write_text(json.dumps({'edition':'SuperGrafx','paired_static_screens':sgx_report},indent=2)+'\n')
    data = archive.finish()
    (out / 'ui.bin').write_bytes(data)
    h = ['typedef struct { uint32_t pal, tiles, map, sprpal, sprpat, extra; uint16_t ntiles, nsprpat; } PceUiScreen;',
         'extern const PceUiScreen pce_ui[24];',
         '#define PCE_UI_VICTORY_BASE 5',
         'extern const uint16_t pce_victory_sector[19];','extern const uint32_t pce_victory_bytes[19];',
         'typedef struct { uint32_t patterns, palette, pieces; uint16_t count; } PceUiPortrait;',
         'extern const PceUiPortrait pce_ui_portrait[4];',
         f'#define PCE_UI_BYTES {len(data)}UL', f'#define PCE_UI_CREDITS {credits_off}UL',
         f'#define PCE_UI_CREDIT_PAGES {ncredits}',
         f'#define PCE_UI_TITLE_START {title.items["start"][0]}', f'#define PCE_UI_TITLE_START_W {title.items["start"][1]}',
         f'#define PCE_UI_TITLE_OPTION {title.items["option"][0]}', f'#define PCE_UI_TITLE_OPTION_W {title.items["option"][1]}',
         f'#define PCE_UI_GAMEOVER_GLOW {glow_off}UL', f'#define PCE_UI_GAMEOVER_SLOT {GAMEOVER_GLOW_SLOTS[0]}',
         f'#define PCE_UI_GAMEOVER_SLOTS {len(GAMEOVER_GLOW_SLOTS)}',
         f'#define PCE_UI_RAMP_BYTES {RINGS * RAMP * 2}',
         f'#define PCE_UI_STATE_BYTES {4 * PANEL_PALETTES_RUNTIME * 32}']
    if sgx:
        h += [f'#define PCE_UI_CORE_BYTES {core_bytes}UL',
              f'#define PCE_SGX_SELECT_ANIMATION {sgx_select["animation"]}UL',
              f'#define PCE_SGX_SELECT_FRAMES {sgx_select["animation_frames"]}',
              f'#define PCE_SGX_SELECT_INITIAL_TILES {sgx_select["tiles1"]}UL',
              f'#define PCE_SGX_SELECT_INITIAL_MAP {sgx_select["map1"]}UL',
              f'#define PCE_SGX_SELECT_INITIAL_COUNT {sgx_select["ntiles1"]}',
              f'#define PCE_SGX_SELECT_CYCLE {sgx_select["cycle"]}UL', 'typedef struct { uint32_t palette, tiles0, map0, tiles1, map1; uint16_t ntiles0, ntiles1, colors; } PceSgxUiPair;',
              'extern const PceSgxUiPair pce_sgx_ui_pair[21];',
              'typedef struct { uint32_t palette, tiles0, maps[4], tiles1, map1, cycle; uint16_t ntiles0, ntiles1; } PceSgxSelect;',
              'extern const PceSgxSelect pce_sgx_select;']
    names = [select.info[f'name{k}'] for k in range(4)]
    h.append('#define PCE_UI_NAME_PATTERNS {' + ','.join(str(n[0]) for n in names) + '}')
    h.append('#define PCE_UI_NAME_WIDTHS {' + ','.join(str(n[1]) for n in names) + '}')
    for key in ('left', 'right'):
        i = select.info[key]
        h.append(f'#define PCE_UI_ARROW_{key.upper()} {{{i[0]},{i[1]},{i[2]},{i[3][0]},{i[3][1]}}}')
    ui_rows=','.join('{%d,%d,%d,%d,%d,%d,%d,%d}' % (r['pal'], r['tiles'], r['map'], r['sprpal'], r['sprpat'], r['extra'], r['ntiles'], r['nsprpat']) for r in recs)
    c = ['const uint16_t pce_victory_sector[19]={' + ','.join(str(int(v)) for v in sectors) + '};',
         'const uint32_t pce_victory_bytes[19]={' + ','.join(str(len(d)) + 'UL' for _, d in victory) + '};',
         'const PceUiScreen pce_ui[24]={' + ui_rows + '};',
         'const PceUiPortrait pce_ui_portrait[4]={' + ','.join('{%d,%d,%d,%d}' % p for p in portraits) + '};']
    if sgx:
        r=sgx_select
        c.append('const PceSgxSelect pce_sgx_select __attribute__((section(".ram_bank124.rodata")))={%d,%d,{%s},%d,%d,%d,%d,%d};' % (r['palette'],r['tiles0'],','.join(map(str,r['maps'])),r['tiles1'],r['map1'],r['cycle'],r['ntiles0'],r['ntiles1']))
        pair_rows=','.join('{%d,%d,%d,%d,%d,%d,%d,%d}' % tuple(r[k] for k in
                           ('sgx_palette','sgx_tiles0','sgx_map0','sgx_tiles1','sgx_map1','sgx_ntiles0','sgx_ntiles1','sgx_colors'))
                           for r,_ in victory)
        title_row='{%d,%d,%d,%d,%d,%d,%d,%d}' % tuple(sgx_title_pair[k] for k in
                   ('sgx_palette','sgx_tiles0','sgx_map0','sgx_tiles1','sgx_map1','sgx_ntiles0','sgx_ntiles1','sgx_colors'))
        gameover_row='{%d,%d,%d,%d,%d,%d,%d,%d}' % tuple(sgx_gameover_pair[k] for k in
                   ('sgx_palette','sgx_tiles0','sgx_map0','sgx_tiles1','sgx_map1','sgx_ntiles0','sgx_ntiles1','sgx_colors'))
        c.append('const PceSgxUiPair pce_sgx_ui_pair[21] __attribute__((section(".ram_bank124.rodata")))={' + pair_rows + ',' + title_row + ',' + gameover_row + '};')
    return h, c, len(data)

PANEL_PALETTES_RUNTIME = 1      # one BG palette per hero (frame + silhouette)
