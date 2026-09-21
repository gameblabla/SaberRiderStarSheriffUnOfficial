#!/usr/bin/env python3
"""Build assets/mode7.png + assets/mode7.txt for the Mode-7 stage ("The All Galaxy Grand Prix").

Sources (all in the project root, none redistributed by the game):
  video.twimg.com_tweet_video_DXEYQnaXkAAXBY8.mp4   Fireball's buggy, rear view, 2x, green-keyed. The clip is a STEERING
                                                     set, not a loop: hard left, left, straight, right, hard right (+ a
                                                     near-duplicate of the last one, dropped)
  video.twimg.com_tweet_video_DUaM4BaW0AAnRbO.mp4   Fireball rear view, 9 frames at 2x (victory hop)
  decoded/level1/06_Playfield_D8B018EE.png           level-1 cacti / rock spires (billboards)
Recolours of the buggy give the Black Hornets, their leader, Marco Firenza and the field; the floor materials, shots,
explosions, mines and the finish gate are drawn here.

The clips are lossy (chroma-subsampled), so the key is a soft one: alpha from how green a pixel is relative to the
key, the colour un-premultiplied against the key, then the 2x frames are reduced to native with a majority vote.

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

KEY = np.array([3, 111, 43])

def key_soft(rgb, key=KEY):
    """Soft chroma key against the tweet clips' green screen. Returns (rgb un-premultiplied, alpha 0..1).
    Greenness = g - max(r, b): the key scores ~68, real pixels of the sprite score <= 0 (nothing on the buggy is
    greener than it is red/blue). A pixel half blended with the screen (a dark tyre edge) scores ~34 -> alpha 0.5,
    and dividing the key's share back out recovers the dark colour instead of leaving a green fringe."""
    rgb = rgb.astype(float)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    kg = float(key[1] - max(key[0], key[2]))
    green = g - np.maximum(r, b)
    a = np.clip(1.0 - (green - 8.0) / (kg - 8.0), 0.0, 1.0)
    # anything far from the key in plain distance is opaque whatever its greenness (bright colours near the screen)
    a[np.abs(rgb - key).sum(2) > 200] = 1.0
    out = np.zeros_like(rgb)
    m = a > 0.05
    out[m] = (rgb[m] - (1 - a[m])[:, None] * key) / a[m][:, None]
    out = np.clip(out, 0, 255)
    # despill what is left: a kept pixel is never greener than its red/blue
    out[..., 1] = np.minimum(out[..., 1], np.maximum(out[..., 0], out[..., 2]) + 6)
    return out, a

def downscale2(rgb, a):
    """2x-upscaled clip -> native: a 2x2 block is opaque when most of it is, its colour the median of the covered
    pixels (robust against codec noise). rgb/a from key_soft."""
    h, w = a.shape
    h2, w2 = h // 2, w // 2
    out = np.zeros((h2, w2, 4), np.uint8)
    for y in range(h2):
        for x in range(w2):
            ab = a[2*y:2*y+2, 2*x:2*x+2].reshape(-1)
            if ab.sum() < 2.0: continue
            blk = rgb[2*y:2*y+2, 2*x:2*x+2].reshape(-1, 3)
            sel = blk[ab >= 0.5] if (ab >= 0.5).sum() >= 2 else blk[ab > 0.25]
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
    """5 steering poses: 0 hard left, 1 left, 2 straight, 3 right, 4 hard right (the clip's 6th is a near copy of 4)"""
    fr = unique_frames(frames_of("video.twimg.com_tweet_video_DXEYQnaXkAAXBY8.mp4"))[:5]
    res = []
    for f in fr:
        res.append(downscale2(*key_soft(f)))
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
def leader(h, s, v):        # the Hornet leader: black body, blood-red trim, darker tyres
    if is_red(h, s): return (0.98, 0.95, min(1, v * 1.1))
    if s < 0.3 and v > 0.35: return (h, 0.0, v * 0.28)
    return (h, s, v * 0.8)
def firenza(h, s, v):       # Marco Firenza: white and green
    if is_red(h, s): return (0.36, 0.85, v)
    return (h, s * 0.5, min(1, v * 1.1))
def racer_blue(h, s, v):
    if is_red(h, s): return (0.6, 0.85, v)
    return (h, s, v)
def racer_purple(h, s, v):
    if is_red(h, s): return (0.8, 0.7, v)
    return (h, s, v)

# ---------------------------------------------------------------- Fireball rear view (victory)
def fireball_frames():
    fr = frames_of("video.twimg.com_tweet_video_DUaM4BaW0AAnRbO.mp4")
    res = [downscale2(*key_soft(f)) for f in fr]
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
    """32x32 floor materials, sampled by world position so they tile seamlessly across the 8-unit material map:
    0 sand A, 1 sand B, 2 asphalt, 3 edge line, 4 kerb red, 5 kerb white, 6 start/finish checker, 7 dirt road,
    8 dark sand (scorched), 9 centre dash, 10 dirt shoulder"""
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
    asphalt = (74, 70, 82); asphalt2 = (66, 62, 74)
    def flat(seed, c1, c2, p=0.15):
        rnd = random.Random(seed); img = np.zeros((T, T, 4), np.uint8)
        for y in range(T):
            for x in range(T):
                img[y, x] = (*(c1 if rnd.random() > p else c2), 255)
        return img
    tiles.append(flat(3, asphalt, asphalt2))                    # 2 asphalt
    tiles.append(flat(4, (232, 232, 236), (214, 214, 220)))     # 3 edge line
    tiles.append(flat(5, (222, 44, 44), (200, 36, 36)))         # 4 kerb red
    tiles.append(flat(6, (242, 242, 242), (222, 222, 226)))     # 5 kerb white
    chk = flat(8, asphalt, asphalt2)
    for y in range(T):
        for x in range(T):
            chk[y, x, :3] = (240, 240, 240) if ((x // 8) + (y // 8)) % 2 == 0 else (20, 20, 24)
    tiles.append(chk)                                           # 6 checker
    dirt = sand_tile(9)
    rnd = random.Random(19)
    for y in range(T):
        for x in range(T):
            if rnd.random() < 0.55: dirt[y, x, :3] = (170, 96, 50) if rnd.random() < 0.7 else (150, 82, 40)
    tiles.append(dirt)                                          # 7 dirt road
    tiles.append(sand_tile(10, dark=True))                      # 8 dark sand
    tiles.append(flat(11, (226, 200, 70), (206, 182, 60)))      # 9 centre dash
    sh = sand_tile(12)
    rnd = random.Random(20)
    for y in range(T):
        for x in range(T):
            if rnd.random() < 0.5: sh[y, x, :3] = (176, 104, 56) if rnd.random() < 0.6 else (156, 92, 48)
    tiles.append(sh)                                            # 10 dirt shoulder
    return tiles

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
    # the other cars get three steering poses: left, straight, right
    for name, fn in (("hornet", hornet), ("leader", leader), ("firenza", firenza), ("racer_blue", racer_blue), ("racer_purple", racer_purple)):
        entries.append((name, [recolour(bug[0], fn), recolour(bug[2], fn), recolour(bug[4], fn)]))
    entries.append(("fireball", fireball_frames()))
    for n, im in level_props().items(): entries.append((n, [im]))
    entries.append(("floor", floor_tiles()))
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
