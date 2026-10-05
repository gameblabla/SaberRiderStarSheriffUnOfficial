#!/usr/bin/env python3
"""Bake current sources to one VDC background and canonical-facing sprites.

Source extraction uses the game's pack decoder and active stage construction.
PCE output never needs PNG, packs, floating point or a filesystem at runtime.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import story
import timeline
import presentation
import tile_budget
import palfit
import hudart
import frontend
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
CAR_WIDTHS = (16, 24, 32, 40, 52, 64)   # baked widths of the race cars
ORB_SIZES = (14, 11, 8, 6, 4)              # baked heights of the race's shots, near to far (twice as wide)
POLE_SIZES = ((10, 28), (14, 40), (18, 52), (24, 70), (32, 92), (44, 124))   # the finish line's flag poles: width, height in dots, far to near
SPIN_FRAMES = 12                         # poses of a full turn of the spinning buggy (one is the upright car)
M6_HORIZON = 128                         # the arena's horizon row
M6_SCALES = (0.15, 0.175, 0.205, 0.24, 0.28, 0.325, 0.38, 0.44, 0.51, 0.59, 0.68)   # the mechs' baked scales (canvas 138 x 146), a ladder of 1.16
M6_BIG_SCALES = (0.74, 0.88, 1.01, 1.16)   # the nearest mech's bigger steps: pieces of 32x32 (at most 16 of them), drawn by m6_d.c's own emitter into two alternating VRAM buffers
M6_PROP_SCALES = (0.18, 0.28, 0.42, 0.64, 0.95)
M6_FX_SCALES = (0.3, 0.45, 0.7, 1.1, 1.7)
M6_Z = 1.6                               # the arena's zoom over the source's law (214 / distance x 0.85): sizes and the rows under the horizon grow with it, so a mech
                                         # that has walked up to Ramrod fills the screen (the bearings keep the source's 1344 dots to the turn)
M6_ALL_SCALES = M6_SCALES + M6_BIG_SCALES
sys.path[:0] = [str(ROOT / 'tools/saturn'), str(ROOT / 'tools/dc')]
import levl
import texbake
from formats import Archive, BG_TILES, indexed, palette_for, planar_tile, planar_sprite, pack_sprite, pair_characters, vce_rgb, vce_colors

def run(args, **kwargs):
    result = subprocess.run([str(a) for a in args], cwd=ROOT, check=True, capture_output=True, **kwargs)
    return result

def extract(work):
    work.mkdir(parents=True, exist_ok=True)
    srgb = work / 'srgb'; srgb.mkdir(exist_ok=True)
    run(['make', '-f', 'Makefile.headless', '-j4'])
    run(['make', '-f', 'Makefile.headless', 'build/headless/obj/hero_test_platform.o'])
    objects = [p for p in (ROOT / 'build/headless/obj').rglob('*.o')
               if p.name != 'main_null.o' and p.name != 'hero_shadows_test.o']
    exe = work / 'export'
    run(['cc', '-std=gnu11', '-O2', '-Isrc', 'tools/pce/export.c', *objects, '-lpng', '-lm', '-o', exe])
    env = dict(os.environ, SABER_ASSETS=str(ROOT / 'assets'), SABER_START='100', SABER_HERO='1')
    for stage in (1, 3, 4, 5): run([exe, ROOT / 'SaberRider/data', stage, work], env=env)
    # The source game's barrel position for each aim pose, per hero (shots must leave the drawn gun).
    exe = work / 'export_muzzle'
    run(['cc', '-std=gnu11', '-O2', '-Isrc', 'tools/pce/export_muzzle.c', *objects, '-lpng', '-lm', '-o', exe])
    muzzle = []
    for hero in range(4):
        out = run([exe, ROOT / 'SaberRider/data'], env=dict(env, SABER_HERO=str(hero))).stdout.decode().split()
        muzzle.append([int(v) for v in out])
    (work / 'muzzle.json').write_text(json.dumps(muzzle))
    exe = work / 'texprep'
    run(['cc', '-std=gnu11', '-O2', '-Isrc', 'tools/dc/texprep.c', 'src/gfx.c', 'src/font.c',
         'src/pack.c', 'src/lzo1z.c', 'src/assets.c', 'src/namehash.c', '-o', exe])
    run([exe, ROOT / 'SaberRider/data', srgb])
    # Definitions supply the same asset IDs/anchors as the gameplay sources.
    ids = ['7E11BC19', '8403195A', '9C8F9A9E', '79260A58', '26818B85', '112DF34C', '02A38AFB', 'D39700C4', '2A02BD4F', 'FBFAF817', '4042CD71', '71887ECA', 'BFDAB70F', '1D724DD9', '211F5D78', '9393E59B', '20C6FAEF', 'ECC992CB', '72B53EF8', '925534E2', '916137ED', '906D3698', 'F5975DCF', 'F4A55EDE', 'F4A25ED9', 'F3B05E28', '0DB9F0E0', '29CAD5D3']
    exe = work / 'levprep'
    run(['cc', '-O2', '-std=gnu11', '-Isrc', 'tools/saturn/levprep.c', 'src/pack.c', 'src/lzo1z.c', '-o', exe])
    run([exe, ROOT / 'SaberRider/data', work, *ids])
    exe=work/'export_track'
    run(['cc','-std=gnu11','-O2','-Isrc','tools/pce/export_track.c',*[p for p in objects if p.name!='mode7.o'],'-lpng','-lm','-o',exe])
    run([exe,work/'track.bin'],env=env)

def cblock_frame(path, frame):
    kind, px, meta = texbake.load_srgb(path)
    if kind == 1:
        w, h, n, _ = struct.unpack_from('<4H', meta)
        return Image.fromarray(px[:, (frame % n) * w:(frame % n + 1) * w])
    n, cols, rows, tw, th, nt, sc = struct.unpack_from('<7H', meta)
    cells = np.frombuffer(meta, '<u2', n * cols * rows, 16)
    # character_draw uses cblock_draw_cell: an animation index selects one
    # cell of a frame, not the whole multi-cell tilemap frame.
    t = int(cells[frame % len(cells)])
    if t == 65535: return Image.new('RGBA', (tw, th))
    cx, cy = (t % sc) * tw, (t // sc) * th
    return Image.fromarray(px[cy:cy+th, cx:cx+tw])

def cblock_whole_frame(path, frame):
    """One whole multi-cell frame of a cblock (what the game's cblock_draw_frame draws), cells laid out on its grid."""
    kind, px, meta = texbake.load_srgb(path)
    n, cols, rows, tw, th, nt, sc = struct.unpack_from('<7H', meta)
    cells = np.frombuffer(meta, '<u2', n * cols * rows, 16)
    im = Image.new('RGBA', (cols * tw, rows * th))
    for r in range(rows):
        for c in range(cols):
            t = int(cells[(frame % n) * cols * rows + r * cols + c])
            if t != 65535:
                im.alpha_composite(Image.fromarray(px[(t // sc) * th:(t // sc + 1) * th, (t % sc) * tw:(t % sc + 1) * tw]), (c * tw, r * th))
    return im

CAMERA_WALL = {(123,123,132),(148,148,148),(173,173,173),(90,90,107),(58,66,82)}
def camera_wall_mask(im):
    """The wall greys of the security camera's cell that are connected to its left and bottom edges (True)."""
    a = np.array(im.convert('RGBA')); h, w = a.shape[:2]
    seen = np.zeros((h, w), bool)
    stack = [(y, 0) for y in range(h)] + [(h - 1, x) for x in range(w)]
    while stack:
        y, x = stack.pop()
        if not (0 <= y < h and 0 <= x < w) or seen[y, x] or tuple(int(v) for v in a[y, x, :3]) not in CAMERA_WALL: continue
        seen[y, x] = True
        stack += [(y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)]
    return seen

def key_wall(im):
    """The security camera's cell carries a patch of the building wall behind its bracket. That wall is a different grey from
    the background tiles it sits on (and from each building), so the patch showed as a coloured block. The wall greys
    connected to the cell's left and bottom edges are made transparent: the scenery shows through instead (the background
    then carries that wall itself: see camera_wall_background)."""
    a = np.array(im.convert('RGBA'))
    a[camera_wall_mask(im), 3] = 0
    return Image.fromarray(a)

def camera_wall_background(out, work):
    """The level's background has sky where the camera's cell stands (the source draws the wall as part of the camera's art),
    so with the cell's wall made transparent there was a hole beside the camera. The wall is continued into the background
    here, from the wall directly below the cell, which is the same surface and the same greys."""
    d = (work / '211F5D78.levl').read_bytes(); aid = struct.unpack_from('<I', d, 4)[0]
    first = struct.unpack_from('<I', d, 0x34 + 24 + 4)[0]
    art = work / 'srgb' / f'{aid:08X}.srgb'
    whole = bool(struct.unpack_from('<I', d, 0x34 + 24 + 20)[0] & 8)
    mask = camera_wall_mask(cblock_whole_frame(art, first) if whole else cblock_frame(art, first))
    h, w = mask.shape
    px = np.array(out)
    meta = json.loads((work / 'stage1.json').read_text())
    for t in meta['triggers']:
        if t['type'] != 16: continue
        x0, y0 = t['waypoints'][0]
        for y in range(h):
            for x in range(w):
                if not mask[y, x]: continue
                # (whatever the background has there: sky at the first camera, the next building behind the second)
                src = px[y0 + y + h, x0 + x]                             # the wall below the cell
                if src[3] < 128 or int(src[2]) > int(src[0]) + 30: continue
                px[y0 + y, x0 + x] = src
    return Image.fromarray(px)

HORSE_FRAMES = 5

def horse_frames(work, archive):
    """The robot-horse gallop at full size, as VDC big sprite cells: each frame (128x80) is four columns of one 32x64
    and one 32x16 sprite, so a horse costs 8 SAT entries instead of 33 pieces. Per frame, 40 patterns: the four 32x64
    blocks (8 patterns each, row-major two wide), then the four 32x16 blocks (2 each). One shared palette first."""
    art = work / 'srgb' / '8873D18C.srgb'
    frames = [cblock_whole_frame(art, k) for k in range(HORSE_FRAMES)]
    pal = palette_for(frames)
    out = pal.astype('<u2').tobytes()
    for im in frames:
        idx = indexed(im, pal)
        pats = []
        for c in range(4):
            for r in range(4):
                for h in range(2): pats.append(planar_sprite(idx[16*r:16*r+16, 32*c+16*h:32*c+16*h+16]))
        for c in range(4):
            for h in range(2): pats.append(planar_sprite(idx[64:80, 32*c+16*h:32*c+16*h+16]))
        out += b''.join(pats)
    assert len(out) == 32 + HORSE_FRAMES * 5120
    return archive.add('horse_frames', out)

FG_MAX_PIECES, FG_MAX_UNITS = 20, 8    # foreground sprite pieces per 288-px window / per 16-line row

def thin_foreground(fg):
    """Drop the foreground chunks that cannot be drawn steadily. The foreground is sprite pieces re-emitted every
    frame after the actors; the SAT holds 64 (HUD ~16, hero ~10, shots and enemies the rest) and a scanline 16
    units, so where more than FG_MAX_PIECES pieces (or FG_MAX_UNITS in one row) fall inside the camera window
    some of them are refused or not, frame by frame, and flicker. Remove 32x32 chunks, most crowded first, until
    every window fits: the removal is fixed, so what stays is stable."""
    a = np.asarray(fg).copy()
    h, w = a.shape[:2]
    blocks = np.zeros((h // 16, w // 16), bool)
    for y in range(h // 16):
        for x in range(w // 16): blocks[y, x] = (a[y*16:y*16+16, x*16:x*16+16, 3] > 0).any()
    removed = 0
    again = True
    while again:
        again = False
        for cam in range(0, max(1, w - 255), 16):
            c0, c1 = max(0, (cam - 31) // 16), (cam + 256) // 16 + 1
            sub = blocks[:, c0:c1]
            if sub.sum() <= FG_MAX_PIECES and sub.sum(1).max() <= FG_MAX_UNITS: continue
            best = None
            for cy in range(0, blocks.shape[0], 2):
                for cx in range((c0 // 2) * 2, c1, 2):
                    n = blocks[cy:cy+2, cx:cx+2].sum()
                    if n and (best is None or n > best[0]): best = (n, cy, cx)
            _, cy, cx = best
            blocks[cy:cy+2, cx:cx+2] = False
            a[cy*16:cy*16+32, cx*16:cx*16+32] = 0
            removed += 1; again = True
            break
    print(f'  foreground thinned: {removed} chunks removed', flush=True)
    return Image.fromarray(a)

# A scrolling column releases the tiles only it used (up to 30) but they stay unavailable until the next frame, because the
# old picture is still being displayed from them (video_pce.c, held slots); the column scrolling in needs its own new tiles
# meanwhile. A window filled to the brim of the cache left no room for that, and the left edge showed another column's tiles.
COLUMN_SLACK = 32

def native_background(image, archive, previews, name):
    w = (image.width + 7) // 8 * 8
    canvas = Image.new('RGBA', (w, 240), (0, 0, 0, 255)); canvas.paste(image)
    rgba = np.asarray(canvas)
    cells = rgba.reshape(30, 8, w // 8, 8, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 8, 8, 4)
    dialog_palette=name in ('stage1','stage3','stage4','stage5','stage6')   # palette 15 belongs to the dialogue box and the font
    cap = (512 - 4 - COLUMN_SLACK) if name == 'stage6' else BG_TILES - 4 - COLUMN_SLACK   # (Ramrod's arena keeps the cache's top 384 characters for sprite patterns)
    # The 16 palettes and the palette of every cell are optimised together (palfit.py), not by vertical bands of mean colour.
    palette, groups = palfit.fit_palettes(cells, 16, fixed=(15,) if dialog_palette else ())
    palette = [palette[k] for k in range(16)]
    if not dialog_palette:
        # The renderer forces BG colour 255 (palette 15, index 15) to white for the text font, so a baked cell must
        # never pick it: the last palette's last colour repeats its neighbour.
        palette[15] = palette[15].copy(); palette[15][15] = palette[15][14]
    # Transparent sky pixels all reveal VCE entry 0, independent of the cell's palette.
    backdrop = presentation.sky_color(int(name[5:]))
    tiles, tile_lookup, names = [], {}, []
    preview = np.zeros_like(rgba)
    cell_idx = palfit.index_cells(cells, np.asarray(palette), groups)
    cell_idx, groups, merged = tile_budget.limit_tiles(cells, cell_idx, groups, palette, w // 8, cap, reserved=(15,) if dialog_palette else ())
    if merged: print(f'{name}: redrew {merged} cells with neighbouring characters to fit the {BG_TILES}-tile cache', flush=True)
    palette[0][0] = backdrop
    for i, cell in enumerate(cells):
        pal = int(groups[i]); idx = cell_idx[i]
        encoded = planar_tile(idx)
        key = encoded  # palette is stored per map entry, not duplicated in patterns
        if key not in tile_lookup: tile_lookup[key] = len(tiles); tiles.append(encoded)
        names.append((tile_lookup[key], pal))
        y, x = divmod(i, w // 8)
        preview[y * 8:y * 8 + 8, x * 8:x * 8 + 8, :3] = np.where(
            idx[..., None] > 0, vce_rgb(palette[pal])[idx], vce_rgb(backdrop))
        preview[y * 8:y * 8 + 8, x * 8:x * 8 + 8, 3] = 255
    if len(tiles) > 65535: raise ValueError(f'{name}: too many background characters')
    n = np.asarray(names, np.uint16).reshape(30, w // 8, 2).transpose(1, 0, 2)
    mapping = b''.join(struct.pack('<HB', int(t), int(p)) for t, p in n.reshape(-1, 2))
    pal_off = archive.add('bg_palette', np.asarray(palette, '<u2').tobytes())
    tile_off = archive.add('bg_patterns', b''.join(tiles))
    map_off = archive.add('bg_columns', mapping)
    Image.fromarray(preview).save(previews / f"{name}.png")
    return dict(pal=pal_off, tiles=tile_off, map=map_off, cols=w // 8, tile_count=len(tiles))

BOSS_BG_W,BOSS_BG_H=180,98     # the battle cruiser (224x123 in the source) at four fifths: the screen is 256 dots wide, not 320
BOSS_COLS,BOSS_ROWS=23,13

def boss_background(archive, previews):
    """The stage 7 cruiser as background characters (a 23x13 character picture on a black playfield), moved about with the scroll
    registers instead of as sprites. Record: u16 tile count, u8 columns, u8 rows; per column the first and last solid row (the hull's
    outline, 255: none), 4 palettes, the character map (tile | palette << 12, tile 0 blank) and the tiles themselves."""
    im = Image.open(ROOT / 'assets/space/boss.png').convert('RGBa').resize((BOSS_BG_W, BOSS_BG_H), Image.Resampling.LANCZOS).convert('RGBA')
    canvas = Image.new('RGBA', (BOSS_COLS * 8, BOSS_ROWS * 8), (0, 0, 0, 0)); canvas.paste(im, (0, 0))
    px = np.asarray(canvas).copy()
    px[..., 3] = np.where(px[..., 3] >= 128, 255, 0)
    cells = px.reshape(BOSS_ROWS, 8, BOSS_COLS, 8, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 8, 8, 4)
    palettes, groups = palfit.fit_palettes(cells, 4, verbose=False)
    idx = palfit.index_cells(cells, palettes, groups)
    tiles, lookup, entries = [planar_tile(np.zeros((8, 8), np.uint8))], {}, []
    lookup[tiles[0]] = 0
    for k in range(len(cells)):
        enc = planar_tile(idx[k])
        if enc not in lookup: lookup[enc] = len(tiles); tiles.append(enc)
        entries.append(lookup[enc] | (int(groups[k]) << 12) if idx[k].any() else 0)
    solid = px[..., 3] > 0
    top = bytearray([255] * 24); bottom = bytearray([255] * 24)
    for c in range(BOSS_COLS):
        rows = np.nonzero(solid[:, c * 8:c * 8 + 8].any(1))[0]
        if len(rows): top[c], bottom[c] = int(rows[0]), int(rows[-1])
    # The nose cannon's beam as background characters (palette 4, kept after the tiles): five tiles whose lines are the beam's vertical
    # profile (1 white core, 2 pale yellow, 3 orange, 4 red edge): rows 5-6 make the 11-dot beam, rows 4-7 the 22-dot one of the last stage.
    def beam_tile(profile):
        t = np.zeros((8, 8), np.uint8)
        for line, v in profile.items(): t[line, :] = v
        return planar_tile(t)
    beam = [beam_tile({3: 4, 4: 3, 5: 2, 6: 1, 7: 1}), beam_tile({0: 1, 1: 1, 2: 2, 3: 3, 4: 4}),    # top / bottom half of the thin beam
            beam_tile({k: 1 for k in range(8)}), beam_tile({5: 4, 6: 3, 7: 2}), beam_tile({0: 2, 1: 3, 2: 4})]   # body, top / bottom edge of the wide one
    tiles += beam
    beam_pal = np.zeros(16, '<u2')
    for k, c in enumerate([(255, 255, 255), (255, 255, 150), (255, 150, 40), (210, 40, 20)]): beam_pal[k + 1] = vce_colors(np.asarray(c, np.uint8))
    blob = bytearray(struct.pack('<HBB', len(tiles), BOSS_COLS, BOSS_ROWS)) + top + bottom
    blob += bytes(64 - len(blob))
    blob += np.asarray(palettes, '<u2').tobytes()                       # offset 64: 4 palettes of 16 words
    blob += np.asarray(entries, '<u2').tobytes().ljust(640, b'\0')       # offset 192: the map, row by row
    blob += b''.join(tiles)                                             # offset 832 (the last five are the beam's), then the beam's palette
    blob += beam_pal.tobytes()
    preview = np.zeros((BOSS_ROWS * 8, BOSS_COLS * 8, 3), np.uint8)
    for k in range(len(cells)):
        y, x = divmod(k, BOSS_COLS)
        preview[y * 8:y * 8 + 8, x * 8:x * 8 + 8] = vce_rgb(palettes[groups[k]])[idx[k]] * (idx[k][..., None] > 0)
    Image.fromarray(preview).save(previews / 'boss_bg.png')
    print(f'  stage 7 cruiser: {len(tiles)} characters', flush=True)
    return archive.add('boss_bg', bytes(blob), 32), len(tiles)

ROAD_LINES = 112          # image lines: scanlines 112..223 of the road region (the horizon is scanline 113)
ROAD_STRIPE_FIRST = 24    # image line where the two stripe phases start (the lines above are the haze rows)
def road_assets(sand, previews):
    """The classic racing road (see docs/PCE_CLASSIC_ROAD_20261005.md): one static picture of a straight road in perspective,
    1024 dots wide with its centre at dot 512, one image line for each scanline below the horizon. Edges are drawn at integer
    or quarter slopes (dots per line), so the sloped tiles repeat from row to row and the whole picture is about a hundred
    characters. The race scrolls the picture sideways one scanline at a time (the road's curve and the camera's offset), and
    shows each band of the road in one of two colour phases by choosing which copy of the BAT rows a scanline reads (BYR):
    the copies use the same characters with different palettes. The far rows fade into the haze of the sky."""
    W, C = 1024, 512
    SLOPE_KERB, SLOPE_ROAD, SLOPE_EDGE, SLOPE_DASH = 4.5, 3.75, 3.5, 0.1   # dots a line: kerb outside, road, white edge line, centre dash
    # (the source map's widths, 120/102 units, at 0.0376 dots a unit and line: the kerb at the nearest line is 495 dots from the middle, just
    # inside the picture. When a scanline's window runs past the picture's edge the wrapped part would show the road's other kerb, so the
    # lower rows have two more copies where the half that would wrap in is sand: see the variants below)
    # 1 sand, 2 kerb, 3 edge line, 4 asphalt, 5 centre dash
    img = np.ones((ROAD_LINES, W), np.uint8)
    ax = np.abs(np.arange(W) - C + 0.5)
    for i in range(ROAD_LINES):
        d = i - 1
        if d <= 0: continue
        img[i, ax < SLOPE_KERB * d] = 2
        img[i, ax < SLOPE_ROAD * d] = 3
        img[i, ax < SLOPE_EDGE * d] = 4
        img[i, ax < SLOPE_DASH * d] = 5
    tiles, lookup = [], {}
    ids = np.zeros((ROAD_LINES // 8, W // 8), np.uint16)
    for r in range(ROAD_LINES // 8):
        for c in range(W // 8):
            enc = planar_tile(img[r * 8:r * 8 + 8, c * 8:c * 8 + 8])
            if enc not in lookup: lookup[enc] = len(tiles); tiles.append(enc)
            ids[r, c] = lookup[enc]
    assert len(tiles) <= 256, f'Road picture needs {len(tiles)} characters'
    sand = np.asarray(sand, float)
    haze = np.array((216, 144, 108.))   # (already one of the mountains' colours)
    asphalt_a, asphalt_b = np.array((66, 66, 78.)), np.array((80, 80, 94.))
    red, white = np.array((226, 44, 40.)), np.array((236, 236, 244.))
    phase_a = [sand, red, white, asphalt_a, white]
    phase_b = [sand * 0.86, white, white, asphalt_b, asphalt_b]
    def palette(colours, mix):
        out = np.zeros(16, '<u2')
        for k, c in enumerate(colours):
            out[k + 1] = vce_colors(np.clip(np.asarray(c) * (1 - mix) + haze * mix, 0, 255).astype(np.uint8))
        return out
    # hardware palettes 1..8: haze, far haze, then the phase colours fading in over the first rows
    palettes = [palette(phase_a, 1.0), palette(phase_a, 0.6), palette(phase_a, 0.4), palette(phase_a, 0.2), palette(phase_a, 0.0),
                palette(phase_b, 0.4), palette(phase_b, 0.2), palette(phase_b, 0.0)]
    def row_palette(r, b):   # image row r (0..13), phase b
        if r < 2: return 1
        if r == 2: return 2
        if r == 3: return 6 if b else 3
        if r == 4: return 7 if b else 4
        return 8 if b else 5
    bat = bytearray()
    for b in (0, 1):
        for r in range(2, ROAD_LINES // 8):
            bat += b''.join(struct.pack('<H', 0x200 + int(t) | row_palette(r, b) << 12) for t in ids[r])
    for r in (0, 1):
        bat += b''.join(struct.pack('<H', 0x200 + int(t) | row_palette(r, 0) << 12) for t in ids[r])
    # Variants of the lower image rows (8..13: scanlines 176..223), after the haze rows: in a window that runs past the picture's right
    # edge (the road far to the left of the screen) the part that wraps in comes from the picture's left half, and the other way round, so
    # the half that is not shown by itself is sand. BAT rows 24-47: right-wrap (phase A, B) then left-wrap (phase A, B); irq.S picks the
    # copy from the sign and size of the scanline's BXR (road_fill.S road_stripes).
    for side in (0, 1):
        for b in (0, 1):
            for r in range(8, ROAD_LINES // 8):
                row = ids[r].copy()
                if side == 0: row[:W // 16] = ids[r, 0]
                else: row[W // 16:] = ids[r, W // 8 - 1]
                bat += b''.join(struct.pack('<H', 0x200 + int(t) | row_palette(r, b) << 12) for t in row)
    preview = np.zeros((ROAD_LINES, W, 3), np.uint8)
    pa = vce_rgb(palettes[4]); preview[:] = pa[img]
    Image.fromarray(preview).save(previews / 'road.png')
    print(f'  road picture: {len(tiles)} characters', flush=True)
    return dict(tiles=b''.join(tiles).ljust(8192, b'\0'), bat=bytes(bat), palette=b''.join(p.tobytes() for p in palettes))

def road_tables():
    """C tables for the road builder (bank $6f): the scanline of a distance (half units of distance, d = 10080 / f) and the
    distance of a scanline (z = 10080 / d; only its low byte, which is all a 32-unit band needs)."""
    d = [min(255, round(10080 / max(2 * k, 1))) for k in range(384)]
    z = [65535] + [min(65535, round(10080 / k)) for k in range(1, ROAD_LINES + 1)]
    return d, z

MTN_TOP = 56   # the race sky's near layer starts at this scanline (irq.S: the RCR that scrolls the mountains faster than the clouds)
def race_mountains(haze, top, bottom):
    """The race's mountain range as an RGBA strip (bottom - top rows, 512 dots, alpha 0 or 255): the PC's mesas and boulders (assets/mode7.png, 2:1 for the
    512-dot clock) in a far range and a nearer one, both hazed toward the horizon colour and more so at their feet."""
    im = Image.open(ROOT / 'assets/mode7.png').convert('RGBA')
    mesa = im.crop((862, 57, 862 + 114, 57 + 54)); rock = im.crop((797, 57, 797 + 64, 57 + 61))
    rows = bottom - top
    out = np.zeros((rows, 512, 4), np.float64)
    def layer(items, haze_t, lift):
        for src, x, height, flip in items:
            s = src.transpose(Image.Transpose.FLIP_LEFT_RIGHT) if flip else src
            w = max(4, round(s.width * height / s.height * 2))
            a = np.asarray(s.resize((w, height), Image.Resampling.LANCZOS)).astype(np.float64)
            for dx in (-512, 0, 512):
                x0 = x + dx - w // 2; y0 = rows - height - lift
                for yy in range(height):
                    for xx in range(w):
                        px, py = x0 + xx, y0 + yy
                        if 0 <= px < 512 and 0 <= py < rows and a[yy, xx, 3] >= 128:
                            t = haze_t + max(0, py - (rows - 14)) / 14 * 0.35
                            out[py, px, :3] = a[yy, xx, :3] * (1 - t) + np.asarray(haze) * t; out[py, px, 3] = 255
    layer([(mesa, 40, 22, False), (rock, 150, 17, True), (mesa, 262, 26, True), (rock, 372, 19, False), (mesa, 470, 23, False)], 0.55, 3)
    layer([(mesa, 96, 32, True), (rock, 208, 22, False), (mesa, 318, 36, False), (rock, 432, 26, True)], 0.30, 0)
    return out

def race_sky(archive, previews, sand):
    """The Grand Prix's sky as plain background tiles: a blue gradient from the zenith to a pale horizon (scanline 113), then
    the haze of the ground out to the floor's first scanline (120). One 15-colour palette: 13 shades of the gradient and the
    haze; ordered dithering (8x8 Bayer, 64 steps between neighbouring shades) keeps the bands invisible, and because the
    dither repeats every 8 dots a tile row is the same all across, so the whole sky is 16 distinct characters."""
    HORIZON = 128   # the sky's feet: the road's far rows are fully hazed down to here (road_assets), so the sky and its mountains fill that stretch (irq.S starts the road's scanlines at 128)
    def word(c): return int(vce_colors(np.clip(np.asarray(c, float), 0, 255).astype(np.uint8)))
    zenith, pale = np.array((12, 60, 188.)), np.array((200, 220, 255.))
    haze = np.array((216, 144, 108.))   # the ground's colour at the horizon, one of the mountains' own
    # The hardware has 8 levels a channel, so shades picked one by one wander in hue (a teal band appeared). The shades are
    # instead the lattice points of a straight walk from the zenith to the horizon colour, 12 steps of at most one level a
    # channel, and the dither between neighbours is then a one-level difference.
    z_lat, p_lat = np.round(zenith * 7 / 255), np.round(pale * 7 / 255)
    # (green rounds down and blue up, so that no step has more green than a sky has)
    walk = []
    for k in range(13):
        f = k / 12
        r = np.floor(z_lat[0] + (p_lat[0] - z_lat[0]) * f + 0.5); g = np.floor(z_lat[1] + (p_lat[1] - z_lat[1]) * f)
        bl = np.ceil(z_lat[2] + (p_lat[2] - z_lat[2]) * f)
        walk.append(np.array((r, g, bl)) * 255 / 7)
    palette = np.zeros(16, '<u2')
    for k, c in enumerate(walk): palette[k + 1] = word(c)
    palette[15] = word(haze)
    bayer = np.array([[0]])
    for _ in range(3): bayer = np.block([[4 * bayer, 4 * bayer + 2], [4 * bayer + 3, 4 * bayer + 1]])   # 8x8 ordered dither, 64 levels
    rows = []
    for r in range(16):
        tile = np.zeros((8, 8), np.uint8)
        for dy in range(8):
            y = r * 8 + dy
            for x in range(8):
                if y >= HORIZON - 16: tile[dy, x] = 15   # the last sixteen lines (scanlines 112-127; screenshot rows 123-138) are the ground's haze colour wherever there is no mountain: no sky shows under the mountains' feet
                else:
                    pos = (y / (HORIZON - 1)) ** 1.45 * 12
                    k = min(int(pos), 11); frac = pos - k
                    tile[dy, x] = k + 1 + (1 if frac * 64 > bayer[y % 8, x] else 0)
        rows.append(tile)
    pixels = np.tile(np.concatenate(rows, axis=0), (1, 64))
    # Wisps from the source panorama, using its cloud-only stretch. Keep the blue gradient and omit the planet. The clouds stay in the far layer
    # (the upper 56 scanlines): the near layer (scanline 56 down, irq.S) scrolls twice as fast and carries the mountains.
    cloud = np.asarray(Image.open(ROOT/'assets/sky_mode7.png').convert('RGB').crop((340,0,768,80)).resize((512,80),Image.Resampling.LANCZOS))
    light = np.maximum(cloud[...,0].astype(float)-35,0) / 220
    pixels[:MTN_TOP] = np.minimum(13, pixels[:MTN_TOP] + (light[:MTN_TOP]*5).astype(np.uint8))
    # The mountains: the PC's own mesa and boulder art (assets/mode7.png), shrunk and stood in two hazed ranges along the horizon (far, pale; near, warmer),
    # wrapping at the panorama's 512 dots. They get a hardware palette of their own (palette 9: the gradient's shades that reach down here, and the range's
    # own colours fitted to it), flat shaded, so the tiles repeat enough for the 256 characters of the sky.
    rgb = vce_rgb(palette)
    strip = race_mountains(haze, MTN_TOP, HORIZON)
    region = pixels[MTN_TOP:HORIZON].copy()
    shades = sorted(set(int(v) for v in np.unique(region)))
    solid = strip[..., 3] > 0
    # (the range's colours, by hand on the hardware's lattice: a shaded side stays brown and a lit one gold, which a fit to the haze-blended pixels turned olive;
    # as many as there are free entries, darkest first)
    MESA_COLOURS = ((109, 73, 73), (146, 109, 109), (182, 109, 73), (219, 146, 73), (219, 182, 109), (219, 219, 182), (182, 146, 109), (146, 73, 36), (73, 36, 36), (255, 219, 146))
    mtn_pal = np.zeros(16, '<u2'); mtn_pal[1:16 - len(shades)] = [word(c) for c in MESA_COLOURS[:15 - len(shades)]]
    mtn_rgb = vce_rgb(mtn_pal[1:16 - len(shades)]).astype(float)
    pal2 = np.zeros(16, '<u2')
    for k, v in enumerate(shades): pal2[k + 1] = palette[v]
    for k, c in enumerate(mtn_pal[1:16 - len(shades)]): pal2[len(shades) + 1 + k] = c
    remap = np.zeros(16, np.uint8)
    for k, v in enumerate(shades): remap[v] = k + 1
    out = remap[region]
    near = ((strip[..., None, :3] - mtn_rgb[None, None]) ** 2).sum(-1).argmin(-1).astype(np.uint8) + len(shades) + 1
    out[solid] = near[solid]
    pixels[MTN_TOP:HORIZON] = out
    sky_rgb = np.zeros((pixels.shape[0], 512, 3), np.uint8)
    sky_rgb[:MTN_TOP] = rgb[pixels[:MTN_TOP]]
    sky_rgb[MTN_TOP:HORIZON] = vce_rgb(pal2)[pixels[MTN_TOP:HORIZON]]
    palette[14] = word(np.asarray(sand)*0.68 + np.array((110,135,180))*0.32)
    tiles, lookup, columns = [], {}, []
    for x in range(64):
        column = bytearray()
        for y in range(30):
            enc = planar_tile(pixels[min(y,15)*8:min(y,15)*8+8,x*8:x*8+8])
            if enc not in lookup: lookup[enc]=len(tiles);tiles.append(enc)
            column += struct.pack('<HB',lookup[enc],9 if MTN_TOP // 8 <= y < 16 else 0)
        columns.append(column)
    assert len(tiles)<=256, f'Race panorama exceeds sky VRAM: {len(tiles)} characters'
    pals = [palette] * 16; pals[9] = pal2
    pal_off = archive.add('bg_palette', np.asarray(pals, '<u2').tobytes())
    tile_off = archive.add('bg_patterns', b''.join(tiles))
    map_off = archive.add('bg_columns', b''.join(columns))
    print(f'race sky: {len(tiles)} characters, {len(shades)} gradient shades + {15 - len(shades)} mountain colours')
    preview = np.tile(vce_rgb(palette)[pixels[-8:]], (30,1,1))
    preview[:128] = sky_rgb[:128]
    Image.fromarray(preview.astype(np.uint8)).save(previews / 'stage2.png')
    return dict(pal=pal_off, tiles=tile_off, map=map_off, cols=64, tile_count=len(tiles))

NIGHT_TINT = {'FarMountains': (58, 62, 118), 'Mountains': (66, 70, 130), 'NearMountains': (76, 80, 142), 'MidBG': (88, 90, 150),
              'Cars MidBG': (88, 90, 150), 'Playfield': (112, 110, 168), 'Platforms': (116, 114, 172), 'Cars': (116, 114, 172),
              'ForegroundStuff': (100, 98, 156), 'ForegroundStuf2': (52, 52, 92)}

def continuous_layer(ly, bank, width):
    """Bake intact source scenery at native scale, without camera-sector joins."""
    if 'Mountains' not in ly.name:
        # Buildings keep their original placement relative to scenery actors.
        # These layers have empty gaps where the sector offset can change.
        p = ly.parallax
        full = levl.render_layer(ly, bank, 0.0, max(ly.w * bank.tw, width), 240, None)
        occupied = np.concatenate(([0], np.cumsum((full[..., 3] >= 128).sum(0))))
        n = width // 256
        shift = [int(round((1 - p) * 256 * k)) for k in range(n)]
        cuts = [0]
        for k in range(1, n):
            best = None
            for cand in range(256 * k - 112, 256 * k + 112):
                lo, hi = max(cand - shift[k], 0), max(min(cand - shift[k - 1], full.shape[1]), 0)
                cost = int(occupied[hi] - occupied[min(lo, hi)]) if hi > lo else 0
                if best is None or (cost, abs(cand - 256 * k)) < best[:2]: best = (cost, abs(cand - 256 * k), cand)
            cuts.append(best[2])
        cuts.append(width)
        out = np.zeros((240, width, 4), np.uint8)
        for k in range(n):
            for x in range(cuts[k], cuts[k + 1]):
                u = x - shift[k]
                if 0 <= u < full.shape[1]: out[:, x] = full[:, u]
        return out
    # Keep hills at their original width and texture scale. A continuous native
    # strip trades their slow motion for an intact silhouette on the single VDC.
    # Short source layers repeat only at their own tilemap boundary, never at
    # arbitrary 256-pixel camera sectors.
    full = levl.render_layer(ly, bank, 0.0, ly.w * bank.tw, 240, None)
    strip = np.concatenate((full, full[:, ::-1]), axis=1)
    return np.tile(strip, (1, math.ceil(width / strip.shape[1]), 1))[:, :width]

def flat_sky_rows(ly, bank):
    """Stage 4's wavy sky as flat horizontal bands (the commonest colour of each scanline, 256 dots wide). The waves made
    every sky character unique, which overran the VRAM tile cache while scrolling; and a layer moving at 0.03 of the camera
    left black gaps wherever the sector shift outran it. A band is one character repeated across the whole level."""
    full = levl.render_layer(ly, bank, 0.0, ly.w * bank.tw, 240, None)
    rows = np.zeros((240, 256, 4), np.uint8)
    for y in range(240):
        px = full[y][full[y][:, 3] >= 128].reshape(-1, 4)
        if len(px):
            colours, counts = np.unique(px, axis=0, return_counts=True)
            rows[y, :] = colours[counts.argmax()]
    return rows

def platform_background(stage, work):
    level, files = levl.load_dump(work / f'stage{stage}.layers')
    banks = {i: levl.png_bank(path, i) if path else levl.load_bank(work / 'srgb' / f'{i:08X}.srgb')
             for i, path in files.items()}
    width = math.ceil(level.width / 256) * 256
    out = Image.new('RGBA', (width, 240), (0, 0, 0, 255))
    foreground = Image.new('RGBA',(width,240))
    continuous = {}
    after = False
    flat_sky = None
    for ly in level.layers:
        if ly.name == 'PlayerSprites': after = True
        if stage == 4 and ly.name == 'SkyBG': flat_sky = flat_sky_rows(ly, banks[ly.cblock]); continue
        if ly.is_tilemap and not after and 0 < ly.parallax < 1 and ly.extra != 1:
            continuous[ly.name] = continuous_layer(ly, banks[ly.cblock], width)
    for x in range(0,width,256):
        pixels=np.zeros((240,256,4),np.uint8)
        front=np.zeros_like(pixels);after_player=False
        # Stages 1 and 3 leave the sky transparent for the fixed scene backdrop.
        for ly in level.layers:
            if ly.name=='PlayerSprites':after_player=True
            if not ly.is_tilemap:continue
            if stage in (1,3) and ly.name=='SkyBG':continue
            if stage==4 and ly.name=='ForegroundStuf2':continue   # the plants: they flickered as sprites and cost a tile each as background
            bank=banks[ly.cblock]
            if stage==4 and ly.name=='SkyBG': layer=flat_sky
            elif after_player: layer=levl.render_layer(ly,bank,x,256,240,None)
            elif ly.name in continuous: layer=continuous[ly.name][:,x:x+256].copy()
            else: layer=levl.render_layer(ly,bank,levl.layer_offset(ly,x),256,240,None)
            if stage==3:
                # Stage 3 is the night variant of level 1: each layer is drawn in the night colour of night.c night_layer_tint
                # (the source multiplies the layer by r/255, g/255, b/255).
                tint=np.array(NIGHT_TINT.get(ly.name,(100,100,160)),np.int32)
                layer[...,:3]=(layer[...,:3].astype(np.int32)*tint//255).astype(np.uint8)
            if after_player: front=np.where(layer[...,3:4]>=128,layer,front)
            else: pixels=np.where(layer[...,3:4]>=128,layer,pixels)
        out.paste(Image.fromarray(pixels),(x,0))
        foreground.paste(Image.fromarray(front),(x,0))
    if stage == 1: out = camera_wall_background(out, work)
    return out,foreground

def add_sprites(archive, sprites, previews):
    rows, costs = [], []
    fg=[im for name,im,_ in sprites if name.startswith('foreground_')]
    fg_palette=palette_for(fg) if fg else None
    # The HUD pieces of a stage share one palette (the cache gives them the shared-palette slots, see sprite_cache_pce.c).
    hud=[im for name,im,_ in sprites if name.startswith('hudp_')]
    hud_palette=palette_for(hud,unique=True) if hud else None
    power_palette={}
    for h in range(4):   # the portrait's 14 colours, then white for the name beside it (all of a hero's parts share the palette)
        parts=[im for name,im,_ in sprites if name.startswith(f'pwr{h}_') and not name.endswith('name')]
        if not parts: continue
        face=parts[0].crop((0,0,64,50))   # the face counts ten times in the choice of colours: it is small and its skin tones and eyes would otherwise be lost to the clothes
        pal=palette_for(parts+[face]*10,14);pal[15]=vce_colors(np.array((255,255,255),np.uint8))   # (the name's outline takes the darkest entry, the art's own outline colour)
        power_palette[h]=pal
    boss=[im for name,im,_ in sprites if name in ('gunship_left','gunship_right','hyperjumper_left','hyperjumper_right')]
    boss_palette=palette_for(boss) if boss else None
    for name, im, anchor in sprites:
        pat, parts, palette, line = pack_sprite(im, anchor,boss_palette if name in ('gunship_left','gunship_right','hyperjumper_left','hyperjumper_right') else fg_palette if name.startswith("foreground_") else hud_palette if name.startswith('hudp_') else power_palette[int(name[3])] if name.startswith('pwr') else None)
        if (not parts and name != 'battle_cruiser') or len(parts)>32:
            raise ValueError(f'{name}: expected 1..32 visible sprite pieces, got {len(parts)}')
        offset = archive.add(name + '_patterns', pat)
        desc = archive.add(name + '_pieces', b''.join(struct.pack('<hhH', *p) for p in parts))
        pal = archive.add(name + '_palette', palette.tobytes())
        rows.append((offset, desc, pal, len(parts), im.width, im.height))
        costs.append(dict(name=name, patterns=len(pat), entries=len(parts), units_per_line=line,
                          width=im.width, height=im.height, facing_variants=1))
    table = archive.add('sprite_table', b''.join(struct.pack('<IIIHBB', *r) for r in rows))
    return table, rows, costs

JUMP_RUN_FRAME = 1

def heroes(work):
    out = []
    fire = (work / '9C8F9A9E.levl').read_bytes()
    fid = struct.unpack_from('<I', fire, 4)[0]
    for hero, filename in enumerate(('saber.png', None, 'april.png', 'colt.png')):
        sheet = Image.open(ROOT / 'assets' / filename).convert('RGBA') if filename else None
        def frame(n):
            if sheet:
                x, y = n % 8 * 64, n // 8 * 64
                return sheet.crop((x, y, x + 64, y + 64))
            return cblock_frame(work / 'srgb' / f'{fid:08X}.srgb', n)
        # The right-facing idle and run source cells. Left is SAT X flip.
        idle = 168 if hero == 0 else 176 if hero == 2 else 4
        out.append((f'hero{hero}_idle', frame(idle), (32, 32)))
        run_frames = []
        for k in range(6):
            legs = frame(104 + k)
            bob=(0,1,2,0,1,2)[k] if hero==0 else (1 if k in (2,5) else 0) if hero in (1,3) else 0
            a = np.asarray(legs).copy(); a[:38 + bob if hero==0 else 0] = 0
            merged = Image.fromarray(a)
            merged.alpha_composite(frame(88 + k), (0, bob))
            out.append((f'hero{hero}_run{k}', merged, (32, 32))); run_frames.append(merged)
        # Airborne = one run frame held (saves the somersault's sprite budget).
        out.append((f'hero{hero}_jump', run_frames[JUMP_RUN_FRAME], (32, 32)))
        out.append((f'hero{hero}_crouch', frame(120), (32, 32)))
    # A native projectile and blast can coexist with every stage palette.
    # The shots are the source's own 8x8 orbs (blue for the heroes, red for the Outriders), centred on the shot.
    for name, rid in [('shot', 0xF0FB3C78), ('enemy_shot', 0x7027A26E)]:
        out.append((name, cblock_frame(work / 'srgb' / f'{rid:08X}.srgb', 0), (4, 4)))
    for name, color, radius in [('blast', (255, 160, 30, 255), 7)]:
        im = Image.new('RGBA', (16, 16)); ImageDraw.Draw(im).ellipse((8-radius, 8-radius, 8+radius, 8+radius), fill=color)
        out.append((name, im, (8, 8)))
    for name, crhc in [('walker', '02A38AFB'), ('grunt', '112DF34C'), ('sniper', 'D39700C4')]:
        d = (work / f'{crhc}.levl').read_bytes(); aid = struct.unpack_from('<I', d, 4)[0]
        # Animation 2 is the canonical right-facing idle.
        first = struct.unpack_from('<I', d, 0x34 + 2 * 24 + 4)[0]
        im = cblock_frame(work / 'srgb' / f'{aid:08X}.srgb', first)
        ox, oy = struct.unpack_from('<ff', d, 8)
        out.append((name, im, (round(ox), round(oy))))
    return out

def atlas(path, kind, max_width=None):
    sheet = Image.open(path).convert('RGBA')
    out = []
    for row in path.with_suffix('.txt').read_text().splitlines():
        fields = row.split()
        if len(fields) != 8 or fields[0] != kind: continue
        _, n, x, y, w, h, ax, ay = fields
        n, x, y, w, h, ax, ay = map(int, (n, x, y, w, h, ax, ay))
        im = sheet.crop((x, y, x+w, y+h))
        if max_width:
            nh = round(h * max_width / w); im = im.resize((max_width, nh), Image.Resampling.NEAREST)
            ax, ay = round(ax * max_width / w), round(ay * max_width / w)
        out.append((f'{kind}{n}', im, (ax, ay)))
    return out

def platform_dialog(archive, work):
    records=[]
    corner_records=[]
    original=[]
    masks=[]
    for ch in range(32,128):
        im=cblock_frame(work/'srgb'/'12072E60.srgb',ch-0x21) if ch>32 else Image.new('RGBA',(8,8))
        mask=np.asarray(im)[...,3]>=64
        masks.append(mask);original.append(planar_tile((mask*15).astype(np.uint8)))
    for rid in presentation.BOX_TILESETS:
        box=presentation.dialog_box([cblock_frame(work/'srgb'/f'{rid:08X}.srgb',n) for n in range(9)])
        palette=palette_for([box],colors=14);pixels=indexed(box,palette)
        fill=int(pixels[24,112]);palette[15]=0x1ff
        tiles=[];lookup={};mapping=[]
        for y in range(0,48,8):
            for x in range(0,224,8):
                tile=planar_tile(pixels[y:y+8,x:x+8])
                if tile not in lookup:lookup[tile]=len(tiles);tiles.append(tile)
                mapping.append(0xf400+lookup[tile])
        assert len(tiles)<=16,'Platform dialogue overlaps dedicated corners'
        corner_records.append(palette.tobytes()+b''.join(planar_sprite(pixels[y:y+16,x:x+16]) for y in (0,32) for x in (0,208)))
        pal=archive.add(f'dialog{rid}_palette',palette.tobytes())
        pat=archive.add(f'dialog{rid}_patterns',b''.join(tiles))
        bat=archive.add(f'dialog{rid}_map',struct.pack('<168H',*mapping))
        font=archive.add(f'dialog{rid}_font',b''.join(planar_tile(np.where(m,15,fill).astype(np.uint8)) for m in masks))
        records.append(struct.pack('<4IH',pal,pat,bat,font,len(tiles)*32))
    archive.add('dialog_bg',b''.join(records))
    archive.add('dialog_corners',b''.join(corner_records))
    archive.add('dialog_original_font',b''.join(original))

def race_dialog(archive, work):
    """Wide BAT dialogue; glyphs occupy unused race BAT rows 24..47.

    The box uses at most 32 characters at $4000, below the normal font.
    Sky characters stay below $4000 and sprite patterns start at $4800.
    """
    records=[]
    for rid in presentation.BOX_TILESETS:
        box=presentation.dialog_box([cblock_frame(work/'srgb'/f'{rid:08X}.srgb',n) for n in range(9)])
        box=box.resize((448,48),Image.Resampling.NEAREST)
        palette=palette_for([box],colors=14)
        # The flat centre colour is also the opaque background of each glyph.
        pixels=indexed(box,palette)
        fill=int(pixels[24,224]);palette[15]=0x1ff
        tiles=[];lookup={};mapping=[]
        for y in range(0,48,8):
            for x in range(0,448,8):
                tile=planar_tile(pixels[y:y+8,x:x+8])
                if tile not in lookup:lookup[tile]=len(tiles);tiles.append(tile)
                mapping.append(0xe400+lookup[tile])
        assert len(tiles)<=32,'Race dialogue overlaps normal font'
        font=[]
        for ch in range(32,128):
            im=cblock_frame(work/'srgb'/'12072E60.srgb',ch-0x21) if ch>32 else Image.new('RGBA',(8,8))
            mask=np.repeat(np.asarray(im)[...,3]>=64,2,axis=1)
            glyph=np.where(mask,15,fill).astype(np.uint8)
            font.extend(planar_tile(glyph[:,x:x+8]) for x in (0,8))
        pal=archive.add(f'dialog{rid}_palette',palette.tobytes())
        pat=archive.add(f'dialog{rid}_patterns',b''.join(tiles))
        bat=archive.add(f'dialog{rid}_map',struct.pack('<336H',*mapping))
        glyphs=archive.add(f'dialog{rid}_font',b''.join(font))
        records.append(struct.pack('<4IH',pal,pat,bat,glyphs,len(tiles)*32))
    archive.add('dialog_wide',b''.join(records))

def power_wave():
    """The hero power's blue wave (power_pce.c): 14 ramp colours (VCE words) then 64 background characters. The picture is a field of
    slanted curling crests, 64 dots across and 48 down, so eight characters across and six down tile it; a contour of the field is one
    palette entry, and the cut-in rotates the 14 ramp colours through palette 15 (entries 1-14) so the crests flow. Entry 15 is the
    black outline. Characters: kind (0-5: the picture's character rows), 6 the band's top row (two black lines), 7 its bottom row
    (kind 1 with the last two lines black); character = kind * 8 + the column & 7."""
    ramp = []
    for i in range(14):
        if i >= 13: c = (255, 255, 255)
        elif i == 12: c = (200, 244, 255)
        else:
            a = np.array((8, 28, 110.)); b = np.array((60, 170, 255.)); c = tuple(int(v) for v in a + (b - a) * (i / 11) ** 1.4)
        ramp.append(int(vce_colors(np.array(c, np.uint8))))
    y, x = np.mgrid[0:48, 0:64]
    s = 14 * y / 48 + 3.0 * np.sin(2 * np.pi * x / 64 + 2 * np.pi * y / 48) + 1.5 * np.sin(2 * np.pi * 2 * x / 64 - 2 * np.pi * 2 * y / 48)
    field = (np.floor(s).astype(int) % 14 + 1).astype(np.uint8)
    tiles = []
    for kind in range(8):
        row = kind if kind < 6 else 0 if kind == 6 else 1
        for col in range(8):
            t = field[row * 8:row * 8 + 8, col * 8:col * 8 + 8].copy()
            if kind == 6: t[:2] = 15
            if kind == 7: t[6:] = 15
            tiles.append(planar_tile(t))
    return struct.pack('<14H', *ramp) + b''.join(tiles)

def font_glyphs(work):
    """The 96 ASCII glyphs of the dialogue font (font.bin; palette 15)."""
    glyphs = []
    font_path = work/'srgb'/'12072E60.srgb'
    for ch in range(32, 128):
        px = np.asarray(cblock_frame(font_path, ch - 0x21)) if ch > 32 and ch - 0x21 < 106 else np.zeros((8,8,4), np.uint8)
        glyphs.append(planar_tile(((px[..., 3] >= 64) * 15).astype(np.uint8)))
    return b''.join(glyphs)

def make_scene(stage, work, previews, shared):
    a = Archive()
    meta = dict(stage=stage)
    if stage in (1, 3, 4, 5):
        meta.update(json.loads((work / f'stage{stage}.json').read_text()))
        bg,foreground = platform_background(stage, work)
        # Native collision matrix is column-major for a hot 32-column cache.
        raw = np.frombuffer((work / f'stage{stage}.collision').read_bytes(), np.uint8).reshape(meta['rows'], meta['cols'])
        collision = a.add('collision_columns', raw.T.tobytes())
        triggers = []
        for t in meta['triggers']:
            points=[]
            for x,y in t['waypoints'][:8]:
                points += [max(-32767,min(32767,x)),max(-32767,min(32767,y))]
            if not points: points=[0,0]
            triggers.append(struct.pack('<4h3HBbB', *t['zone'], max(1,round(t['interval']*60/1000)),t['delay'],t['type'],min(255,t['rand']),max(-1,min(127,t['loops'])),len(points)//2)
                            +struct.pack('<16h',*(points+[0]*(16-len(points)))))
        if len(triggers)>60 or meta['rows']>32 or (meta['cellw'],meta['cellh'])!=(8,8):
            raise ValueError('Platform stage exceeds the native hot-cache limits')
        meta['trigger_offset']=a.add('triggers',b''.join(triggers));meta['ntr']=len(triggers)
        zones=meta['dialogs'] if stage==1 else []
        deaths=meta['deathzones'] if stage==1 else []
        stops=meta['stops'] if stage==1 else []
        rules=bytes([len(zones),len(deaths),len(stops)])
        # Dialogue zone + camera focus x (0 = none) + ticks to hold before / after the text (60 Hz).
        # + the voice tone (audio_effect) the script's first line names: the sample plays when the box opens.
        voices={0:0,0xFDB525F9:10}
        rules+=b''.join(struct.pack('<4hhHHH',*z['zone'],z['focus'][0],round(z['hold'][0]*60/1000),round(z['hold'][1]*60/1000),voices[z['sfx']]) for z in zones)
        rules+=b''.join(struct.pack('<6h',*z) for z in deaths)
        rules+=b''.join(struct.pack('<4h',*z) for z in stops)
        meta['rules_offset']=a.add('flow_zones',rules)
        if stage==1: meta['horse_offset']=horse_frames(work,a)

        sprites = list(shared)
        if stage in (1,5):
            d=(work/'2A02BD4F.levl').read_bytes();aid=struct.unpack_from('<I',d,4)[0]
            im=cblock_whole_frame(work/'srgb'/f'{aid:08X}.srgb',0)
            name,anchor,split='gunship',(107,46),112
        else:
            im=Image.open(ROOT/'assets/hyperjumper/side_normal.png').convert('RGBA')
            name,anchor,split='hyperjumper',(65,54),64
        # Hardware 32x32 cells leave SAT room for the full hull and its lasers. The hull exists at full size for the fight
        # and at 3/4 and 1/2 size for the mid and far passes (the ship flies in from the distance): three records,
        # reloaded into the same pattern pages when the ship changes layer.
        hull_records=b''
        def hull_set(hulls,hull_anchor,mirror_from=None):
            """Records of 32x32 cells, one for each picture of `hulls` (the same size, one palette): identical cells share their patterns, so
            the poses of a ship (its engines lit, its gun firing) cost only the cells that differ, and a pose change swaps a piece list
            instead of reloading VRAM. With mirror_from (a symmetric hull on an odd number of cells: the centre cell is its own
            mirror), only the left half and the centre are stored; the right half is the same patterns drawn flipped
            (bit 15 of the pattern number), which halves the VRAM patterns the hull needs."""
            pal=palette_for(hulls);patterns=[];lookup={};lists=[]
            def add_cell(cell):
                key=cell.tobytes()
                if key in lookup:return lookup[key]
                n=len(patterns);lookup[key]=n
                for row in (0,16):
                    for col in (0,16):patterns.append(planar_sprite(cell[row:row+16,col:col+16]))
                return n
            def cell_at(source,x,y):
                cell=source[y:y+32,x:x+32]
                return np.pad(cell,((0,32-cell.shape[0]),(0,32-cell.shape[1])))
            for hull in hulls:
                idx=indexed(hull,pal);pieces=[]
                cols=(hull.width+31)//32
                for y in range(0,hull.height,32):
                    if mirror_from is None:
                        for x in range(0,hull.width,32):
                            cell=cell_at(idx,x,y)
                            if cell.any():pieces.append((x-hull_anchor[0],y-hull_anchor[1],add_cell(cell)))
                        continue
                    assert cols%2==1 and hull.width==cols*32
                    made={}
                    for c in range(cols//2+1):
                        cell=cell_at(idx,c*32,y)
                        if cell.any():
                            made[c]=add_cell(cell);pieces.append((c*32-hull_anchor[0],y-hull_anchor[1],made[c]))
                    for c in range(cols//2):                         # the mirrored right half
                        if c in made:pieces.append(((cols-1-c)*32-hull_anchor[0],y-hull_anchor[1],made[c]|0x8000))
                lists.append(pieces)
            assert len(patterns)<=96 and all(len(p)<=28 for p in lists),(len(patterns),[len(p) for p in lists])
            bp=a.add('boss_big_palette',pal.tobytes())
            bt=a.add('boss_big_patterns',b''.join(patterns))
            return [struct.pack('<IIIHB',bp,a.add('boss_big_pieces',b''.join(struct.pack('<hhH',*r) for r in pieces)),bt,len(patterns)*128,len(pieces)) for pieces in lists]
        def hull_record(hull,hull_anchor,mirror_from=None):
            return hull_set([hull],hull_anchor,mirror_from)[0]
        # The level-1 gunship carries a second rider (the source's clone, enemies.c update_boss_rider) whose gun covers the side the
        # hull's does not: record 0 is the full hull with its level gun, record 3 the same hull with the gun aimed down at 45
        # degrees (the rider lowers it when the hero is 60 px below). Both are baked into the hull cells, so the rider costs no
        # sprites of its own and the scanline load is the hull's.
        rider=None
        if name=='gunship':
            art=work/'srgb'/f'{aid:08X}.srgb'
            rider=[]
            for frame_no in (6,12):
                composite=im.copy();composite.alpha_composite(cblock_whole_frame(art,frame_no));rider.append(composite)
        poses=None
        if name=='hyperjumper':
            # Records 0 and 4-6: the side hull normal, boosting (engines lit: moving about) and the two frames of its gun firing (night.c
            # night_draw_layer: the muzzle-flash frame, then the other), one set of patterns (23 cells), four piece lists.
            poses=hull_set([im]+[Image.open(ROOT/f'assets/hyperjumper/{n}.png').convert('RGBA') for n in ('side_boost','side_fire1','side_fire2')],anchor)
        for scale in (1.0,0.75,0.5):
            if poses and scale==1.0:hull_records+=poses[0];continue
            source=rider[0] if rider and scale==1.0 else im
            hull=source if scale==1.0 else source.resize((round(source.width*scale),round(source.height*scale)),Image.Resampling.LANCZOS)
            hull_records+=hull_record(hull,tuple(round(v*scale) for v in anchor))
        if rider:hull_records+=hull_record(rider[1],anchor)
        if name=='hyperjumper':
            # The Hyperjumper's front pose (it drops in facing the hero, fires straight down and leaves upward; night.c front_pose)
            # is a record of its own, the fourth: Arcade RAM keeps it and it replaces the side hull in the pattern pages while the
            # ship is off screen (boss_pce.c hull_level 3). The art is left-right symmetric about column 100 (1% of its pixels
            # differ), so it is baked 160 dots wide (five cells: ten width units on a scanline, where the old 161-dot hull needed
            # twelve and lost cells to the HUD and the hero) from its left half mirrored, with the cockpit column from the original.
            front=Image.open(ROOT/'assets/hyperjumper/front_idle.png').convert('RGBA')
            raw=np.asarray(front).copy();raw[:,100:]=raw[:,100::-1]
            size=(160,round(front.height*160/front.width))
            sym=np.asarray(Image.fromarray(raw).resize(size,Image.Resampling.LANCZOS)).copy()
            orig=np.asarray(front.resize(size,Image.Resampling.LANCZOS))
            sym[:,64:96]=orig[:,64:96]
            fire=np.asarray(Image.open(ROOT/'assets/hyperjumper/front_fire.png').convert('RGBA').resize(size,Image.Resampling.LANCZOS)).copy()
            fire_raw=np.asarray(Image.open(ROOT/'assets/hyperjumper/front_fire.png').convert('RGBA')).copy();fire_raw[:,100:]=fire_raw[:,100::-1]
            fire_sym=np.asarray(Image.fromarray(fire_raw).resize(size,Image.Resampling.LANCZOS)).copy();fire_sym[:,64:96]=fire[:,64:96]
            # record 3 the front pose, then (after the side poses) record 7 the front pose firing: one set of patterns
            front_set=hull_set([Image.fromarray(sym),Image.fromarray(fire_sym)],(80,size[1]//2),mirror_from=True)
            hull_records+=front_set[0]
            hull_records+=b''.join(poses[1:])+front_set[1]
        meta['boss_big_offset']=a.add('boss_big',hull_records)
        # Two independently cached metasprites preserve the native art and anchor.
        sprites.append((name+'_left',im.crop((0,0,split,im.height)),anchor))
        sprites.append(('dark_april',shared[18][1],shared[18][2]))
        sprites.append((name+'_right',im.crop((split,0,im.width,im.height)),(anchor[0]-split,anchor[1])))
        laser=cblock_frame(work/'srgb'/'A2E02F5A.srgb',0)
        for label,angle in (('horizontal',0),('diagonal',-45),('vertical',90)):
            sprites.append(('boss_laser_'+label,laser.rotate(angle,resample=Image.Resampling.NEAREST),(8,8)))
        meta['actor_ids']=[255]*33
        for t in range(1,33):meta['actor_ids'][t]=39 if t in (1,3,4) else 40 if t in (2,5,28) else 41
        for t,crhc in enumerate(['FBFAF817','4042CD71','71887ECA','BFDAB70F','1D724DD9','211F5D78','9393E59B','20C6FAEF','ECC992CB','72B53EF8','925534E2','916137ED','906D3698','F5975DCF','F4A55EDE','F4A25ED9','F3B05E28'],11):
            d=(work/f'{crhc}.levl').read_bytes();aid=struct.unpack_from('<I',d,4)[0]
            first=struct.unpack_from('<I',d,0x34+(2 if t>=24 or t==11 else 1)*24+4)[0]
            art=work/'srgb'/f'{aid:08X}.srgb'
            if not art.exists():
                meta['actor_ids'][t]=255;continue
            if t==11:      # the galloping robot horses are drawn by the herd code from their own big-cell frames
                meta['actor_ids'][t]=255;continue
            if 24<=t<=27:      # the airships of the far background layers: not drawn (the PCE has no such layer)
                meta['actor_ids'][t]=255;continue
            flags=struct.unpack_from('<I',d,0x34+24+20)[0]
            whole=bool(flags&8)
            im=cblock_whole_frame(art,first) if whole else cblock_frame(art,first)
            if not im.getchannel('A').getbbox():
                meta['actor_ids'][t]=255;continue
            anchor=tuple(round(v) for v in struct.unpack_from('<ff',d,8))
            # Wide scenery retains a bounded canonical metasprite.
            if im.width>128 or im.height>64:
                scale=min(128/im.width,64/im.height)
                im=im.resize((max(1,round(im.width*scale)),max(1,round(im.height*scale))),Image.Resampling.NEAREST)
                anchor=tuple(round(v*scale) for v in anchor)
            if t==16:
                im=key_wall(im)
            meta['actor_ids'][t]=len(sprites)
            sprites.append((f'actor_type{t}',im,anchor))
            if t==16:
                # Security camera: both original animation cells, no static substitute.
                sprites.append(('actor_type16_frame1',key_wall(cblock_whole_frame(art,first+1) if whole else cblock_frame(art,first+1)),anchor))
        # The cutscene Outrider (type 28) is the blue one: standing, the "!" alarm pose, then the six run cells.
        art=work/'srgb'/'6338F34D.srgb';meta['actor_ids'][28]=len(sprites)
        for name,n in [('idle',24),('alarm',36)]+[(f'run{k}',42+k) for k in range(6)]:
            sprites.append((f'outrider_{name}',cblock_frame(art,n),(32,32)))
    elif stage == 6:
        fonts = hudart.Fonts(work, cblock_frame)
        # Ramrod's arena as a sprite scaler (Space Harrier): the sky panorama round the planet, all 1344 dots of its turn, scrolls with the heading
        # (an ordinary scrolling background, 168 characters wide, the horizon at row M6_HORIZON); the desert floor under it is a few soft tones, and
        # the mechs, rocks, shots and bursts are sprites baked at a ladder of sizes (the source's scale law, 214 / distance x 0.85), nearest
        # size at or above what the distance asks for, so nothing is stretched at run time.
        pano = Image.open(ROOT / 'assets/ramrod/sky.png').convert('RGBA')
        bg = Image.new('RGBA', (pano.width, 240), (34, 126, 200, 255))
        bg.paste(pano.crop((0, 167 - M6_HORIZON, pano.width, 167)), (0, 0))
        # The floor (BAT rows 16-27: 96 lines, then two rows of haze): a ground texture seen from above, laid out flat. Stripes of 16 lines in two sand tones, a thin line every
        # 64 dots and a few speckles, from 13 characters in all; periodic every 64 columns, so that the BAT ring is seamless under any scroll. The picture warps it at
        # run time: m6_d.c gives every group of four scanlines its own BYR (the texture line the depth of that row asks for, plus the way Ramrod has walked) and BXR (the
        # strafing, by depth) through the horizontal-blank interrupt (irq.S), which is how the flat texture comes to run away to the horizon.
        dust = np.array([217, 170, 122], np.uint8)
        light, dark, grid_c, spk_l, spk_d = (np.array(c, np.uint8) for c in ((216, 168, 120), (198, 150, 106), (176, 128, 90), (228, 184, 138), (180, 134, 94)))
        rng = np.random.RandomState(6)
        def tile_pixels(stripe, kind):
            base = np.tile((dark if stripe else light), (8, 8, 1)).astype(np.uint8)
            if kind == 5: base[:, 0] = grid_c                       # the grid line
            elif kind > 0:                                          # four speckle variants: a pair of dots
                sx, sy = ((1, 2), (5, 6), (3, 4), (6, 1))[kind - 1]
                base[sy, sx] = base[sy, sx + 1] = (spk_l if stripe else spk_d)
            return base
        floor_rows = np.zeros((240 - M6_HORIZON, 64 * 8, 3), np.uint8)
        for tr in range(14):
            for tc in range(64):
                if tr < 12:
                    stripe = (tr // 2) & 1
                    kind = 5 if tc % 8 == 0 else (int(rng.randint(1, 5)) if rng.rand() < 0.22 else 0)
                    floor_rows[tr * 8:tr * 8 + 8, tc * 8:tc * 8 + 8] = tile_pixels(stripe, kind)
                else: floor_rows[tr * 8:tr * 8 + 8, tc * 8:tc * 8 + 8] = dust
        floor_full = np.tile(floor_rows, (1, pano.width // 512 + 1, 1))[:, :pano.width]
        floor_img = Image.fromarray(floor_full).convert('RGBA')
        bg.paste(floor_img, (0, M6_HORIZON))
        sprites = shared[36:39]
        macros = []
        def scaled(im, anchor, k):
            w, h = max(1, round(im.width * k)), max(1, round(im.height * k))
            small = im.convert('RGBa').resize((w, h), Image.Resampling.BOX).convert('RGBA')
            a = np.asarray(small).copy(); a[..., 3] = np.where(a[..., 3] >= 128, 255, 0)
            return Image.fromarray(a), (round(anchor[0] * k), round(anchor[1] * k))
        def mark(name): macros.append(f'#define PCE_M6_{name} {len(sprites)}')
        atl = ROOT / 'assets/ramrod/atlas.png'
        # the mechs: 3 kinds x 8 poses (walk 0-3, aim, wind-up, punch, stagger) x the size ladder
        mark('MECH')
        for kind in ('mech', 'mech_red', 'mech_gold'):
            frames = atlas(atl, kind)
            for fr in range(8):
                for k in M6_SCALES:
                    im, anc = scaled(frames[fr][1], frames[fr][2], k)
                    sprites.append((f'{kind}_{fr}_{round(k*1000)}', im, anc))
        # scenery: boulders and cacti (the source's props), five sizes each
        for name, mult in (('rock_big', 1.3), ('rock_small', 1.0), ('cactus', 0.7)):
            mark(name.upper())
            frame = atlas(atl, name)[0]
            for k in M6_PROP_SCALES:
                im, anc = scaled(frame[1], frame[2], k * mult)
                sprites.append((f'{name}_{round(k*1000)}', im, anc))
        # shots: Ramrod's bolt is three frames made here and written to VRAM at $7c80 (m6_c.c m6_start, drawn by m6_d.c bolt_draw), not cached sprites: a very near one (32x32),
        # a middle one and a small far one (16x16), a glowing ball each (orange, yellow, white core) in the HUD's palette (their images are HUD pieces below, so the palette has
        # their colours; the patterns are packed after the sprite table with it, at the end of make_scene)
        def ball(size, radii):
            im = Image.new('RGBA', (size, size)); dr = ImageDraw.Draw(im); mid = size / 2 - 0.5
            for r, col in radii: dr.ellipse((mid - r, mid - r, mid + r, mid + r), fill=(*col, 255))
            return im
        bolt_images = [ball(32, ((15.5, (255, 150, 40)), (12, (240, 190, 60)), (8, (255, 235, 190)), (4.2, (255, 255, 255)))),
                       ball(16, ((7.5, (255, 150, 40)), (5.5, (240, 190, 60)), (3.3, (255, 235, 190)), (1.6, (255, 255, 255)))),
                       ball(16, ((3.4, (255, 150, 40)), (2.2, (240, 190, 60)), (1.0, (255, 255, 255))))]   # near, middle, far
        mark('PLASMA')
        plasma = atlas(atl, 'plasma')
        for fr in (0, 2):
            for k in M6_FX_SCALES:
                im, anc = scaled(plasma[fr][1], plasma[fr][2], k * 1.6)
                sprites.append((f'plasma{fr}_{round(k*1000)}', im, anc))
        # bursts: five frames (the sixth is a fade too faint to keep) at four sizes
        mark('EXPL')
        expl = atlas(atl, 'expl')
        for k in (0.3, 0.55, 0.9, 1.35):
            for fr in range(5):
                im, anc = scaled(expl[fr][1], expl[fr][2], k)
                sprites.append((f'expl{fr}_{round(k*1000)}', im, anc))
        # Ramrod's arm reaching out past the glass (the ten frames of the source's punch; one the left fist's, the right is mirrored), drawn by m6_d.c's own emitter into
        # one of two alternating pattern buffers of 32 patterns ($2800 and $4000): the pieces are 16x16 cells where two neighbours in a row are one 32x16 sprite (an entry
        # fewer; the pair starts at an even pattern), as large as 32 patterns allow. An entry (frame): u32 piece list, u32 patterns, u8 entries, u8 patterns; a piece is
        # (dx i16, dy i16, kind u8 (0 one cell, 1 a pair), pattern u8). One palette for all ten.
        arm_heights = []
        arm_frames = atlas(atl, 'arm')
        def arm_pack(img, pal):
            idx = indexed(img, pal); h_, w_ = idx.shape
            pats, pieces = [], []
            for y in range(0, h_, 16):
                strip = idx[y:y + 16]
                occ = np.flatnonzero(strip.any(0))
                if not len(occ): continue
                cells = [x for x in range(int(occ[0]), int(occ[-1]) + 1, 16) if strip[:, x:x + 16].any()]
                def cell(x):
                    c = np.pad(strip[:, x:x + 16], ((0, 16 - strip.shape[0]), (0, 16 - min(16, w_ - x)))); return planar_sprite(c[:16, :16])
                i = 0
                while i < len(cells):
                    if i + 1 < len(cells) and cells[i + 1] == cells[i] + 16:
                        if len(pats) & 1: pats.append(bytes(128))
                        pieces.append((cells[i], y, 1, len(pats))); pats += [cell(cells[i]), cell(cells[i + 1])]; i += 2
                    else:
                        pieces.append((cells[i], y, 0, len(pats))); pats.append(cell(cells[i])); i += 1
            return pats, pieces
        arm_k = 0.62
        while True:
            imgs = [scaled(arm_frames[fr][1], (0, 0), arm_k)[0] for fr in range(10)]
            arm_pal = palette_for(imgs)
            packed = [arm_pack(im_, arm_pal) for im_ in imgs]
            if max(len(pp[0]) for pp in packed) <= 32: break
            arm_k -= 0.01
        print(f'  arm: scale {arm_k:.2f}, entries {[len(pp[1]) for pp in packed]}', flush=True)
        arm_rows = []
        for fr, (pp, im_) in enumerate(zip(packed, imgs)):
            arm_heights.append(im_.height)
            arm_rows.append((a.add('arm_pieces', b''.join(struct.pack('<hhBB', *pc) for pc in pp[1])), a.add('arm_patterns', b''.join(pp[0])), len(pp[1]), len(pp[0])))
        meta['m6_arm'] = a.add('m6_arm_table', b''.join(struct.pack('<IIBB', po, pa, n, np_) for po, pa, n, np_ in arm_rows))
        meta['m6_armpal'] = a.add('m6_arm_palette', arm_pal.tobytes())
        # the nearest mech at four more steps (M6_BIG_SCALES): 32x32 hardware pieces, the grid placed where it needs the fewest, each piece four 16x16 patterns (TL, TR,
        # BL, BR); one palette a variant. Entries are (variant * 8 + pose) * 4 + step: u32 piece list, u32 patterns, u8 pieces (<= 16), pad; a piece is (dx, dy) from the
        # feet (i16 each), its patterns being the i-th group of four.
        big_pal = []
        big_rows = []
        big_frames = {}
        for kind in ('mech', 'mech_red', 'mech_gold'):
            frames = atlas(atl, kind)
            big_frames[kind] = [[scaled(frames[fr][1], frames[fr][2], k) for k in M6_BIG_SCALES] for fr in range(8)]
            big_pal.append(palette_for([big_frames[kind][fr][3][0] for fr in range(8)] + [big_frames[kind][fr][1][0] for fr in range(0, 8, 2)]))
        for v, kind in enumerate(('mech', 'mech_red', 'mech_gold')):
            for fr in range(8):
                for st in range(4):
                    img, (ax, ay) = big_frames[kind][fr][st]
                    idx = indexed(img, big_pal[v]); h_, w_ = idx.shape
                    ys, xs = np.nonzero(idx)
                    best = None
                    for ox in range(0, 32, 2):
                        for oy in range(0, 32, 2):
                            cells = sorted({(int((x - ax + ox) // 32), int((y - ay + oy) // 32)) for x, y in zip(xs.tolist(), ys.tolist())})
                            if best is None or len(cells) < len(best[0]): best = (cells, ox, oy)
                    cells, ox, oy = best
                    if len(cells) > 16: raise ValueError(f'big mech {kind} {fr} {st}: {len(cells)} pieces')
                    pieces = b''; pat = b''
                    for (cx, cy) in cells:
                        x0, y0 = cx * 32 - ox + ax, cy * 32 - oy + ay      # the cell's top left in the image
                        blk = np.zeros((32, 32), np.uint8)
                        for yy in range(32):
                            for xx in range(32):
                                X, Y = x0 + xx, y0 + yy
                                if 0 <= X < w_ and 0 <= Y < h_: blk[yy, xx] = idx[Y, X]
                        pieces += struct.pack('<hh', cx * 32 - ox, cy * 32 - oy)
                        for (bx, by) in ((0, 0), (16, 0), (0, 16), (16, 16)): pat += planar_sprite(blk[by:by + 16, bx:bx + 16])
                    big_rows.append((a.add('big_pieces', pieces), a.add('big_patterns', pat), len(cells)))
        meta['m6_big'] = a.add('m6_big_table', b''.join(struct.pack('<IIBB', po, pa, n, 0) for po, pa, n in big_rows))
        meta['m6_bigpal'] = a.add('m6_big_palettes', b''.join(p.tobytes() for p in big_pal))
        # the portraits and dialogue pieces first (their ids come before the HUD's, so they get slots of their own palette, as on the platform stages)
        meta['presentation'] = presentation.add_art(ROOT, work, stage, sprites, cblock_frame)
        hud = hudart.Hud(sprites)
        # (the shadows, the reticle and its brackets share the HUD's palette: they are few colours and the cache has only fifteen palettes of its own)
        def hmark(name): macros.append(f'#define PCE_M6_{name} {hud.base + len(sprites) - hud.base}')
        for nm, im in zip(('bolt_near', 'bolt_mid', 'bolt_far'), bolt_images): hud.add(nm, im)   # (their colours in the HUD's palette; never drawn from the table)
        hmark('SHADOW')
        for i, k in enumerate(M6_ALL_SCALES[::2]):
            w = min(48, max(6, round(88 * k))); h = max(3, round(w * min(0.5, max(0.08, 0.3 / (1 + i * 0.7)))))
            im = Image.new('RGBA', (w, h)); dr = ImageDraw.Draw(im); dr.ellipse((0, 0, w - 1, h - 1), fill=(60, 36, 22, 255))
            chk = np.asarray(im).copy(); chk[(np.indices(chk.shape[:2]).sum(0) & 1) == 1] = 0   # a checker of the shadow's colour: half see-through
            hud.add(f'shadow{i}', Image.fromarray(chk), (w // 2, h // 2))
        hmark('RETICLE')
        for lit in (0, 1):
            im = Image.new('RGBA', (16, 16)); dr = ImageDraw.Draw(im)
            col = (255, 235, 220, 255) if lit else (255, 40, 30, 255)
            for (x0, y0, dx, dy) in ((1, 1, 1, 1), (14, 1, -1, 1), (1, 14, 1, -1), (14, 14, -1, -1)):
                dr.line((x0, y0, x0 + 3 * dx, y0), fill=col); dr.line((x0, y0, x0, y0 + 3 * dy), fill=col)
            dr.line((8, 5, 8, 10), fill=col); dr.line((5, 8, 10, 8), fill=col); dr.point((8, 8), fill=(255, 255, 255, 255))
            hud.add(f'reticle{lit}', im, (8, 8))
        # the radar in the top right corner (ramrod.c render_monitors): a 32x32 panel, Ramrod at (16,24), the view cone and two range rings (one pixel is 80 units); the
        # dots are 3x3 pieces
        im = Image.new('RGBA', (32, 32)); dr = ImageDraw.Draw(im)
        dr.rectangle((0, 0, 31, 31), fill=(6, 34, 20, 255), outline=(60, 200, 110, 255))
        for rad in (8, 16):
            for k in range(40 if rad == 8 else 64):
                ang = k * 2 * math.pi / (40 if rad == 8 else 64); px, py = round(16 + math.cos(ang) * rad), round(24 + math.sin(ang) * rad)
                if 1 <= px <= 30 and 1 <= py <= 30: dr.point((px, py), fill=(45, 150, 80, 255))
        dr.line((16, 24, 7, 8), fill=(45, 150, 80, 255)); dr.line((16, 24, 25, 8), fill=(45, 150, 80, 255))
        dr.rectangle((15, 23, 17, 25), fill=(255, 255, 255, 255))
        for cell, (cx, cy) in enumerate(((0, 0), (16, 0), (0, 16), (16, 16))): hud.add(f'radar_{cell}', im.crop((cx, cy, cx + 16, cy + 16)))   # (four single pieces: the HUD's own pattern slots, m6_a.c)
        for nm, col in (('green', (120, 255, 200)), ('red', (255, 70, 60)), ('gold', (255, 210, 40)), ('white', (255, 255, 255)), ('pink', (255, 110, 230))):
            im = Image.new('RGBA', (16, 16)); ImageDraw.Draw(im).rectangle((0, 0, 2, 2), fill=(*col, 255)); hud.add('rdot_' + nm, im)
        # the HUD: armour and gun-heat bars, digits, the arrows at the screen's edge and the wave banners
        hudart.add_bar_fills(hud, ('green', 'yellow', 'red', 'orange'))
        hudart.add_digits(hud, fonts, ('white', 'cyan', 'pink'))
        for danger, c in (('', (255, 200, 60)), ('_danger', (255, 60, 60))):
            im = Image.new('RGBA', (16, 16)); dd = ImageDraw.Draw(im)
            for k in range(5): dd.line((k, 5 - k, k, 5 + k), fill=(*c, 255))
            hud.add('chevron' + danger, im)
        for name, text, color, big in (('wave1', 'WAVE 1', (255, 182, 0), True), ('wave2', 'WAVE 2', (255, 182, 0), True), ('wave3', 'WAVE 3', (255, 182, 0), True),
                                       ('cleared', 'WAVE CLEARED', (255, 255, 255), True), ('destroyed', 'SQUADRON DESTROYED', (255, 255, 255), True),
                                       ('down', 'RAMROD IS DOWN!', (255, 60, 60), True), ('warning', 'WARNING: COMMAND MECH', (255, 80, 80), False),
                                       ('arm', 'ARM', (255, 210, 120), False), ('gun', 'GUN', (255, 210, 120), False), ('hot', 'HOT', (255, 70, 60), False),
                                       ('counter', 'COUNTER!', (255, 255, 255), True), ('parry', 'PARRY!', (255, 255, 255), True),
                                       ('wlab', 'W', (150, 200, 255), False), ('xlab', 'x', (255, 255, 255), False)):
            hudart.add_text(hud, fonts, name, text, color, big)
        # the status line in single pieces: AR / GN (HT when the guns are hot), W1-3, and x0-9 for the spares
        for nm, text, col in (('lab_ar', 'AR', (255, 210, 120)), ('lab_gn', 'GN', (255, 210, 120)), ('lab_ht', 'HT', (255, 70, 60)), ('lab_w1', 'W1', (150, 200, 255)),
                              ('lab_w2', 'W2', (150, 200, 255)), ('lab_w3', 'W3', (150, 200, 255))) + tuple((f'lab_x{n}', f'x{n}', (255, 255, 255)) for n in range(10)):
            hud.add(nm, hudart.piece(fonts.text(text, col, False, True), 0, 0))
        meta['hud'] = hud.base; meta['hud_macros'] = hud.macros('H6') + macros
        meta['arm_heights'] = arm_heights
        collision = 0
    elif stage == 7:
        bg = Image.open(ROOT / 'assets/space/nebula.png').convert('RGBA').resize((512, 224), Image.Resampling.NEAREST)
        sprites = shared[36:39] + atlas(ROOT / 'assets/space/atlas.png', 'player') + atlas(ROOT / 'assets/space/atlas.png', 'fighter') + atlas(ROOT / 'assets/space/atlas.png', 'gunship')
        # Keep the fixed ID; the complete cruiser is background art, never a sprite.
        sprites.append(('battle_cruiser',Image.new('RGBA',(1,1)),(0,0)))
        meta['boss_bg_offset'], meta['boss_bg_tiles'] = boss_background(a, previews)
        sprites+=atlas(ROOT/'assets/space/atlas.png','drone')+atlas(ROOT/'assets/space/atlas.png','mine')
        sprites+=atlas(ROOT/'assets/space/atlas.png','cap')
        # The ships' explosions (space.c blast: six frames of 64x64, a big one and smaller ones round it; the sixth is a fade too faint
        # to keep): ids 12-16 at 48 dots, 17-21 at 32
        sprites+=atlas(ROOT/'assets/space/atlas.png','expl',48)[:5]
        sprites+=[(f'expl_small{n}',im,anchor) for n,(_,im,anchor) in enumerate(atlas(ROOT/'assets/space/atlas.png','expl',32)[:5])]
        meta['track_offset']=a.add('space_timeline',timeline.bake(ROOT/'src/space.c'))
        # the portraits and dialogue pieces before the HUD (their ids come before the HUD's, so they get slots of their own palette: the box and the speaker's portrait
        # would otherwise share the HUD's palette and the last one written wins)
        meta['presentation']=presentation.add_art(ROOT,work,stage,sprites,cblock_frame)
        hud=hudart.Hud(sprites)
        hudart.space_hud(hud,hudart.Fonts(work,cblock_frame),Image.open(ROOT/'assets/space/atlas.png').convert('RGBA'))
        meta['hud']=hud.base;meta['hud_macros']=hud.macros('H7')
        collision = 0
    else:
        bg = Image.new('RGBA', (512,224), (0,0,0,255))   # (the sky is baked by race_sky below: plain tiles, no picture)
        sprites = shared[36:39]
        im = Image.open(ROOT / 'assets/mode7.png').convert('RGBA')
        rows7 = {r.split()[0]: [int(v) for v in r.split()[1:6]] for r in (ROOT / 'assets/mode7.txt').read_text().splitlines()}
        def car(name, frame, scale):
            x, y, w, h, frames = rows7[name]
            c = im.crop((x + frame * w, y, x + (frame + 1) * w, y + h))
            nh = round(h * scale / w)
            c = c.resize((scale, nh), Image.Resampling.NEAREST).resize((scale * 2, nh), Image.Resampling.NEAREST)
            return c, (scale, nh)
        # Each car at five baked widths (the race draws one at the nearest width at or above its size and pulls its
        # slices together to the exact one), the straight-ahead frame; the player's buggy also keeps its four steering frames.
        for name in ('buggy', 'hornet', 'leader', 'firenza', 'racer_blue', 'racer_purple', 'mine'):
            for scale in CAR_WIDTHS:
                c, anchor = car(name, rows7[name][4] // 2, scale)
                sprites.append((f'{name}{scale}', c, anchor))
        for frame in (0, 1, 3, 4):
            c, anchor = car('buggy', frame, CAR_WIDTHS[-1])
            sprites.append((f'buggy_steer{frame}', c, anchor))
        # The player's straight-ahead car (standing still, steering within the dead zone) keeps the two aerials the turning frames show:
        # they are one source dot wide there, and the nearest-neighbour shrink to 64 dots dropped them. Every destination dot takes
        # the nearest source dot, or an opaque dot of its footprint when that one is clear (the rows above the body, 0-15, only).
        bx0, by0, bw0, bh0, bn0 = rows7['buggy']
        src = im.crop((bx0 + (bn0 // 2) * bw0, by0, bx0 + (bn0 // 2 + 1) * bw0, by0 + bh0))
        sw = CAR_WIDTHS[-1]; sh = round(bh0 * sw / bw0)
        keep = Image.new('RGBA', (sw, sh)); sp, kp = src.load(), keep.load()
        for dy in range(sh):
            for dx in range(sw):
                sx0, sx1 = dx * bw0 // sw, max(dx * bw0 // sw, ((dx + 1) * bw0 - 1) // sw)
                sy = min(bh0 - 1, int((dy + 0.5) * bh0 / sh))
                px_ = sp[min(bw0 - 1, (2 * dx + 1) * bw0 // (2 * sw)), sy]
                if px_[3] == 0 and sy < 16:
                    for xx in range(sx0, sx1 + 1):
                        if sp[xx, sy][3]: px_ = sp[xx, sy]; break
                kp[dx, dy] = px_
        sprites.append(('buggy_steer2', keep.resize((sw * 2, sh), Image.Resampling.NEAREST), (sw, sh)))
        # The spin-out (mode7.c render_player: one whole turn, easing out): the buggy at SPIN_FRAMES-1 angles of a full turn, each
        # rotated at the car's true shape and then stretched to the 512-dot clock. A rotated car is wider than one sprite object
        # can hold (32 pieces), so each pose is two objects (left and right half, drawn at the same point), as the flying bosses.
        bx, by, bw, bh, bn = rows7['buggy']
        base = im.crop((bx + (bn // 2) * bw, by, bx + (bn // 2 + 1) * bw, by + bh))
        base = base.resize((CAR_WIDTHS[-1], round(bh * CAR_WIDTHS[-1] / bw)), Image.Resampling.NEAREST)
        for k in range(1, SPIN_FRAMES):
            r = base.rotate(-360 * k / SPIN_FRAMES, resample=Image.Resampling.NEAREST, expand=True)
            r = r.resize((r.width * 2, r.height), Image.Resampling.NEAREST)
            half = r.width // 2
            ax, ay = r.width // 2, r.height // 2 + base.height // 2     # the car's centre stays where the upright car's is
            sprites.append((f'buggy_spin{k}_left', r.crop((0, 0, half, r.height)), (ax, ay)))
            sprites.append((f'buggy_spin{k}_right', r.crop((half, 0, r.width, r.height)), (ax - half, ay)))
        # The afterburner (mode7.c render_player: a flame over each exhaust nozzle, four frames at 18 a second): the 10x10 cells
        # at the buggy's 64/82 scale, stretched to the 512-dot clock, so one 16x8 piece each; the nozzle offsets are in draw().
        tx, ty, tw, th, tn = rows7['turbo']
        for k in range(tn):
            f = im.crop((tx + k * tw, ty, tx + (k + 1) * tw, ty + th)).resize((8, 8), Image.Resampling.NEAREST)
            sprites.append((f'turbo{k}', f.resize((16, 8), Image.Resampling.NEAREST), (8, 8)))
        # The explosions of wrecked cars (mode7.c spawn_expl: frames of 64x64): a big one (96 dots wide, 48 high) and a small one for
        # far cars (48 x 24), stretched to the 512-dot clock; anchored at the middle of the car's body (PCE_CAR_EXPL: five big frames, then five small).
        ex, ey, ew, eh, en = rows7['explosion']
        for size in (48, 24):
            for k in range(en - 1):   # (the sixth frame is a fade too faint to keep)
                f = im.crop((ex + k * ew, ey, ex + (k + 1) * ew, ey + eh)).resize((size, size), Image.Resampling.NEAREST)
                sprites.append((f'expl{size}_{k}', f.resize((size * 2, size), Image.Resampling.NEAREST), (size, size * 3 // 4)))
        # The shots (PCE_CAR_ORB: the car's and the enemies', five sizes each, near to far): a glowing orb, 2:1 for the 512-dot clock, big when it
        # leaves a gun and smaller as it flies away. And the start / finish line's flag poles (PCE_CAR_POLE: six sizes, a checkered flag
        # streaming to the right of the pole; the right-hand pole is the same sprite flipped), anchored at the foot of the pole.
        for who, (core, mid, rim) in enumerate((((255, 255, 255), (140, 210, 255), (30, 110, 235)), ((255, 245, 200), (255, 130, 50), (200, 30, 30)))):
            for step, size in enumerate(ORB_SIZES):
                orb = Image.new('RGBA', (size * 2, size)); dr = ImageDraw.Draw(orb)
                for frac, col in ((1.0, rim), (0.72, mid), (0.4, core)):
                    rx, ry = size * frac - 0.5, size * frac / 2 - 0.5
                    dr.ellipse((size - rx, size / 2 - ry, size + rx, size / 2 + ry), fill=(*col, 255))
                sprites.append((f'orb{who}_{step}', orb, (size, size // 2)))
        for step, (pw_, ph) in enumerate(POLE_SIZES):
            pole = Image.new('RGBA', (pw_, ph)); dr = ImageDraw.Draw(pole)
            shaft = max(2, pw_ // 7)
            dr.rectangle((0, 0, shaft - 1, ph - 1), fill=(150, 150, 165, 255)); dr.rectangle((0, 0, shaft // 2 - 1, ph - 1), fill=(245, 245, 250, 255))
            fh = max(6, ph // 4)
            for r in range(3):
                for c in range(4):
                    x0, x1 = shaft + c * (pw_ - shaft) // 4, shaft + (c + 1) * (pw_ - shaft) // 4 - 1
                    dr.rectangle((x0, r * fh // 3, x1, (r + 1) * fh // 3 - 1), fill=(0, 0, 0, 255) if (r + c) & 1 else (255, 255, 255, 255))
            sprites.append((f'pole{step}', pole, (shaft // 2, ph)))
        env = dict(os.environ, SABER_ASSETS=str(ROOT / 'assets'), SABER_FRAMES='1', SABER_M7MAP=str(work/'race.pgm'))
        run([ROOT/'build/headless/saber_headless', ROOT/'SaberRider/data', 2], env=env)
        race = np.asarray(Image.open(work/'race.pgm'), np.uint8) // 20
        floor = im.crop((0, 119, 352, 151))
        # 14 floor colours + white (index 15 is the text font's white, so the floor never picks it for a cell). Every
        # 8-unit map cell of the ground is baked to ONE byte: its colour (low nibble: the mean of the material's 8x8-texel
        # block that cell covers) and its class (high nibble: 0 sand, 1 kerb, 2 road), so a floor sample is a single
        # Arcade read and the car's physics reads the same byte.
        pal = palette_for([floor], colors=14); pal[15] = 0x1ff
        rgb = vce_rgb(pal[1:15]).astype(np.int32)
        def nearest(mean): return int(((rgb - mean[None]) ** 2).sum(-1).argmin()) + 1
        ROAD = (2, 3, 6, 7, 9); KERB = (4, 5, 10)
        blocks = np.zeros((11, 4, 4), np.uint8)
        texture = np.asarray(floor.convert('RGB')).astype(np.float64)
        for k in range(11):
            for by in range(4):
                for bx in range(4):
                    blocks[k, bx, by] = nearest(texture[by*8:by*8+8, k*32+bx*8:k*32+bx*8+8].reshape(-1, 3).mean(0))
        cls = np.array([2 if k in ROAD else 1 if k in KERB else 0 for k in range(11)], np.uint8)
        cx = np.arange(1024)[None, :] & 3; cy = np.arange(1024)[:, None] & 3
        ground = blocks[np.minimum(race, 10), cx, cy] | (cls[np.minimum(race, 10)] << 4)
        a.add('race_map', ground.astype(np.uint8).tobytes())    # first: the sampler addresses it from Arcade offset 0
        # the commonest sand colour of the ground: the haze between the sky and the floor's first scanline matches it
        sand_index = int(np.bincount((ground & 15)[(ground >> 4) == 0].ravel(), minlength=16).argmax())
        race_sand = vce_rgb(pal[sand_index:sand_index + 1])[0]
        meta['track_offset']=a.add('track', (work/'track.bin').read_bytes())
        road = road_assets(race_sand, previews)
        a.add('road_tiles', road['tiles']); a.add('road_bat', road['bat']); a.add('road_palette', road['palette'])
        # Pursuit is the source's straight desert road; both working sets fit by representing its repeated row
        # (indexed by the x cell, 1 KiB aligned) rather than another 1 MiB map.
        road = np.zeros(1024, np.uint8); road[496:528] = 7; road[498:526] = 2; road[511:513] = 3
        row = np.array([int(blocks[m, 0, 0]) | (int(cls[m]) << 4) for m in range(11)], np.uint8)
        a.add('pursuit_row', row[road].tobytes(), 1024)
        race_dialog(a,work)
        hud=hudart.Hud(sprites)
        sheet=im
        bx,by,bw,bh,bn=[int(v) for v in next(r for r in (ROOT/'assets/mode7.txt').read_text().splitlines() if r.split()[0]=='buggy').split()[1:6]]
        hudart.race_hud(hud,hudart.Fonts(work,cblock_frame),sheet.crop((bx+2*bw,by,bx+3*bw,by+bh)) if bn>=3 else sheet.crop((bx,by,bx+bw,by+bh)))
        meta['hud']=hud.base;meta['hud_macros']=hud.macros('H2')
        collision = 0
    # Append presentation art after fixed gameplay IDs to retain mission IDs.
    if stage in (1,3,4,5,6):platform_dialog(a,work)
    if stage not in (6,7): meta['presentation']=presentation.add_art(ROOT,work,stage,sprites,cblock_frame)
    if stage in (1,3,4,5,7): a.add('power_wave', power_wave())
    if stage == 7: a.add('dialog_original_font', font_glyphs(work))   # (the cut-in puts the font back; the other stages' copy comes with their dialogue panels)
    meta['foreground_offset']=0;meta['foreground_count']=0
    if stage in (1,3,4,5):
        # Platform playfields now include the source's top 16 lines. Gameplay
        # sprites passed in world-16 coordinates are baked 16 lines lower, so
        # the renderer needs no per-draw Y adjustment.
        hud0=meta['presentation']['hud'][0][0];aim0=meta['presentation']['aim'][0][0];motion0=meta['presentation']['motion'][0][0]
        for i,(name,im,(ax,ay)) in enumerate(sprites):
            if i<hud0 or aim0<=i<meta['presentation']['power']:   # gameplay, aim and motion poses (not the HUD, not the power portraits)
                sprites[i]=(name,im,(ax,ay-16))
        if stage in (1,3):   # no foreground layer at all: whatever is left of it flickers (stage 4 keeps its cabin walls)
            foreground=Image.new('RGBA',foreground.size);print(f'  stage {stage}: foreground removed', flush=True)
        else: foreground=thin_foreground(foreground)
        entries=presentation.add_foreground(foreground,sprites)
        meta['foreground_offset']=a.add('foreground_sprites',b''.join(struct.pack('<hhH',*v) for v in entries))
        meta['foreground_count']=len(entries)
        foreground.crop((0,0,1024,224)).save(previews/f'foreground{stage}.png')
    if stage == 6: meta['m6_image'] = a.add('m6_image', bytes(4 * 8192), 2048)   # the arena's code images are put here after the application is linked (m6_image.py, build_disc.py)
    meta['story_offset']=story.bake(ROOT,work,stage,a,meta['presentation']['portraits'])
    meta.update(race_sky(a, previews, race_sand) if stage == 2 else native_background(bg, a, previews, f'stage{stage}'))
    sprite_table, rows, costs = add_sprites(a, list(sprites), previews)
    if stage == 6:
        # the bolt's patterns in the HUD's palette (add_sprites gave every 'hudp_' sprite of the stage one: the same call on the same images): middle and far 16x16 (64 words each), then the near 32x32 (TL, TR, BL, BR)
        hud_pal = palette_for([im for name, im, _ in sprites if name.startswith('hudp_')], unique=True)
        near, mid, far = (indexed(im, hud_pal) for im in bolt_images)
        blob = planar_sprite(mid) + planar_sprite(far) + b''.join(planar_sprite(near[by:by + 16, bx:bx + 16]) for by, bx in ((0, 0), (0, 16), (16, 0), (16, 16)))
        meta['m6_bolts'] = a.add('m6_bolts', blob)
    meta.update(collision=collision, sprite_table=sprite_table, sprite_count=len(rows), sprites=costs,
                records=a.records, bytes=len(a.finish()), color_palettes=16, sprite_palettes=16)
    return a.finish(), meta

def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--out', type=Path, default=ROOT/'build/pce')
    args = parser.parse_args(); out = args.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    work = out/'work'; previews = out/'preview'; previews.mkdir(exist_ok=True)
    extract(work)
    shared = heroes(work)
    scenes = []
    for stage in range(1, 8):
        print(f'Baking PCE stage {stage}', flush=True)
        data, meta = make_scene(stage, work, previews, shared)
        (out/f's{stage}.bin').write_bytes(data); scenes.append(meta)
    ui_h,ui_c,ui_bytes=frontend.bake(ROOT,work,out,previews,cblock_frame)
    # 96 ASCII glyphs use background characters, palette 15: the Saturn small
    # font's 8x8 frames (frame = char - 0x21; 0x7f is its dialogue arrow).
    (out/'font.bin').write_bytes(font_glyphs(work))
    h = ['/* Generated: all offsets are Arcade RAM byte addresses. */', '#pragma once', '#include <stdint.h>',
         'typedef struct { uint32_t bytes, pal, tiles, map, collision, sprites, triggers, story, track, rules, occlusion, foreground, horse; uint16_t cols, ccols, crows, nsprites, nforeground; int16_t sx, sy, width; uint8_t ntr, cw, ch; } PceScene;',
         'extern const PceScene pce_scenes[7];','extern const uint8_t pce_actor_ids[33];']
    c=['#include "pce_config.h"','#include "assets.h"','const PceScene pce_scenes[7] = {']
    for m in scenes:
        values = [m['bytes'],m['pal'],m['tiles'],m['map'],m['collision'],m['sprite_table'],m.get('trigger_offset',0),m['story_offset'],m.get('track_offset',0),m.get('rules_offset',0),0,m['foreground_offset'],m.get('horse_offset',0),m['cols'],m.get('cols',0) if 'cellw' not in m else m['width']//m['cellw'],m.get('rows',0),m['sprite_count'],m['foreground_count'],*m.get('start',(128,180)),m.get('width',m['cols']*8),m.get('ntr',0),m.get('cellw',8),m.get('cellh',8)]
        # Native map columns and collision columns are independent.
        if 'cellw' in m: values[13] = json.loads((work/f"stage{m['stage']}.json").read_text())['cols']
        c.append('    {' + ','.join(str(v) for v in values) + '},')
    c.append('};')
    presentation.emit_tables(out,scenes,h,c)
    muzzle=json.loads((work/'muzzle.json').read_text())
    # Shots leave the barrel of the pose as the PCE draws it: the art's own tip where it can be measured (the
    # source's muzzle table is for the Saturn's composed poses), straight up from its composite, and the source's
    # figure for straight down (the legs hide the gun there).
    for hero,tip in enumerate(next((m['presentation']['up_tip'] for m in scenes if m['presentation']['up_tip']),[])):
        muzzle[hero][6:8]=tip
    for hero,tips in enumerate(next((m['presentation']['art_muzzle'] for m in scenes if m['presentation']['art_muzzle']),[])):
        for case,tip in enumerate(tips):
            if tip: muzzle[hero][2*case:2*case+2]=tip
    h.append('extern const int8_t pce_muzzle[4][9][2];')
    c.append('const int8_t pce_muzzle[4][9][2]={'+','.join('{'+','.join('{%d,%d}'%(row[2*k],row[2*k+1]) for k in range(9))+'}' for row in muzzle)+'};')
    h+=ui_h;c+=ui_c
    h.append('extern const uint32_t pce_boss_bg[7];')
    c.append('const uint32_t pce_boss_bg[7]={'+','.join(str(m.get('boss_bg_offset',0))+'UL' for m in scenes)+'};')
    h.append('extern const uint32_t pce_boss_big[7];')
    c.append('const uint32_t pce_boss_big[7]={'+','.join(str(m.get('boss_big_offset',0))+'UL' for m in scenes)+'};')
    h.append('extern const uint32_t pce_power_wave[7];')
    c.append('const uint32_t pce_power_wave[7] __attribute__((section(".ram_bank123.rodata")))={'+','.join(str(m['records'].get('power_wave',{}).get('offset',0))+'UL' for m in scenes)+'};')
    for name in ('dialog_bg','dialog_corners','dialog_original_font'):
        h.append(f'extern const uint32_t pce_{name}[7];')
        c.append(f'const uint32_t pce_{name}[7]={{'+','.join(str(m['records'].get(name,{}).get('offset',0))+'UL' for m in scenes)+'};')
    h.append('extern const uint16_t pce_hud_base[7];')
    c.append('const uint16_t pce_hud_base[7]={'+','.join(str(m.get('hud',0)) for m in scenes)+'};')
    for m in scenes: h += m.get('hud_macros',[])
    h += [f'#define PCE_CAR_STEPS {len(CAR_WIDTHS)}', '#define PCE_CAR_STEER (3+7*PCE_CAR_STEPS)', '#define PCE_CAR_SPIN (PCE_CAR_STEER+5)', f'#define PCE_CAR_SPIN_FRAMES {SPIN_FRAMES}', '#define PCE_CAR_TURBO (PCE_CAR_SPIN+2*(PCE_CAR_SPIN_FRAMES-1))', '#define PCE_CAR_EXPL (PCE_CAR_TURBO+4)', f'#define PCE_CAR_ORB (PCE_CAR_EXPL+10)', f'#define PCE_CAR_POLE (PCE_CAR_ORB+{2*len(ORB_SIZES)})', f'#define PCE_ORB_STEPS {len(ORB_SIZES)}', f'#define PCE_POLE_STEPS {len(POLE_SIZES)}', 'extern const uint8_t pce_car_widths[PCE_CAR_STEPS];']
    h += [f'#define PCE_M6_STEPS {len(M6_SCALES)}', f'#define PCE_M6_BIG_STEPS {len(M6_BIG_SCALES)}', f"#define PCE_M6_IMAGE {next(m['m6_image'] for m in scenes if 'm6_image' in m)}UL",
          f"#define PCE_M6_BIG {next(m['m6_big'] for m in scenes if 'm6_big' in m)}UL", f"#define PCE_M6_BIGPAL {next(m['m6_bigpal'] for m in scenes if 'm6_bigpal' in m)}UL",
          f"#define PCE_M6_BOLTS {next(m['m6_bolts'] for m in scenes if 'm6_bolts' in m)}UL", f"#define PCE_M6_ARMT {next(m['m6_arm'] for m in scenes if 'm6_arm' in m)}UL", f"#define PCE_M6_ARMPAL {next(m['m6_armpal'] for m in scenes if 'm6_armpal' in m)}UL"]
    c.append('const uint8_t pce_car_widths[PCE_CAR_STEPS]={'+','.join(map(str,CAR_WIDTHS))+'};')
    d_table,z_table=road_tables()
    h.append('extern const uint8_t pce_road_d[384];extern const uint8_t pce_road_z[113];')
    c.append('const uint8_t pce_road_d[384] __attribute__((section(".ram_bank111.rodata")))={'+','.join(map(str,d_table))+'};')
    c.append('const uint8_t pce_road_z[113] __attribute__((section(".ram_bank109.rodata")))={'+','.join(str(v&255) for v in z_table)+'};')
    c.append('const uint8_t pce_actor_ids[33] = {'+','.join(map(str,scenes[0]['actor_ids']))+'};')
    (out/'assets.c').write_text('\n'.join(c)+'\n')
    h += ['#define PCE_HERO_FRAMES 9', '#define PCE_SHOT_ID 36', '#define PCE_ENEMY_SHOT_ID 37', '#define PCE_BLAST_ID 38']
    for name in ('road_tiles','road_bat','road_palette','race_map','pursuit_row','dialog_wide'):
        h.append(f"#define PCE_RACE_{name.upper()} {scenes[1]['records'][name]['offset']}UL")
    (out/'assets.h').write_text('\n'.join(h)+'\n')
    # Ramrod's arena: the size ladder and the projection tables, indexed by the distance f in units (f >> 4, or f >> 3 for the lateral factor)
    def at_or_above(scales, law):
        out = []
        for f in range(0, 1616, 16):
            want = law / max(f, 16)
            out.append(next((i for i, k in enumerate(scales) if k >= want), len(scales) - 1))
        return out
    m6 = ['/* Generated: Ramrod\'s arena (tools/pce/build_assets.py). Distances f in units; the includer defines M6_SECTION, and M6_RCP_ONLY to leave out all but the reciprocal. */',
          'static const uint16_t m6_rcp[201] __attribute__((section(M6_SECTION)))={' + ','.join(str(round(214 * 256 / max(f, 24))) for f in range(0, 1608, 8)) + '};   /* 214 * 256 / f, f >> 3 */',
          'static const uint8_t m6_vtab[24] __attribute__((section(M6_SECTION)))={' + ','.join(str(255 if 4 * g + 2 < 20 else round(1926 / (4 * g + 2))) for g in range(24)) + '};   /* the floor texture line (of 96) at the first of each four scanlines from 128; 255: haze */',
          '#ifndef M6_RCP_ONLY',
          'static const uint8_t m6_size[101] __attribute__((section(M6_SECTION)))={' + ','.join(map(str, at_or_above(M6_ALL_SCALES, 182.0 * M6_Z))) + '};   /* mech ladder index (11 and up: the big steps), f >> 4 */',
          'static const uint8_t m6_psize[101] __attribute__((section(M6_SECTION)))={' + ','.join(map(str, at_or_above(M6_PROP_SCALES, 182.0 * M6_Z))) + '};   /* prop ladder index, f >> 4 */',
          'static const uint8_t m6_fsize[101] __attribute__((section(M6_SECTION)))={' + ','.join(map(str, at_or_above(M6_FX_SCALES, 280.0 * M6_Z))) + '};   /* shot ladder index, f >> 4 */',
          'static const uint8_t m6_esize[101] __attribute__((section(M6_SECTION)))={' + ','.join(map(str, at_or_above((0.3, 0.55, 0.9, 1.35), 255.0 * M6_Z))) + '};   /* burst size index, f >> 4 */',
          'static const uint8_t m6_row[101] __attribute__((section(M6_SECTION)))={' + ','.join(str(min(95, round(9630 * M6_Z / max(f, 16)))) for f in range(0, 1616, 16)) + '};   /* rows under the horizon, f >> 4 */',
          'static const uint16_t m6_scq[15] __attribute__((section(M6_SECTION)))={' + ','.join(str(round(k * 256)) for k in M6_ALL_SCALES) + '};   /* mech scale, Q8 */',
          'static const uint8_t m6_armh[10] __attribute__((section(M6_SECTION)))={' + ','.join(map(str, next(m['arm_heights'] for m in scenes if m.get('arm_heights')))) + '};   /* the arm frames\' heights */',
          '#endif']
    (out/'m6.h').write_text('\n'.join(m6) + '\n')
    # Runtime work buffers are separate from BIOS/compiler console RAM.
    manifest = dict(format='PCE1', toolchain=str((ROOT.parent/'PCE/llvm-mos8').resolve()),
                    source='Current host pack decoder and active stage construction', scenes=scenes,
                    vram=dict(bat=4096, bg_cache=BG_TILES*32, dialog=1024, font=3072, sprites=24576,
                              clipped_sprites=3584, sat=512),
                    facing_policy='One canonical facing; mirror placement and SAT bit 0x0800 at runtime',
                    adaptations=['One baked background; intact source scenery at native scale',
                                 'Platform playfields fill 240 lines; world and collision coordinates retained'])
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    digest = hashlib.sha256()
    for p in sorted((ROOT/'src').glob('*.[ch]')): digest.update(p.read_bytes())
    (out/'source.sha256').write_text(digest.hexdigest()+'\n')

if __name__ == '__main__': main()
