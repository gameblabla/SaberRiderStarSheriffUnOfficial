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

def sky(stage,size):
    top,bottom=((36,100,190),(116,190,250)) if stage==1 else ((14,8,34),(66,36,90))
    w,h=size;v=np.linspace(top,bottom,h).astype(np.uint8)
    a=np.zeros((h,w,4),np.uint8);a[:,:,:3]=v[:,None,:];a[:,:,3]=255
    return Image.fromarray(a)

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
        portraits[name]=add('portrait_'+name,im)
        portraits[f'{rid:08X}']=portraits[name]
    hud=[];digits=[];aim=[];motion=[];pending=[]
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
            def cell(n):
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
            aim.append(row)
            pending.append((hero,cell))
    # Motion poses follow every aim row, so each group has a constant stride
    # per hero (aim: 14, motion: 5) from its base ID.
    for hero,cell in pending:
        row=[]
        for name,n in [('shoot',40),('recoil',41),('jump0',132),('jump1',133),('jump2',134)]:
            row.append(add(f'hero{hero}_{name}',cell(n),(32,32)))
        motion.append(row)
    return dict(portraits=portraits,hud=hud,digits=digits,aim=aim,motion=motion)

def emit_tables(out,scenes,h,c):
    m=scenes[0]['presentation']
    for name,shape in [('hud','[4][4]'),('digits','[10]'),('aim','[4][14]')]:
        def values(v):return '{'+','.join(values(x) if isinstance(x,list) else str(x) for x in v)+'}'
        h.append(f'extern const uint16_t pce_{name}_ids{shape};')
        c.append(f'const uint16_t pce_{name}_ids{shape}='+values(m[name])+';')
    # Actor inventories vary; presentation IDs use per-scene base addresses.
    h.append('extern const uint16_t pce_motion_base[7];')
    c.append('const uint16_t pce_motion_base[7]={'+','.join(str(m['presentation']['motion'][0][0]) if m['presentation']['motion'] else '0' for m in scenes)+'};')
    h.append('extern const uint16_t pce_present_base[7][3];')
    c.append('const uint16_t pce_present_base[7][3]={'+','.join('{%d,%d,%d}'%(m['presentation']['hud'][0][0],m['presentation']['digits'][0],m['presentation']['aim'][0][0]) if m['presentation']['hud'] else '{0,0,0}' for m in scenes)+'};')
