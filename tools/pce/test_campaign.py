#!/usr/bin/env python3
"""Campaign behavior through the accurate emulator, with explicit scenario seeds.

Debugger writes seed positions/timers/HP for long encounters; all damage,
dialog paging, transitions and game-over decisions execute on the native CPU.
This is scenario coverage, not a certification of a complete unassisted run.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from emulator import Emulator,boot,symbol,BIOS,BINARY,ROOT
from test_port import Test
FIELDS='state lives powers result event story page diagnostic boss_kind boss_round wave lap rank score boss_hp timer boost power_cd'.split()

class Campaign(Test):
    def __init__(self,out,sgx=False):
        super().__init__(out)
        self.campaign=symbol(out/'app.elf','pce_campaign')
        self.sgx=sgx
        if sgx:
            self.sgx_address=symbol(out/'app.elf','pce_sgx_metrics')
            self.sgx_peak_units=0
            self.sgx_peak_sat=0
            with tempfile.TemporaryDirectory(prefix='sgx-code-') as base:
                code=Path(base)/'bank135.bin'
                subprocess.run([str(ROOT/'PCE/llvm-mos8/bin/llvm-objcopy'),
                                '--dump-section',f'.ram_bank135={code}',
                                str(out/'app_full.elf'),str(Path(base)/'app.elf')],check=True)
                self.sgx_code=code.read_bytes()
    def verify_sgx_code(self,e,checkpoint):
        if not self.sgx:return
        actual=e.memory(135*8192,len(self.sgx_code),logical=False)
        differences=[f'${0xc000+i:04x}: {expected:02x}->{actual[i]:02x}'
                     for i,expected in enumerate(self.sgx_code) if actual[i]!=expected]
        assert not differences,('SGX code bank was overwritten',checkpoint,differences[:16])
        self.results.setdefault('sgx_code_integrity',[]).append(checkpoint)
    def metrics(self,e):
        d=super().metrics(e)
        if self.sgx:
            raw=e.memory(self.sgx_address,13)
            assert raw[:4]==b'SGX1',raw.hex()
            failures=raw[8]
            units,sat=raw[11],raw[12]
            assert failures==0,('SGX runtime failure',raw.hex(),d)
            assert units<=16 and sat<=64,('VDC1 sprite budget',units,sat,d)
            self.sgx_peak_units=max(self.sgx_peak_units,units)
            self.sgx_peak_sat=max(self.sgx_peak_sat,sat)
        return d
    def state(self,e):return dict(zip(FIELDS,struct.unpack('<13B5H',e.memory(self.campaign,23))))
    def capture(self,e,name):
        self.results.setdefault('captures',{})[name]=self.metrics(e)
        super().capture(e,name)
        print(f'Captured {name}',flush=True)
    def verify_hull(self,e):
        if not self.sgx:return
        self.until(e,lambda:e.memory(symbol(self.out/'app.elf','hull_ready'),1)==b'\1')
        stage=self.metrics(e)['stage']
        level=e.memory(symbol(self.out/'app.elf','hull_level'),1)[0]
        manifest=json.loads((self.out/'manifest.json').read_text())
        scene=manifest['scenes'][stage-1]
        blob=(self.out/f's{stage}.bin').read_bytes()
        _,_,address,size,_=struct.unpack_from('<IIIHB',blob,scene['boss_big_offset']+15*level)
        # The platform hull lives on VDC1; VDC0 owns independent foreground
        # patterns at the same addresses. Requiring a mirrored hull there
        # would erase the very foreground this campaign must retain.
        actual=bytes.fromhex(e.call('asread','vram1',(0x4800+16*256)*2,size)['hex'])
        assert actual==blob[address:address+size],(stage,level,'vram1','boss hull patterns differ')
        self.results[f'stage{stage}_hull_patterns']='VDC1 matches the stage archive'
    def press(self,e,key,n=30):
        ui=symbol(self.out/'app.elf','pce_ui_state')
        # Between selection and stage readiness, the loader does not poll
        # input. SGX's larger archives can outlast an input handshake.
        if e.memory(ui,1)==b'\0' and not self.metrics(e)['ready']:
            self.until(e,lambda:e.memory(ui,1)!=b'\0' or self.metrics(e)['ready'],limit=12000)
        if self.state(e)['state']==2:
            self.until(e,lambda:e.memory(ui,1)==b'\5',limit=6000)
            e.run(40)
        e.input(0);e.run(30)
        previous=symbol(self.out/'app.elf','previous')
        self.until(e,lambda:e.memory(previous,1)==b'\0',limit=12000,step=10)
        e.input(key);e.run(n)
        self.until(e,lambda:e.memory(previous,1)[0]&key==key,limit=2000,step=10)
        e.input(0);e.run(n)
        self.until(e,lambda:e.memory(previous,1)==b'\0',limit=12000,step=10)
    def seed(self,e,name,value,width=2):e.write(symbol(self.out/'app.elf',name),int(value).to_bytes(width,'little',signed=value<0))
    def field(self,e,name,value):
        i=FIELDS.index(name);offset=i if i<13 else 13+(i-13)*2
        e.write(self.campaign+offset,value.to_bytes(1 if i<13 else 2,'little'))
    def until(self,e,condition,limit=10000,step=30):
        for _ in range(0,limit,step):
            if condition():return
            e.run(step)
        ui=symbol(self.out/'app.elf','pce_ui_state')
        raise AssertionError(f'Campaign timeout: {self.state(e)}, {self.metrics(e)}, ui={e.memory(ui,1).hex()}')
    def dialogs(self,e):
        for _ in range(100):
            if self.state(e)['state']!=1:return
            self.press(e,1,15)
        raise AssertionError('Dialog did not finish')
    def clear(self,e,limit=10000):
        # Boss destruction can open its final story after the hit helper
        # returns. Page that native story before waiting for CAM_CLEAR.
        for _ in range(0,limit,30):
            self.dialogs(e)
            if self.state(e)['state']==2:
                self.metrics(e)
                return
            e.run(30)
        raise AssertionError(f'Stage did not clear: {self.state(e)}, {self.metrics(e)}')
    def advance(self,e,stage):
        # Retail has no debug stage selector. Confirm the real victory screen
        # and let change_stage load the next scene (stage 6 advances directly).
        if self.state(e)['state']==2 and self.metrics(e)['stage']!=6:self.press(e,1)
        self.until(e,lambda:self.metrics(e)['stage']==stage and self.metrics(e)['ready'])
        e.run(120)
    def move(self,e,x,y=160):
        # Pause before changing the body: a video-frame boundary can land
        # inside physics(), whose local coordinates would overwrite a write.
        self.press(e,8);e.run(120)
        e.write(symbol(self.out/'app.elf','player'),struct.pack('<4h4B',x,y,0,0,0,0,4,4))
        self.seed(e,'camera',max(0,x-120))   # seed the view with the warp; native bosses now glide to their arena
        self.press(e,8)
    def hit_platform_boss(self,e,hp=1):
        if self.state(e)['boss_kind']==3:
            # Dark April has her own Body and introduction state machine.
            # Wait for its real fight, then aim at that Body, not the flying
            # boss coordinates left over from the preceding gunship.
            dark=symbol(self.out/'app.elf','da')
            for _ in range(80):
                self.dialogs(e)
                if e.memory(dark+12,1)==b'\5':break
                e.run(30)
            assert e.memory(dark+12,1)==b'\5',('Dark April did not enter her fight',self.state(e))
        self.press(e,8);e.run(120)
        kind=self.state(e)['boss_kind']
        # The flying bosses take hits in their fighting phases only (gunship 2, Hyperjumper 4-11); Dark April always.
        self.field(e,'boss_hp',hp);self.seed(e,'boss_phase',{1:2,2:5,3:3}[kind],1);self.seed(e,'boss_time',0)
        # A shot at the boss's own centre (the flying bosses steer themselves; Dark April follows the hero's height).
        bx=int.from_bytes(e.memory(symbol(self.out/'app.elf','boss_x'),2),'little')
        by=int.from_bytes(e.memory(symbol(self.out/'app.elf','boss_y'),2),'little')
        if kind==3:
            bx,by=struct.unpack('<2h',e.memory(dark,4))
            self.seed(e,'safe_timer',250,1)
        e.write(symbol(self.out/'app.elf','shots'),struct.pack('<4h2B',bx,by,0,0,1,0))
        self.press(e,8);e.run(60)
        if hp==1 and kind!=3:
            # The wreck falls and burns for 228 steps before the stage clears.
            def gone():
                st=self.state(e);return st['boss_hp']==0 or st['state']!=0 or st['boss_kind']!=kind
            self.until(e,gone,limit=1500)
    def run(self):
        with tempfile.TemporaryDirectory(prefix='campaign-',dir=self.out) as base,Emulator(self.out/'saber_rider.cue',base,sgx=self.sgx) as e:
            boot(e,self.address);e.run(120)
            assert self.state(e)['diagnostic']==0
            reads=self.metrics(e)['disc_reads'];x=self.metrics(e)['player_x']
            # The "Star Sheriffs" scene: the hero stops, the camera pans right to the outrider on the platform
            # (focus x 730, 4 px a step), the text runs there, and the camera comes back to the hero.
            cut=symbol(self.out/'app.elf','cut_phase');peak=0
            e.input(32);self.until(e,lambda:self.state(e)['state']==1,limit=1000);e.input(0)
            assert self.metrics(e)['player_x']>x
            def below_box():
                # Sprites on the ground (screen y >= 110) lie below the box, which sits at the top: hero, actors.
                sat=bytes.fromhex(e.call('asread','vram0',0xfe00,512)['hex'])
                return sum(1 for k in range(64) if (struct.unpack_from('<4H',sat,k*8)[0]&0x3ff)-64>=110)
            e.run(30);assert below_box(),'The hero must stay on screen while the dialogue runs'
            self.capture(e,'campaign-dialog');self.dialogs(e)    # the opening dialogue (x 192)
            e.input(32);self.until(e,lambda:self.state(e)['state']==1,limit=1500,step=10);e.input(0)
            assert self.metrics(e)['camera_x']>=600,self.metrics(e)
            assert e.memory(cut,1)[0]==4
            assert e.call('registers')['registers']['Playing']==1,'The dialogue voice line plays when the box opens'
            e.run(30);assert int.from_bytes(e.memory(symbol(self.out/'app.elf','story_y'),1),'little')==20,'Outrider dialogue must sit at the bottom'
            self.capture(e,'campaign-outrider');self.dialogs(e)
            def panned():
                nonlocal peak
                peak=max(peak,self.metrics(e)['camera_x']);return e.memory(cut,1)==b'\0'
            self.until(e,panned,limit=2000,step=10)
            m=self.metrics(e);assert m['camera_x']==m['player_x']-120 and peak>=600,(m,peak)
            assert self.metrics(e)['disc_reads']==reads,'Dialogs must use preloaded data'
            # Run pauses and resumes the same position without a disc reload.
            x=self.metrics(e)['player_x'];self.press(e,8);e.run(120);self.press(e,8)
            assert self.metrics(e)['player_x']==x and self.metrics(e)['disc_reads']==reads
            # Saber clears bullets, consumes a charge, and enters cooldown.
            self.press(e,5,8)
            self.until(e,lambda:self.state(e)['state']==0 and self.state(e)['power_cd']>0,limit=180)
            assert self.state(e)['powers']==1 and self.state(e)['power_cd']>0
            self.results['power']=self.state(e)
            # A diagonal shot leaves the barrel of the pose drawn (pce_muzzle, up-diagonal standing), at 6 px a
            # step on each axis; it used to leave a fixed point that missed the gun by up to 13 px.
            m=self.metrics(e);table=symbol(self.out/'app.elf','pce_muzzle')
            mx,my=struct.unpack('<2b',e.memory(table+(m['hero']*9+5)*2,2))
            e.write(symbol(self.out/'app.elf','shots'),bytes(16*13))
            self.seed(e,'fire_timer',0,1)
            e.input(4);e.run(4)
            e.input(4|16|32|1)
            for _ in range(30):
                previous_tick=int.from_bytes(e.memory(symbol(self.out/'app.elf','frame'),2),'little')
                e.run(1)
                ticks=(int.from_bytes(e.memory(symbol(self.out/'app.elf','frame'),2),'little')-previous_tick)&65535
                raw=e.memory(symbol(self.out/'app.elf','shots'),16*13)
                found=[struct.unpack_from('<4h2B',raw,k*13) for k in range(16) if raw[k*13+8]]
                if found:break
            e.input(0)
            assert found and found[0][2:4]==(6<<8,-6<<8),found
            m=self.metrics(e)
            # A rendered frame may service multiple gameplay ticks. Both
            # direction keys keep the body still; account for each possible
            # projectile update since the last sample of the empty pool,
            # including a tick already entered at that sample.
            px,py=struct.unpack('<2h',e.memory(symbol(self.out/'app.elf','player'),4))
            assert any((found[0][0]-6*age,found[0][1]+6*age)==(px+mx,py+my)
                       for age in range(ticks+2)),(found[0],px,py,mx,my,ticks)
            # Town boss arrival, player-shot damage, then native clear decision.
            self.move(e,9800);self.until(e,lambda:self.state(e)['boss_kind']==1,limit=600)
            # The gunship takes a hit in its sweep phase (hit points drop by one), then the next one burns it out.
            self.seed(e,'safe_timer',250,1)
            self.verify_hull(e)
            self.hit_platform_boss(e,2);assert self.state(e)['boss_hp']==1,self.state(e)
            self.hit_platform_boss(e,1)
            self.until(e,lambda:self.state(e)['state']==2,limit=600);self.capture(e,'campaign-town-clear')
            self.metrics(e)
            self.results['town']=self.state(e)
            self.press(e,1);self.until(e,lambda:self.metrics(e)['stage']==2 and self.metrics(e)['ready'])
            assert self.state(e)['state']==1
            e.run(120);self.capture(e,'campaign-race-dialog')
            # The race's dialogue spans 448 dots, with two 8-dot BAT
            # characters per source glyph. It used to occupy only 224 dots.
            bat=bytes.fromhex(e.call('asread','vram0',(53*128+6)*2,112)['hex'])
            words=struct.unpack('<56H',bat)
            assert all(w>>12==14 for w in words),words
            self.results['race_dialog_width']=448
            self.dialogs(e)
            assert self.state(e)['lives']==3,'Lives must carry into the next stage'
            # Drive the race against its seven rivals (the countdown, then the car on the circuit), seed only the final lap.
            app=self.out/'app.elf'
            def sym(n):return symbol(app,n)
            def word(n,signed=False):return int.from_bytes(e.memory(sym(n),2),'little',signed=signed)
            e.input(16);e.run(900);e.input(0)
            assert self.metrics(e)['floor_commits']>20,self.metrics(e)
            assert e.memory(sym('rphase'),1)[0]==1,'The countdown must hand over to the race'
            assert word('speed',True)>100,'Holding up must accelerate the car'
            self.results['race_running']=self.state(e)
            # The rivals are out of the way (rank 1) and the car is on its last lap; the finish hands over to the pursuit.
            e.write(sym('rv'),bytes(7*23));e.write(sym('lapp'),bytes([3]))
            reads=self.metrics(e)['disc_reads'];e.input(16);e.run(600);e.input(0);self.dialogs(e)
            assert self.metrics(e)['phase']==1 and self.metrics(e)['disc_reads']==reads,self.metrics(e)
            assert e.memory(sym('rphase'),1)[0]==3
            # Catch the leader: put the car just behind him (he is a few hundred units up the road).
            bx,by=struct.unpack('<2h',e.memory(sym('boss')+8,4))
            e.write(sym('px'),struct.pack('<2H',bx,(by+120)&8191))   # (not paused: the pause menu restarts the stage)
            e.input(16);e.run(30);e.input(0);self.dialogs(e)
            assert self.state(e)['boss_kind']==4 and e.memory(sym('rphase'),1)[0]==4,self.state(e)
            # One more hit: a shot of the car's own on him with a single hit point left (after any dialogue that has begun by now:
            # the pool is cleared while one is up, and when it begins depends on the loop's speed).
            e.run(30);self.dialogs(e)
            bx,by=struct.unpack('<2h',e.memory(sym('boss')+8,4))
            e.write(sym('boss')+19,bytes([1]))
            e.write(sym('race_bolts'),struct.pack('<4h2B',bx,by,0,0,20,1)+bytes(10*5))
            for _ in range(40):   # the rest of the boss's dialogue, the burn-out, the victory dialogue
                self.dialogs(e)
                if self.state(e)['state']==2:break
                e.run(120)
            self.until(e,lambda:self.state(e)['state']==2,limit=300);self.capture(e,'campaign-pursuit-clear');self.results['race']=self.state(e)
            self.metrics(e)
            # Remaining platform bosses, including Dark April as a second fight.
            for stage,x in ((3,6920),(4,6340),(5,6580)):
                ui=symbol(self.out/'app.elf','pce_ui_state')
                print('platform stage entry',stage,self.state(e),self.metrics(e)['stage'],e.memory(ui,1).hex(),flush=True)
                self.advance(e,stage);self.dialogs(e);self.move(e,x)
                self.seed(e,'safe_timer',250,1);e.run(60);self.dialogs(e)
                if stage==4:self.seed(e,'arena_time',1439)
                self.until(e,lambda:self.state(e)['boss_kind']!=0,limit=1000);self.dialogs(e)
                self.capture(e,f'campaign-boss{stage}')
                self.verify_hull(e)
                if stage==4:
                    self.press(e,8);e.run(120)
                    actor=struct.pack('<4h13B',self.metrics(e)['camera_x']+80,161,0,0,0,0,4,4,1,6,1,0,1,0,0,4,0)   # a sniper stands where it is
                    e.write(symbol(self.out/'app.elf','actors'),actor+bytes(7*21))
                    self.press(e,8)
                # Inject an ordinary player projectile inside the active hit box.
                self.hit_platform_boss(e)
                self.dialogs(e)
                if stage==4:
                    # Mopup stays in play until the surviving arena enemy dies.
                    self.until(e,lambda:self.state(e)['boss_round']==2)
                    assert self.state(e)['state']==0
                    self.press(e,8);e.run(120)
                    ax,ay=struct.unpack('<2h',e.memory(symbol(self.out/'app.elf','actors'),4))
                    e.write(symbol(self.out/'app.elf','shots'),struct.pack('<4h2B',ax,ay,0,0,1,0))
                    self.press(e,8);e.run(120);self.dialogs(e)
                if stage==5:
                    self.until(e,lambda:self.state(e)['boss_kind']==3,limit=600)
                    assert 0<self.state(e)['boss_hp']<=30,self.state(e)
                    self.capture(e,'campaign-dark-april');self.hit_platform_boss(e)
                self.dialogs(e)
                self.clear(e);self.results[f'stage{stage}']=self.state(e)
                print('platform stage clear',stage,self.state(e),flush=True)
            self.advance(e,6);self.dialogs(e);e.input(1);e.run(600);e.input(0)
            self.capture(e,'campaign-mech');assert self.state(e)['score']>0
            # Verify actual hits to a close target and the final wave transition.
            # Arena6 is packed in trigger_cache (m6_state.h), not the retired
            # arena prototype's mechs/aim globals. Offsets verified against
            # the target compiler: Mech6=23 bytes, aim=505, counters=524.
            self.press(e,8);e.run(120)
            arena=symbol(self.out/'app.elf','trigger_cache')
            aim=struct.unpack('<h',e.memory(arena+505,2))[0]
            if self.sgx:
                # Keep this mech alive long enough to publish a real BG
                # frame, before setting up the one-hit final-wave target.
                target=struct.pack('<3h3H11B',aim,500,0,1000,600,850,4,2,200,0,0,0,0,0,0,0,0)
                e.write(arena,target+bytes(46+16*17))
                e.write(arena+524,bytes([4,0,0,0,0,0]));e.write(arena+531,b'\xfa')
                self.field(e,'wave',2);self.press(e,8)
                self.until(e,lambda:e.memory(arena+602,1)==b'\1',limit=120,step=1)
                self.verify_sgx_code(e,'active arena BG mech')
                self.capture(e,'campaign-arena-bg-mech');self.press(e,8)
            target=struct.pack('<3h3H11B',aim,1,0,600,600,850,4,2,200,0,0,0,0,0,0,0,0)
            e.write(arena,target+bytes(46+16*17))
            e.write(arena+524,bytes([4,3,0,0,0,0]))  # spawned, killed, heat, gun_cd, punch_cd, overheated
            e.write(arena+531,b'\xfa');self.field(e,'wave',2)
            score=self.state(e)['score'];self.press(e,8)
            e.input(1);e.run(120);e.input(0)
            self.until(e,lambda:self.state(e)['state']==1 and self.state(e)['story']==3)
            assert e.memory(arena+525,1)==b'\4' and self.state(e)['score']>score,'Native arena bolt must kill the final mech'
            self.results['mech']=self.state(e);self.metrics(e);self.dialogs(e)
            # Stage 6 advances directly after its final story.
            self.until(e,lambda:self.metrics(e)['stage']==7 and self.metrics(e)['ready']);e.run(600)
            self.verify_sgx_code(e,'after arena exit')
            self.capture(e,'campaign-space');self.seed(e,'flight_clock',6359)
            # Remaining foes drain, WARNING lasts 216 ticks, then the hull
            # flies in for 300 ticks before opening its greeting.
            self.until(e,lambda:self.state(e)['boss_kind']==5,limit=1500)
            self.until(e,lambda:self.state(e)['state']==1 and self.state(e)['story']==1,limit=1000)
            self.dialogs(e);e.run(30)
            self.capture(e,'campaign-cruiser')
            # Park the living ship above the hull for the power fixture. The
            # larger SGX nose reaches the old stationary test position; a
            # pending death intentionally rejects controls in space_tick.
            self.press(e,8);e.run(120)
            self.seed(e,'ship_x',16);self.seed(e,'ship_y',20)
            self.seed(e,'pce_death',0,1);self.seed(e,'dead_t',0,1)
            e.write(self.address+34,struct.pack('<H',4));self.seed(e,'hurt',255,1)
            self.field(e,'powers',2);self.field(e,'boss_hp',1200);self.press(e,8)
            self.press(e,5,8);self.until(e,lambda:self.state(e)['state']==0 and self.state(e)['power_cd']>0,limit=2000)
            assert self.state(e)['powers']==1 and self.state(e)['power_cd']>0
            assert self.state(e)['boss_hp']==1116,'Space hero power must take 7 percent of the cruiser hull'
            assert e.memory(symbol(self.out/'app.elf','port_hp'),5)==bytes(5)
            self.results['space_power']=self.state(e);self.field(e,'boss_hp',1)
            self.verify_sgx_code(e,'after cruiser power')
            by=int.from_bytes(e.memory(symbol(self.out/'app.elf','space_boss_y'),2),'little')
            e.write(symbol(self.out/'app.elf','bolts'),struct.pack('<2h2b2B',180,by+50,0,0,1,0));e.run(120);self.dialogs(e)
            self.until(e,lambda:self.state(e)['state']==2);self.press(e,1)
            self.until(e,lambda:self.state(e)['state']==4,limit=2000)
            assert self.state(e)['state']==4;self.capture(e,'campaign-ending');self.results['ending']=self.state(e)
            # Leave credits and deliberately start a new game. RUN held across
            # a frontend transition must no longer confirm the title for us.
            ui=symbol(self.out/'app.elf','pce_ui_state')
            self.until(e,lambda:e.memory(ui,1)!=b'\0',limit=6000)
            for _ in range(40):
                if e.memory(ui,1)==b'\1':break
                self.press(e,1);e.run(97)   # not 120: a credits page lasts 210 frames, and a press every 210 can fall in its first 20 frames for ever
            self.until(e,lambda:e.memory(ui,1)==b'\1',limit=6000)
            self.press(e,1);self.until(e,lambda:e.memory(ui,1)==b'\2')
            self.press(e,1)
            self.until(e,lambda:e.memory(ui,1)==b'\0' and self.metrics(e)['stage']==1 and self.metrics(e)['ready'],limit=6000)
            # A zero-lives collision must reach game over, with no underflow.
            self.field(e,'lives',0);e.write(self.address+34,b'\x01\x00');self.seed(e,'pce_continues',0,1)
            self.seed(e,'safe_timer',0,1)
            px=self.metrics(e)['player_x'];py=self.metrics(e)['player_y']
            e.write(symbol(self.out/'app.elf','shots'),struct.pack('<4h2B',px,py,0,0,1,1));e.run(120)
            assert self.state(e)['state']==3 and self.state(e)['lives']==0
            self.results['game_over']=self.state(e)
            self.press(e,1)
            ui=symbol(self.out/'app.elf','pce_ui_state')
            for _ in range(40):
                e.run(120)
                if e.memory(ui,1)==b'\0' and self.metrics(e)['stage']==1 and self.metrics(e)['ready']:break
                self.press(e,1)
            self.until(e,lambda:self.metrics(e)['stage']==1 and self.metrics(e)['ready'])
            self.move(e,728,97);assert self.metrics(e)['player_y']==97
            self.press(e,66,8);e.run(60)
            assert self.metrics(e)['player_y']>117,'Down + II must drop through a one-way platform'
            self.results['drop_through']=self.metrics(e)
            self.press(e,2,8);e.run(60)
            assert self.metrics(e)['player_y']==97,'The same one-way platform must be reachable after dropping through it'
            self.results['jump_back']=self.metrics(e)
            for hero in range(1,4):
                # Exercise retail hero selection. RUN only pauses retail play;
                # the former debug-menu sequence never changed the hero.
                self.seed(e,'pce_continues',0,1);self.field(e,'state',3)
                self.until(e,lambda:e.memory(ui,1)==b'\4',limit=10000)
                e.run(240);self.press(e,1)
                self.until(e,lambda:e.memory(ui,1)==b'\1',limit=10000)
                e.run(240);self.press(e,1)
                self.until(e,lambda:e.memory(ui,1)==b'\2');e.run(240)
                for _ in range(hero-self.metrics(e)['hero']):self.press(e,32)
                self.press(e,1)
                self.until(e,lambda:e.memory(ui,1)==b'\0' and self.metrics(e)['ready'] and self.metrics(e)['hero']==hero)
                self.seed(e,'dialogs_done',255,1)
                self.field(e,'powers',2);self.seed(e,'safe_timer',250,1)
                self.press(e,5,8);self.until(e,lambda:self.state(e)['state']==0 and self.state(e)['power_cd']>0)
                power_state=self.state(e)
                assert power_state['powers']==1 and power_state['power_cd']>0
                if hero>=2:assert 0<power_state['boost']<=(480 if hero==2 else 600)
                self.press(e,5,8);assert self.state(e)['powers']==1,'Cooldown must reject a second use'
                self.results[f'hero{hero}_power']=power_state
        self.results['scenario_seeding']='Debugger writes to positions, timers, pools, and boss HP; native code executes damage and transitions'
        self.results['emulator']=str(BINARY);self.results['bios']=str(BIOS)
        if self.sgx:
            self.results['sgx_runtime']=dict(vdc1_peak_units=self.sgx_peak_units,
                                             vdc1_peak_sat_count=self.sgx_peak_sat,
                                             failures=0,hardware_sprite_limit=True)
        (self.out/'campaign-verification.json').write_text(json.dumps(self.results,indent=2)+'\n')

def main():
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--sgx',action='store_true');a=p.parse_args()
    Campaign(a.out.resolve(),sgx=a.sgx).run()
    print(('SGX' if a.sgx else 'PCE')+' campaign scenario checks passed.')
if __name__=='__main__':main()
