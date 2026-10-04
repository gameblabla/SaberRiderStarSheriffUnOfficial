"""Reuse the Saturn/source artwork for PCE menus, portraits, HUD and aim poses."""
import json
import struct
import textwrap
import numpy as np
from PIL import Image, ImageDraw

def namehash(s):
    b=s.encode();a=sum(c*(i+1) for i,c in enumerate(b));v=sum(c*(i+1) for i,c in enumerate(b[1:]))
    h=(v<<16)|a
    for c,d in zip(b,b[1:]):h=(h+~((c<<24)|d))&0xffffffff
    for c in b[:0:-1]:h^=c|((~c<<8)&0xffffffff)
    return h&0xffffffff

# Fixed backdrop colours: blue cyan by day, dark purple at night.
SKY_COLOURS = {1: (72, 182, 218), 3: (36, 0, 72)}

def sky_color(stage):
    """One VCE backdrop colour per stage, loaded with the scene palette."""
    from formats import vce_colors
    return int(vce_colors(SKY_COLOURS.get(stage, (0, 0, 0))))

def add_foreground(image,sprites):
    entries=[];cache={}
    # Sparse canonical 32x32 chunks share sprite patterns across repeated rails.
    for x in range(0,image.width,32):
        for y in range(0,image.height,32):
            im=image.crop((x,y,x+32,y+32))
            if not im.getchannel('A').getbbox():continue
            key=im.tobytes()
            if key not in cache:
                cache[key]=len(sprites);sprites.append((f'foreground_{len(cache)}',im,(0,0)))
            entries.append((x,y,cache[key]))
    return entries

# Dialogue box tilesets as the Saturn build picks them: green (default), purple, red, blue.
BOX_TILESETS=(0x8D39AA67,0xA2122E71,0x1495B0AB,0x84652CBC)
BOX_W,BOX_H=224,48

def dialog_box(tiles):
    """Nine-slice the source tileset exactly like dialog.c draw_box does."""
    im=Image.new('RGBA',(BOX_W,BOX_H));cw=ch=16;mx=BOX_W-2*cw;my=BOX_H-2*ch
    def put(n,x,y,w,h):im.alpha_composite(tiles[n].resize((w,h),Image.Resampling.NEAREST),(x,y))
    put(0,0,0,cw,ch);put(1,cw,0,mx,ch);put(2,BOX_W-cw,0,cw,ch)
    put(3,0,ch,cw,my);put(4,cw,ch,mx,my);put(5,BOX_W-cw,ch,cw,my)
    put(6,0,BOX_H-ch,cw,ch);put(7,cw,BOX_H-ch,mx,ch);put(8,BOX_W-cw,BOX_H-ch,cw,ch)
    # The source fills the inside with a vertical gradient. Text glyphs are opaque cells of one colour, so the inside is a
    # single flat colour (the top one): nothing can clash with the characters.
    a=np.asarray(im).copy();fill=a[BOX_H//2-1,BOX_W//2].copy()
    inside=(a[...,3]>=128)&~((a[...,:3]>=250).all(axis=-1))
    a[inside]=fill
    return Image.fromarray(a)


def art_tips(hero,cell):
    """Barrel tips (offset from the cell's anchor) of the aim and shoot poses exactly as the PCE draws them, in
    pce_muzzle order (None = not derived here). The gun is the furthest-reaching part of each pose along its
    aim; checked against the source's own barrel-pixel table (heroes.c) it agrees to a pixel for Saber and April."""
    def alpha(im):return np.asarray(im)[:,:,3]>=64
    def reach(a,ymax):                    # furthest right, among the rows above ymax
        ys,xs=np.nonzero(a);keep=ys<=ymax;xs,ys=xs[keep],ys[keep];i=np.argmax(xs)
        return xs[i]-32,float(ys[xs==xs[i]].mean())-32
    def diag_up(a):
        ys,xs=np.nonzero(a);i=np.argmax(xs-ys);return xs[i]-32,float(ys[i])-32
    def mean(tips):return [int(round(sum(t[k] for t in tips)/len(tips))) for k in (0,1)]
    def run(torso,fn,ymax):
        tips=[]
        for k in range(6):
            bob=(0,1,2,0,1,2)[k] if hero==0 else (1 if k in (2,5) else 0) if hero in (1,3) else 0
            legs=np.asarray(cell(104+k)).copy();legs[:38+bob if hero==0 else 0]=0
            im=Image.fromarray(legs);im.alpha_composite(cell(torso),(0,bob))
            tips.append(fn(alpha(im)) if fn else reach(alpha(im),ymax))
        return mean(tips)
    stand=mean([reach(alpha(cell(40)),48),reach(alpha(cell(41)),48)])
    return [stand,run(88,None,40),mean([reach(alpha(cell(120)),64)]),None,None,
            mean([diag_up(alpha(cell(44)))]),mean([reach(alpha(cell(42)),50)]),run(100,diag_up,0),run(98,None,44)]

def add_art(root,work,stage,sprites,frame):
    def get(rid,n=0):return frame(work/'srgb'/f'{rid:08X}.srgb',n)
    def add(name,im,anchor=(0,0)):
        i=len(sprites);sprites.append((name,im,anchor));return i
    # Source avatars retain expression variants and speaker identity.
    portraits={}
    names=['fireball1','saber2','april2','colt2','outrider','darkapril','darkapril1','darkapril2']
    # Every portrait referenced by a source script is included, even beyond the
    # common preloaded five. Avoid guessing a speaker from the selected hero.
    import re
    for p in (root/'src').glob('*.c'):
        names+=re.findall(r'dialog_avatar_([a-zA-Z0-9_]+)',p.read_text())
    for name in sorted(set(names)):
        rid=namehash('dialog_avatar_'+name);path=work/'srgb'/f'{rid:08X}.srgb'
        if not path.exists():continue
        im=get(rid).resize((32,32),Image.Resampling.NEAREST)
        if stage==2:im=im.resize((64,32),Image.Resampling.NEAREST)
        portraits[name]=add('portrait_'+name,im)
        portraits[f'{rid:08X}']=portraits[name]
    # Dialogue box: two halves per colour (a sprite object holds at most 32
    # pieces). Ids are consecutive: base + colour*2 + half.
    dialog=len(sprites)
    for rid in BOX_TILESETS:
        box=dialog_box([get(rid,n) for n in range(9)])
        if stage in (1,3,4,5):
            # BG characters carry the panel and text. Only rounded corners
            # remain sprites, preserving the scenery through their alpha.
            corners=Image.new('RGBA',box.size)
            for x in (0,BOX_W-16):
                for y in (0,BOX_H-16):
                    corners.paste(box.crop((x,y,x+16,y+16)),(x,y))
            box=corners
        for half in range(2):
            add(f'dialog_box{rid:08X}_{half}',box.crop((half*BOX_W//2,0,(half+1)*BOX_W//2,BOX_H)))
    hud=[];digits=[];aim=[];motion=[];pending=[];pose=[];up_tip=[];art_muzzle=[]
    if stage in (1,3,4,5):
        def glyph(n):return get(0x87A5333C,n)
        for hero in range(4):
            row=[]
            for hp in range(4):
                im=Image.new('RGBA',(80,56));d=ImageDraw.Draw(im)
                d.rounded_rectangle((8,8,77,24),radius=3,fill=(8,8,16,255))
                d.rounded_rectangle((8,8,24,51),radius=3,fill=(8,8,16,255))
                for x in range(0,80,8):
                    for gy in range(0x26,0x3e,8):
                        im.alpha_composite(glyph((gy>>3)*16+(x>>3)),(x+3,gy-0x23))
                for ox,oy,n in [(8,8,0xa0+hero*3),(16,8,0xa1+hero*3),(8,16,0xb0+hero*3),(16,16,0xb1+hero*3)]:
                    im.alpha_composite(glyph(n),(ox,oy))
                for k in range(3):im.alpha_composite(glyph(0x0e+k*16 if k<hp else 0x4e),(12,24+k*8))
                row.append(add(f'hud{hero}_{hp}',im))
            hud.append(row)
        digits=[add(f'hud_digit{k}',glyph(0x80+k)) for k in range(10)]
        for hero,filename in enumerate(('saber.png',None,'april.png','colt.png')):
            sheet=Image.open(root/'assets'/filename).convert('RGBA') if filename else None
            fid=struct.unpack_from('<I',(work/'9C8F9A9E.levl').read_bytes(),4)[0]
            def cell(n,sheet=sheet):    # bind now: the loop variable would otherwise leave every hero on Colt's sheet
                if sheet:return sheet.crop((n%8*64,n//8*64,n%8*64+64,n//8*64+64))
                return get(fid,n)
            # Canonical right-facing standing aim and recoil frames.
            row=[]
            for name,n in [('up_right',44),('down_right',42)]:
                row.append(add(f'hero{hero}_{name}',cell(n),(32,32)))
            # Walking uses native legs with the corresponding aim torso.
            for torso in (100,98):
                for k in range(6):
                    bob=(0,1,2,0,1,2)[k] if hero==0 else (1 if k in (2,5) else 0) if hero in (1,3) else 0
                    legs=np.asarray(cell(104+k)).copy();legs[:38+bob if hero==0 else 0]=0
                    im=Image.fromarray(legs);im.alpha_composite(cell(torso),(0,bob))
                    row.append(add(f'hero{hero}_aim{torso}_run{k}',im,(32,32)))
            # Straight up / down (standing aim legs + the vertical torso, as player_resolve's CS_AIM).
            for name,torso,dy in [('aimup',38,-30),('aimdown',46,-9)]:
                im=Image.new('RGBA',(64,64));im.alpha_composite(cell(22));im.alpha_composite(cell(torso),(0,dy))
                row.append(add(f'hero{hero}_{name}',im,(32,32)))
                if name=='aimup':   # the shot leaves the top of the drawn gun (the cell is cut off at the top of its 64 px)
                    ys,xs=np.nonzero(np.asarray(im)[:,:,3]>=64)
                    up_tip.append((int(round(xs[ys==ys.min()].mean()))-32,int(ys.min())-32))
            aim.append(row)
            art_muzzle.append(art_tips(hero,cell))
            pending.append((hero,cell))
    # Motion poses follow every aim row, so each group has a constant stride
    # per hero (aim: 16, motion: 3) from its base ID. Falling off a ledge reuses
    # one frozen run frame (hero*9+7); a real jump plays the somersault below.
    # The player's muzzle flash: the source's strip 8623249C, frames 0-3 (diagonal) and 4-7 (straight).
    flash=len(sprites)
    if stage in (1,3,4,5):
        for k in range(8):add(f'muzzle_flash{k}',get(0x8623249C,k),(8,8))
    motion0=len(sprites)
    for hero,cell in pending:
        row=[]
        for name,n in [('shoot',40),('recoil',41),('slide',149)]:
            row.append(add(f'hero{hero}_{name}',cell(n),(32,32)))
        motion.append(row)
    # Extra animation strips follow the motion rows. Each hero's idle breathing,
    # somersault jump and death cells are the right-facing source animations
    # (idle: Saber's 12-frame cycle halved, April's 6, Fireball/Colt's 2; death:
    # anim 51, April's patched 8-frame one). `pose` holds, per hero, offsets
    # from the motion base: idle, idle count, jump (4), death, death count.
    for hero,cell in pending:
        idle=[168+2*k for k in range(6)] if hero==0 else list(range(176,182)) if hero==2 else [4,5]
        death=list(range(160,168)) if hero==2 else list(range(144,149))
        entry=[]
        for kind,cells in (('idle',idle),('jump',range(132,136)),('death',death)):
            entry+=[len(sprites)-motion0,len(cells)]
            for k,n in enumerate(cells):
                add(f'hero{hero}_{kind}{k}',cell(n),(32,32))
        pose.append(entry)
    enemy=0
    if stage in (1,3,4,5):
        # Run (cells 0-5) and death frames of the three enemy bodies, right-facing; left is a flip.
        enemy=len(sprites)
        for name,crhc,run,death in [('walker','02A38AFB',range(6),range(18,24)),('grunt','112DF34C',range(6),range(54,60)),('sniper','D39700C4',(),range(54,60))]:
            d=(work/f'{crhc}.levl').read_bytes();aid=struct.unpack_from('<I',d,4)[0];ox,oy=struct.unpack_from('<ff',d,8)
            for kind,cells in (('run',run),('death',death)):
                for k,n in enumerate(cells):
                    add(f'{name}_{kind}{k}',frame(work/'srgb'/f'{aid:08X}.srgb',n),(round(ox),round(oy)))
        # Falling (anim 0x30: two cells) for the walker and the grunt: drawn while they are off the ground.
        for name,crhc,cells in [('walker','02A38AFB',(8,9)),('grunt','112DF34C',(6,7))]:
            d=(work/f'{crhc}.levl').read_bytes();aid=struct.unpack_from('<I',d,4)[0];ox,oy=struct.unpack_from('<ff',d,8)
            for k,n in enumerate(cells):
                add(f'{name}_fall{k}',frame(work/'srgb'/f'{aid:08X}.srgb',n),(round(ox),round(oy)))
        # The sniper body's standing-aim cells (also the kneeler's and the shield's), right-facing: the source's
        # character.c aim table -> level 16, up-diagonal 14, up 12, down-diagonal 18, down 20, level recoil 17;
        # then the kneeler's crouch 30 and its three throwing cells 31-33 (enemy_base + 34..43).
        d=(work/'D39700C4.levl').read_bytes();aid=struct.unpack_from('<I',d,4)[0];ox,oy=struct.unpack_from('<ff',d,8)
        for name,n in [('sniper_aim0',16),('sniper_aim1',14),('sniper_aim2',12),('sniper_aim3',18),('sniper_aim4',20),('sniper_recoil',17),
                       ('kneel',30),('kneel_throw0',31),('kneel_throw1',32),('kneel_throw2',33)]:
            add(name,frame(work/'srgb'/f'{aid:08X}.srgb',n),(round(ox),round(oy)))
    return dict(portraits=portraits,dialog=dialog,hud=hud,digits=digits,aim=aim,motion=motion,pose=pose,up_tip=up_tip,art_muzzle=art_muzzle,flash=flash,enemy=enemy,end=len(sprites))

def emit_tables(out,scenes,h,c):
    m=scenes[0]['presentation']
    for name,shape in [('hud','[4][4]'),('digits','[10]'),('aim','[4][16]')]:
        def values(v):return '{'+','.join(values(x) if isinstance(x,list) else str(x) for x in v)+'}'
        h.append(f'extern const uint16_t pce_{name}_ids{shape};')
        c.append(f'const uint16_t pce_{name}_ids{shape}='+values(m[name])+';')
    # Actor inventories vary; presentation IDs use per-scene base addresses.
    h.append('extern const uint16_t pce_dialog_base[7];')
    c.append('const uint16_t pce_dialog_base[7]={'+','.join(str(m['presentation']['dialog']) for m in scenes)+'};')
    h.append('extern const uint16_t pce_enemy_base[7];')
    c.append('const uint16_t pce_enemy_base[7]={'+','.join(str(m['presentation']['enemy']) for m in scenes)+'};')
    h.append('extern const uint16_t pce_motion_base[7];')
    c.append('const uint16_t pce_motion_base[7]={'+','.join(str(m['presentation']['motion'][0][0]) if m['presentation']['motion'] else '0' for m in scenes)+'};')
    h.append('extern const uint8_t pce_hero_pose[4][6];')
    pose=next((m['presentation']['pose'] for m in scenes if m['presentation']['pose']),[[0]*6]*4)
    c.append('const uint8_t pce_hero_pose[4][6]={'+','.join('{'+','.join(map(str,e))+'}' for e in pose)+'};')
    h.append('extern const uint16_t pce_flash_base[7];')
    c.append('const uint16_t pce_flash_base[7]={'+','.join(str(m['presentation']['flash']) for m in scenes)+'};')
    h.append('extern const uint16_t pce_present_base[7][3];')
    c.append('const uint16_t pce_present_base[7][3]={'+','.join('{%d,%d,%d}'%(m['presentation']['hud'][0][0],m['presentation']['digits'][0],m['presentation']['aim'][0][0]) if m['presentation']['hud'] else '{0,0,0}' for m in scenes)+'};')
