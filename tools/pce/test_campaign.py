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
    def hit_platform_boss(self,e):
        self.press(e,8);e.run(120)
        self.field(e,'boss_hp',1);self.seed(e,'boss_phase',3,1);self.seed(e,'boss_time',0)
        bx=self.metrics(e)['camera_x']+180;by=96
        if self.state(e)['boss_kind']==3:
            bx=int.from_bytes(e.memory(symbol(self.out/'app.elf','boss_x'),2),'little')
            by=int.from_bytes(e.memory(symbol(self.out/'app.elf','player')+2,2),'little')
        self.seed(e,'boss_x',bx);self.seed(e,'boss_y',by)
        e.write(symbol(self.out/'app.elf','shots'),struct.pack('<4h2B',bx,by,0,0,1,0))
        self.press(e,8);e.run(60)
    def run(self):
        with tempfile.TemporaryDirectory(prefix='campaign-',dir=self.out) as base,Emulator(self.out/'saber_rider.cue',base) as e:
            boot(e,self.address);e.run(120)
            assert self.state(e)['diagnostic']==0
            reads=self.metrics(e)['disc_reads'];x=self.metrics(e)['player_x']
            e.input(32);self.until(e,lambda:self.state(e)['state']==1,limit=1000);e.input(0)
            assert self.metrics(e)['player_x']>x
            self.capture(e,'campaign-dialog');self.dialogs(e)
            assert self.metrics(e)['disc_reads']==reads,'Dialogs must use preloaded data'
            # Run pauses and resumes the same position without a disc reload.
            x=self.metrics(e)['player_x'];self.press(e,8);e.run(120);self.press(e,8)
            assert self.metrics(e)['player_x']==x and self.metrics(e)['disc_reads']==reads
            # Saber clears bullets, consumes a charge, and enters cooldown.
            self.press(e,5,20);self.until(e,lambda:self.state(e)['state']==0,limit=1000)
            assert self.state(e)['powers']==1 and self.state(e)['power_cd']>0
            self.results['power']=self.state(e)
            # Town boss arrival, player-shot damage, then native clear decision.
            self.move(e,9800);self.until(e,lambda:self.state(e)['boss_kind']==1,limit=600)
            self.seed(e,'boss_phase',3,1);self.seed(e,'boss_time',0);self.field(e,'boss_hp',2)
            self.seed(e,'safe_timer',250,1)
            self.move(e,9780,96);e.input(1);self.until(e,lambda:self.state(e)['result']==1 or self.state(e)['state']==2,limit=2000);e.input(0)
            self.until(e,lambda:self.state(e)['state']==2,limit=600);self.capture(e,'campaign-town-clear')
            self.results['town']=self.state(e)
            self.press(e,1);self.until(e,lambda:self.metrics(e)['stage']==2 and self.metrics(e)['ready']);self.dialogs(e)
            assert self.state(e)['lives']==3,'Lives must carry into the next stage'
            # Run the race and seven rivals, seed only the final gate crossing.
            e.input(16);e.run(600);e.input(0)
            assert self.metrics(e)['floor_commits']>20
            self.results['race_running']=self.state(e)
            self.field(e,'lap',3);self.seed(e,'race_progress',65500)
            self.seed(e,'speed',112,1);e.write(symbol(self.out/'app.elf','rival_laps'),bytes(7))
            reads=self.metrics(e)['disc_reads'];e.input(16);e.run(120);e.input(0);self.dialogs(e)
            assert self.metrics(e)['phase']==1 and self.metrics(e)['disc_reads']==reads
            self.seed(e,'gap',261);self.seed(e,'speed',112,1);e.input(16);e.run(120);e.input(0);self.dialogs(e)
            assert self.state(e)['boss_kind']==4
            self.field(e,'boss_hp',1);e.input(1);e.run(120);e.input(0);self.dialogs(e)
            self.until(e,lambda:self.state(e)['state']==2);self.capture(e,'campaign-pursuit-clear');self.results['race']=self.state(e)
            # Remaining platform bosses, including Dark April as a second fight.
            for stage,x in ((3,6920),(4,6340),(5,6580)):
                self.stage(e,stage);self.dialogs(e);self.move(e,x)
                self.seed(e,'safe_timer',250,1);e.run(60);self.dialogs(e)
                if stage==4:self.seed(e,'arena_time',1439)
                self.until(e,lambda:self.state(e)['boss_kind']!=0,limit=1000);self.dialogs(e)
                self.capture(e,f'campaign-boss{stage}')
                if stage==4:
                    self.press(e,8);e.run(120)
                    actor=struct.pack('<4h9B',self.metrics(e)['camera_x']+80,161,0,0,0,0,4,4,1,2,1,250,1)
                    e.write(symbol(self.out/'app.elf','actors'),actor+bytes(7*17))
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
            for _ in range(30):
                e.run(120)
                if e.memory(ui,1)==b'\0':break
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
