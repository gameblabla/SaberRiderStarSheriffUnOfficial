#!/usr/bin/env python3
"""Exercise actual title/selection/briefing inputs and every hero's diagonal SAT."""
import argparse
import json
from pathlib import Path
import struct
import tempfile
import numpy as np
from PIL import Image
from emulator import Emulator,symbol
from test_campaign import Campaign

def verify(out):
    sprites=json.load(open(out/'manifest.json'))['scenes'][0]['sprites']
    t=Campaign(out);ui=symbol(out/'app.elf','pce_ui_state');report={}
    captures=out/'presentation-review';captures.mkdir(exist_ok=True)
    def picture(e,name):
        p=captures/(name+'.png');e.screenshot(p)
        im=np.asarray(Image.open(p).convert('RGB'))
        assert len(np.unique(im.reshape(-1,3),axis=0))>16,(name,'Blank screen')
    with tempfile.TemporaryDirectory(prefix='presentation-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        e.run(120);e.input(8);e.run(5);e.input(0)
        options=symbol(out/'app.elf','pce_options.0');conts=symbol(out/'app.elf','pce_continues')
        # BIOS/work RAM can contain 1 at the future UI address before the
        # application starts. Require its initialized metrics before capturing.
        t.until(e,lambda:e.memory(t.address,4)==b'SRPC' and
                e.memory(t.address+6,1)==b'\x0f' and e.memory(ui,1)==b'\1',
                limit=20000);e.run(360);picture(e,'title')
        # Options: difficulty to HARD caps lives/continues at 3; music volume cycles.
        t.press(e,64);t.press(e,1);t.until(e,lambda:e.memory(ui,1)==b'\3');e.run(200);picture(e,'options')
        t.press(e,32)                                                # difficulty NORMAL -> HARD
        assert e.memory(options,4)[0]==2 and e.memory(options,4)[1]==3,e.memory(options,4)
        t.press(e,64);t.press(e,128)                                 # lives 3 -> 2 (left)
        t.press(e,64);t.press(e,32)                                  # continues stays capped at 3
        t.press(e,64);t.press(e,128);t.press(e,128);e.run(240)       # music HIGH -> MID -> LOW
        opt=e.memory(options,4)
        assert opt[0]==2 and opt[1]==2 and opt[2]==3 and opt[3]==1,opt
        picture(e,'options-changed')
        t.press(e,64);t.press(e,1);t.until(e,lambda:e.memory(ui,1)==b'\1');e.run(60)
        t.press(e,1);t.until(e,lambda:e.memory(ui,1)==b'\2');e.run(240);picture(e,'select-saber')
        for hero in (1,2,3):
            t.press(e,32);e.run(120);picture(e,f'select-{hero}')
        t.press(e,1)
        t.until(e,lambda:e.memory(t.address+5,1)==b'\1');e.run(120)
        m=t.metrics(e)
        assert m['hero']==3,'Selected Colt must reach actual gameplay'
        assert m['hp']==1,'HARD difficulty gives one heart'
        assert t.state(e)['lives']==opt[1],'Options lives must start the game'
        report['frontend']='title -> options (difficulty, lives, continues, music) -> hero select -> Colt gameplay'
        # Continue: a lethal hit with continues left offers CONTINUE and restarts the stage.
        e.write(conts,bytes([1]));t.field(e,'lives',0);t.seed(e,'safe_timer',0,1);t.seed(e,'dialogs_done',255,1);t.field(e,'state',0)
        e.write(symbol(out/'app.elf','shots'),struct.pack('<4h2B',m['player_x'],m['player_y'],0,0,1,1))
        t.until(e,lambda:e.memory(ui,1)==b'\4',limit=3000);e.run(120);picture(e,'continue')
        t.press(e,1);t.until(e,lambda:e.memory(t.address+5,1)==b'\1' and e.memory(ui,1)==b'\0',limit=6000);e.run(120)
        assert e.memory(conts,1)==b'\0' and t.state(e)['lives']==opt[1] and t.metrics(e)['hero']==3
        report['continue']='CONTINUE screen restarts the stage, consumes a continue and restores lives'
        # Use the existing Run menu to change hero without rebuilding test ROMs.
        for hero in range(4):
            t.press(e,8);current=t.metrics(e)['hero']
            for _ in range((hero-current)&3):t.press(e,16)
            t.press(e,8);t.until(e,lambda:e.memory(t.address+5,1)==b'\1');e.run(120)
            t.seed(e,'dialogs_done',255,1);t.field(e,'state',0)
            picture(e,f'hero-{hero}-hud')
            reads=t.metrics(e)['disc_reads']
            for direction,keys in [('up-right',52),('down-right',100),('up-left',148),('down-left',196)]:
                e.input(keys);e.run(30);picture(e,f'hero-{hero}-{direction}')
                sat=bytes.fromhex(e.call('asread','vram0',0xfe00,512)['hex'])
                ids=struct.unpack('<48H',e.memory(symbol(out/'app.elf','sprite_ids'),96))
                found=[]
                for k in range(t.metrics(e)['sat_count']):
                    y,x,pat,attr=struct.unpack_from('<4H',sat,k*8)
                    if attr&15>=15:continue
                    name=sprites[ids[attr&15]]['name']
                    if name==f'hero{hero}_'+('up_right' if direction.startswith('up') else 'down_right'):
                        found.append(bool(attr&0x800))
                assert found and all(v==direction.endswith('left') for v in found),(hero,direction,found)
                assert e.call('registers')['registers']['BYR']==0
                e.input(0);e.run(30)
            assert t.metrics(e)['disc_reads']==reads,'Aim/HUD must use preloaded graphics'
        report['aim_cases']=16;report['hud_heroes']=4;report['platform_vertical_scroll']=0
        # Restart through the actual game-over/title/selection flow. One seeded
        # lethal projectile still runs the normal death animation and life logic.
        e.write(options,bytes([1,3,3,3]))  # Normal difficulty, default lives and full music volume.
        for old,new in ((3,0),(0,1),(1,2),(2,3)):
            m=t.metrics(e);assert m['hero']==old
            t.seed(e,'dialogs_done',255,1);t.field(e,'state',0)
            t.field(e,'lives',0);e.write(conts,b'\0')
            e.write(t.address+34,b'\x01\x00')
            t.seed(e,'safe_timer',0,1)
            e.write(symbol(out/'app.elf','shots'),struct.pack('<4h5B',m['player_x'],m['player_y'],0,0,1,1,0,0,0))
            t.until(e,lambda:e.memory(ui,1)==b'\4',limit=6000)
            e.run(180);picture(e,f'gameover-{old}')
            # The text pulse must change palettes while the painting stays put.
            frame=np.asarray(Image.open(captures/f'gameover-{old}.png')).copy()
            changed=False
            for pulse in range(8):
                e.run(8);e.screenshot(captures/f'gameover-{old}-pulse.png')
                next_frame=np.asarray(Image.open(captures/f'gameover-{old}-pulse.png'))
                assert np.array_equal(frame[:160],next_frame[:160]),'Painting must stay still'
                changed|=not np.array_equal(frame[168:200],next_frame[168:200])
            assert changed,'GAME OVER lettering must pulse'
            e.input(1);t.until(e,lambda:e.memory(ui,1)==b'\1',limit=12000,step=10)
            e.run(90)
            assert e.memory(ui,1)==b'\1','Held GAME OVER confirmation must not select START'
            e.input(0);e.run(30)
            t.press(e,1);t.until(e,lambda:e.memory(ui,1)==b'\2')
            e.run(90)
            for _ in range(abs(new-old)):t.press(e,32 if new>old else 128)
            e.run(60);t.press(e,1)
            t.until(e,lambda:e.memory(ui,1)==b'\0' and e.memory(t.address+5,1)==b'\1',limit=12000)
            e.run(120);m=t.metrics(e)
            assert m['hero']==new and m['hp']==2 and t.state(e)['lives']==3
            picture(e,f'restart-{old}-to-{new}')
            x=m['player_x'];e.input(32);e.run(30);e.input(0)
            assert t.metrics(e)['player_x']>x,'Restarted hero must respond to input'
        report['new_game_after_death']='four hero changes through game over, title and hero selection; visible, responsive gameplay'
    (out/'presentation-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));a=p.parse_args();verify(a.out.resolve())
