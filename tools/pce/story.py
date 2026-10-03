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
    7: ('space.c', ['SCRIPT_OUTRO']),
}

def source_script(path, name):
    match = re.search(r'\b'+re.escape(name)+r'\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+);',path.read_text())
    if not match: raise ValueError(f'Missing story script {path}:{name}')
    return ''.join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"',match[1]))

def pages(script):
    out=[]
    for chunk in script.split('<<>>'):
        speaker=re.search(r'</dialog_avatar_([^/]+)/>',chunk)
        title=speaker[1].rstrip('12').replace('darkapril','DARK APRIL').upper() if speaker else 'RADIO'
        chunk=re.sub(r'<[^>]*>','',chunk.replace('<r>','\n')).strip()
        chunk=' '.join(chunk.split())
        # Text before the first color tag may be an SFX header.
        if not chunk or chunk.endswith('.wav'): continue
        lines=textwrap.wrap(chunk,30)
        for i in range(0,len(lines),7):
            out.append((title+'\n'+'\n'.join(lines[i:i+7])).encode('ascii','replace')+b'\0')
    return out

def bake(root,work,stage,archive):
    if stage==1:
        scripts=[(work/f'dialog{k}.txt').read_text() for k in range(4)]
    else:
        filename,names=SCRIPTS[stage]
        scripts=[source_script(root/'src'/filename,name) for name in names]
    offsets=[]
    for hero,name in enumerate(('Saber Rider','Fireball','April','Colt')):
        for i,script in enumerate(scripts):
            if stage in (1,3,4,5) and hero!=1 and not(stage==5 and i>=4):
                avatar=('saber','fireball','april','colt')[hero]
                script=script.replace('dialog_avatar_fireball1','__ACTIVE_HERO__')
                script=script.replace('dialog_avatar_'+avatar+'2','dialog_avatar_fireball1')
                script=script.replace('__ACTIVE_HERO__','dialog_avatar_'+avatar+'2')
                script=script.replace('Fireball',name)
            chunks=pages(script)
            pointers=[archive.add(f'hero{hero}_story{i}_page{j}',p) for j,p in enumerate(chunks)]
            offsets.append(archive.add(f'hero{hero}_story{i}',bytes([len(pointers)])+b''.join(struct.pack('<I',p) for p in pointers)))
    return archive.add('story_directory',bytes([len(scripts)])+b''.join(struct.pack('<I',p) for p in offsets))
