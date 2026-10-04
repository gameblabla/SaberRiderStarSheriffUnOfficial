#!/usr/bin/env python3
"""Native checks for dedicated dialogue corners, race parallax and the BG cruiser."""
import argparse,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def verify(out):
    c=Campaign(out);elf=out/'app.elf';a={n:symbol(elf,n) for n in
        'space_hull_ready space_boss_y flight_clock space_flash space_flashing pce_sky_far pce_sky_near sprite_slot_of previous'.split()}
    review=out/'wip-review';review.mkdir(exist_ok=True)
    manifest=json.loads((out/'manifest.json').read_text())
    report={}
    with tempfile.TemporaryDirectory(prefix='wip-',dir=out) as b,Emulator(out/'saber_rider.cue',b) as e:
        boot(e,c.address);e.run(120)
        # Open the actual first dialogue. Its four corners use reserved VRAM
        # and a private palette, independent of actor cache assignments.
        e.input(32);c.until(e,lambda:c.state(e)['state']==1,limit=1000,step=1);e.input(0);e.run(120)
        sat=bytes.fromhex(e.call('asread','sat0',0,512)['hex'])
        corners=[(y,x,p,attr) for y,x,p,attr in struct.iter_unpack('<4H',sat) if y and 0x208<=p<0x210 and attr&15==15]
        assert len(corners)==4,corners
        assert len({(y,x) for y,x,p,attr in corners})==4
        assert e.memory(a['sprite_slot_of']+manifest['scenes'][0]['presentation']['dialog'],8)==bytes([255]*8)
        e.screenshot(review/'opening-dialog.png');c.dialogs(e)
        report['dialogue_corners']={'entries':4,'dedicated_word':0x4100,'palette':31}
        # Reproduce both reported level-1 locations with the rebuilt program.
        for cam in (770,6154,7384):
            c.press(e,8);c.seed(e,'camera',cam);c.seed(e,'dialogs_done',255,1);c.seed(e,'safe_timer',0,1)
            e.write(symbol(elf,'actors'),bytes(168))
            if cam==6154:
                e.write(symbol(elf,'actors'),struct.pack('<4h13B',6160,64,0,0,0,0,0,0,1,16,1,0,0,0,0,0,0))
            e.write(symbol(elf,'player'),struct.pack('<4h4B',cam+120,160,0,0,0,0,4,4));c.press(e,8)
            e.run(30);e.screenshot(review/f'stage1-{cam}.png')
        c.stage(e,2);c.dialogs(e);e.input(16|64);e.run(300);e.input(0)
        far=int.from_bytes(e.memory(a['pce_sky_far'],2),'little');near=int.from_bytes(e.memory(a['pce_sky_near'],2),'little')
        assert far!=near and far<512 and near<512,(far,near)
        # Both halves of the sky BAT repeat, so BXR can wrap without exposing an empty region.
        sky=bytes.fromhex(e.call('asread','vram0',48*128*2,16*128*2)['hex'])
        for row in range(16):assert sky[row*256:row*256+128]==sky[row*256+128:(row+1)*256]
        e.screenshot(review/'race-parallax.png');report['race_parallax']={'far_scroll':far,'near_scroll':near,'wrapping_bat':True}
        c.stage(e,7);e.run(60);reads=c.metrics(e)['disc_reads'];c.seed(e,'flight_clock',6359)
        c.until(e,lambda:e.memory(a['space_hull_ready'],1)==b'\1',limit=600,step=1);e.run(30)
        assert c.state(e)['boss_kind']==5 and c.metrics(e)['disc_reads']==reads
        assert e.memory(a['sprite_slot_of']+6,1)==b'\xff','Cruiser must never enter the sprite cache'
        scene=manifest['scenes'][6];blob=(out/'s7.bin').read_bytes();off=scene['boss_bg_offset'];count,cols,rows=struct.unpack_from('<HBB',blob,off)
        bat=struct.unpack('<2048H',bytes.fromhex(e.call('asread','vram0',0,4096)['hex']))
        expected=struct.unpack_from('<299H',blob,off+192)
        for y in range(rows):
            for x in range(cols):assert bat[y*64+x]==expected[y*cols+x]+128
        assert bytes.fromhex(e.call('asread','vram0',0x1000,count*32)['hex'])==blob[off+832:off+832+count*32]
        e.screenshot(review/'cruiser.png')
        # Catch the bomb during its native timer, not after a long input helper.
        e.input(0);e.run(30);e.input(2)
        c.until(e,lambda:e.memory(a['space_flashing'],1)==b'\1',limit=60,step=1)
        e.screenshot(review/'bomb.png');e.input(0);e.run(2)
        assert bytes.fromhex(e.call('asread','pram',0,1024)['hex'])==b'\xff\x01'*512
        c.until(e,lambda:e.memory(a['space_flashing'],1)==b'\0',limit=60,step=1);e.run(20)
        assert bytes.fromhex(e.call('asread','pram',0,2)['hex'])==b'\0\0'
        e.screenshot(review/'after-bomb.png')
        # Final-round steering moves only BXR/BYR: BAT and patterns stay intact.
        before=e.memory(a['space_boss_y'],2);c.field(e,'boss_hp',300);c.seed(e,'beam_clock',180);c.seed(e,'ship_y',40)
        e.run(30);assert e.memory(a['space_boss_y'],2)!=before
        assert e.memory(a['sprite_slot_of']+6,1)==b'\xff';e.screenshot(review/'cruiser-moving.png')
        report['cruiser']={'size':[180,98],'tiles':count,'sprite_entries':0,'disc_reads_in_fight':0,'white_flash_restored':True,'scroll_motion':True}
    (out/'wip-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(report)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));verify(p.parse_args().out.resolve())
