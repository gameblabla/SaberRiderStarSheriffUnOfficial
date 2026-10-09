#!/usr/bin/env python3
"""Native SGX rendering checks with seeded camera, projectile and death fixtures."""
import argparse,json,struct
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def moon_cell(blob,patterns,camera,scroll,world,row):
    left=179-(camera*5>>8)
    position=left+scroll
    column=position>>3
    if patterns and -96<left<256 and 2<=row<15 and column<=world<column+13:
        cell=(row-2)*13+world-column
        if not blob[patterns+43264+(position&7)*169+cell]:return None
        offset=patterns+(position&7)*5408+cell*32
        return blob[offset:offset+32]
    return None

class Rendering(Campaign):
    def __init__(self,out):
        super().__init__(out,sgx=True)
        self.manifest=json.loads((out/'manifest.json').read_text())
        self.sym={n:symbol(out/'app.elf',n) for n in ('player','actors','shots','camera','safe_timer','dialogs_done',
            'pce_presented','pce_sat_pending','fg_count','fg_parts','fg_camera','fg_camera_slow',
            'pce_sgx_sky_first','pce_sgx_sky_scroll_x','pce_sgx_sky_near_x','pce_sgx_vdc1_hidden',
            'sprite_slot_of','sprite_words','sprite_words1','sprite_pb_hi','sprite_count','sprite_ids','herd_on')}
    def stage(self,e,n):
        # Seed the requested next stage, then use the retail victory/load path.
        # Full unmodified transitions are covered separately by test_campaign.
        self.seed(e,'stage',n-1,1);self.field(e,'state',2);self.advance(e,n)
        self.dialogs(e);e.run(120)
    def word(self,e,n):return int.from_bytes(e.memory(self.sym[n],2),'little')
    def sat(self,e,vdc):return list(struct.iter_unpack('<4H',bytes.fromhex(e.call('asread',f'sat{vdc}',0,512)['hex'])))
    def settle(self,e):
        before=self.word(e,'pce_presented')
        self.until(e,lambda:self.word(e,'pce_presented')!=before,step=1,limit=120)
        # SAT DMA completes inside this same vertical blank.
        e.run(1)
    def position(self,e,x):
        e.input(0);e.write(self.sym['actors'],bytes(168));e.write(self.sym['shots'],bytes(208))
        e.write(self.sym['player'],struct.pack('<4h4B',x,160,0,0,0,0,4,4))
        self.seed(e,'camera',max(0,x-120));self.seed(e,'safe_timer',250,1);self.seed(e,'dialogs_done',255,1)
        e.run(90)
    def sky(self,e):
        stage=self.metrics(e)['stage'];scene=self.manifest['scenes'][stage-1];blob=(self.out/f's{stage}.bin').read_bytes()
        off=scene['records']['sgx_sky_record']['offset']
        (patterns,mapping,_count,_bat,_fg,_nfg,_slow,_nslow,_first_tile,cols,speed,
         wrap,near_map,near_cols,near_first_tile,near_speed,split_row,moon_patterns,_moon_palette)=struct.unpack_from(
             '<IIHBIHIHHHHBIHHHBII',blob,off)
        camera=self.metrics(e)['camera_x'];scroll=self.word(e,'pce_sgx_sky_scroll_x');column=self.word(e,'pce_sgx_sky_first')
        expect=(camera*speed+255)//256
        near_scroll=int.from_bytes(e.memory(self.sym['pce_sgx_sky_near_x'],2),'little')
        near_expect=(camera*near_speed+255)//256
        assert scroll==expect and column==scroll>>3,(stage,camera,scroll,column,expect)
        assert near_scroll==near_expect,(stage,camera,near_scroll,near_expect)
        assert e.memory(self.sym['pce_sgx_vdc1_hidden'],1)==b'\0',(stage,'VDC1 sky hidden')
        vram=bytes.fromhex(e.call('asread','vram1',0,65536)['hex']);checked=0
        bat=(e.memory(symbol(self.out/'app.elf','pce_sgx_sky_page'),1)[0]*0x800 if moon_patterns else 0)
        for screen_col in range(33):
            for y in range(30):
                near=bool(split_row and y>=split_row)
                source_map,source_cols,id_base,first=(near_map,near_cols,near_first_tile,near_scroll>>3) if near else (mapping,cols,0,column)
                world=first+screen_col
                source=world%source_cols if (wrap or near) else world
                if source<source_cols:
                    tile,pal=struct.unpack_from('<HB',blob,source_map+source*90+y*3);tile+=id_base
                else:tile,pal=0,0
                cell=struct.unpack_from('<H',vram,(bat+y*64+(world&63))*2)[0];word=(cell&4095)*16
                moon=moon_cell(blob,moon_patterns,camera,scroll,world,y)
                if moon is not None:
                    assert cell>>12==14 and vram[word*2:word*2+32]==moon,(stage,world,y,'moon BG1 tile')
                    checked+=1;continue
                assert cell>>12==pal,(stage,world,y,'sky palette',cell>>12,pal)
                assert vram[word*2:word*2+32]==blob[patterns+tile*32:patterns+(tile+1)*32],(stage,world,y,'sky patterns',tile)
                checked+=1
        return checked
    def foreground_snapshot(self,e):
        count=e.memory(self.sym['fg_count'],1)[0];address=134*8192+(self.sym['fg_parts']&8191)
        parts=list(struct.iter_unpack('<4H4BH',e.memory(address,count*14,logical=False)))
        normal=self.word(e,'fg_camera');slow=self.word(e,'fg_camera_slow');checked=0
        expected=[]
        for y,x,pattern,attr,lo,hi,slot,group,index in parts:
            assert attr&15==12,('foreground palette',attr)
            x=(x-(normal if group&1 else slow))&65535
            screen_y=y-64
            if not 17<=x<288 or screen_y<=-32 or screen_y>=224:continue
            expected.append((y,x,pattern,attr,index))
            checked+=1
        return {'normal':normal,'slow':slow,'expected':expected,'checked':checked}

    def validate_foreground(self,e,snapshot,allow_herd_shake=False,allow_camera_lag=False):
        sat=self.sat(e,0);normal=snapshot['normal'];slow=snapshot['slow'];expected=snapshot['expected']
        if allow_herd_shake:
            assert all((y,x,pattern,attr) in sat for y,x,pattern,attr,_index in expected),(
                'foreground moved or disappeared during herd shake',normal,slow,expected[:8])
            self.last_fg_shake=0
        elif allow_camera_lag:
            # During a moving boss encounter the CPU's retained foreground
            # list can be one draw ahead of the SAT published at VBlank. Match
            # all retained parts under at most one native camera step,
            # preserving each part's exact pattern and palette.
            shifts=[(dx,0) for dx in range(-5,6)
                    if all((y,x+dx,pattern,attr) in sat
                           for y,x,pattern,attr,_index in expected)]
            assert shifts,('missing foreground under camera publication lag',normal,slow,expected[:8],
                           [row for row in sat if row[0] and row[3]&15==12][:24])
            self.last_fg_camera_shift=shifts[0]
        else:
            missing=[(index,x,y) for y,x,pattern,attr,index in expected
                     if (y,x,pattern,attr) not in sat]
            assert not missing,('missing foreground',normal,slow,missing[:8],
                                 [row for row in sat if row[0] and row[3]&15==12][:24])
            self.last_fg_shake=0
        assert not any(s[0] and s[3]&15==12 for s in self.sat(e,1))
        return snapshot['checked']

    def foreground(self,e,allow_herd_shake=False,allow_camera_lag=False):
        return self.validate_foreground(e,self.foreground_snapshot(e),
                                        allow_herd_shake,allow_camera_lag)

    def sprite_entries(self,e,sprite_id):
        slot=e.memory(self.sym['sprite_slot_of']+sprite_id,1)[0]
        if slot>=48:return []
        ids=struct.unpack('<48H',e.memory(self.sym['sprite_ids'],96))
        if ids[slot]!=sprite_id:return []
        flags=e.memory(self.sym['sprite_pb_hi']+slot,1)[0]
        words0=struct.unpack('<48H',e.memory(self.sym['sprite_words'],96))
        words1=struct.unpack('<48H',e.memory(self.sym['sprite_words1'],96))
        counts=e.memory(self.sym['sprite_count'],48)
        result=[]
        for vdc,table,bit in ((0,words0,0x40),(1,words1,0x80)):
            if not flags&bit:continue
            first=table[slot]>>5
            end=first+counts[slot]*2
            result.extend((vdc,index,row) for index,row in enumerate(self.sat(e,vdc))
                          if row[0] and first<=row[2]<end)
        return result
    def rates(self,e):
        result={}
        for name,key in [('idle',0),('fire',1)]:
            e.input(key);e.run(120);start=self.word(e,'pce_presented');e.run(600)
            result[name]=((self.word(e,'pce_presented')-start)&65535)/10
        e.input(0);return result
    def run_checks(self):
        reports={}
        with Emulator(self.out/'saber_rider.cue',self.out/'rendering-emulator',sgx=True) as e:
            boot(e,self.address);self.dialogs(e);e.run(120)
            reports['fps']=self.rates(e);print('Cadence',reports['fps'],flush=True)
            for stage in (1,3,4,5):
                if stage!=1:self.stage(e,stage)
                scene=self.manifest['scenes'][stage-1];blob=(self.out/f's{stage}.bin').read_bytes()
                off=scene['records']['sgx_sky_record']['offset'];rec=struct.unpack_from('<IIHBIHIHHHHB',blob,off)
                # First full source foreground chunk, plus native sky column boundaries.
                xs=[160,1000,2200]
                if stage==1: xs += [120+c for c in (2012,2044,2048,2052,2080,2044,4092,4096,4100)]
                if rec[5]:xs.append(max(160,struct.unpack_from('<h',blob,rec[4])[0]+40))
                checks=[]
                for x in xs:
                    self.position(e,x);self.settle(e);checks.append(dict(camera=self.metrics(e)['camera_x'],sky=self.sky(e),foreground=self.foreground(e)))
                    if stage==1:e.screenshot(self.out/f'sky-repeat-{x-120}.png')
                e.screenshot(self.out/f'rendering-stage{stage}.png');reports[f'stage{stage}']=checks
                print('Stage',stage,'sky/foreground passed',flush=True)
            self.stage(e,1);self.position(e,400)
            # A stationary active hero bullet, above the terrain and clear of enemies.
            e.write(self.sym['shots'],struct.pack('<4h5B',self.metrics(e)['player_x']+60,100,0,0,1,0,0,0,0)+bytes(195))
            # The debugger may seed the object after this draw's shot pass.
            self.settle(e);self.settle(e)
            bullet=[]
            for _ in range(30):
                self.settle(e);slot=e.memory(self.sym['sprite_slot_of']+36,1)[0]
                assert slot<48
                matches=self.sprite_entries(e,36)
                assert matches,'Active hero projectile disappeared'
                bullet.append(len(matches))
            reports['bullet_generations']=len(bullet)
            # Observe all six walker death poses on the native CPU.
            e.write(self.sym['shots'],bytes(208));self.position(e,400)
            actor=struct.pack('<4h4B9B',490,177,0,0,0,0,4,4,1,1,0,0,0,1,0,0,0)
            e.write(self.sym['actors'],actor+bytes(147));poses=[];rendered=set()
            scene=self.manifest['scenes'][0];blob=(self.out/'s1.bin').read_bytes()
            ids=[next(i for i,s in enumerate(scene['sprites']) if s['name']==f'walker_death{k}') for k in range(6)]
            for _ in range(80):
                e.run(1);a=e.memory(self.sym['actors'],21)
                if not a[12]:break
                if a[17]:poses.append((a[17]-1)//4)
                entries=self.sat(e,1)
                cache_ids=struct.unpack('<48H',e.memory(self.sym['sprite_ids'],96))
                words=[struct.unpack('<48H',e.memory(self.sym[name],96)) for name in ('sprite_words','sprite_words1')]
                slots=e.memory(self.sym['sprite_slot_of'],512)
                pb_hi=e.memory(self.sym['sprite_pb_hi'],48)
                for pose,id in enumerate(ids):
                    slot=slots[id]
                    if pose in rendered or slot>=48 or cache_ids[slot]!=id or not pb_hi[slot]&0x80:continue
                    first=words[1][slot]>>5
                    hits=[r for r in entries if r[0] and first<=r[2]<first+scene['sprites'][id]['entries']*2]
                    if not hits:continue
                    pat,parts,pal,*_=struct.unpack_from('<III4B',blob,scene['sprite_table']+id*16)
                    for y,x,pattern,attr in hits:
                        piece=(pattern-first)//2
                        data=bytes.fromhex(e.call('asread','vram1',pattern*64,128)['hex'])
                        assert data==blob[pat+piece*128:pat+(piece+1)*128],('death patterns',pose)
                        palette=bytes.fromhex(e.call('asread','pram',(16+(attr&15))*32,32)['hex'])
                        assert palette==blob[pal:pal+32],('death palette',pose)
                    rendered.add(pose)
            assert set(poses)==set(range(6)),poses
            assert rendered==set(range(6)),('missing rendered death poses',rendered)
            reports['death_poses']=sorted(rendered);self.metrics(e)
        (self.out/'rendering-verification.json').write_text(json.dumps(reports,indent=2)+'\n')
        print('SGX rendering checks passed',flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'));Rendering(p.parse_args().out.resolve()).run_checks()
