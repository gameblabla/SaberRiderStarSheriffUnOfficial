"""Serialize current source dialog scripts into paged, wrapped native text."""
import ast
import re
import struct
import textwrap
from pathlib import Path

SCRIPTS = {
    2: ('mode7.c', ['SCRIPT_INTRO','SCRIPT_BREAKAWAY','SCRIPT_CAUGHT','SCRIPT_VICTORY']),
    3: ('night.c', ['NIGHT_SCRIPT_INTRO','NIGHT_SCRIPT_TAUNT','NIGHT_SCRIPT_OUTRO']),
    4: ('forest.c', ['FOREST_SCRIPT_INTRO','FOREST_SCRIPT_AMBUSH','FOREST_SCRIPT_OUTRO']),
    5: ('darkapril.c', ['LAB_SCRIPT_INTRO','DARK_SCRIPT_CALL','DARK_SCRIPT_MEET','DARK_SCRIPT_OUTRO',
                          'DARK_SCRIPT_CALL_APRIL','DARK_SCRIPT_MEET_APRIL','DARK_SCRIPT_OUTRO_APRIL']),
    6: ('ramrod.c', ['SCRIPT_INTRO','SCRIPT_AFTER1','SCRIPT_AFTER2','SCRIPT_OUTRO']),
    7: ('space.c', ['SCRIPT_OUTRO','SCRIPT_BOSS']),   # (the cruiser's greeting is story 1, after the ending)
}

def source_script(path, name):
    match = re.search(r'\b'+re.escape(name)+r'\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+);',path.read_text())
    if not match: raise ValueError(f'Missing story script {path}:{name}')
    return ''.join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"',match[1]))

COLOURS={'GREEN':0,'PURPLE':1,'RED':2,'BLUE':3}
TEXT_COLUMNS=23     # BG text cells inside the dialogue box

def pages(script,portraits):
    out=[]
    colour=0
    for chunk in script.split('<<>>'):
        # dialog.c keys on the first two letters; the colour persists until the next tag.
        tag=re.search(r'<\|([A-Za-z]+)',chunk)
        if tag:colour=next((v for k,v in COLOURS.items() if k[:2]==tag[1][:2].upper()),0)
        speaker=re.search(r'</dialog_avatar_([^/]+)/>',chunk)
        packed=re.search(r'<avatar:([0-9A-F]+)>',chunk)
        avatar=portraits.get(speaker[1] if speaker else packed[1] if packed else '',65535)
        chunk=re.sub(r'<[^>]*>','',chunk.replace('<r>','\n')).strip()
        chunk=' '.join(chunk.split())
        # Text before the first color tag may be an SFX header.
        if not chunk or chunk.endswith('.wav'): continue
        lines=textwrap.wrap(chunk,TEXT_COLUMNS)
        for i in range(0,len(lines),4):
            out.append(struct.pack('<HB',avatar,colour)+'\n'.join(lines[i:i+4]).encode('ascii','replace')+b'\0')
    return out

def bake(root,work,stage,archive,portraits):
    if stage==1:
        scripts=[(work/f'dialog{k}.txt').read_text() for k in range(4)]
    else:
        filename,names=SCRIPTS[stage]
        scripts=[source_script(root/'src'/filename,name) for name in names]
    from presentation import namehash
    for j,script in enumerate(scripts):
        for name in portraits:
            if re.fullmatch('[0-9A-F]{8}',name):continue
            script=script.replace(f'<avatar:{namehash("dialog_avatar_"+name):08X}>',f'</dialog_avatar_{name}/>')
        scripts[j]=script
    offsets=[]
    for hero,name in enumerate(('Saber Rider','Fireball','April','Colt')):
        for i,script in enumerate(scripts):
            if stage in (1,3,4,5) and hero!=1 and not(stage==5 and i>=4):
                avatar=('saber','fireball','april','colt')[hero]
                script=script.replace('dialog_avatar_fireball1','__ACTIVE_HERO__')
                script=script.replace('dialog_avatar_'+avatar+'2','dialog_avatar_fireball1')
                script=script.replace('__ACTIVE_HERO__','dialog_avatar_'+avatar+'2')
                script=script.replace('Fireball',name)
            chunks=pages(script,portraits)
            pointers=[archive.add(f'hero{hero}_story{i}_page{j}',p) for j,p in enumerate(chunks)]
            offsets.append(archive.add(f'hero{hero}_story{i}',bytes([len(pointers)])+b''.join(struct.pack('<I',p) for p in pointers)))
    return archive.add('story_directory',bytes([len(scripts)])+b''.join(struct.pack('<I',p) for p in offsets))
