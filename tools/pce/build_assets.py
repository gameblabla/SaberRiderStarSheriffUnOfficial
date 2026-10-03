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
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tools/saturn'), str(ROOT / 'tools/dc')]
import levl
import texbake
from formats import Archive, indexed, palette_for, planar_tile, pack_sprite, pair_characters, vce_rgb

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
    exe = work / 'texprep'
    run(['cc', '-std=gnu11', '-O2', '-Isrc', 'tools/dc/texprep.c', 'src/gfx.c', 'src/font.c',
         'src/pack.c', 'src/lzo1z.c', 'src/assets.c', 'src/namehash.c', '-o', exe])
    run([exe, ROOT / 'SaberRider/data', srgb])
    # Definitions supply the same asset IDs/anchors as the gameplay sources.
    ids = ['8403195A', '9C8F9A9E', '79260A58', '26818B85', '112DF34C', '02A38AFB', 'D39700C4', '2A02BD4F', 'FBFAF817', '4042CD71', '71887ECA', 'BFDAB70F', '1D724DD9', '211F5D78', '9393E59B', '20C6FAEF', 'ECC992CB', '72B53EF8', '925534E2', '916137ED', '906D3698', 'F5975DCF', 'F4A55EDE', 'F4A25ED9', 'F3B05E28', '0DB9F0E0', '29CAD5D3']
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

def native_background(image, archive, previews, name):
    w = (image.width + 7) // 8 * 8
    canvas = Image.new('RGBA', (w, 224), (0, 0, 0, 255)); canvas.paste(image)
    # Four vertical color bands, four palettes each. Group by per-cell mean
    # color; each group keeps 15 colors plus a common black backdrop.
    rgba = np.asarray(canvas)
    cells = rgba.reshape(28, 8, w // 8, 8, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 8, 8, 4)
    means = cells[..., :3].mean((1, 2))
    groups = np.zeros(len(cells), np.uint8)
    palette = []
    row_band = np.repeat(np.arange(28) // 7, w // 8)
    for band in range(0 if name=='stage2' else 4):
        select = np.nonzero(row_band == band)[0]
        m = means[select]
        centers = m[np.linspace(0, len(m) - 1, 4, dtype=int)]
        for _ in range(8):
            g = ((m[:, None] - centers[None]) ** 2).sum(-1).argmin(1)
            for k in range(4):
                if (g == k).any(): centers[k] = m[g == k].mean(0)
        groups[select] = band * 4 + g
        for k in range(4):
            group = cells[select[g == k]]
            palette.append(palette_for([Image.fromarray(group.reshape(-1, 8, 4))]) if len(group) else np.zeros(16, '<u2'))
    if name=='stage2':
        # A 128x32 four-color source gives <=256 distinct 2x2 source cells
        # after 4x nearest expansion, fitting the 288-character sky budget.
        p=palette_for([canvas.crop((0,0,512,128))],colors=4)
        p[5:]=p[1];palette=[p]*16
    tiles, tile_lookup, names = [], {}, []
    preview = np.zeros_like(rgba)
    for i, cell in enumerate(cells):
        pal = int(groups[i]); idx = indexed(Image.fromarray(cell), palette[pal])
        encoded = planar_tile(idx)
        key = encoded  # palette is stored per map entry, not duplicated in patterns
        if key not in tile_lookup: tile_lookup[key] = len(tiles); tiles.append(encoded)
        names.append((tile_lookup[key], pal))
        y, x = divmod(i, w // 8)
        preview[y * 8:y * 8 + 8, x * 8:x * 8 + 8, :3] = vce_rgb(palette[pal])[idx]
        preview[y * 8:y * 8 + 8, x * 8:x * 8 + 8, 3] = 255
    if len(tiles) > 65535: raise ValueError(f'{name}: too many background characters')
    n = np.asarray(names, np.uint16).reshape(28, w // 8, 2).transpose(1, 0, 2)
    mapping = b''.join(struct.pack('<HB', int(t), int(p)) for t, p in n.reshape(-1, 2))
    pal_off = archive.add('bg_palette', np.asarray(palette, '<u2').tobytes())
    tile_off = archive.add('bg_patterns', b''.join(tiles))
    map_off = archive.add('bg_columns', mapping)
    Image.fromarray(preview[:, :min(w, 1024)]).save(previews / f'{name}.png')
    return dict(pal=pal_off, tiles=tile_off, map=map_off, cols=w // 8, tile_count=len(tiles))

def platform_background(stage, work):
    level, files = levl.load_dump(work / f'stage{stage}.layers')
    banks = {i: levl.png_bank(path, i) if path else levl.load_bank(work / 'srgb' / f'{i:08X}.srgb')
             for i, path in files.items()}
    width = math.ceil(level.width / 256) * 256
    out = Image.new('RGBA', (width, 224), (0, 0, 0, 255))
    foreground = Image.new('RGBA',(width,224))
    for x in range(0,width,256):
        pixels=np.zeros((240,256,4),np.uint8)
        front=np.zeros_like(pixels);after_player=False
        if stage in (1,3):
            pixels[:]=np.asarray(presentation.sky(stage,(256,240)))
        for ly in level.layers:
            if ly.name=='PlayerSprites':after_player=True
            if not ly.is_tilemap:continue
            if stage in (1,3) and ly.name=='SkyBG':continue
            bank=banks[ly.cblock]
            if after_player:
                front=levl.render_layer(ly,bank,levl.layer_offset(ly,x),256,240,front)
            else:
                pixels=levl.render_layer(ly,bank,levl.layer_offset(ly,x),256,240,pixels)
        out.paste(Image.fromarray(pixels[16:240]),(x,0))
        foreground.paste(Image.fromarray(front[16:240]),(x,0))
    return out,foreground

def add_sprites(archive, sprites, previews):
    rows, costs = [], []
    fg=[im for name,im,_ in sprites if name.startswith('foreground_')]
    fg_palette=palette_for(fg) if fg else None
    for name, im, anchor in sprites:
        pat, parts, palette, line = pack_sprite(im, anchor,fg_palette if name.startswith("foreground_") else None)
        if not parts or len(parts)>32:
            raise ValueError(f'{name}: expected 1..32 visible sprite pieces, got {len(parts)}')
        offset = archive.add(name + '_patterns', pat)
        desc = archive.add(name + '_pieces', b''.join(struct.pack('<hhH', *p) for p in parts))
        pal = archive.add(name + '_palette', palette.tobytes())
        rows.append((offset, desc, pal, len(parts), im.width, im.height))
        costs.append(dict(name=name, patterns=len(pat), entries=len(parts), units_per_line=line,
                          width=im.width, height=im.height, facing_variants=1))
    table = archive.add('sprite_table', b''.join(struct.pack('<IIIHBB', *r) for r in rows))
    return table, rows, costs

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
        for k in range(6):
            legs = frame(104 + k)
            bob=(0,1,2,0,1,2)[k] if hero==0 else (1 if k in (2,5) else 0) if hero in (1,3) else 0
            a = np.asarray(legs).copy(); a[:38 + bob if hero==0 else 0] = 0
            merged = Image.fromarray(a)
            merged.alpha_composite(frame(88 + k), (0, bob))
            out.append((f'hero{hero}_run{k}', merged, (32, 32)))
        out.append((f'hero{hero}_jump', frame(132), (32, 32)))
        out.append((f'hero{hero}_crouch', frame(120), (32, 32)))
    # A native projectile and blast can coexist with every stage palette.
    for name, color, radius in [('shot', (255, 255, 180, 255), 2), ('enemy_shot', (255, 50, 50, 255), 3), ('blast', (255, 160, 30, 255), 7)]:
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
            triggers.append(struct.pack('<4h3HBbB', *t['zone'], max(1,round(t['interval']*60/1000)),t['delay'],t['type'],t['layer'],max(-1,min(127,t['loops'])),len(points)//2)
                            +struct.pack('<16h',*(points+[0]*(16-len(points)))))
        if len(triggers)>60 or meta['rows']>32 or (meta['cellw'],meta['cellh'])!=(8,8):
            raise ValueError('Platform stage exceeds the native hot-cache limits')
        meta['trigger_offset']=a.add('triggers',b''.join(triggers));meta['ntr']=len(triggers)
        zones=meta['dialogs'] if stage==1 else []
        deaths=meta['deathzones'] if stage==1 else []
        rules=bytes([len(zones),len(deaths)])
        rules+=b''.join(struct.pack('<4h',*z['zone']) for z in zones)
        rules+=b''.join(struct.pack('<6h',*z) for z in deaths)
        meta['rules_offset']=a.add('flow_zones',rules)

        sprites = list(shared)
        if stage in (1,5):
            d=(work/'2A02BD4F.levl').read_bytes();aid=struct.unpack_from('<I',d,4)[0]
            first=struct.unpack_from('<I',d,0x34+2*24+4)[0]
            im=cblock_frame(work/'srgb'/f'{aid:08X}.srgb',first)
            im=im.resize((96,48),Image.Resampling.NEAREST)
            sprites.append(('gunship',im,(48,24)))
        else:
            im=Image.open(ROOT/'assets/hyperjumper/side_normal.png').convert('RGBA').resize((96,48),Image.Resampling.NEAREST)
            sprites.append(('hyperjumper',im,(48,24)))
        sprites.append(('dark_april',shared[18][1],shared[18][2]))
        meta['actor_ids']=[255]*33
        for t in range(1,33):meta['actor_ids'][t]=39 if t in (1,3,4) else 40 if t in (2,5,28) else 41
        for t,crhc in enumerate(['FBFAF817','4042CD71','71887ECA','BFDAB70F','1D724DD9','211F5D78','9393E59B','20C6FAEF','ECC992CB','72B53EF8','925534E2','916137ED','906D3698','F5975DCF','F4A55EDE','F4A25ED9','F3B05E28'],11):
            d=(work/f'{crhc}.levl').read_bytes();aid=struct.unpack_from('<I',d,4)[0]
            first=struct.unpack_from('<I',d,0x34+(2 if t>=24 or t==11 else 1)*24+4)[0]
            art=work/'srgb'/f'{aid:08X}.srgb'
            if not art.exists():
                meta['actor_ids'][t]=255;continue
            im=cblock_frame(art,first)
            if not im.getchannel('A').getbbox():
                meta['actor_ids'][t]=255;continue
            anchor=tuple(round(v) for v in struct.unpack_from('<ff',d,8))
            # Wide scenery retains a bounded canonical metasprite.
            if im.width>128 or im.height>64:
                scale=min(128/im.width,64/im.height)
                im=im.resize((max(1,round(im.width*scale)),max(1,round(im.height*scale))),Image.Resampling.NEAREST)
                anchor=tuple(round(v*scale) for v in anchor)
            meta['actor_ids'][t]=len(sprites)
            sprites.append((f'actor_type{t}',im,anchor))
    elif stage == 6:
        bg = Image.open(ROOT / 'assets/ramrod/cockpit.png').convert('RGBA').resize((256, 224), Image.Resampling.NEAREST)
        sky = Image.open(ROOT / 'assets/ramrod/sky.png').convert('RGBA').resize((256, 136), Image.Resampling.NEAREST)
        # Recompose a rectangular viewing window. All mech pieces are clipped
        # to it at runtime; background priority cannot mask an opaque sky.
        bg.paste(sky.crop((0, 0, 224, 116)), (16, 20))
        floor = Image.open(ROOT / 'assets/ramrod/floor.png').convert('RGBA').resize((224, 44), Image.Resampling.NEAREST)
        bg.paste(floor, (16, 136))
        sprites = shared[36:39]
        for kind in ('mech','mech_red','mech_gold'):
            for size in (32, 48, 64, 80): sprites += atlas(ROOT / 'assets/ramrod/atlas.png', kind, size)
        sprites += atlas(ROOT / 'assets/ramrod/atlas.png', 'arm', 64)
        collision = 0
    elif stage == 7:
        bg = Image.open(ROOT / 'assets/space/nebula.png').convert('RGBA').resize((512, 224), Image.Resampling.NEAREST)
        sprites = shared[36:39] + atlas(ROOT / 'assets/space/atlas.png', 'player') + atlas(ROOT / 'assets/space/atlas.png', 'fighter') + atlas(ROOT / 'assets/space/atlas.png', 'gunship')
        im=Image.open(ROOT/'assets/space/boss.png').convert('RGBA').resize((128,64),Image.Resampling.NEAREST)
        sprites.append(('battle_cruiser',im,(0,32)))
        sprites+=atlas(ROOT/'assets/space/atlas.png','drone')+atlas(ROOT/'assets/space/atlas.png','mine')
        sprites+=atlas(ROOT/'assets/space/atlas.png','cap')
        meta['track_offset']=a.add('space_timeline',timeline.bake(ROOT/'src/space.c'))
        collision = 0
    else:
        bg = Image.new('RGBA', (512,224), (0,0,0,255))
        bg.paste(Image.open(ROOT/'assets/sky_mode7.png').convert('RGBA').resize((128,32)).resize((512,128),Image.Resampling.NEAREST), (0,0))
        sprites = shared[36:39]
        im = Image.open(ROOT / 'assets/mode7.png').convert('RGBA')
        for row in (ROOT / 'assets/mode7.txt').read_text().splitlines():
            name, x, y, w, h, frames = row.split(); x, y, w, h, frames = map(int, (x,y,w,h,frames))
            if name not in ('buggy', 'hornet', 'leader', 'firenza', 'racer_blue', 'racer_purple', 'mine'): continue
            for scale in (24, 40, 64):
                nh = round(h * scale / w)
                car = im.crop((x, y, x+w, y+h)).resize((scale, nh), Image.Resampling.NEAREST)
                car=car.resize((scale*2,nh),Image.Resampling.NEAREST)
                sprites.append((f'{name}{scale}', car, (scale, nh)))
        env = dict(os.environ, SABER_ASSETS=str(ROOT / 'assets'), SABER_FRAMES='1', SABER_M7MAP=str(work/'race.pgm'))
        run([ROOT/'build/headless/saber_headless', ROOT/'SaberRider/data', 2], env=env)
        race = np.asarray(Image.open(work/'race.pgm'), np.uint8) // 20
        a.add('race_map',race.tobytes())
        meta['track_offset']=a.add('track', (work/'track.bin').read_bytes())
        a.add('pair_characters', pair_characters())
        floor = im.crop((0, 119, 352, 151))
        pal = palette_for([floor]); pal[15]=0x1ff; a.add('floor_palette', pal.tobytes())
        # 11 materials share the palette, so either half of a BAT pair can
        # sample any material without selecting conflicting subpalettes.
        tex = indexed(floor, pal)
        a.add('floor_textures', np.stack([tex[:, k*32:(k+1)*32] for k in range(11)]).tobytes())
        a.add('floor_mips', np.stack([tex[2::4,k*32+2:(k+1)*32:4] for k in range(11)]).tobytes())
        # Pursuit is the source's straight desert road; both working sets fit
        # by representing its repeated row rather than another 1 MiB map.
        road = np.zeros(1024, np.uint8); road[496:528] = 7; road[498:526] = 2; road[511:513] = 3
        a.add('pursuit_row', road.tobytes())
        collision = 0
    # Append presentation art after fixed gameplay IDs to retain mission IDs.
    meta['presentation']=presentation.add_art(ROOT,work,stage,sprites,cblock_frame)
    meta['foreground_offset']=0;meta['foreground_count']=0
    if stage in (1,3,4,5):
        entries=presentation.add_foreground(foreground,sprites)
        meta['foreground_offset']=a.add('foreground_sprites',b''.join(struct.pack('<hhH',*v) for v in entries))
        meta['foreground_count']=len(entries)
        foreground.crop((0,0,1024,224)).save(previews/f'foreground{stage}.png')
    meta['story_offset']=story.bake(ROOT,work,stage,a,meta['presentation']['portraits'])
    meta.update(native_background(bg, a, previews, f'stage{stage}'))
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
    ui_values,briefing,ui_bytes=presentation.bake_frontend(ROOT,work,out,previews,native_background,cblock_frame)
    # 96 ASCII glyphs use background characters, palette 15.
    glyphs = []
    font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf', 8)
    for ch in range(32, 128):
        im = Image.new('1', (8,8)); ImageDraw.Draw(im).text((0,-2), chr(ch), font=font, fill=1)
        glyphs.append(planar_tile(np.asarray(im, np.uint8)*15))
    # Four spare glyphs form the bordered dialogue panel in the BAT.
    for k in range(4):
        tile=np.zeros((8,8),np.uint8)
        if k==1:tile[0:2,:]=15
        if k==2:tile[:,0:2]=15
        if k==3:tile[:,6:8]=15
        glyphs[91+k]=planar_tile(tile)
    (out/'font.bin').write_bytes(b''.join(glyphs))
    h = ['/* Generated: all offsets are Arcade RAM byte addresses. */', '#pragma once', '#include <stdint.h>',
         'typedef struct { uint32_t bytes, pal, tiles, map, collision, sprites, triggers, story, track, rules, occlusion, foreground; uint16_t cols, ccols, crows, nsprites, nforeground; int16_t sx, sy, width; uint8_t ntr, cw, ch; } PceScene;',
         'extern const PceScene pce_scenes[7];','extern const uint8_t pce_actor_ids[33];']
    c=['#include "pce_config.h"','#include "assets.h"','const PceScene pce_scenes[7] = {']
    for m in scenes:
        values = [m['bytes'],m['pal'],m['tiles'],m['map'],m['collision'],m['sprite_table'],m.get('trigger_offset',0),m['story_offset'],m.get('track_offset',0),m.get('rules_offset',0),0,m['foreground_offset'],m['cols'],m.get('cols',0) if 'cellw' not in m else m['width']//m['cellw'],m.get('rows',0),m['sprite_count'],m['foreground_count'],*m.get('start',(128,180)),m.get('width',m['cols']*8),m.get('ntr',0),m.get('cellw',8),m.get('cellh',8)]
        # Native map columns and collision columns are independent.
        if 'cellw' in m: values[13] = json.loads((work/f"stage{m['stage']}.json").read_text())['cols']
        c.append('    {' + ','.join(str(v) for v in values) + '},')
    c.append('};')
    presentation.emit_tables(out,scenes,h,c)
    h+=['extern const PceScene pce_ui_scenes[6];',f'#define PCE_UI_BYTES {ui_bytes}UL',f'#define PCE_BRIEFING_TEXT {briefing}UL']
    c.append('const PceScene pce_ui_scenes[6]={'+','.join('{'+','.join(map(str,v))+'}' for v in ui_values)+'};')
    c.append('const uint8_t pce_actor_ids[33] = {'+','.join(map(str,scenes[0]['actor_ids']))+'};')
    (out/'assets.c').write_text('\n'.join(c)+'\n')
    h += ['#define PCE_HERO_FRAMES 9', '#define PCE_SHOT_ID 36', '#define PCE_ENEMY_SHOT_ID 37', '#define PCE_BLAST_ID 38']
    for name in ('pair_characters','floor_palette','floor_mips','race_map','pursuit_row'):
        h.append(f"#define PCE_RACE_{name.upper()} {scenes[1]['records'][name]['offset']}UL")
    (out/'assets.h').write_text('\n'.join(h)+'\n')
    # Runtime work buffers are separate from BIOS/compiler console RAM.
    manifest = dict(format='PCE1', toolchain=str((ROOT.parent/'PCE/llvm-mos8').resolve()),
                    source='Current host pack decoder and active stage construction', scenes=scenes,
                    vram=dict(bat=4096, bg_cache=29696, font=3072, sprites=24576,
                              clipped_sprites=3584, sat=512),
                    facing_policy='One canonical facing; mirror placement and SAT bit 0x0800 at runtime',
                    adaptations=['One baked background; source parallax anchored per 256-pixel sector',
                                 'Top 16 source lines cropped; world and collision coordinates retained'])
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    digest = hashlib.sha256()
    for p in sorted((ROOT/'src').glob('*.[ch]')): digest.update(p.read_bytes())
    (out/'source.sha256').write_text(digest.hexdigest()+'\n')

if __name__ == '__main__': main()
