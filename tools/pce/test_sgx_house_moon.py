#!/usr/bin/env python3
"""Building/prop depth and continuous native moon page publication."""
import argparse,json,struct,tempfile
from pathlib import Path
import numpy as np
from PIL import Image
import build_assets
from emulator import Emulator,boot,symbol
from test_sgx_rendering import Rendering
from test_sgx_house import source_openings

def verify(out):
    out=out.resolve();c=Rendering(out);work=out/'work'
    bake=build_assets.scenery_background
    try:
        build_assets.scenery_background=lambda image,*_:image
        raw=build_assets.platform_background(1,work,sgx=True)[3]
    finally:build_assets.scenery_background=bake
    level,files=build_assets.levl.load_dump(work/'stage1.layers')
    cover=Image.new('L',raw.size)
    # Independent source terrain mask: these layers follow the scenery actors.
    for layer in level.layers:
        if layer.name not in ('Playfield','Platforms','Cars'):continue
        bank=(build_assets.levl.png_bank(files[layer.cblock],layer.cblock)
              if files[layer.cblock] else build_assets.levl.load_bank(work/'srgb'/f'{layer.cblock:08X}.srgb'))
        for x in range(0,raw.width,256):
            pixels=build_assets.levl.render_layer(layer,bank,x,256,240,None)
            mask=np.maximum(np.asarray(cover.crop((x,0,x+256,240))),pixels[...,3])
            cover.paste(Image.fromarray(mask),(x,0))
    baked=bake(raw.copy(),work,1,cover)
    actual=build_assets.platform_background(1,work,sgx=True)[3]
    assert np.array_equal(np.asarray(actual),np.asarray(baked)), 'Source layer ordering changed'
    protected=np.asarray(cover)>=128
    assert np.array_equal(np.asarray(raw)[protected],np.asarray(actual)[protected]), 'Prop overwrote building art'
    # The source openings need their clipped static window art. Equality with
    # the unfilled tilemap accepted the unrelated MidBG building underneath.
    source_openings(work)
    fans=[]
    for t in json.loads((work/'stage1.json').read_text())['triggers']:
        if t['type'] not in (17,18):continue
        region=(t['waypoints'][0][0],t['waypoints'][0][1],
                t['waypoints'][0][0]+48,t['waypoints'][0][1]+32)
        assert c.manifest['scenes'][0]['actor_ids'][t['type']]==255,('Fan still drawable',t['type'])
        assert not any(s['name']==f"actor_type{t['type']}" for s in c.manifest['scenes'][0]['sprites']),('Fan asset still generated',t['type'])
        fans.append(region)
    assert len(fans)==2
    for scene in c.manifest['scenes']:
        if 'actor_ids' in scene:
            assert all(scene['actor_ids'][kind]==255 for kind in (17,18)),('Fan returned in another stage',scene['stage'])
    for name,x in (('saloon',6480),('hotel',7100)):
        actual.crop((x,0,x+256,224)).save(out/f'{name}-source-fixed.png')
    with tempfile.TemporaryDirectory(prefix='moon-motion-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=True) as e:
        boot(e,c.address);c.dialogs(e)
        for name,x in (('saloon',6596),('hotel',7216)):
            c.position(e,x);c.settle(e)
            e.screenshot(out/f'{name}-disc-fixed.png')
            # Check the complete visible facade in the native VDC, not just
            # a host preview or the two original fan rectangles.
            records=c.manifest['scenes'][0]['records'];blob=(out/'s1.bin').read_bytes()
            scroll=int.from_bytes(e.memory(symbol(out/'app.elf','pce_scroll_x'),2),'little')
            vram=bytes.fromhex(e.call('asread','vram0',0,65536)['hex'])
            for world in range(scroll//8,scroll//8+33):
                for row in range(2,25):
                    tile,pal=struct.unpack_from('<HB',blob,records['sgx_bg_columns']['offset']+world*90+row*3)
                    word=struct.unpack_from('<H',vram,(row*64+(world&63))*2)[0]
                    pattern=(word&4095)*32
                    source=records['sgx_bg_patterns']['offset']+tile*32
                    assert word>>12==pal and vram[pattern:pattern+32]==blob[source:source+32],(name,scroll,world,row,hex(word),tile,pal,vram[pattern:pattern+32].hex(),blob[source:source+32].hex(),'building BAT/pattern mismatch')
        e.screenshot(out/'house-props-fixed.png')
        c.stage(e,3);c.position(e,400);c.settle(e);c.sky(e)
        page=symbol(out/'app.elf','pce_sgx_sky_page');counts=[];pages=[];scrolls=[]
        e.input(32)
        for frame in range(180):
            e.run(1)
            path=out/'moon-current.png';e.screenshot(path)
            a=np.asarray(Image.open(path).convert('RGB'))[11:80,12:268].astype(int)
            red=(a[...,0]>a[...,1]*1.5)&(a[...,0]>a[...,2]*1.5)&(a[...,0]>60)
            counts.append(int(red.sum()));pages.append(e.memory(page,1)[0])
            if frame%30==0:e.screenshot(out/f'moon-moving-{frame}.png')
            scrolls.append(c.metrics(e)['camera_x'])
        e.input(0);c.position(e,400);c.settle(e);c.sky(e)
        report=dict(fan_regions=fans,red_pixels=counts,pages=pages,cameras=scrolls)
        (out/'house-moon-verification.json').write_text(json.dumps(report,indent=2)+'\n')
        assert set(pages)=={0,1},'Moon motion must exercise both background pages'
        assert min(counts)>100,('Moon disappeared during active motion',counts)
        print('House and moon passed',dict(fans=len(fans),min_red_pixels=min(counts),max_red_pixels=max(counts)),flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'));verify(p.parse_args().out)
