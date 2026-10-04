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
import tempfile
from emulator import Emulator,boot,symbol,BIOS,BINARY
from test_port import Test
FIELDS='state lives powers result event story page diagnostic boss_kind boss_round wave lap rank score boss_hp timer boost power_cd'.split()

class Campaign(Test):
    def __init__(self,out):
        super().__init__(out)
        self.campaign=symbol(out/'app.elf','pce_campaign')
    def state(self,e):return dict(zip(FIELDS,struct.unpack('<13B5H',e.memory(self.campaign,23))))
    def press(self,e,key,n=30):
        e.input(0);e.run(30)
        previous=symbol(self.out/'app.elf','previous')
        self.until(e,lambda:e.memory(previous,1)==b'\0',limit=2000,step=10)
        e.input(key);e.run(n)
        self.until(e,lambda:e.memory(previous,1)[0]&key==key,limit=2000,step=10)
        e.input(0);e.run(n)
        self.until(e,lambda:e.memory(previous,1)==b'\0',limit=2000,step=10)
    def seed(self,e,name,value,width=2):e.write(symbol(self.out/'app.elf',name),int(value).to_bytes(width,'little',signed=value<0))
    def field(self,e,name,value):
        i=FIELDS.index(name);offset=i if i<13 else 13+(i-13)*2
        e.write(self.campaign+offset,value.to_bytes(1 if i<13 else 2,'little'))
    def until(self,e,condition,limit=10000,step=30):
        for _ in range(0,limit,step):
            if condition():return
            e.run(step)
        raise AssertionError(f'Campaign timeout: {self.state(e)}, {self.metrics(e)}')
    def dialogs(self,e):
        for _ in range(100):
            if self.state(e)['state']!=1:return
            self.press(e,1,15)
        raise AssertionError('Dialog did not finish')
    def move(self,e,x,y=160):
        # Pause before changing the body: a video-frame boundary can land
        # inside physics(), whose local coordinates would overwrite a write.
        self.press(e,8);e.run(120)
        e.write(symbol(self.out/'app.elf','player'),struct.pack('<4h4B',x,y,0,0,0,0,4,4))
        self.press(e,8)
    def hit_platform_boss(self,e,hp=1):
        self.press(e,8);e.run(120)
        kind=self.state(e)['boss_kind']
        # The flying bosses take hits in their fighting phases only (gunship 2, Hyperjumper 4-11); Dark April always.
        self.field(e,'boss_hp',hp);self.seed(e,'boss_phase',{1:2,2:5,3:3}[kind],1);self.seed(e,'boss_time',0)
        # A shot at the boss's own centre (the flying bosses steer themselves; Dark April follows the hero's height).
        bx=int.from_bytes(e.memory(symbol(self.out/'app.elf','boss_x'),2),'little')
        by=int.from_bytes(e.memory(symbol(self.out/'app.elf','boss_y'),2),'little')
        if kind==3:by=int.from_bytes(e.memory(symbol(self.out/'app.elf','player')+2,2),'little');self.seed(e,'boss_y',by)
        e.write(symbol(self.out/'app.elf','shots'),struct.pack('<4h2B',bx,by,0,0,1,0))
        self.press(e,8);e.run(60)
        if hp==1 and kind!=3:
            # The wreck falls and burns for 228 steps before the stage clears.
            def gone():
                st=self.state(e);return st['boss_hp']==0 or st['state']!=0 or st['boss_kind']!=kind
            self.until(e,gone,limit=1500)
    def run(self):
        with tempfile.TemporaryDirectory(prefix='campaign-',dir=self.out) as base,Emulator(self.out/'saber_rider.cue',base) as e:
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
            e.run(30);assert below_box(),'The outrider must stay on screen while the text runs'
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
            self.press(e,5,20);self.until(e,lambda:self.state(e)['state']==0,limit=1000)
            assert self.state(e)['powers']==1 and self.state(e)['power_cd']>0
            self.results['power']=self.state(e)
            # A diagonal shot leaves the barrel of the pose drawn (pce_muzzle, up-diagonal standing), at 6 px a
            # step on each axis; it used to leave a fixed point that missed the gun by up to 13 px.
            m=self.metrics(e);table=symbol(self.out/'app.elf','pce_muzzle')
            mx,my=struct.unpack('<2b',e.memory(table+(m['hero']*9+5)*2,2))
            e.write(symbol(self.out/'app.elf','shots'),bytes(16*13))
            e.input(4);e.run(4);e.input(4|16|32|1)
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
            self.hit_platform_boss(e,2);assert self.state(e)['boss_hp']==1,self.state(e)
            self.hit_platform_boss(e,1)
            self.until(e,lambda:self.state(e)['state']==2,limit=600);self.capture(e,'campaign-town-clear')
            self.results['town']=self.state(e)
            self.press(e,1);self.until(e,lambda:self.metrics(e)['stage']==2 and self.metrics(e)['ready']);self.dialogs(e)
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
            # One more hit: a shot of the car's own on him with a single hit point left.
            e.run(30)
            bx,by=struct.unpack('<2h',e.memory(sym('boss')+8,4))
            e.write(sym('boss')+19,bytes([1]))
            e.write(sym('race_bolts'),struct.pack('<4h2B',bx,by,0,0,20,1)+bytes(10*5))
            for _ in range(40):   # the rest of the boss's dialogue, the burn-out, the victory dialogue
                self.dialogs(e)
                if self.state(e)['state']==2:break
                e.run(120)
            self.until(e,lambda:self.state(e)['state']==2,limit=300);self.capture(e,'campaign-pursuit-clear');self.results['race']=self.state(e)
            # Remaining platform bosses, including Dark April as a second fight.
            for stage,x in ((3,6920),(4,6340),(5,6580)):
                self.stage(e,stage);self.dialogs(e);self.move(e,x)
                self.seed(e,'safe_timer',250,1);e.run(60);self.dialogs(e)
                if stage==4:self.seed(e,'arena_time',1439)
                self.until(e,lambda:self.state(e)['boss_kind']!=0,limit=1000);self.dialogs(e)
                self.capture(e,f'campaign-boss{stage}')
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
                self.until(e,lambda:self.state(e)['state']==2);self.results[f'stage{stage}']=self.state(e)
            self.stage(e,6);self.dialogs(e);e.input(1);e.run(600);e.input(0)
            self.capture(e,'campaign-mech');assert self.state(e)['score']>0
            # Verify actual hits to a close target and the final wave transition.
            mech=symbol(self.out/'app.elf','mechs')
            e.write(mech,struct.pack('<h3H2B',128,150,1,0,2,1)+bytes(20))
            self.field(e,'wave',2);self.seed(e,'spawned',8,1);self.seed(e,'killed',7,1)
            self.seed(e,'aim',128,1);self.seed(e,'gun_cd',0,1);self.seed(e,'overheated',0,1)
            e.input(1);e.run(120);e.input(0);self.dialogs(e)
            self.until(e,lambda:self.state(e)['state']==2);self.results['mech']=self.state(e)
            self.press(e,1);self.until(e,lambda:self.metrics(e)['stage']==7 and self.metrics(e)['ready']);e.run(600)
            self.capture(e,'campaign-space');self.seed(e,'flight_clock',6359);e.run(120)
            assert self.state(e)['boss_kind']==5
            self.capture(e,'campaign-cruiser');self.field(e,'powers',2);self.field(e,'boss_hp',1200)
            self.press(e,5,20);self.until(e,lambda:self.state(e)['timer']==0,limit=2000)
            assert self.state(e)['powers']==1 and self.state(e)['power_cd']>0
            assert self.state(e)['boss_hp']==1116,'Space hero power must take 7 percent of the cruiser hull'
            assert e.memory(symbol(self.out/'app.elf','port_hp'),5)==bytes(5)
            self.results['space_power']=self.state(e);self.field(e,'boss_hp',1)
            by=int.from_bytes(e.memory(symbol(self.out/'app.elf','space_boss_y'),2),'little')
            e.write(symbol(self.out/'app.elf','bolts'),struct.pack('<2h2b2B',130,by+14,0,0,1,0));e.run(120);self.dialogs(e)
            self.until(e,lambda:self.state(e)['state']==2);self.press(e,1)
            assert self.state(e)['state']==4;self.capture(e,'campaign-ending');self.results['ending']=self.state(e)
            # A zero-lives collision must reach game over, with no underflow.
            self.stage(e,1);self.field(e,'lives',0);e.write(self.address+34,b'\x01\x00');self.seed(e,'pce_continues',0,1)
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
            for hero in range(1,4):
                self.press(e,8)
                for _ in range((hero-self.metrics(e)['hero'])&3):self.press(e,16)
                self.press(e,8)
                self.until(e,lambda:self.metrics(e)['ready'] and self.metrics(e)['hero']==hero)
                self.field(e,'powers',2);self.seed(e,'safe_timer',250,1)
                self.press(e,5,20);self.until(e,lambda:self.state(e)['state']==0)
                power_state=self.state(e)
                assert power_state['powers']==1 and power_state['power_cd']>0
                if hero>=2:assert 0<power_state['boost']<=(480 if hero==2 else 600)
                self.press(e,5,20);assert self.state(e)['powers']==1,'Cooldown must reject a second use'
                self.results[f'hero{hero}_power']=power_state
        self.results['scenario_seeding']='Debugger writes to positions, timers, pools, and boss HP; native code executes damage and transitions'
        self.results['emulator']=str(BINARY);self.results['bios']=str(BIOS)
        (self.out/'campaign-verification.json').write_text(json.dumps(self.results,indent=2)+'\n')

def main():
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    Campaign(a.out.resolve()).run();print('PCE campaign scenario checks passed.')
if __name__=='__main__':main()
