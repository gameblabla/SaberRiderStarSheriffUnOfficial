"""HUD art for the stages that have no sprite HUD of their own: the race, the Ramrod cockpit and the space flight.

Everything is rendered at build time with the source game's own fonts (small 12072E60, big 4058897F: 8x8 frames, frame =
character - 0x21) as RGBA images; the runtime only places pre-made sprite pieces, so no text is drawn on background
cells (those left black boxes on the art). `Hud` collects sprites and remembers each name's offset from its base id;
`macros` writes the offsets for the C side.
"""
import numpy as np
from PIL import Image, ImageDraw

SMALL, BIG = '12072E60', '4058897F'

class Fonts:
    def __init__(self, work, cblock_frame):
        self.work, self.frame, self.cache = work, cblock_frame, {}
    def glyph(self, ch, big=False):
        key = (ch, big)
        if key not in self.cache:
            n = ord(ch) - 0x21
            if ch == ' ' or n < 0 or n > 105: im = Image.new('RGBA', (8, 8))
            else: im = self.frame(self.work / 'srgb' / f'{BIG if big else SMALL}.srgb', n).convert('RGBA')
            self.cache[key] = im
        return self.cache[key]
    def advance(self, ch, big=False):
        if ch == ' ': return 4
        a = np.asarray(self.glyph(ch, big))[..., 3] > 0
        cols = np.nonzero(a.any(0))[0]
        return int(cols.max()) + 2 if len(cols) else 4
    def text(self, s, color=(255, 255, 255), big=False, shadow=False):
        """A line of text as an RGBA image (ink = `color`, optionally with a black drop shadow)."""
        w = sum(self.advance(c, big) for c in s) + 1
        im = Image.new('RGBA', (w + 1, 10))
        x = 0
        for c in s:
            g = np.asarray(self.glyph(c, big)).copy()
            ink = g[..., 3] > 0
            lit = np.zeros_like(g); lit[ink] = (*color, 255)
            gi = Image.fromarray(lit)
            if shadow:
                dark = np.zeros_like(g); dark[ink] = (0, 0, 0, 255)
                im.alpha_composite(Image.fromarray(dark), (x + 1, 1))
            im.alpha_composite(gi, (x, 0))
            x += self.advance(c, big)
        return im.crop(im.getbbox()) if im.getbbox() else im

def piece(im, x=0, y=0, size=16):
    """Place an image on a transparent size x size canvas."""
    out = Image.new('RGBA', (size, size)); out.alpha_composite(im, (x, y)); return out

class Hud:
    """The sprites a stage's HUD adds, in order; `offsets[name]` is the id relative to the first of them."""
    def __init__(self, sprites):
        self.sprites, self.base, self.offsets, self.widths = sprites, len(sprites), {}, {}
    def add(self, name, im, anchor=(0, 0)):
        self.offsets[name] = len(self.sprites) - self.base
        self.sprites.append(('hudp_' + name, im, anchor))   # the build gives every 'hudp_' sprite of a stage one palette
    def macros(self, prefix):
        out = [f'#define PCE_{prefix}_{n.upper()} {o}' for n, o in self.offsets.items()]
        out += [f'#define PCE_{prefix}_{n.upper()}_W {w}' for n, w in self.widths.items()]
        return out

# ------------------------------------------------------------------ shared parts
BAR_COLORS = {'green': (90, 220, 60), 'yellow': (240, 190, 60), 'red': (250, 70, 60), 'orange': (255, 150, 40),
              'blue': (90, 170, 255), 'white': (255, 255, 255), 'gold': (255, 182, 0), 'cyan': (150, 200, 255), 'pink': (255, 120, 120)}

def add_digits(hud, fonts, colors, prefix='digit'):
    """Digits 0-9, one 8x8 glyph at the top-left of a 16x16 piece, per colour name."""
    for cname in colors:
        for d in range(10):
            hud.add(f'{prefix}_{cname}_{d}', piece(fonts.text(str(d), BAR_COLORS[cname]), 0, 0))

def add_bar_fills(hud, colors, height=5, width=16):
    """Bar fill pieces: `k` px of a `width` px wide, `height` px tall bar (k = 1..width), per colour name."""
    for cname in colors:
        c = BAR_COLORS[cname]
        for k in range(1, width + 1):
            im = Image.new('RGBA', (16, 16)); d = ImageDraw.Draw(im)
            d.rectangle((0, 0, k - 1, height - 1), fill=(*c, 255))
            if height > 2: d.rectangle((0, 0, k - 1, 0), fill=(*[min(255, v + 60) for v in c], 255))
            hud.add(f'bar_{cname}_{k}', im)

def add_text(hud, fonts, name, s, color, big=False, shadow=True):
    """A pre-rendered line of text as one sprite (<= 16 px tall; its pieces are 16x16 cells)."""
    im = fonts.text(s, color, big, shadow)
    hud.widths[name] = im.width
    hud.add(name, im)

# ------------------------------------------------------------------ the space flight (space.c render_hud)
CELL = [(90, 220, 60), (170, 225, 50), (250, 210, 40), (250, 140, 30), (230, 30, 30)]

def space_hud(hud, fonts, atlas_sheet):
    """The space flight's HUD as one narrow column down the top left (at most one piece on any scanline, two with the cruiser's
    gauge on the right): shield cells (green at the bottom, red on top; the four of hp_max 4 in two pieces of two), then one
    piece each for the lives, the gun power pips, the torpedoes and the hero-power stars (an icon and its count), and a
    vertical hull gauge for the cruiser. The source's labels are gone: the icons carry the meaning."""
    colors = (0, 2, 3, 4)                      # the source's MAP[4]: bottom to top
    for pname, pair in (('top', (3, 2)), ('bot', (1, 0))):   # piece (upper cell, lower cell) = cell indices
        for mask in range(4):
            im = Image.new('RGBA', (16, 16)); d = ImageDraw.Draw(im)
            for slot, cell in enumerate(pair):
                y = slot * 9; c = CELL[colors[cell]]
                d.rectangle((4, y, 10, y + 6), fill=(10, 10, 20, 255))
                if mask >> slot & 1:
                    d.rectangle((4, y, 10, y + 6), fill=(*c, 255)); d.rectangle((5, y + 1, 6, y + 2), fill=(255, 255, 255, 255))
                else: d.rectangle((5, y + 1, 9, y + 5), fill=(c[0] // 5, c[1] // 5, c[2] // 5, 255))
            hud.add(f'cells_{pname}_{mask}', im)
    white, cyan = (255, 255, 255), (80, 230, 255)
    for n in range(10):                        # spare lives: "x3"
        hud.add(f'life_{n}', piece(fonts.text(f'x{n}', white, False, True), 0, 0))
    for n in range(4):                         # gun power: three pips
        im = Image.new('RGBA', (16, 16)); d = ImageDraw.Draw(im)
        for i in range(3):
            c = cyan if i < n else (20, 40, 60)
            d.rectangle((i * 5, 2, i * 5 + 3, 6), fill=(*c, 255))
        hud.add(f'pwr_{n}', im)
    torp = atlas_sheet.crop((367, 0, 378, 11)).resize((6, 6), Image.Resampling.NEAREST)
    star = Image.new('RGBA', (7, 7)); d = ImageDraw.Draw(star)
    d.rectangle((2, 0, 4, 6), fill=(255, 200, 240, 255)); d.rectangle((0, 2, 6, 4), fill=(255, 200, 240, 255)); d.point((3, 3), fill=(255, 255, 255, 255))
    for name, icon, count in (('trp', torp, 6), ('spc', star, 3)):
        for n in range(count):
            im = Image.new('RGBA', (16, 16)); im.alpha_composite(icon, (0, 1))
            im.alpha_composite(fonts.text(str(n), white, False, True), (icon.width + 2, 0))
            hud.add(f'{name}_{n}', im)
    for k in range(17):                        # the cruiser's hull: one 16-dot segment of a vertical gauge, k dots filled from the bottom
        im = Image.new('RGBA', (16, 16)); d = ImageDraw.Draw(im)
        d.rectangle((8, 0, 15, 15), fill=(10, 10, 20, 255)); d.rectangle((8, 0, 8, 15), fill=(90, 90, 150, 255)); d.rectangle((15, 0, 15, 15), fill=(90, 90, 150, 255))
        if k:
            d.rectangle((10, 16 - k, 13, 15), fill=(230, 70, 40, 255)); d.rectangle((10, 16 - k, 10, 15), fill=(255, 150, 100, 255))
        hud.add(f'hull_{k}', im)

# ------------------------------------------------------------------ the race (mode7.c render_hud)
def wide(im):
    """The race runs at the 512-dot clock: every picture is stretched to twice its width to keep its shape."""
    return im.resize((im.width * 2, im.height), Image.Resampling.NEAREST)

def race_hud(hud, fonts, buggy):
    """Digits, bar fills, the car icon, lap / place / pursuit labels and the countdown, all two dots per pixel."""
    for cname in ('white', 'gold', 'pink'):
        for d in range(10):
            hud.add(f'digit_{cname}_{d}', piece(wide(fonts.text(str(d), BAR_COLORS[cname], True, True)), 0, 0))
    add_bar_fills(hud, ('green', 'yellow', 'red', 'orange'), 5)
    hud.add('icon', piece(wide(buggy.resize((14, 10), Image.Resampling.NEAREST)), 0, 0, 32))
    for name, text, color in (('x', 'x', (255, 255, 255)), ('lap', 'LAP', (255, 255, 255)), ('slash3', '/3', (255, 255, 255)), ('gap', 'GAP', (255, 255, 255)),
                              ('m', 'M', (255, 255, 255)), ('kmh', 'KM/H', (200, 200, 200)), ('go', 'GO!', (255, 255, 255)),
                              ('finish', 'FINISH!', (255, 182, 0)), ('place', 'PLACE', (255, 255, 255)), ('turbo', 'TURBO', (255, 182, 0)),
                              ('c3', '3', (255, 60, 60)), ('c2', '2', (255, 60, 60)), ('c1', '1', (255, 60, 60))):
        im = fonts.text(text, color, True, True)
        hud.widths[name] = im.width * 2
        hud.add(name, wide(im))
    for n, text in enumerate(('1ST', '2ND', '3RD', '4TH', '5TH', '6TH', '7TH', '8TH')):
        im = wide(fonts.text(text, (255, 182, 0) if n == 0 else (255, 255, 255), True, True))
        hud.widths[f'ord{n + 1}'] = im.width
        hud.add(f'ord{n + 1}', im)
