#!/usr/bin/env python3
"""Build assets/mode7.png + assets/mode7.txt for the Mode-7 stage ("The All Galaxy Grand Prix").

Sources (all in the project root, none redistributed by the game):
  video.twimg.com_tweet_video_DXEYQnaXkAAXBY8.mp4   Fireball's buggy, rear view, 6 frames at 2x, green-keyed
  video.twimg.com_tweet_video_DZFh80TX4AAEO93.mp4   the 320x240 Mode-7 mockup: the Outrider tank is lifted from it
  media_DXtfxJQXcAE9ahL.webp                         April on Nova, rear view, 1x, green-keyed
  video.twimg.com_tweet_video_DUaM4BaW0AAnRbO.mp4   Fireball rear view, 9 frames at 2x (victory hop)
  decoded/level1/06_Playfield_D8B018EE.png           level-1 cacti / rock spires (billboards)
Recolours of the buggy give the Black Hornets, Marco Firenza and the field; the floor tiles, Dome City, shots,
explosions, mines and the finish gate are drawn here.

Atlas text format: one line per sprite  "name x y w h frames [ox oy]"  (frames laid out horizontally).
"""
import os, sys, subprocess, tempfile, colorsys, math, random
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT_DIR = os.path.join(ROOT, "game", "assets")
TMP = tempfile.mkdtemp()
random.seed(7)

def frames_of(video):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", os.path.join(ROOT, video), os.path.join(TMP, "f%03d.png")], check=True)
    out = []
    for n in sorted(os.listdir(TMP)):
        if n.startswith("f") and n.endswith(".png"):
            out.append(np.array(Image.open(os.path.join(TMP, n)).convert("RGB")).astype(int))
            os.remove(os.path.join(TMP, n))
    return out

def key_green(rgb, tol=70):
    """alpha from the tweet clips' green screen (3,111,43)"""
    key = np.array([3, 111, 43])
    d = np.abs(rgb - key).sum(2)
    a = (d > tol).astype(np.uint8) * 255
    return a

def downscale2(rgb, a):
    """2x-upscaled clip -> native: majority colour per 2x2 block (robust against codec noise)"""
    h, w = a.shape
    h2, w2 = h // 2, w // 2
    out = np.zeros((h2, w2, 4), np.uint8)
    for y in range(h2):
        for x in range(w2):
            blk = rgb[2*y:2*y+2, 2*x:2*x+2].reshape(-1, 3); ab = a[2*y:2*y+2, 2*x:2*x+2].reshape(-1)
            if ab.sum() < 2 * 255: continue
            sel = blk[ab > 0]
            out[y, x, :3] = np.median(sel, axis=0); out[y, x, 3] = 255
    return out

def crop_alpha(img):
    ys, xs = np.where(img[:, :, 3] > 0)
    return img[ys.min():ys.max()+1, xs.min():xs.max()+1]

def unique_frames(frames):
    out = []
    for f in frames:
        if not out or np.abs(out[-1] - f).sum() > 2000: out.append(f)
    return out

# ---------------------------------------------------------------- buggy
def buggy_frames():
    fr = unique_frames(frames_of("video.twimg.com_tweet_video_DXEYQnaXkAAXBY8.mp4"))
    res = []
    for f in fr:
        a = key_green(f)
        res.append(downscale2(f, a))
    # common crop box
    ys, xs = [], []
    for r in res:
        yy, xx = np.where(r[:, :, 3] > 0); ys += [yy.min(), yy.max()]; xs += [xx.min(), xx.max()]
    y0, y1, x0, x1 = min(ys), max(ys) + 1, min(xs), max(xs) + 1
    return [r[y0:y1, x0:x1] for r in res]

def recolour(img, fn):
    out = img.copy()
    for y in range(out.shape[0]):
        for x in range(out.shape[1]):
            if out[y, x, 3] == 0: continue
            r, g, b = out[y, x, :3] / 255.0
            h, s, v = colorsys.rgb_to_hsv(r, g, b)
            h2, s2, v2 = fn(h, s, v)
            r2, g2, b2 = colorsys.hsv_to_rgb(h2 % 1.0, min(1, max(0, s2)), min(1, max(0, v2)))
            out[y, x, :3] = (int(r2 * 255), int(g2 * 255), int(b2 * 255))
    return out

def is_red(h, s): return s > 0.35 and (h < 0.06 or h > 0.9)

def hornet(h, s, v):        # Black Hornets: black body, yellow trim
    if is_red(h, s): return (0.14, 0.9, min(1, v * 1.05))
    if s < 0.3 and v > 0.35: return (h, 0.0, v * 0.35)
    return (h, s, v)
def firenza(h, s, v):       # Marco Firenza: white and green
    if is_red(h, s): return (0.36, 0.85, v)
    return (h, s * 0.5, min(1, v * 1.1))
def racer_blue(h, s, v):
    if is_red(h, s): return (0.6, 0.85, v)
    return (h, s, v)
def racer_purple(h, s, v):
    if is_red(h, s): return (0.8, 0.7, v)
    return (h, s, v)

# ---------------------------------------------------------------- tank (from the mockup)
def tank_sprite():
    fr = frames_of("video.twimg.com_tweet_video_DZFh80TX4AAEO93.mp4")
    st = np.stack(fr); moving = (st.max(0) - st.min(0)).sum(2) >= 24
    x0, y0, x1, y1 = 195, 95, 270, 150; HORIZON = 126
    im = fr[0]; crop = im[y0:y1, x0:x1]
    def is_floor(px):
        r, g, b = px[..., 0], px[..., 1], px[..., 2]
        return (r > g + 15) & (g > b + 10) & (r - b > 60) & (r > 120)
    mask = np.zeros((y1 - y0, x1 - x0), bool)
    for y in range(y0, y1):
        row = im[y, x0:x1]
        margin = np.concatenate([im[yy, max(0, x0 - 10):x0] for yy in (y - 1, y, y + 1)] + [im[yy, x1:x1 + 10] for yy in (y - 1, y, y + 1)])
        dbg = np.abs(row[:, None, :] - margin[None, :, :]).sum(2).min(1)
        bg = (is_floor(row) | (dbg <= 30)) if y >= HORIZON else (dbg <= 45)
        mask[y - y0] = ~bg
    mv = moving[y0:y1, x0:x1]
    mask |= (mv & ~is_floor(crop)); mask &= ~(is_floor(crop) & ~mv)
    mask = ndimage.binary_opening(mask, structure=np.ones((2, 2))) | (mv & ~is_floor(crop))
    lab, n = ndimage.label(mask, structure=np.ones((3, 3)))
    sizes = ndimage.sum(mask, lab, range(1, n + 1)); big = int(np.argmax(sizes)) + 1
    keep = ndimage.binary_fill_holes(lab == big)
    # the muzzle flashes were animated over the floor: drop the orange residue on the bottom rows
    keep[-8:] &= ~is_floor(crop[-8:])
    rgba = np.dstack([crop.astype(np.uint8), (keep * 255).astype(np.uint8)])
    rgba = crop_alpha(rgba)
    # the thin antennae were keyed away against the mountains: redraw two, in the hull's grey
    h, w = rgba.shape[:2]
    pad = np.zeros((h + 14, w, 4), np.uint8); pad[14:] = rgba
    grey = (150, 160, 178, 255); dark = (60, 64, 90, 255)
    for ax in (10, w - 11):
        for yy in range(0, 16):
            pad[yy, ax] = grey; pad[yy, ax + 1] = dark
        pad[0, ax] = (230, 60, 60, 255); pad[0, ax + 1] = (230, 60, 60, 255)
    # upscale 2x (the mockup drew it at half the sheet scale)
    up = np.repeat(np.repeat(pad, 2, axis=0), 2, axis=1)
    return up

# ---------------------------------------------------------------- April on Nova
def april_sprite():
    im = np.array(Image.open(os.path.join(ROOT, "media_DXtfxJQXcAE9ahL.webp")).convert("RGB")).astype(int)
    key = np.array([5, 111, 46]); a = (np.abs(im - key).sum(2) > 70).astype(np.uint8) * 255
    a = ndimage.binary_opening(a > 0, structure=np.ones((2, 2))).astype(np.uint8) * 255
    rgba = np.dstack([im.astype(np.uint8), a])
    base = crop_alpha(rgba)
    # two-frame wing flap: the wings (the pink region right of the rider's back) are sheared up a little
    h, w = base.shape[:2]
    f2 = base.copy()
    wing = np.zeros((h, w), bool)
    for y in range(h):
        for x in range(w):
            r, g, b, al = base[y, x]
            if al and x > w * 0.45 and y < h * 0.6 and r > 150 and g < 110: wing[y, x] = True
    f2[wing] = 0
    ys, xs = np.where(wing)
    for y, x in zip(ys, xs):
        ny = y - int((x - w * 0.45) / (w * 0.55) * 6)
        if 0 <= ny < h: f2[ny, x] = base[y, x]
    return [base, f2]

# ---------------------------------------------------------------- Fireball rear view (victory)
def fireball_frames():
    fr = frames_of("video.twimg.com_tweet_video_DUaM4BaW0AAnRbO.mp4")
    res = [downscale2(f, key_green(f)) for f in fr]
    ys, xs = [], []
    for r in res:
        yy, xx = np.where(r[:, :, 3] > 0); ys += [yy.min(), yy.max()]; xs += [xx.min(), xx.max()]
    y0, y1, x0, x1 = min(ys), max(ys) + 1, min(xs), max(xs) + 1
    return [r[y0:y1, x0:x1] for r in res]

# ---------------------------------------------------------------- level-1 props
def level_props():
    im = np.array(Image.open(os.path.join(ROOT, "decoded/level1/06_Playfield_D8B018EE.png")).convert("RGBA"))
    boxes = {"cactus": (65, 131, 91, 191), "rock_small": (353, 144, 381, 191), "rock_big": (0, 130, 64, 191), "mesa": (1252, 137, 1366, 191)}
    out = {}
    for n, (x0, y0, x1, y1) in boxes.items():
        out[n] = im[y0:y1, x0:x1].copy()
    return out

# ---------------------------------------------------------------- drawn art
def pil(w, h): return Image.new("RGBA", (w, h), (0, 0, 0, 0))

def floor_tiles():
    """32x32 tiles: 0 sand A, 1 sand B, 2 asphalt, 3 asphalt+left line, 4 asphalt+right line, 5 rumble L, 6 rumble R,
    7 start/finish checker, 8 dirt road, 9 dark sand (shadow / scorched), 10 asphalt centre dash, 11 dome plaza"""
    T = 32
    tiles = []
    sand = [(214, 128, 62), (226, 143, 74), (238, 158, 88), (200, 112, 52), (190, 100, 46), (246, 172, 104)]
    def sand_tile(seed, dark=False):
        rnd = random.Random(seed); img = np.zeros((T, T, 4), np.uint8)
        for y in range(T):
            streak = rnd.random()
            for x in range(T):
                c = sand[1]
                r = rnd.random()
                if r < 0.10: c = sand[3]
                elif r < 0.16: c = sand[2]
                elif r < 0.19: c = sand[0]
                if streak < 0.18 and rnd.random() < 0.6: c = sand[3]
                if streak > 0.9 and rnd.random() < 0.5: c = sand[5]
                if dark: c = tuple(int(v * 0.84) for v in c)
                img[y, x] = (*c, 255)
        return img
    tiles.append(sand_tile(1)); tiles.append(sand_tile(2))
    asphalt = (74, 70, 82); asphalt2 = (66, 62, 74); line = (236, 236, 236)
    def asp_tile(seed, left=False, right=False, dash=False):
        rnd = random.Random(seed); img = np.zeros((T, T, 4), np.uint8)
        for y in range(T):
            for x in range(T):
                c = asphalt if rnd.random() > 0.15 else asphalt2
                if left and x < 3: c = line
                if right and x >= T - 3: c = line
                if dash and 14 <= x < 18 and y < 20: c = (220, 200, 70)
                img[y, x] = (*c, 255)
        return img
    tiles.append(asp_tile(3)); tiles.append(asp_tile(4, left=True)); tiles.append(asp_tile(5, right=True))
    def rumble(seed, left):
        img = asp_tile(seed)
        for y in range(T):
            for x in range(T):
                edge = x < 6 if left else x >= T - 6
                if edge: img[y, x, :3] = (220, 40, 40) if (y // 8) % 2 == 0 else (240, 240, 240)
        return img
    tiles.append(rumble(6, True)); tiles.append(rumble(7, False))
    chk = asp_tile(8)
    for y in range(T):
        for x in range(T):
            if ((x // 4) + (y // 4)) % 2 == 0: chk[y, x, :3] = (240, 240, 240)
            else: chk[y, x, :3] = (20, 20, 24)
    tiles.append(chk)
    dirt = sand_tile(9)
    for y in range(T):
        for x in range(T):
            if random.random() < 0.55: dirt[y, x, :3] = (170, 96, 50) if random.random() < 0.7 else (150, 82, 40)
    tiles.append(dirt)
    tiles.append(sand_tile(10, dark=True))
    tiles.append(asp_tile(11, dash=True))
    plaza = np.zeros((T, T, 4), np.uint8)
    for y in range(T):
        for x in range(T):
            c = (120, 130, 150) if (x % 16 and y % 16) else (90, 98, 116)
            plaza[y, x] = (*c, 255)
    tiles.append(plaza)
    return tiles

def dome_city():
    """Dome City on the horizon: a big glass dome with the Cavalry Command tower, plus outbuildings. 240x96."""
    W, H = 240, 96
    img = pil(W, H); d = ImageDraw.Draw(img)
    # ground plate
    d.rectangle([0, H - 10, W, H], fill=(96, 104, 124, 255))
    # outbuildings
    for x, w, h, c in [(4, 30, 30, (150, 158, 176)), (36, 20, 40, (130, 138, 158)), (190, 26, 34, (150, 158, 176)), (218, 18, 26, (130, 138, 158))]:
        d.rectangle([x, H - 10 - h, x + w, H - 10], fill=(*c, 255))
        for wy in range(H - 10 - h + 4, H - 12, 6):
            for wx in range(x + 3, x + w - 3, 5): d.point((wx, wy), fill=(255, 236, 150, 255))
    # dome
    cx, cy, rx, ry = 120, H - 10, 72, 58
    for yy in range(cy - ry, cy + 1):
        t = (cy - yy) / ry
        hw = int(rx * math.sqrt(max(0, 1 - t * t)))
        shade = int(150 + 80 * t)
        d.line([(cx - hw, yy), (cx + hw, yy)], fill=(120, shade, 230, 255))
    for yy in range(cy - ry, cy + 1, 7):
        t = (cy - yy) / ry; hw = int(rx * math.sqrt(max(0, 1 - t * t)))
        d.line([(cx - hw, yy), (cx + hw, yy)], fill=(70, 100, 170, 255))
    for k in range(-3, 4):
        d.line([(cx, cy - ry), (cx + k * 22, cy)], fill=(70, 100, 170, 255))
    # the Nerve Center tower
    d.rectangle([cx - 8, 6, cx + 8, cy - ry + 20], fill=(210, 214, 226, 255))
    d.rectangle([cx - 12, 4, cx + 12, 10], fill=(230, 60, 60, 255))
    d.line([(cx, 0), (cx, 4)], fill=(255, 255, 255, 255))
    d.rectangle([cx - 5, 12, cx + 5, 16], fill=(90, 200, 255, 255))
    return np.array(img)

def shot_sprite():
    img = pil(16, 8); d = ImageDraw.Draw(img)
    d.ellipse([0, 1, 15, 6], fill=(255, 230, 90, 255)); d.ellipse([3, 2, 12, 5], fill=(255, 255, 220, 255))
    return np.array(img)

def enemy_shot_sprite():
    img = pil(12, 12); d = ImageDraw.Draw(img)
    d.ellipse([0, 0, 11, 11], fill=(120, 40, 220, 255)); d.ellipse([3, 3, 8, 8], fill=(240, 200, 255, 255))
    return np.array(img)

def explosion_frames():
    out = []
    for i in range(6):
        s = 64; img = pil(s, s); d = ImageDraw.Draw(img)
        r = 8 + i * 5
        cols = [(255, 255, 220), (255, 220, 80), (255, 140, 40), (200, 60, 20), (90, 40, 30)]
        for k, c in enumerate(cols):
            rr = r - k * 4 - i
            if rr <= 0: continue
            a = 255 if i < 4 else max(0, 255 - (i - 3) * 100)
            rnd = random.Random(i * 10 + k)
            for _ in range(5 + i):
                ox, oy = rnd.randint(-i * 3, i * 3), rnd.randint(-i * 3, i * 3)
                d.ellipse([s // 2 + ox - rr // 2, s // 2 + oy - rr // 2, s // 2 + ox + rr // 2, s // 2 + oy + rr // 2], fill=(*c, a))
        out.append(np.array(img))
    return out

def flash_sprite():
    img = pil(24, 24); d = ImageDraw.Draw(img)
    d.ellipse([2, 2, 21, 21], fill=(255, 200, 60, 255)); d.ellipse([7, 7, 16, 16], fill=(255, 255, 230, 255))
    for a in range(0, 360, 45):
        x, y = 12 + 11 * math.cos(math.radians(a)), 12 + 11 * math.sin(math.radians(a))
        d.line([(12, 12), (x, y)], fill=(255, 240, 160, 255))
    return np.array(img)

def mine_sprite():
    out = []
    for i in range(2):
        img = pil(20, 14); d = ImageDraw.Draw(img)
        d.ellipse([0, 2, 19, 13], fill=(40, 40, 48, 255)); d.ellipse([4, 4, 15, 10], fill=(70, 70, 80, 255))
        d.ellipse([8, 0, 11, 3], fill=(255, 40, 40, 255) if i == 0 else (120, 20, 20, 255))
        out.append(np.array(img))
    return out

def finish_gate():
    W, H = 160, 70
    img = pil(W, H); d = ImageDraw.Draw(img)
    for x in (2, W - 8):
        d.rectangle([x, 8, x + 5, H], fill=(200, 200, 210, 255)); d.rectangle([x + 1, 8, x + 2, H], fill=(240, 240, 250, 255))
    d.rectangle([0, 0, W, 18], fill=(230, 40, 40, 255))
    for x in range(0, W, 8):
        for y in range(0, 18, 6):
            if ((x // 8) + (y // 6)) % 2 == 0: d.rectangle([x, y, x + 7, y + 5], fill=(245, 245, 245, 255))
    return np.array(img)

def smoke_frames():
    out = []
    for i in range(4):
        img = pil(24, 24); d = ImageDraw.Draw(img)
        r = 4 + i * 3; a = 200 - i * 45
        d.ellipse([12 - r, 12 - r, 12 + r, 12 + r], fill=(160, 150, 150, a))
        out.append(np.array(img))
    return out

def portrait_claudia():
    """Claudia (the fan who lures Fireball): a small dialog avatar, 32x32, drawn in the game's avatar style"""
    img = pil(32, 32); d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 31, 31], fill=(40, 30, 60, 255))
    d.ellipse([7, 4, 25, 26], fill=(120, 70, 40, 255))      # hair
    d.ellipse([10, 8, 22, 24], fill=(240, 200, 170, 255))    # face
    d.rectangle([8, 22, 24, 31], fill=(180, 40, 90, 255))    # dress
    d.point((13, 15), fill=(20, 20, 40, 255)); d.point((19, 15), fill=(20, 20, 40, 255))
    d.line([(14, 20), (18, 20)], fill=(200, 60, 80, 255))
    return np.array(img)

# ---------------------------------------------------------------- atlas packing
def main():
    entries = []  # (name, [frames], ox, oy)
    bug = buggy_frames()
    entries.append(("buggy", bug))
    entries.append(("hornet", [recolour(bug[0], hornet), recolour(bug[3], hornet)]))
    entries.append(("firenza", [recolour(bug[0], firenza), recolour(bug[3], firenza)]))
    entries.append(("racer_blue", [recolour(bug[0], racer_blue), recolour(bug[3], racer_blue)]))
    entries.append(("racer_purple", [recolour(bug[0], racer_purple), recolour(bug[3], racer_purple)]))
    entries.append(("tank", [tank_sprite()]))
    entries.append(("april", april_sprite()))
    entries.append(("fireball", fireball_frames()))
    for n, im in level_props().items(): entries.append((n, [im]))
    entries.append(("floor", floor_tiles()))
    entries.append(("dome", [dome_city()]))
    entries.append(("shot", [shot_sprite()]))
    entries.append(("eshot", [enemy_shot_sprite()]))
    entries.append(("explosion", explosion_frames()))
    entries.append(("flash", [flash_sprite()]))
    entries.append(("mine", mine_sprite()))
    entries.append(("gate", [finish_gate()]))
    entries.append(("smoke", smoke_frames()))
    entries.append(("claudia", [portrait_claudia()]))

    W = 1024; x = y = 0; rowh = 0; placed = []
    for name, frames in entries:
        fh, fw = frames[0].shape[:2]
        tw = fw * len(frames)
        if x + tw > W: x = 0; y += rowh + 1; rowh = 0
        placed.append((name, x, y, fw, fh, len(frames), frames))
        x += tw + 1; rowh = max(rowh, fh)
    H = y + rowh + 1
    atlas = np.zeros((H, W, 4), np.uint8)
    lines = []
    for name, ax, ay, fw, fh, n, frames in placed:
        for i, f in enumerate(frames):
            atlas[ay:ay + fh, ax + i * fw:ax + (i + 1) * fw] = f
        lines.append(f"{name} {ax} {ay} {fw} {fh} {n}")
    os.makedirs(OUT_DIR, exist_ok=True)
    Image.fromarray(atlas, "RGBA").save(os.path.join(OUT_DIR, "mode7.png"))
    with open(os.path.join(OUT_DIR, "mode7.txt"), "w") as f: f.write("\n".join(lines) + "\n")
    print("\n".join(lines)); print("atlas", W, H)

if __name__ == "__main__":
    main()
