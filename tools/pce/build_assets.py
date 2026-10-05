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
SPIN_FRAMES = 12                         # poses of a full turn of the spinning buggy (one is the upright car)
MECH_SIZES = (24, 32, 40, 48, 56, 64, 72, 80)   # baked widths of the Ramrod mechs; the cockpit scales between them
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
                cur = px[y0 + y, x0 + x]
                if cur[3] >= 128 and not (int(cur[2]) > int(cur[0]) + 30): continue        # only where the background shows sky
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
    dialog_palette=name in ('stage1','stage3','stage4','stage5')   # palette 15 belongs to the dialogue box and the font
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
    cell_idx, groups, merged = tile_budget.limit_tiles(cells, cell_idx, groups, palette, w // 8, BG_TILES - 4 - COLUMN_SLACK, reserved=(15,) if dialog_palette else ())
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
    blob = bytearray(struct.pack('<HBB', len(tiles), BOSS_COLS, BOSS_ROWS)) + top + bottom
    blob += bytes(64 - len(blob))
    blob += np.asarray(palettes, '<u2').tobytes()                       # offset 64: 4 palettes of 16 words
    blob += np.asarray(entries, '<u2').tobytes().ljust(640, b'\0')       # offset 192: the map, row by row
    blob += b''.join(tiles)                                             # offset 832
    preview = np.zeros((BOSS_ROWS * 8, BOSS_COLS * 8, 3), np.uint8)
    for k in range(len(cells)):
        y, x = divmod(k, BOSS_COLS)
        preview[y * 8:y * 8 + 8, x * 8:x * 8 + 8] = vce_rgb(palettes[groups[k]])[idx[k]] * (idx[k][..., None] > 0)
    Image.fromarray(preview).save(previews / 'boss_bg.png')
    print(f'  stage 7 cruiser: {len(tiles)} characters', flush=True)
    return archive.add('boss_bg', bytes(blob), 32), len(tiles)

ROAD_LINES = 112          # image lines: scanlines 112..223 of the road region (the horizon is scanline 113)
ROAD_STRIPE_FIRST = 16    # image line where the two stripe phases start (the lines above are the haze rows)
def road_assets(sand, previews):
    """The classic racing road (see docs/PCE_CLASSIC_ROAD_20261005.md): one static picture of a straight road in perspective,
    1024 dots wide with its centre at dot 512, one image line for each scanline below the horizon. Edges are drawn at integer
    or quarter slopes (dots per line), so the sloped tiles repeat from row to row and the whole picture is about a hundred
    characters. The race scrolls the picture sideways one scanline at a time (the road's curve and the camera's offset), and
    shows each band of the road in one of two colour phases by choosing which copy of the BAT rows a scanline reads (BYR):
    the copies use the same characters with different palettes. The far rows fade into the haze of the sky."""
    W, C = 1024, 512
    SLOPE_KERB, SLOPE_ROAD, SLOPE_EDGE, SLOPE_DASH = 5.0, 4.25, 4.0, 0.11   # dots a line: kerb outside, road, white edge line, centre dash
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
    haze = sand * 0.72 + np.array((200, 220, 255.)) * 0.28
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
        if r == 0: return 1
        if r == 1: return 2
        if r < 4: return 6 if b else 3
        if r < 6: return 7 if b else 4
        return 8 if b else 5
    bat = bytearray()
    for b in (0, 1):
        for r in range(2, ROAD_LINES // 8):
            bat += b''.join(struct.pack('<H', 0x200 + int(t) | row_palette(r, b) << 12) for t in ids[r])
    for r in (0, 1):
        bat += b''.join(struct.pack('<H', 0x200 + int(t) | row_palette(r, 0) << 12) for t in ids[r])
    preview = np.zeros((ROAD_LINES, W, 3), np.uint8)
    pa = vce_rgb(palettes[4]); preview[:] = pa[img]
    Image.fromarray(preview).save(previews / 'road.png')
    print(f'  road picture: {len(tiles)} characters', flush=True)
    return dict(tiles=b''.join(tiles).ljust(8192, b'\0'), bat=bytes(bat), palette=b''.join(p.tobytes() for p in palettes))

def road_tables():
    """C tables for the road builder (bank $6f): the scanline of a distance (half units of distance, d = 10080 / f) and the
    distance of a scanline (z = 10080 / d; only its low byte, which is all a 32-unit band needs)."""
    d = [min(255, round(10080 / max(2 * k, 1))) for k in range(512)]
    z = [65535] + [min(65535, round(10080 / k)) for k in range(1, ROAD_LINES + 1)]
    return d, z

def race_sky(archive, previews, sand):
    """The Grand Prix's sky as plain background tiles: a blue gradient from the zenith to a pale horizon (scanline 113), then
    the haze of the ground out to the floor's first scanline (120). One 15-colour palette: 13 shades of the gradient and the
    haze; ordered dithering (8x8 Bayer, 64 steps between neighbouring shades) keeps the bands invisible, and because the
    dither repeats every 8 dots a tile row is the same all across, so the whole sky is 16 distinct characters."""
    HORIZON = 113
    def word(c): return int(vce_colors(np.clip(np.asarray(c, float), 0, 255).astype(np.uint8)))
    zenith, pale = np.array((12, 60, 188.)), np.array((200, 220, 255.))
    haze = np.asarray(sand, float) * 0.72 + pale * 0.28
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
                if y >= HORIZON: tile[dy, x] = 15
                else:
                    pos = (y / (HORIZON - 1)) ** 1.45 * 12
                    k = min(int(pos), 11); frac = pos - k
                    tile[dy, x] = k + 1 + (1 if frac * 64 > bayer[y % 8, x] else 0)
        rows.append(tile)
    pixels = np.tile(np.concatenate(rows, axis=0), (1, 64))
    # Wisps from the source panorama, using its cloud-only stretch. Keep the
    # blue gradient and omit the planet; the lower band is a distant ridge.
    cloud = np.asarray(Image.open(ROOT/'assets/sky_mode7.png').convert('RGB').crop((340,0,768,80)).resize((512,80),Image.Resampling.LANCZOS))
    light = np.maximum(cloud[...,0].astype(float)-35,0) / 220
    pixels[:80] = np.minimum(13, pixels[:80] + (light*5).astype(np.uint8))
    palette[14] = word(np.asarray(sand)*0.68 + np.array((110,135,180))*0.32)
    for x in range(512):
        ridge = round(103 + 5*math.sin(x*2*math.pi/512) + 3*math.sin(x*6*math.pi/512))
        pixels[ridge:HORIZON,x] = 14
    tiles, lookup, columns = [], {}, []
    for x in range(64):
        column = bytearray()
        for y in range(30):
            enc = planar_tile(pixels[min(y,15)*8:min(y,15)*8+8,x*8:x*8+8])
            if enc not in lookup: lookup[enc]=len(tiles);tiles.append(enc)
            column += struct.pack('<HB',lookup[enc],0)
        columns.append(column)
    assert len(tiles)<=256, 'Race panorama exceeds sky VRAM'
    pal_off = archive.add('bg_palette', np.asarray([palette] * 16, '<u2').tobytes())
    tile_off = archive.add('bg_patterns', b''.join(tiles))
    map_off = archive.add('bg_columns', b''.join(columns))
    preview = np.tile(vce_rgb(palette)[pixels[-8:]], (30,1,1))
    preview[:128] = vce_rgb(palette)[pixels]
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
    boss=[im for name,im,_ in sprites if name in ('gunship_left','gunship_right','hyperjumper_left','hyperjumper_right')]
    boss_palette=palette_for(boss) if boss else None
    for name, im, anchor in sprites:
        pat, parts, palette, line = pack_sprite(im, anchor,boss_palette if name in ('gunship_left','gunship_right','hyperjumper_left','hyperjumper_right') else fg_palette if name.startswith("foreground_") else hud_palette if name.startswith('hudp_') else None)
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
        def hull_record(hull,hull_anchor,mirror_from=None):
            """One record of 32x32 cells. With mirror_from (a symmetric hull on an odd number of cells: the centre cell is its own
            mirror), only the left half and the centre are stored; the right half is the same patterns drawn flipped
            (bit 15 of the pattern number), which halves the VRAM patterns the hull needs."""
            pal=palette_for([hull]);idx=indexed(hull,pal);patterns=[];pieces=[]
            def cell_at(source,x,y):
                cell=source[y:y+32,x:x+32]
                return np.pad(cell,((0,32-cell.shape[0]),(0,32-cell.shape[1])))
            def add_cell(cell):
                n=len(patterns)
                for row in (0,16):
                    for col in (0,16):patterns.append(planar_sprite(cell[row:row+16,col:col+16]))
                return n
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
            assert len(patterns)<=96 and len(pieces)<=28,(len(patterns),len(pieces))
            bp=a.add('boss_big_palette',pal.tobytes())
            bd=a.add('boss_big_pieces',b''.join(struct.pack('<hhH',*r) for r in pieces))
            bt=a.add('boss_big_patterns',b''.join(patterns))
            return struct.pack('<IIIHB',bp,bd,bt,len(patterns)*128,len(pieces))
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
        for scale in (1.0,0.75,0.5):
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
            hull_records+=hull_record(Image.fromarray(sym),(80,size[1]//2),mirror_from=True)
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
        # The world: the sky panorama round the planet above a horizon at row 112 and the desert floor below, full
        # screen; the cockpit art (centre 256 of its 426 columns, like the source's 4:3 view) goes over it with its
        # monitors and consoles, and the viewing window is whatever the art leaves transparent.
        sky = Image.open(ROOT / 'assets/ramrod/sky.png').convert('RGBA').crop((330, 17, 700, 170)).resize((256, 105), Image.Resampling.LANCZOS)
        floor = Image.open(ROOT / 'assets/ramrod/floor.png').convert('RGBA').resize((256, 112), Image.Resampling.LANCZOS)
        bg = Image.new('RGBA', (256, 224), (34, 126, 200, 255))
        bg.paste(sky, (0, 7)); bg.paste(floor, (0, 112))
        shade = np.linspace(0.74, 1.0, 112)[:, None, None]
        arr = np.asarray(bg).astype(float); arr[112:, :, :3] *= shade; bg = Image.fromarray(arr.astype(np.uint8))
        cock = Image.open(ROOT / 'assets/ramrod/cockpit.png').convert('RGBA').crop((85, 0, 341, 224))
        bg.alpha_composite(cock)
        # The two monitors the source draws over the art (ramrod.c render_monitors, 52x33 each): the radar's rings and
        # view cone and the status screen's labels and bar troughs are static; dots, bars and digits are sprites.
        d = ImageDraw.Draw(bg)
        rx, ry = 33, 23
        d.rectangle((rx, ry, rx + 51, ry + 32), fill=(6, 34, 20, 255))
        rcx, rcy = rx + 26, ry + 18
        for radius in (8, 16):
            d.ellipse((rcx - radius, rcy - radius, rcx + radius, rcy + radius), outline=(20, 90, 50, 255))
        d.rectangle((rx, ry, rx + 51, ry + 1), fill=(6, 34, 20, 255)); d.rectangle((rx, ry + 31, rx + 51, ry + 32), fill=(6, 34, 20, 255))
        d.line((rcx, rcy, rcx - 12, rcy - 14), fill=(20, 90, 50, 255)); d.line((rcx, rcy, rcx + 12, rcy - 14), fill=(20, 90, 50, 255))
        sx, sy = 174, 23
        d.rectangle((sx, sy, sx + 51, sy + 32), fill=(18, 22, 60, 255))
        for label, ly, color in (('ARM', 1, (255, 210, 120)), ('GUN', 11, (255, 210, 120)), ('W', 22, (150, 200, 255)), ('x', 22, (255, 255, 255))):
            lim = fonts.text(label, color)
            bg.alpha_composite(lim, (sx + (38 if label == 'x' else 2), sy + ly + (1 if label != 'x' else 2)))
        for ty in (3, 13): d.rectangle((sx + 27, sy + ty, sx + 27 + 21, sy + ty + 4), fill=(4, 6, 20, 255))
        sprites = shared[36:39]
        # Mech frames at eight baked widths: the sprite pulls its 16 px slices together (sprite_generic's scale) between
        # two baked widths, which hides the steps (the multiple-versions-plus-slices scheme of the Plutiedev scaling article).
        for kind in ('mech','mech_red','mech_gold'):
            for size in MECH_SIZES: sprites += atlas(ROOT / 'assets/ramrod/atlas.png', kind, size)
        sprites += atlas(ROOT / 'assets/ramrod/atlas.png', 'arm', 64)
        hud = hudart.Hud(sprites)
        hudart.add_bar_fills(hud, ('green', 'yellow', 'red', 'orange'))
        hudart.add_digits(hud, fonts, ('white', 'cyan', 'pink'))
        for name, c, n in (('dot_green', (120, 255, 200), 2), ('dot_gold', (255, 210, 40), 3), ('dot_red', (255, 70, 60), 3), ('dot_white', (255, 255, 255), 3)):
            im = Image.new('RGBA', (16, 16)); ImageDraw.Draw(im).rectangle((0, 0, n - 1, n - 1), fill=(*c, 255)); hud.add(name, im)
        for name, c in (('cross_green', (120, 255, 140)), ('cross_red', (255, 70, 60))):
            im = Image.new('RGBA', (16, 16)); dd = ImageDraw.Draw(im)
            dd.line((0, 8, 5, 8), fill=(*c, 230)); dd.line((10, 8, 15, 8), fill=(*c, 230)); dd.line((8, 0, 8, 5), fill=(*c, 230)); dd.line((8, 10, 8, 15), fill=(*c, 230)); dd.point((8, 8), fill=(*c, 230))
            hud.add(name, im, (8, 8))
        for danger, c in (('', (255, 200, 60)), ('_danger', (255, 60, 60))):
            im = Image.new('RGBA', (16, 16)); dd = ImageDraw.Draw(im)
            for k in range(5): dd.line((k, 5 - k, k, 5 + k), fill=(*c, 255))
            hud.add('chevron' + danger, im)
        for name, text, color, big in (('wave1', 'WAVE 1', (255, 182, 0), True), ('wave2', 'WAVE 2', (255, 182, 0), True), ('wave3', 'WAVE 3', (255, 182, 0), True),
                                       ('cleared', 'WAVE CLEARED', (255, 255, 255), True), ('destroyed', 'SQUADRON DESTROYED', (255, 255, 255), True),
                                       ('down', 'RAMROD IS DOWN!', (255, 60, 60), True), ('warning', 'WARNING: COMMAND MECH', (255, 80, 80), False)):
            hudart.add_text(hud, fonts, name, text, color, big)
        meta['hud'] = hud.base; meta['hud_macros'] = hud.macros('H6')
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
    if stage in (1,3,4,5):platform_dialog(a,work)
    meta['presentation']=presentation.add_art(ROOT,work,stage,sprites,cblock_frame)
    meta['foreground_offset']=0;meta['foreground_count']=0
    if stage in (1,3,4,5):
        # Platform playfields now include the source's top 16 lines. Gameplay
        # sprites passed in world-16 coordinates are baked 16 lines lower, so
        # the renderer needs no per-draw Y adjustment.
        hud0=meta['presentation']['hud'][0][0];aim0=meta['presentation']['aim'][0][0];motion0=meta['presentation']['motion'][0][0]
        for i,(name,im,(ax,ay)) in enumerate(sprites):
            if i<hud0 or aim0<=i<meta['presentation']['end']:   # gameplay, aim and motion poses (not the HUD)
                sprites[i]=(name,im,(ax,ay-16))
        if stage in (1,3):   # no foreground layer at all: whatever is left of it flickers (stage 4 keeps its cabin walls)
            foreground=Image.new('RGBA',foreground.size);print(f'  stage {stage}: foreground removed', flush=True)
        else: foreground=thin_foreground(foreground)
        entries=presentation.add_foreground(foreground,sprites)
        meta['foreground_offset']=a.add('foreground_sprites',b''.join(struct.pack('<hhH',*v) for v in entries))
        meta['foreground_count']=len(entries)
        foreground.crop((0,0,1024,224)).save(previews/f'foreground{stage}.png')
    meta['story_offset']=story.bake(ROOT,work,stage,a,meta['presentation']['portraits'])
    meta.update(race_sky(a, previews, race_sand) if stage == 2 else native_background(bg, a, previews, f'stage{stage}'))
    sprite_table, rows, costs = add_sprites(a, list(sprites), previews)
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
    glyphs = []
    font_path = work/'srgb'/'12072E60.srgb'
    for ch in range(32, 128):
        px = np.asarray(cblock_frame(font_path, ch - 0x21)) if ch > 32 and ch - 0x21 < 106 else np.zeros((8,8,4), np.uint8)
        glyphs.append(planar_tile(((px[..., 3] >= 64) * 15).astype(np.uint8)))
    (out/'font.bin').write_bytes(b''.join(glyphs))
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
    for name in ('dialog_bg','dialog_corners','dialog_original_font'):
        h.append(f'extern const uint32_t pce_{name}[7];')
        c.append(f'const uint32_t pce_{name}[7]={{'+','.join(str(m['records'].get(name,{}).get('offset',0))+'UL' for m in scenes)+'};')
    h.append('extern const uint16_t pce_hud_base[7];')
    c.append('const uint16_t pce_hud_base[7]={'+','.join(str(m.get('hud',0)) for m in scenes)+'};')
    for m in scenes: h += m.get('hud_macros',[])
    h += [f'#define PCE_CAR_STEPS {len(CAR_WIDTHS)}', '#define PCE_CAR_STEER (3+7*PCE_CAR_STEPS)', '#define PCE_CAR_SPIN (PCE_CAR_STEER+4)', f'#define PCE_CAR_SPIN_FRAMES {SPIN_FRAMES}', '#define PCE_CAR_TURBO (PCE_CAR_SPIN+2*(PCE_CAR_SPIN_FRAMES-1))', 'extern const uint8_t pce_car_widths[PCE_CAR_STEPS];']
    h += [f'#define PCE_MECH_STEPS {len(MECH_SIZES)}', f'#define PCE_MECH_ARM (3+3*PCE_MECH_STEPS*8)', 'extern const uint8_t pce_mech_sizes[PCE_MECH_STEPS];']
    c.append('const uint8_t pce_car_widths[PCE_CAR_STEPS]={'+','.join(map(str,CAR_WIDTHS))+'};')
    c.append('const uint8_t pce_mech_sizes[PCE_MECH_STEPS]={'+','.join(map(str,MECH_SIZES))+'};')
    d_table,z_table=road_tables()
    h.append('extern const uint8_t pce_road_d[512];extern const uint8_t pce_road_z[113];')
    c.append('const uint8_t pce_road_d[512] __attribute__((section(".ram_bank111.rodata")))={'+','.join(map(str,d_table))+'};')
    c.append('const uint8_t pce_road_z[113] __attribute__((section(".ram_bank111.rodata")))={'+','.join(str(v&255) for v in z_table)+'};')
    c.append('const uint8_t pce_actor_ids[33] = {'+','.join(map(str,scenes[0]['actor_ids']))+'};')
    (out/'assets.c').write_text('\n'.join(c)+'\n')
    h += ['#define PCE_HERO_FRAMES 9', '#define PCE_SHOT_ID 36', '#define PCE_ENEMY_SHOT_ID 37', '#define PCE_BLAST_ID 38']
    for name in ('road_tiles','road_bat','road_palette','race_map','pursuit_row','dialog_wide'):
        h.append(f"#define PCE_RACE_{name.upper()} {scenes[1]['records'][name]['offset']}UL")
    (out/'assets.h').write_text('\n'.join(h)+'\n')
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
