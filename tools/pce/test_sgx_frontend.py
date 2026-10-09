#!/usr/bin/env python3
"""Source-content and native retail SGX front-end composition regressions."""
import argparse, hashlib, json, re, struct, tempfile
from pathlib import Path
import numpy as np
from PIL import Image
from emulator import Emulator, symbol
from test_campaign import Campaign
import build_assets
from frontend import vce_colors

def source_check(out):
    level,banks=build_assets.levl.load_dump(out/'work/stage1.layers')
    ly=next(l for l in level.layers if l.name=='SkyBG')
    bank=build_assets.levl.png_bank(banks[ly.cblock],ly.cblock) if banks[ly.cblock] else build_assets.levl.load_bank(out/'work/srgb'/f'{ly.cblock:08X}.srgb')
    raw=build_assets.levl.render_layer(ly,bank,build_assets.levl.layer_offset(ly,0),ly.w*bank.tw,240,None)
    cols=np.flatnonzero((raw[...,3]>=128).any(axis=0))
    assert cols[0]==0 and cols[-1]+1==512,('source art extent',cols)
    manifest=json.loads((out/'manifest.json').read_text());scene=manifest['scenes'][0]
    rec=scene['records']['sgx_sky_record']['offset'];blob=(out/'s1.bin').read_bytes()
    assert struct.unpack_from('<H',blob,rec+25)[0]==64,'Level-1 repeat must be 64 native columns'
    # Synthetic edge fixture protects left origin, internal holes, opaque black,
    # vertical position, and seam-filled alignment independently of source art.
    a=np.zeros((16,23,4),np.uint8);a[9,0]=[0,0,0,255];a[10,10]=[20,30,40,128]
    crop=build_assets.repeating_sky(Image.fromarray(a),1)
    assert crop.size==(16,16) and np.array_equal(np.asarray(crop)[:,:11],a[:,:11])
    assert np.array_equal(np.asarray(crop)[:,11:],a[:,:5])
    assert build_assets.repeating_sky(Image.fromarray(a),4).width==23
    report=json.loads((out/'sgx_static_report.json').read_text())['paired_static_screens']
    title=next(r for r in report if r['name']=='title')
    assert min(title['plane_coverage'])>0 and title['rgb_mse']<title['pce_final_rgb_mse'],title
    source=np.asarray(Image.open(out/'preview/sgx_title_source.png').convert('RGB'))
    title['source_vce_colors']=len(np.unique(vce_colors(source)))
    baseline=np.asarray(Image.open(out/'preview/sgx_title_pce_baseline.png').convert('RGB').resize((320,224),Image.Resampling.NEAREST))
    title['pce_baseline_rgb_mse']=float(((source.astype(float)-baseline)**2).mean())
    title['pce_baseline_colors']=len(np.unique(baseline.reshape(-1,3),axis=0))
    assert title['colors']>=title['source_vce_colors'],'Dithered title should expand its VCE color range'

    return title

def verify(out):
    title=source_check(out);t=Campaign(out,sgx=True)
    ui=symbol(out/'app.elf','pce_ui_state');page=symbol(out/'app.elf','pce_sgx_select_page')
    sgx=symbol(out/'app.elf','pce_sgx_metrics')
    raw=(out/'ui.bin').read_bytes();c=(out/'assets.c').read_text()
    pair=re.search(r'pce_sgx_ui_pair\[21\].*?=\{(.*?)\};',c).group(1)
    title_rec=[tuple(map(int,r.split(','))) for r in re.findall(r'\{([^{}]+)\}',pair)][19]
    select=re.search(r'pce_sgx_select .*?=\{(.*?)\};',c).group(1)
    selection=list(map(int,re.findall(r'\d+',select)))
    po,ft,*rest=selection;maps=rest[:4];bt,bm,cycle,fn,bn=rest[4:]
    captures=out/'sgx-frontend-review';captures.mkdir(exist_ok=True)
    def vram(e,n):return bytes.fromhex(e.call('asread',f'vram{n}',0,65536)['hex'])
    def compare(e,n,to,mo,nt,base=0,tileword=0x800):
        vr=vram(e,n)
        assert vr[tileword*2:tileword*2+nt*32]==raw[to:to+nt*32],(n,'resident patterns')
        for row in range(28):
            assert vr[(base+row*64)*2:(base+row*64)*2+80]==raw[mo+row*80:mo+(row+1)*80],(n,row,'complete map')
    def capture(e,name):e.screenshot(captures/f'{name}.png')
    def check_title(e,name):
        p,t0,m0,t1,m1,n0,n1,_=title_rec
        compare(e,0,t0,m0,n0);compare(e,1,t1,m1,n1)
        assert e.memory(sgx+4,1)[0]&4,'Both title planes must be enabled'
        capture(e,name)
    with tempfile.TemporaryDirectory(prefix='sgx-ui-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=True) as e:
        e.run(120);e.input(8);e.run(5);e.input(0)
        t.until(e,lambda:e.memory(t.address,4)==b'SRPC' and e.memory(ui,1)==b'\1',limit=20000)
        e.run(360);check_title(e,'title')
        t.press(e,64);t.press(e,1);t.until(e,lambda:e.memory(ui,1)==b'\3');e.run(200);capture(e,'options')
        option_addrs={i:symbol(out/'app.elf',f'pce_options.{i}') for i in (1,2)}
        def check_numbers():
            values={i:e.memory(addr,1)[0] for i,addr in option_addrs.items()}
            vr=vram(e,0)
            for row,value in ((11,values[1]),(13,values[2])):
                words=struct.unpack_from('<2H',vr,(row*64+26)*2)
                expected=tuple(0xf080+ord(ch)-32 for ch in f'{value:02d}')
                assert words==expected,('options number missing',row,value,words,expected)
        check_numbers()
        # Exercise native redraws and zero, whose glyph must remain visible.
        t.press(e,64)
        t.press(e,32);check_numbers();assert e.memory(option_addrs[1],1)==b'\4'
        capture(e,'options-lives-increased')
        for _ in range(7):t.press(e,128)
        check_numbers();assert e.memory(option_addrs[1],1)==b'\0'
        t.press(e,64)
        t.press(e,128);check_numbers();assert e.memory(option_addrs[2],1)==b'\2'
        capture(e,'options-continues-decreased')
        for _ in range(5):t.press(e,128)
        check_numbers();assert e.memory(option_addrs[2],1)==b'\0'
        capture(e,'options-zero')
        # Restore the starting values before continuing the campaign check.
        for _ in range(3):t.press(e,32)
        t.press(e,16)
        for _ in range(3):t.press(e,32)
        check_numbers()
        t.press(e,4);t.until(e,lambda:e.memory(ui,1)==b'\1');e.run(180);check_title(e,'title-return')
        selection_reads=t.metrics(e)['disc_reads']
        selection_start=e.call('registers')['registers']['FR']
        t.press(e,1);t.until(e,lambda:e.memory(ui,1)==b'\2');e.run(120)
        selection_frames=e.call('registers')['registers']['FR']-selection_start
        assert t.metrics(e)['disc_reads']==selection_reads,'Selection must use the resident UI extent'
        assert selection_frames<600,('Title to selection took too long',selection_frames)
        fixed=bytes.fromhex(e.call('asread','pram',6*32,10*32)['hex'])
        assert '#define PCE_SGX_SELECT_FRAMES 1' in (out/'assets.h').read_text()
        animation_phase=symbol(out/'app.elf','ui_cycle_step')
        backdrop_phases=[raw[cycle+i*192:cycle+(i+1)*192] for i in range(12)]
        phases=set()
        for hero,key in ((0,0),(1,32),(2,32),(3,32),(2,128),(1,128),(0,128),(1,32),(2,32),(3,32)):
            if key:t.press(e,key)
            e.run(8)
            pg=e.memory(page,1)[0]
            compare(e,0,ft,maps[hero],fn,pg*0x800,0x1000)
            phase=e.memory(animation_phase,1)[0];phases.add(phase)
            assert bytes.fromhex(e.call('asread','pram',0,192)['hex']) in backdrop_phases,'Invalid backdrop cycle'
            compare(e,1,bt,bm,bn,0,0x1000)
            assert bytes.fromhex(e.call('asread','pram',6*32,10*32)['hex'])==fixed,'Portrait colors changed'

            capture(e,f'select-{hero}-page{pg}')
        assert len(phases)>1,'Backdrop must cycle while portraits switch'
        for phase in range(4):
            e.run(32);assert bytes.fromhex(e.call('asread','pram',192,320)['hex'])==fixed
            capture(e,f'select-phase{phase}')
        # Inspect every displayed frame under short alternating input pulses.
        # Native color references isolate opaque foreground pixels from the
        # backdrop pulse and sprite arrows/names. A partial BAT must not appear.
        refs=[]
        for hero in range(4):
            mask=np.asarray(Image.open(out/'preview'/f'sgx_select_source_{hero}.png'))[...,3]>=128
            reference=np.asarray(Image.open(captures/f'select-{hero}-page{1-(hero&1)}.png').convert('RGB'))[11:235,26:346]
            refs.append((mask,reference))
        rapid_states=[]
        for frame in range(160):
            e.input((128 if (frame//20)&1==0 else 32) if frame%20<10 else 0)
            e.run(1);capture(e,'switch-current')
            current=np.asarray(Image.open(captures/'switch-current.png').convert('RGB'))[11:235,26:346]
            matches=[h for h,(mask,reference) in enumerate(refs) if np.array_equal(current[mask],reference[mask])]
            assert len(matches)==1,('incomplete visible portrait during rapid input',frame,matches)
            rapid_states.append(matches[0])
            if frame%10==0:capture(e,f'rapid-switch-{frame:03d}')
        e.input(0);e.run(40)
        assert len(set(rapid_states))>1,'Rapid input must exercise a portrait switch'
        # Return to Colt, then confirm while another map update is in flight.
        for _ in range(3):t.press(e,32)
        target=symbol(out/'app.elf','pce_sgx_select_hero')
        e.input(129);t.until(e,lambda:e.memory(target,1)==b'\2',step=1,limit=120)
        e.input(1);e.run(60);e.input(0)
        t.until(e,lambda:e.memory(t.address+5,1)==b'\1' and e.memory(ui,1)==b'\0',limit=16000)
        assert t.metrics(e)['hero']==2
        capture(e,'gameplay')
        # Native terminal-state path must load the enhanced painting and keep
        # its shared palettes intact through the old lettering-pulse interval.
        t.seed(e,'pce_continues',0,1);t.field(e,'state',3)
        t.until(e,lambda:e.memory(ui,1)==b'\4',limit=20000);e.run(180)
        records=[tuple(map(int,r.split(','))) for r in re.findall(r'\{([^{}]+)\}',pair)]
        p,t0,m0,t1,m1,n0,n1,_=records[20]
        compare(e,0,t0,m0,n0);compare(e,1,t1,m1,n1)
        assert e.memory(sgx+4,1)[0]&4,'Game over must enable both BG planes'
        pal=bytes.fromhex(e.call('asread','pram',0,512)['hex'])
        e.run(160)
        after=bytes.fromhex(e.call('asread','pram',0,512)['hex'])
        assert after[:384]==pal[:384],'Game-over pulse overwrote painting colors'
        phases=set()
        for _ in range(16):
            e.run(8);phases.add(bytes.fromhex(e.call('asread','pram',384,128)['hex']))
        assert len(phases)==4,'SGX must show all four additive lettering strengths'
        capture(e,'gameover')
    report=dict(title=title,selection_video_frames=selection_frames,selection_additional_disc_reads=0,source_art_width=512,repeat_columns=64,portrait_switches=10,
                rapid_switch_frames=len(rapid_states),rapid_switch_states=sorted(set(rapid_states)),
                hashes={f:hashlib.sha256((out/f).read_bytes()).hexdigest() for f in ('app.elf','saber_rider.iso')})
    (out/'sgx_frontend_report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('SGX source crop and retail front-end passed',flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'));verify(p.parse_args().out.resolve())
