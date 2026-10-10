#!/usr/bin/env python3
"""Seed final-boss arrival; check native rendering and death/story ordering."""
import argparse
import json
import struct
import tempfile
from pathlib import Path
from emulator import Emulator, boot, symbol
from test_sgx_rendering import Rendering


def verify(out):
    out = out.resolve()
    r = Rendering(out)
    a = {n: symbol(out/'app.elf', n) for n in
         ('flight_clock', 'foes', 'hurt', 'boss_die', 'boss_gone', 'hull_gone',
          'pce_sky_near', 'pce_scroll_x', 'pce_scroll_y', 'expl', 'ship_x', 'ship_y',
          'space_boss_y', 'page_count', 'story_cells', 'story_cells_y', 'story_cells_column')}
    scene = r.manifest['scenes'][6]
    assert [s['name'] for s in scene['sprites'][22:30]] == [f'planet_{i}' for i in range(8)]
    blob = (out/'s7.bin').read_bytes()
    report = {}
    def word(e, name):
        return int.from_bytes(e.memory(a[name], 2), 'little')
    def planet_entries(e):
        slots = e.memory(r.sym['sprite_slot_of']+22, 8)
        words = struct.unpack('<48H', e.memory(r.sym['sprite_words'], 96))
        counts = e.memory(r.sym['sprite_count'], 48)
        ranges = [(words[s]>>5, (words[s]+counts[s]*64)>>5) for s in slots if s<48]
        return [entry for entry in r.sat(e,0) if entry[0] and not entry[3]&128
                and any(lo<=entry[2]<hi for lo,hi in ranges)]
    def assert_story_world(e, frame):
        planets=planet_entries(e)
        assert planets, ('Planet/ship front layer vanished during story transition',frame)
        bat=struct.unpack('<2048H',bytes.fromhex(e.call('asread','vram1',0,4096)['hex']))
        assert any(cell!=0x80 for cell in bat), ('Paired ship/sky BAT became blank during story transition',frame)
        return planets
    def nebula(e):
        # Compare the displayed 33 columns against the repeating source,
        # including world columns beyond the BAT's 64-column wrap.
        scroll = word(e, 'flight_clock') >> 3
        vram = bytes.fromhex(e.call('asread', 'vram1', 0, 65536)['hex'])
        for world in range(scroll//8, scroll//8+33):
            for row in range(28):
                tile, pal = struct.unpack_from('<HB', blob,
                    scene['sgx_map']+(world % scene['cols'])*90+row*3)
                cell = struct.unpack_from('<H', vram, (row*64+(world&63))*2)[0]
                assert cell>>12 == pal, ('nebula palette', world, row)
                address = (cell&4095)*32
                assert vram[address:address+32] == blob[
                    scene['sgx_tiles']+tile*32:scene['sgx_tiles']+(tile+1)*32], ('nebula wrap', world, row)
        return scroll
    with tempfile.TemporaryDirectory(dir=out) as base, Emulator(
            out/'saber_rider.cue', base, sgx=True) as e:
        boot(e, r.address)
        r.dialogs(e)
        r.seed(e, 'stage', 6, 1)
        e.write(r.address+7, b'\6')
        r.field(e, 'state', 2)
        r.advance(e, 7)
        r.dialogs(e)
        e.write(a['foes'], bytes(8*12))
        r.seed(e, 'flight_clock', 6359)
        r.seed(e, 'hurt', 255, 1)
        r.until(e, lambda:r.state(e)['boss_kind']==5, step=1, limit=1500)
        e.run(12)
        planet = planet_entries(e)
        assert word(e, 'pce_sky_near')<260 and planet, 'Planet must survive cruiser arrival'
        report['arrival_planet_entries'] = len(planet)
        e.screenshot(out/'final-boss-arrival.png')
        opening_frames=0
        while r.state(e)['state']!=1:
            e.run(1);opening_frames+=1
            assert opening_frames<=1500, ('Greeting did not open',r.state(e))
            assert_story_world(e,('opening',opening_frames))
        e.run(100)
        # Typed glyphs must occupy the actual scrolled bottom panel.
        x, y = word(e, 'pce_scroll_x'), word(e, 'pce_scroll_y')
        bat = struct.unpack('<2048H', bytes.fromhex(e.call('asread', 'vram0', 0, 4096)['hex']))
        glyphs = [bat[((20+(y>>3)+row)&31)*64+(((x>>3)+col)&63)]
                  for row in range(6) for col in range(5,29)]
        assert any(0xf421 <= cell <= 0xf47f for cell in glyphs), 'Greeting has no visible text'
        report['greeting_visible_text'] = True
        assert planet_entries(e), ('Planet lost during greeting', word(e,'pce_sky_near'),
            list(e.memory(r.sym['sprite_slot_of']+22,8)), r.sat(e,0), r.metrics(e))
        report['greeting_planet_entries'] = len(planet_entries(e))
        e.screenshot(out/'final-boss-greeting.png')
        # The greeting freezes the ship and sky. Advance each real page and
        # compare both the planet sprites and all of VDC1's ship/sky VRAM.
        page_count=e.memory(a['page_count'],1)[0]
        vdc1_story=bytes.fromhex(e.call('asread','vram1',0,65536)['hex'])
        assert any(cell!=0x80 for cell in struct.unpack('<2048H',vdc1_story[:4096])), 'Ship/sky BAT is blank at story open'
        page_samples=[]
        for page in range(page_count):
            e.run(360)  # finish typing before advancing this page
            assert r.state(e)['state']==1 and r.state(e)['page']==page
            planets=planet_entries(e)
            assert planets, ('Planet lost on greeting page',page,word(e,'pce_sky_near'))
            current_vdc1=bytes.fromhex(e.call('asread','vram1',0,65536)['hex'])
            assert current_vdc1==vdc1_story, ('Ship/sky changed on greeting page',page)
            page_samples.append({'page':page,'planet_entries':len(planets),'ship_sky_vram_unchanged':True})
            if page+1<page_count:
                e.input(1)
                for frame in range(1,121):
                    e.run(1)
                    assert_story_world(e,('page advance',page,frame))
                    if r.state(e)['page']==page+1:break
                else:raise AssertionError(('Greeting page did not advance',page))
                e.input(0);e.run(1)
                assert r.state(e)['state']==1 and r.state(e)['page']==page+1, ('Greeting page did not advance',page)
        # Remember the exact platform BAT cells that page zero covered; after
        # the final close, compare the real VRAM against this stage-bank snapshot.
        snapshot_address=110*8192+(a['story_cells']&8191)
        saved_cells=e.memory(snapshot_address,168*2,logical=False)
        saved_y=e.memory(a['story_cells_y'],1)[0]
        saved_col=int.from_bytes(e.memory(a['story_cells_column'],2),'little')
        open_sat=r.sat(e,0)
        dialog=scene['presentation']['dialog']
        cache_ids=struct.unpack('<48H',e.memory(r.sym['sprite_ids'],96))
        words=struct.unpack('<48H',e.memory(r.sym['sprite_words'],96))
        counts=e.memory(r.sym['sprite_count'],48)
        corner_ranges=[(words[slot]>>5,(words[slot]+counts[slot]*64)>>5)
                       for slot,sprite_id in enumerate(cache_ids) if dialog<=sprite_id<dialog+8]
        corner_tuples=[entry for entry in open_sat if entry[0] and
            any(lo<=entry[2]<hi for lo,hi in corner_ranges)]
        assert corner_tuples, 'Greeting dialogue corners were not published'
        e.input(1)
        close_frames=0
        while r.state(e)['state']==1:
            e.run(1);close_frames+=1
            assert close_frames<=120, 'Final greeting page did not close'
            if r.state(e)['state']==1:assert_story_world(e,('close',close_frames))
        e.input(0)
        assert r.state(e)['state']==0, 'Final greeting page did not close'
        expected_cells=struct.unpack('<168H',saved_cells)
        def panel_cells_restored():
            bat=struct.unpack('<2048H',bytes.fromhex(e.call('asread','vram0',0,4096)['hex']))
            for col in range(28):
                first=2 if col<2 or col>=26 else 0
                last=4 if first else 6
                for row in range(first,last):
                    cell=((saved_y+row)&31)*64+((saved_col+3+col)&63)
                    if bat[cell]!=expected_cells[row*28+col]:return False
            return True
        restore_frames=0
        while not panel_cells_restored():
            assert restore_frames<120, ('Dialogue panel cells were not restored',saved_y,saved_col)
            e.run(1);restore_frames+=1
            if panel_cells_restored():
                closed_sat=r.sat(e,0)
                remaining=[entry for entry in closed_sat if entry in corner_tuples]
                assert not remaining, ('Dialogue corners remained when panel cells were restored',corner_tuples,remaining)
                break
        closed_sat=r.sat(e,0)
        remaining=[entry for entry in closed_sat if entry in corner_tuples]
        assert not remaining, ('Dialogue corners remained in the SAT after panel restore',corner_tuples,remaining)
        vdc1_closed=bytes.fromhex(e.call('asread','vram1',0,4096)['hex'])
        assert any(cell!=0x80 for cell in struct.unpack('<2048H',vdc1_closed)), 'Closing greeting blanked the ship/sky BAT'
        report['greeting_pages']=page_samples
        report['greeting_restore']={'source_cells':168,'corners_retired':len(corner_tuples),'ship_sky_vram_unchanged':True,
                                    'opening_frames_checked':opening_frames,'closing_frames_checked':close_frames,
                                    'restore_frames':restore_frames}
        r.seed(e, 'ship_x', 16)
        r.seed(e, 'ship_y', 20)
        r.seed(e, 'hurt', 255, 1)
        e.run(8)
        report['nebula_scroll'] = nebula(e)
        # At rest the source's cropped rear reaches x=256: column 27
        # must remain intact at the screen's right edge.
        assert (word(e, 'pce_scroll_x')+32+int.from_bytes(e.memory(
            symbol(out/'app.elf', 'space_hull_x'),2),'little'))&65535 == 0
        e.screenshot(out/'final-boss-fight.png')
        r.field(e, 'boss_hp', 0)
        r.until(e, lambda:e.memory(a['boss_die'],1)!=b'\0', step=1, limit=120)
        e.run(20)
        assert r.state(e)['state']==0 and e.memory(a['hull_gone'],1)==b'\0'
        assert any(e.memory(a['expl']+k*6+4,1)!=b'\0' for k in range(10)), 'Missing hull explosions'
        slots = e.memory(r.sym['sprite_slot_of'], 22)
        words = struct.unpack('<48H', e.memory(r.sym['sprite_words'], 96))
        counts = e.memory(r.sym['sprite_count'], 48)
        ranges = [(words[slot]>>5, (words[slot]+counts[slot]*64)>>5)
                  for slot in slots[12:22] if slot<48]
        assert any(y and any(lo<=pattern<hi for lo,hi in ranges)
                   for vdc in (0,1) for y,x,pattern,attr in r.sat(e,vdc)), 'Explosion sprites missing from SAT'
        e.screenshot(out/'final-boss-exploding.png')
        # Track the actual death frames. The paired hull must descend every
        # tick, and its BAT must be cleared as soon as the anchor can wrap.
        previous=word(e,'space_boss_y');previous_die=e.memory(a['boss_die'],1)[0]
        fall_frames=0;offscreen_frame=None
        while previous_die:
            e.run(1);fall_frames+=1
            y=word(e,'space_boss_y')
            remaining=e.memory(a['boss_die'],1)[0]
            delta=y-previous
            if remaining==previous_die:
                assert delta==0, ('Boss moved without a death tick',previous,y,fall_frames,previous_die)
            elif remaining:
                ticks=previous_die-remaining
                assert 2*ticks<=delta<=5*ticks, ('Boss fall stopped or jumped',previous,y,fall_frames,previous_die,remaining)
            previous=y
            previous_die=remaining
            if y>=224 and offscreen_frame is None:
                offscreen_frame=fall_frames
                assert e.memory(a['hull_gone'],1)==b'\1', ('Hull BAT not retired at screen edge',y)
                hull_bat=struct.unpack('<2048H',bytes.fromhex(e.call('asread','vram0',0,4096)['hex']))
                leftovers=[(i,cell) for i,cell in enumerate(hull_bat) if cell!=0x80]
                assert not leftovers, ('Offscreen hull wrapped into VDC0 BAT',y,leftovers[:16],len(leftovers))
                e.screenshot(out/'final-boss-offscreen.png')
        assert offscreen_frame is not None, ('Boss did not fall offscreen before death completed',previous,fall_frames)
        report['boss_fall']={'frames':fall_frames,'offscreen_frame':offscreen_frame,'final_y':previous,'bat_cleared_before_death_end':True}
        r.until(e, lambda:e.memory(a['boss_gone'],1)==b'\1', step=1, limit=120)
        e.run(5)
        assert r.state(e)['state']==0 and r.state(e)['event']==0, 'Dialogue opened before explosion completed'
        assert e.memory(a['hull_gone'],1)==b'\1', 'Hull was not removed before dialogue'
        e.screenshot(out/'final-boss-gone.png')
        r.until(e, lambda:r.state(e)['state']==1, step=1, limit=180)
        assert all(e.memory(a['expl']+k*6+4,1)==b'\0' for k in range(10)), 'Dialogue froze active explosions'
        e.run(100)
        e.screenshot(out/'final-boss-victory-dialogue.png')
        report['explosion_completed_before_dialogue'] = True
        r.verify_sgx_code(e, 'final boss')
        r.metrics(e)
    (out/'final-boss-verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--out', type=Path, default=Path('build/sgx'))
    verify(p.parse_args().out)
