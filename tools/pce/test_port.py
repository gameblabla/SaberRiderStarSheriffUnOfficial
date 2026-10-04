#!/usr/bin/env python3
"""Native-disc regression tests; these do not certify the unfinished campaign."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import numpy as np
from PIL import Image
from emulator import BIOS, ROOT, Emulator, boot, symbol
from formats import planar_tile, planar_sprite, pair_characters
from adpcm import encode,decode

FIELDS='magic version ready ports stage frames floor_commits uploads dropped essential_overflow disc_reads forbidden_reads load_error max_units sat_count hero phase player_x player_y camera_x hp'.split()

def decode_tile(data):
    b=np.frombuffer(data,np.uint8)
    planes=[b[:16].reshape(8,2)[:,0],b[:16].reshape(8,2)[:,1],b[16:].reshape(8,2)[:,0],b[16:].reshape(8,2)[:,1]]
    return sum(np.unpackbits(p[:,None],axis=1).astype(np.uint8)<<i for i,p in enumerate(planes))

def decode_sprite(data):
    b=np.frombuffer(data,np.uint8).reshape(4,16,2)[:,:,::-1]
    return sum(np.unpackbits(b[i],axis=1).astype(np.uint8)<<i for i in range(4))

def assets(out):
    tile=np.arange(64,dtype=np.uint8).reshape(8,8)&15
    sprite=np.arange(256,dtype=np.uint8).reshape(16,16)&15
    assert np.array_equal(decode_tile(planar_tile(tile)),tile)
    assert np.array_equal(decode_sprite(planar_sprite(sprite)),sprite)
    pcm=(np.sin(np.arange(800)*2*np.pi*220/8000)*16000).astype(np.int16)
    restored=np.array(decode(encode(pcm)))
    assert np.sqrt(np.mean((restored[32:]-pcm[32:])**2))<1200
    pairs=pair_characters()
    for i in range(256):
        expect=np.repeat(np.array([i>>4,i&15],np.uint8),4)
        assert np.array_equal(decode_tile(pairs[i*32:(i+1)*32]),np.tile(expect,(8,1)))
    manifest=json.loads((out/'manifest.json').read_text())
    for scene in manifest['scenes']:
        data=(out/f's{scene["stage"]}.bin').read_bytes()
        assert len(data)==scene['bytes']<=0x1e0000 and len(data)%2048==0
        for resource in scene['records'].values():assert resource['offset']+resource['bytes']<=len(data)
        for s in scene['sprites']:
            assert 0<s['entries']<=32 and s['patterns']==s['entries']*128
            assert s['facing_variants']==1 and s['units_per_line']<=16
        if scene['stage'] in (1,3,4,5):assert scene['ntr']<=60 and scene['rows']<=32
    audio=json.loads((out/'audio.json').read_text())
    music=[t for t in audio['tracks'] if t['id']!='END']
    assert len(music)==54 and sorted(t['track'] for t in music)==list(range(2,20))+list(range(21,39))+list(range(40,58))
    for track in music:
        data=(out/track['file']).read_bytes()
        assert len(data)%2352==0
        if 'sha256' in track: assert hashlib.sha256(data).hexdigest()==track['sha256']
        assert data[:2352*150]==bytes(2352*150) and any(data[2352*150:])
    return manifest

class Test:
    def __init__(self,out):
        self.out=out;self.address=symbol(out/'app.elf','pce_metrics');self.results={}
    def metrics(self,e):
        d=dict(zip(FIELDS,struct.unpack('<4s4B8H4B4H',e.memory(self.address,36))))
        assert d['magic']==b'SRPC' and d['ports']==15 and d['load_error']==0
        assert d['max_units']<=16 and d['sat_count']<=64
        assert d['essential_overflow']==0 and d['forbidden_reads']==0,d
        d.pop('magic');return d
    def press(self,e,key,n=30):
        e.input(key);e.run(n);e.input(0);e.run(n)
    def stage(self,e,n):
        ui=symbol(self.out/'app.elf','pce_ui_state')
        if e.memory(symbol(self.out/'app.elf','pce_campaign'),1)==b'\2':
            for _ in range(100):
                if e.memory(ui,1)==b'\5':break
                e.run(120)
        if e.memory(ui,1)==b'\5':
            self.press(e,1,60)
            for _ in range(100):
                if e.memory(ui,1)==b'\0' and self.metrics(e)['ready']:break
                e.run(120)
        for _ in range(100):
            if self.metrics(e)['ready']:break
            e.run(120)
        old=self.metrics(e)['stage'];self.press(e,8)
        for _ in range((n-old)%7):self.press(e,32)
        self.press(e,8)
        for _ in range(100):
            d=self.metrics(e)
            if d['stage']==n and d['ready']:
                e.run(120);print(f'Stage {n} ready',flush=True);return self.metrics(e)
            e.run(120)
        raise AssertionError(f'Stage {n} load timed out')
    def diagnostics(self,e):
        self.press(e,8);self.press(e,2);self.press(e,8);e.run(240)
        for _ in range(100):
            if self.metrics(e)["ready"]:return
            e.run(120)
        raise AssertionError("Diagnostic scene load timed out")
    def capture(self,e,name):e.screenshot(self.out/f'{name}.png')
    def run(self):
        with tempfile.TemporaryDirectory(prefix='verify-',dir=self.out) as base:
            with Emulator(self.out/'saber_rider.cue',base) as e:
                boot(e,self.address);self.diagnostics(e);e.run(120);d=self.metrics(e);self.capture(e,'test-town')
                assert d['stage']==1 and d['hp']==2   # NORMAL difficulty: two hearts
                bank=(self.out/'voice0.bin').read_bytes()
                actual=bytes.fromhex(e.call('asread','adpcm',0,len(bank))['hex'])
                assert actual==bank,'Preloaded hardware ADPCM RAM must match the mastered bank'
                snd=e.call('sound_status');e.run(120);snd2=e.call('sound_status')
                assert snd2['nonzero']>snd['nonzero']+1000, 'CD-DA must produce sound without a PSG effect'
                start=d['player_x'];e.input(32);e.run(120);e.input(0);e.run(10);d=self.metrics(e)
                assert d['player_x']>=start+150, 'Walking uses elapsed video ticks'
                y=d['player_y'];self.press(e,2,8);assert self.metrics(e)['player_y']<y
                assert e.call('registers')['registers']['Playing']==1,'Jump must start the hardware ADPCM voice'
                self.capture(e,'test-jump');e.run(120)
                self.press(e,128,10)
                sat=bytes.fromhex(e.call('asread','vram0',0xfe00,512)['hex'])
                entries=np.frombuffer(sat,'<u2').reshape(64,4)
                assert any((a[3]&0x800) for a in entries if a[0]),'Left-facing actor uses the hardware flip bit'
                self.results['platform']=self.metrics(e)
                self.press(e,8);self.press(e,4);self.capture(e,'test-planar')
                # Only high planes change in the four prepared working characters.
                before=bytes.fromhex(e.call('asread','vram0',(0x4200+92*16)*2,128)['hex'])
                self.press(e,0,17)
                after=bytes.fromhex(e.call('asread','vram0',(0x4200+92*16)*2,128)['hex'])
                assert all(before[t*32:t*32+16]==after[t*32:t*32+16] for t in range(4))
                assert before!=after
                self.press(e,8);e.run(120)
                race=self.stage(e,2);self.capture(e,'test-race');e.run(600);r2=self.metrics(e)
                commits=(r2['floor_commits']-race['floor_commits'])&65535
                assert commits>=40, f'Floor refresh regressed: {commits}/600 video frames'
                reads=r2['disc_reads'];self.press(e,4,60);e.run(300);p=self.metrics(e)
                assert p['phase']==1 and p['disc_reads']==reads
                assert e.memory(symbol(self.out/'app.elf','pce_save_status'),1)==b'\0'
                self.capture(e,'test-pursuit');self.results['floor_commits_per_600_video_frames']=commits
                self.results['pursuit']=p
            with Emulator(self.out/'saber_rider.cue',base) as e:
                boot(e,self.address);e.run(120);d=self.metrics(e)
                assert d['stage']==2 and d['phase']==1,'BIOS checkpoint must survive reboot'
                self.results['checkpoint']=d
                self.diagnostics(e)
                for stage in (3,4,5):
                    self.stage(e,stage);self.capture(e,f'test-stage{stage}')
                    e.input(33);e.run(180);e.input(0);self.results[f'stage{stage}']=self.metrics(e)
                self.stage(e,6);self.capture(e,'test-commander')
                for key in (128,32):
                    e.input(key|2|1);e.run(300);e.input(0);self.metrics(e)
                for _ in range(4):self.press(e,16);self.metrics(e)
                self.capture(e,'test-cockpit-clip');self.results['cockpit']=self.metrics(e)
                self.stage(e,7);e.input(1);e.run(300);e.input(0)
                self.capture(e,'test-space');self.results['space']=self.metrics(e)
                for stage in (1,3,4,5,1):self.stage(e,stage)
                self.results['reload']=self.metrics(e)
                for hero in range(1,4):
                    self.press(e,8);self.press(e,16);self.press(e,8)
                    e.run(240)
                    for _ in range(100):
                        if self.metrics(e)['ready']:break
                        e.run(120)
                    e.run(120);d=self.metrics(e);assert d['hero']==hero and d['ready']
                    bank=(self.out/f'voice{hero}.bin').read_bytes()
                    assert bytes.fromhex(e.call('asread','adpcm',0,len(bank))['hex'])==bank
                    y=d['player_y'];self.press(e,2,8);d=self.metrics(e)
                    assert d['player_y']<y and d['sat_count']>0
                    assert e.call('registers')['registers']['Playing']==1
                    self.capture(e,f'test-hero{hero}');self.results[f'hero{hero}']=d
        for name,bios,card in [('no-card',BIOS,False),('v2',ROOT/'PCE/[BIOS] TurboGrafx CD System Card (USA) (v2.0).pce',True)]:
            with tempfile.TemporaryDirectory(prefix=name+'-',dir=self.out) as base:
                with Emulator(self.out/'saber_rider.cue',base,bios=bios,arcade=card) as e:
                    e.run(120);self.press(e,8,10);e.run(2200);self.capture(e,'test-'+name)
                    if card:
                        assert 0x3000<=e.call('registers')['registers']['PC']<0x3800,'Old BIOS must reach the IPL rejection'
                    else:
                        assert e.memory(self.address,4)==b'SRPC','Missing-card test must reach the application rejection'
                        assert not e.memory(self.address+5,1)[0]
                    image=np.asarray(Image.open(self.out/f'test-{name}.png').convert('RGB'))
                    assert (image.max(2)>150).sum()>50,'Requirement message must be visible'
        self.results['bios']=str(BIOS);self.results['emulator']=str(ROOT/'PCE/mednafen-pce-headless')
        self.results['scope']='Native core and renderer prototypes; full campaign acceptance remains pending'
        (self.out/'verification.json').write_text(json.dumps(self.results,indent=2)+'\n')

def main():
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();out=a.out.resolve()
    assets(out);Test(out).run();print('PCE native-disc regression checks passed.')
if __name__=='__main__':main()
