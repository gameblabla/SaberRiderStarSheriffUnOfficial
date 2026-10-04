"""PC Engine front end: title, options/credits, and hero selection screens.

All screens are 320x224 (40x28 BG characters). Art comes from the same source
assets as the Saturn menus. Animation is palette driven so it costs almost no
CPU: the tunnel backdrop cycles its palettes, and the hero panels switch
between selected/unselected palettes. Text is drawn at runtime with BG font
characters on flat panel colours, so no glyph ever sits on a black box.
"""
import math
import struct
import numpy as np
from PIL import Image
from formats import Archive, planar_tile, planar_sprite, vce_colors, vce_rgb, palette_for, indexed

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

def dithered(rgb):
    """Ordered-dither a float RGB picture onto the 9-bit lattice (repeating, so cheap in tiles)."""
    h, w = rgb.shape[:2]
    thr = np.tile(BAYER, (h // 4 + 1, w // 4 + 1))[:h, :w, None]
    level = np.floor(np.clip(rgb, 0, 255) / 255 * 7 + thr).clip(0, 7)
    return (level * 255 / 7).round().astype(np.uint8)

def title_backdrop():
    """The Saturn's blue field, rebuilt as a smooth glow so it quantizes cleanly."""
    yy, xx = np.mgrid[0:H, 0:W]
    d = np.hypot((xx - W / 2) / (W / 2), (yy - 100) / (H * 0.62)).clip(0, 1)
    inner, outer = np.array((10, 66, 130.0)), np.array((0, 18, 70.0))
    rgb = inner + (outer - inner) * (d ** 1.15)[..., None]
    img = np.dstack((dithered(rgb), np.full((H, W), 255, np.uint8)))
    return Image.fromarray(img)

def title_screen(get):
    s = Screen()
    canvas = title_backdrop()
    shadow = get(0x989121EC); sa = np.asarray(shadow).copy(); sa[..., 3] = (sa[..., 3].astype(np.uint16) * 80 // 255).astype(np.uint8)
    canvas.alpha_composite(Image.fromarray(sa), (8, 32))
    canvas.alpha_composite(get(0xB04BAC5F), (8, 32))
    paint_cells(s, canvas, list(range(16)))
    s.preview_source = canvas
    # Menu items are sprites so the highlight is a palette swap.
    start, opt = get(0x01E9B701), get(0x8FF0AB30)
    p0, n0, _ = two_colour_sprites(start, None)
    p1, n1, _ = two_colour_sprites(opt, None)
    s.sprite_patterns = p0 + p1
    ink_outline = lambda ink: [0, word(ink), word((0, 33, 66))]
    for slot, ink in enumerate(((255, 255, 255), (255, 182, 0))):
        s.sprite_palettes[slot][1:3] = ink_outline(ink)[1:]
    s.items = dict(start=(0, n0), option=(n0, n1))
    word_img = Image.new('RGBA', (80, 16))
    for i, ch in enumerate('CONTINUE'):
        g = get(0x4058897F, ord(ch) - 0x21)
        a = np.asarray(g.convert('RGBA')).copy()
        lum = a[..., :3].astype(int).sum(-1)
        a[..., :3] = np.where(lum[..., None] < 150, (0, 33, 66), (255, 255, 255))
        word_img.alpha_composite(Image.fromarray(a), (i * 8, 6))
    pc, nc, _ = two_colour_sprites(word_img, None)
    s.continue_pattern, s.continue_w = len(s.sprite_patterns) + 0, 5
    s.sprite_patterns += pc
    sprite_patches(s, canvas, first_pattern=len(s.sprite_patterns), first_palette=2, max_pieces=44, max_units=11)
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
    """The supplied painting filling the 320x224 screen with the lettering added on top as light.

    The translucency is done with the bit planes (PCE_ISSUES / Transparency notes): the cells under the lettering
    keep the painting in the low two bit planes (black + 3 colours of its own) and the lettering sits in the high
    two. Each of their palettes has four sets of four colours: the painting's, and the same three colours plus the
    light of text level 1, 2 and 3 - so the hardware's index OR is an additive blend, with no masking. Pulsing the
    lettering is rewriting the tail of those few palettes (`s.glow` holds every step)."""
    s = Screen()
    art = (artwork if artwork is not None else get(GAMEOVER_ART)).convert('RGBA')
    w = round(art.width * H / art.height)
    canvas = art.resize((w, H), Image.Resampling.LANCZOS).crop(((w - W) // 2, 0, (w - W) // 2 + W, H)).convert('RGBA')
    s.preview_source = canvas
    # text levels: ink 3, light edge 2, dark outline 1 (a faint halo), placed at the bottom centre
    text = np.asarray(get(GAMEOVER_TEXT).convert('RGBA')).astype(int)
    th, tw = text.shape[:2]
    tx, ty = (W - tw) // 2, GAMEOVER_TEXT_Y
    level = np.zeros((H, W), np.uint8)
    lum = text[..., :3].sum(-1)
    level[ty:ty + th, tx:tx + tw] = np.where(text[..., 3] < 128, 0, np.where(lum > 700, 3, np.where(lum > 450, 2, 1)))
    cell_has_text = level.reshape(ROWS, 8, COLS, 8).max(axis=(1, 3)) > 0
    text_cells = [cy * COLS + cx for cy, cx in zip(*np.nonzero(cell_has_text))]
    free_slots = [k for k in range(16) if k not in GAMEOVER_GLOW_SLOTS]
    paint_cells(s, canvas, free_slots, skip=set(text_cells))
    # the painting under the lettering: black + three colours per palette, four palettes
    rgb = np.asarray(canvas)[..., :3]
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
        return [l.strip() for l in t.splitlines() if l.strip()]
    # [fade] splits pages; a [roll] section is a long list of names.
    for chunk in re.split(r'\[fade\]', raw):
        if 'See YOUR NAME' in chunk: continue
        parts = re.split(r'\[roll\]', chunk)
        head = clean(parts[0])
        if head: pages.append('\n'.join(head))
        for roll in parts[1:]:
            names = clean(roll)
            for i in range(0, len(names), 10): pages.append('\n'.join(names[i:i + 10]))
    texts = [p.upper().encode('ascii', 'replace') + b'\0' for p in pages]
    head = 2 + 2 * len(texts)
    offsets, pos = [], head
    for t in texts: offsets.append(pos); pos += len(t)
    return struct.pack('<H', len(texts)) + struct.pack(f'<{len(texts)}H', *offsets) + b''.join(texts), len(texts)

# ---------------------------------------------------------------- bake
def bake(root, work, out, previews, cblock_frame):
    def get(rid, n=0):
        return cblock_frame(work / 'srgb' / f'{rid:08X}.srgb', n)
    archive = Archive()
    title = title_screen(get); select = select_screen(get); options = options_screen(get)
    panel = options_screen(get, header=False)
    gameover = gameover_screen(get, Image.open(root / 'assets/pce/gameover.png'))
    for name, scr in (('title', title), ('select', select), ('options', options), ('panel', panel),
                      ('gameover', gameover)):
        scr.preview(previews / f'ui_{name}.png')
    recs = [title.emit(archive, 'title'), select.emit(archive, 'select'), options.emit(archive, 'options', options.font),
            panel.emit(archive, 'panel', panel.font), gameover.emit(archive, 'gameover')]
    # Per-screen extra blobs.
    patches = b''.join(struct.pack('<HHHH', *p) for p in title.patches)
    recs[0]['extra'] = archive.add('ui_title_patches', struct.pack('<H', len(title.patches)) + patches)
    ramp = ramp_table(RING_BRIGHT_FULL)
    states = b''.join(np.asarray(st, '<u2').tobytes() for st in select.state_palettes)
    recs[1]['extra'] = archive.add('ui_select_extra', ramp + states)
    recs[2]['extra'] = archive.add('ui_options_extra', ramp_table(RING_BRIGHT_DIM))
    for rec in recs[3:]: rec['extra'] = recs[2]['extra']     # only the panel cycles; the rest just need something to read
    glow_off = archive.add('ui_gameover_glow', gameover.glow)
    portraits = []
    for hero, p in enumerate(select.portraits):
        pat = archive.add(f'ui_portrait{hero}_patterns', b''.join(p['patterns']))
        pal = archive.add(f'ui_portrait{hero}_palette', np.asarray(p['palettes'], '<u2').tobytes())
        table = archive.add(f'ui_portrait{hero}_pieces', b''.join(struct.pack('<HHHH', *t) for t in p['pieces']))
        portraits.append((pat, pal, table, len(p['pieces'])))
    credits, ncredits = credits_pages(work)
    credits_off = archive.add('ui_credits', credits)
    data = archive.finish()
    (out / 'ui.bin').write_bytes(data)
    h = ['typedef struct { uint32_t pal, tiles, map, sprpal, sprpat, extra; uint16_t ntiles, nsprpat; } PceUiScreen;',
         'extern const PceUiScreen pce_ui[5];',
         'typedef struct { uint32_t patterns, palette, pieces; uint16_t count; } PceUiPortrait;',
         'extern const PceUiPortrait pce_ui_portrait[4];',
         f'#define PCE_UI_BYTES {len(data)}UL', f'#define PCE_UI_CREDITS {credits_off}UL',
         f'#define PCE_UI_CREDIT_PAGES {ncredits}',
         f'#define PCE_UI_TITLE_START {title.items["start"][0]}', f'#define PCE_UI_TITLE_START_W {title.items["start"][1]}',
         f'#define PCE_UI_TITLE_OPTION {title.items["option"][0]}', f'#define PCE_UI_TITLE_OPTION_W {title.items["option"][1]}',
         f'#define PCE_UI_CONTINUE_PATTERN {title.continue_pattern}', f'#define PCE_UI_CONTINUE_W {title.continue_w}',
         f'#define PCE_UI_GAMEOVER_GLOW {glow_off}UL', f'#define PCE_UI_GAMEOVER_SLOT {GAMEOVER_GLOW_SLOTS[0]}',
         f'#define PCE_UI_GAMEOVER_SLOTS {len(GAMEOVER_GLOW_SLOTS)}',
         f'#define PCE_UI_RAMP_BYTES {RINGS * RAMP * 2}',
         f'#define PCE_UI_STATE_BYTES {4 * PANEL_PALETTES_RUNTIME * 32}']
    names = [select.info[f'name{k}'] for k in range(4)]
    h.append('#define PCE_UI_NAME_PATTERNS {' + ','.join(str(n[0]) for n in names) + '}')
    h.append('#define PCE_UI_NAME_WIDTHS {' + ','.join(str(n[1]) for n in names) + '}')
    for key in ('left', 'right'):
        i = select.info[key]
        h.append(f'#define PCE_UI_ARROW_{key.upper()} {{{i[0]},{i[1]},{i[2]},{i[3][0]},{i[3][1]}}}')
    c = ['const PceUiScreen pce_ui[5]={' + ','.join('{%d,%d,%d,%d,%d,%d,%d,%d}' % (r['pal'], r['tiles'], r['map'], r['sprpal'], r['sprpat'], r['extra'], r['ntiles'], r['nsprpat']) for r in recs) + '};',
         'const PceUiPortrait pce_ui_portrait[4]={' + ','.join('{%d,%d,%d,%d}' % p for p in portraits) + '};']
    return h, c, len(data)

PANEL_PALETTES_RUNTIME = 1      # one BG palette per hero (frame + silhouette)
