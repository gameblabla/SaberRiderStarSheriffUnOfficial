#!/usr/bin/env python3
"""Native checks for zero-page startup, text, race restoration and pause colors."""
import argparse,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_sgx_rendering import Rendering

def read_space(e,space,address,size):
    return b''.join(bytes.fromhex(e.call('asread',space,address+i,min(16384,size-i))['hex'])
                    for i in range(0,size,16384))

def archive(e,out,stage):
    expected=(out/f's{stage}.bin').read_bytes()
    actual=read_space(e,'acram',0,len(expected))
    assert actual==expected,(stage,'native ZX02 archive differs',len(actual),len(expected))
    return len(expected)

def run(out):
    out=out.resolve();c=Rendering(out);elf=out/'app.elf';report={}
    with tempfile.TemporaryDirectory(prefix='reported-issues-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=True) as e:
        boot(e,c.address)
        assert e.memory(124*8192+(symbol(elf,'pce_car_widths')&8191),6,logical=False)==bytes((16,24,32,40,52,64)), 'Racer perspective table was not initialized'
        report['stage1_decoded_bytes']=archive(e,out,1)
        colors=read_space(e,'pram',31*32,32)
        c.press(e,8);e.run(60)
        assert read_space(e,'pram',31*32,32)==colors,'Pause recolored the HUD'
        e.screenshot(out/'reported-pause.png');c.press(e,8)
        assert read_space(e,'pram',31*32,32)==colors,'Unpause recolored the HUD'
        report['pause_hud_palette_preserved']=True
        reads=c.metrics(e)['disc_reads'];start=e.call('registers')['registers']['FR']
        c.field(e,'result',2)
        c.until(e,lambda:c.metrics(e)['disc_reads']>reads,limit=6000,step=1)
        c.until(e,lambda:not c.metrics(e)['ready'],limit=6000,step=1)
        c.until(e,lambda:c.metrics(e)['ready'] and c.state(e)['result']==0,limit=6000,step=1)
        e.run(120)
        report['retry_disc_reads']=c.metrics(e)['disc_reads']-reads
        assert report['retry_disc_reads']==2,'Retry must read only the font and voice, not the stage archive'
        report['retry_video_frames']=e.call('registers')['registers']['FR']-start
        c.field(e,'result',1)
        c.until(e,lambda:e.memory(symbol(elf,'pce_ui_state'),1)==b'\5',limit=12000)
        c.advance(e,2)
        report['stage2_decoded_bytes']=archive(e,out,2)
        assert e.memory(symbol(elf,'line_count'),1)[0]>0,'Dialogue has no lines'
        e.run(120)
        vram=read_space(e,'vram0',0,65536)
        glyphs=[struct.unpack_from('<H',vram,(row*128+col)*2)[0]
                for row in range(52,56) for col in range(10,56)]
        assert sum(0xe422<=w<0xe4e0 for w in glyphs)>20,'Race dialogue did not write glyphs'
        e.screenshot(out/'reported-race-text.png')
        kept=read_space(e,'acram',0x1e0000,672)
        # Compare against the original map, not against a potentially corrupt
        # backup: copying corrupt saved words back is not restoration.
        scene=c.manifest['scenes'][1];blob=(out/'s2.bin').read_bytes()
        ids=struct.unpack('<256H',read_space(e,'cpu',symbol(elf,'cache_ids'),512))
        slots={id:i for i,id in enumerate(ids) if id!=65535}
        expected=b''.join(struct.pack('<H',0x300+slots[struct.unpack_from('<H',blob,scene['map']+x*90+(row+3)*3)[0]]|
                         blob[scene['map']+x*90+(row+3)*3+2]<<12)
                         for row in range(6) for x in range(6,62))
        assert kept==expected,'Race dialogue saved corrupt sky cells'
        c.dialogs(e)
        restored=b''.join(read_space(e,'vram0',((51+row)*128+6)*2,112) for row in range(6))
        assert restored==kept,'Race left dialogue BAT cells behind'
        report['race_restored_cells']=336
        # SGX's quarter-square kernel uses separate 512-byte low/high tables.
        # The reciprocal rows now start at byte 1024 (race_proj.c).
        refs=read_space(e,'cpu',symbol(elf,'cache_refs'),1545)
        squares=[n*n//4 for n in range(512)]
        expected=bytes(n&255 for n in squares)+bytes(n>>8 for n in squares)+bytes(10080//f for f in range(40,561))
        assert refs==expected,'Race projection tables changed during dialogue'
        e.screenshot(out/'reported-race-after-text.png')
        c.seed(e,'pce_continues',0,1);c.field(e,'state',3)
        over_start=e.call('registers')['registers']['FR']
        ui=symbol(elf,'pce_ui_state')
        c.until(e,lambda:e.memory(ui,1)==b'\4',limit=12000)
        e.run(240)
        report['gameover_video_frames']=e.call('registers')['registers']['FR']-over_start
        reads=c.metrics(e)['disc_reads'];c.press(e,1)
        c.until(e,lambda:e.memory(ui,1)==b'\1',limit=6000);e.run(120)
        assert c.metrics(e)['disc_reads']==reads,'Game Over to title reread the UI archive'
        report['gameover_to_title_disc_reads']=0
        sat=list(struct.iter_unpack('<4H',read_space(e,'sat0',0,512)))
        start=[p for y,x,p,a in sat if y==254]
        options=[p for y,x,p,a in sat if y==266]
        assert start==[0x340+i*2 for i in range(5)],start
        assert options==[0x340+i*2 for i in range(5,9)],options
        e.screenshot(out/'reported-title-options.png')
        report['title_distinct_menu_patterns']=True
    (out/'reported-issues-verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(report)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'))
    run(p.parse_args().out)
